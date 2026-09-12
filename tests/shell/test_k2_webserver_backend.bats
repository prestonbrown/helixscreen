#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The K2 web-server carve-out (prestonbrown/helixscreen#1617). hooks-k2.sh
# spares web-server from its kill list but also runs `/etc/init.d/app
# disable`, and procd disables the app service as a whole — so nothing of
# the stock set, web-server included, starts at boot. The carve-out is an
# rc.common init script we install (/etc/init.d/helix-k2-webserver) that
# starts exactly web-server, independent of the app service.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-k2.sh"
MODULE="$WORKTREE_ROOT/scripts/lib/installer/service.sh"
MAIN_MODULE="$WORKTREE_ROOT/scripts/lib/installer/main.sh"
UNINSTALL_MODULE="$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
UNINSTALL_BUNDLE="$WORKTREE_ROOT/scripts/uninstall.sh"
CROSSMK="$WORKTREE_ROOT/mk/cross.mk"
INIT_SRC="$WORKTREE_ROOT/config/k2-webserver.init"
BACKEND_INIT="/etc/init.d/helix-k2-webserver"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"

    export MOCK_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/etc/rc.d" \
             "$MOCK_ROOT/usr/bin" "$MOCK_ROOT/var/run"

    write_fake_rc_common

    SUDO=""
    export SUDO
    platform="k2"

    # The production service module with the absolute paths it hardcodes
    # redirected into MOCK_ROOT, so the real functions run against a fake
    # root. The rc.common mapping is what makes the installed script's
    # shebang resolvable on the test host.
    local patched="$BATS_TEST_TMPDIR/service.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/etc/rc.d/|$MOCK_ROOT/etc/rc.d/|g" \
        -e "s|/etc/rc\.common|$MOCK_ROOT/etc/rc.common|g" \
        "$MODULE" > "$patched"
    unset _HELIX_SERVICE_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    record_disabled_service() {
        echo "$1:$2" >> "$DISABLED_SERVICES_FILE"
    }
    log_info() { echo "INFO $*" >> "$BATS_TEST_TMPDIR/log"; }
    log_warn() { echo "WARN $*" >> "$BATS_TEST_TMPDIR/log"; }
    log_error() { echo "ERROR $*" >> "$BATS_TEST_TMPDIR/log"; }
    log_success() { echo "OK $*" >> "$BATS_TEST_TMPDIR/log"; }
    export -f record_disabled_service log_info log_warn log_error log_success
}

# Minimal stand-in for OpenWrt's rc.common: source the service script and
# dispatch the action, plus the enable/disable symlink handling the boot
# iterator depends on. START is hardcoded to 99 the way the real one reads
# it from the script's START= line.
write_fake_rc_common() {
    cat > "$MOCK_ROOT/etc/rc.common" << RC_EOF
#!/bin/sh
script="\$1"
action="\${2:-boot}"
name=\$(basename "\$script")
. "\$script"
case "\$action" in
    enable)
        mkdir -p "$MOCK_ROOT/etc/rc.d"
        ln -sfn "../init.d/\$name" "$MOCK_ROOT/etc/rc.d/S99\$name"
        ln -sfn "../init.d/\$name" "$MOCK_ROOT/etc/rc.d/K01\$name"
        ;;
    disable)
        rm -f "$MOCK_ROOT/etc/rc.d/S99\$name" "$MOCK_ROOT/etc/rc.d/K01\$name"
        ;;
    *)
        "\$action"
        ;;
esac
RC_EOF
    chmod +x "$MOCK_ROOT/etc/rc.common"
}

# The kill list of the `for proc in ...` loop in $1, with the hook's
# backslash line continuations joined.
extract_k2_kill_list() {
    tr '\n' ' ' < "$1" | sed 's/.*for proc in //; s/; do.*//; s/\\//g; s/  */ /g; s/ $//'
}

# A redirected copy of the shipped init script: the shebang points at the
# fake rc.common, every absolute path it touches lands under MOCK_ROOT, and
# the PATH hardening is dropped so mocked pidof on the test PATH wins.
redirected_init_script() {
    sed -e "s|#!/bin/sh /etc/rc.common|#!/bin/sh $MOCK_ROOT/etc/rc.common|" \
        -e "s|/usr/sbin:/usr/bin:/sbin:/bin:||" \
        -e "s|/usr/bin/web-server|$MOCK_ROOT/usr/bin/web-server|g" \
        -e "s|/var/run/helix-k2-webserver.pid|$MOCK_ROOT/var/run/helix-k2-webserver.pid|g" \
        "$INIT_SRC"
}

# A fake web-server that records its launch and stays alive long enough for
# lifecycle assertions.
write_fake_webserver() {
    printf '#!/bin/sh\necho "launched web-server" >> "%s/servers.log"\nsleep 30\n' "$BATS_TEST_TMPDIR" \
        > "$MOCK_ROOT/usr/bin/web-server"
    chmod +x "$MOCK_ROOT/usr/bin/web-server"
}

# An executable stock app service in the mock root.
write_stock_app_service() {
    printf '#!/bin/sh /etc/rc.common\nSTART=99\nDEPEND=done\n' > "$MOCK_ROOT/etc/init.d/app"
    chmod +x "$MOCK_ROOT/etc/init.d/app"
    sed -i "s|#!/bin/sh /etc/rc.common|#!/bin/sh $MOCK_ROOT/etc/rc.common|" "$MOCK_ROOT/etc/init.d/app"
}

# --- the runtime hook: what stays killed and what is spared ---

@test "k2: runtime hook kill list spares web-server" {
    local list
    list="$(extract_k2_kill_list "$HOOK")"
    if echo "$list" | grep -qw web-server; then
        echo "web-server must stay off the K2 kill list (got: $list)" >&2
        return 1
    fi
}

@test "k2: runtime hook kill list still takes the display stack and AI daemons down" {
    local list
    list="$(extract_k2_kill_list "$HOOK")"
    [ "$list" = "display-server Monitor master-server audio-server wifi-server app-server upgrade-server" ]
}

@test "k2: runtime hook still disables the stock app service" {
    # The fix must not narrow the disable: the display stack and the AI
    # daemons stay down at boot by design; the carve-out gets its own
    # starter instead.
    grep -q '/etc/init.d/app disable' "$HOOK"
}

@test "k2: hook names the carve-out's boot starter" {
    grep -q 'helix-k2-webserver' "$HOOK"
}

# --- the init script asset: the procd boot contract ---

@test "k2 init script has the rc.common shebang procd's boot iterator requires" {
    head -1 "$INIT_SRC" | grep -q '^#!/bin/sh /etc/rc.common$'
}

@test "k2 init script declares START, STOP and DEPEND" {
    grep -q '^START=99$' "$INIT_SRC"
    grep -q '^STOP=01$' "$INIT_SRC"
    grep -q '^DEPEND="done"$' "$INIT_SRC"
}

@test "k2 init script passes sh syntax check" {
    sh -n "$INIT_SRC"
}

@test "k2 init script passes shellcheck" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    shellcheck -s sh "$INIT_SRC"
}

@test "k2 init script starts exactly web-server" {
    [ "$(sed -n 's|^WEBSERVER_BIN="/usr/bin/\(.*\)"$|\1|p' "$INIT_SRC")" = "web-server" ]
}

@test "k2 init script never kills by name" {
    # stop is pidfile-scoped so it cannot take down a stock web-server that
    # an uninstall-time /etc/init.d/app start just restored.
    if grep -q 'killall' "$INIT_SRC"; then
        echo "stop must kill via pidfile, not killall" >&2
        return 1
    fi
}

# --- the init script asset: behavior, via the fake rc.common ---

@test "k2 init script: start launches web-server once and records the pidfile" {
    write_fake_webserver
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "exit 1"

    run "$dest" start
    [ "$status" -eq 0 ]
    [ "$(grep -c "launched web-server" "$BATS_TEST_TMPDIR/servers.log")" -eq 1 ]
    [ -f "$MOCK_ROOT/var/run/helix-k2-webserver.pid" ]

    # A second start that sees web-server running launches nothing new.
    mock_command_script "pidof" "echo 314; exit 0"
    run "$dest" start
    [ "$status" -eq 0 ]
    [ "$(grep -c "launched web-server" "$BATS_TEST_TMPDIR/servers.log")" -eq 1 ]
}

@test "k2 init script: real lifecycle — stop kills the started pid and clears the pidfile" {
    write_fake_webserver
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "exit 1"

    "$dest" start
    local pid
    pid="$(cat "$MOCK_ROOT/var/run/helix-k2-webserver.pid")"
    kill -0 "$pid"

    run "$dest" stop
    [ "$status" -eq 0 ]
    [ ! -f "$MOCK_ROOT/var/run/helix-k2-webserver.pid" ]
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        echo "stop left the started web-server alive" >&2
        return 1
    fi
}

@test "k2 init script: start skips a missing binary without failing" {
    # A firmware variant without the Creality stack must boot clean.
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "exit 1"

    run "$dest" start
    [ "$status" -eq 0 ]
    [ ! -f "$BATS_TEST_TMPDIR/servers.log" ]
}

@test "k2 init script: status reports running and not-running" {
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"

    mock_command_script "pidof" "echo 4242; exit 0"
    run "$dest" status
    [ "$status" -eq 0 ]
    contains "web-server is running (PID 4242)" "$output"

    mock_command_script "pidof" "exit 1"
    run "$dest" status
    [ "$status" -ne 0 ]
    contains "web-server is not running" "$output"
}

# --- install_k2_webserver_backend ---

@test "k2 install: installs, enables, records and starts the carve-out" {
    write_fake_webserver
    write_stock_app_service
    redirected_init_script > "$INSTALL_DIR/config/k2-webserver.init"
    mock_command_script "pidof" "exit 1"

    run install_k2_webserver_backend k2
    [ "$status" -eq 0 ]

    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    [ -x "$dest" ]
    grep -qF "sysv-created:$dest" "$DISABLED_SERVICES_FILE"
    # Enabled: the boot symlink the procd iterator needs, verified by link.
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helix-k2-webserver")" = "../init.d/helix-k2-webserver" ]
    # Started within the install: the fake binary recorded its launch.
    grep -q "launched web-server" "$BATS_TEST_TMPDIR/servers.log"
}

@test "k2 install: no-op off the k2 platform" {
    write_fake_webserver
    write_stock_app_service
    redirected_init_script > "$INSTALL_DIR/config/k2-webserver.init"

    run install_k2_webserver_backend pi
    [ "$status" -eq 0 ]
    [ ! -e "$MOCK_ROOT/etc/init.d/helix-k2-webserver" ]
    [ ! -f "$DISABLED_SERVICES_FILE" ]
}

@test "k2 install: no-op when the stock app service is absent" {
    # A firmware without /etc/init.d/app has no stock set to carve out of.
    redirected_init_script > "$INSTALL_DIR/config/k2-webserver.init"

    run install_k2_webserver_backend k2
    [ "$status" -eq 0 ]
    [ ! -e "$MOCK_ROOT/etc/init.d/helix-k2-webserver" ]
}

@test "k2 install: warns and continues when the asset is missing" {
    write_stock_app_service
    mock_command_script "pidof" "exit 1"

    run install_k2_webserver_backend k2
    [ "$status" -eq 0 ]
    grep -q "WARN.*k2-webserver.init missing" "$BATS_TEST_TMPDIR/log"
    [ ! -e "$MOCK_ROOT/etc/init.d/helix-k2-webserver" ]
}

@test "k2 install: reached from the install flow after start_service" {
    # The function is worthless if nothing invokes it, and it must run
    # AFTER the service start (which is what stops the stock web-server).
    grep -q 'install_k2_webserver_backend' "$MAIN_MODULE"
    awk '/start_service "\$platform"/{seen=1} seen && /install_k2_webserver_backend/{found=1} END{exit !found}' "$MAIN_MODULE"
}

# --- the dev deploy path ---

@test "k2 deploy ships, enables, verifies and starts the carve-out" {
    grep -q 'k2-webserver.init' "$CROSSMK"
    grep -q 'helix-k2-webserver' "$CROSSMK"
    grep -q '/etc/init.d/helix-k2-webserver enable' "$CROSSMK"
    grep -q 'readlink /etc/rc.d/S99helix-k2-webserver' "$CROSSMK"
    grep -q '/etc/init.d/helix-k2-webserver start' "$CROSSMK"
}

# --- uninstall ---

@test "k2 uninstall: sysv-created removal disables an rc.common script before rm" {
    # With the generated uninstaller patched onto the mock root, a
    # sysv-created rc.common script loses its rc.d boot symlinks when the
    # state file is replayed — a plain stop+rm would leave them dangling.
    local patched="$BATS_TEST_TMPDIR/uninstall.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/etc/rc\.common|$MOCK_ROOT/etc/rc.common|g" \
        "$UNINSTALL_BUNDLE" > "$patched"
    # shellcheck disable=SC1090
    ( INSTALL_DIR="$INSTALL_DIR" SUDO="" _UNINSTALL_BUNDLE_TEST=1
      # shellcheck disable=SC1090
      . "$patched"
      write_fake_webserver
      write_stock_app_service
      redirected_init_script > "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
      chmod +x "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
      "$MOCK_ROOT/etc/rc.common" "$MOCK_ROOT/etc/init.d/helix-k2-webserver" enable
      [ -L "$MOCK_ROOT/etc/rc.d/S99helix-k2-webserver" ] || exit 10
      echo "sysv-created:$MOCK_ROOT/etc/init.d/helix-k2-webserver" \
          > "$INSTALL_DIR/config/.disabled_services"
      reenable_disabled_services
      [ -f "$MOCK_ROOT/etc/init.d/helix-k2-webserver" ] && exit 11
      [ -e "$MOCK_ROOT/etc/rc.d/S99helix-k2-webserver" ] && exit 12
      [ -e "$MOCK_ROOT/etc/rc.d/K01helix-k2-webserver" ] && exit 13
      exit 0 )
    [ "$?" -eq 0 ]
}

@test "k2 uninstall: stock-UI restore kills a running web-server before app start" {
    # The restored stock service spawns its own web-server; one left
    # running by the carve-out would hold port 80 out from under it.
    awk '/Re-enabling Creality stock UI/,/init\.d\/app start/' "$UNINSTALL_MODULE" \
        | grep -q 'killall web-server'
}
