#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The K2 web-server carve-out (prestonbrown/helixscreen#1617). hooks-k2.sh
# spares web-server from its kill list but also runs `/etc/init.d/app
# stop` — and on this Tina/procd box that stop takes a RUNNING web-server
# down with it (killall -9 in the stock app's stop_service; `disable` only
# removes rc.d links). Liveness is therefore the hook's job:
# platform_stop_competing_uis restores web-server at its end, via the
# /etc/init.d/helix-k2-webserver rc.common script we install (manual and
# service semantics, plus a belt-and-braces boot entry).

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-k2.sh"
MODULE="$WORKTREE_ROOT/scripts/lib/installer/service.sh"
MAIN_MODULE="$WORKTREE_ROOT/scripts/lib/installer/main.sh"
UNINSTALL_MODULE="$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
CROSSMK="$WORKTREE_ROOT/mk/cross.mk"
INIT_SRC="$WORKTREE_ROOT/config/k2-webserver.init"
SHIM_SRC="$WORKTREE_ROOT/config/helixscreen-k2-procd-shim.sh"
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
# iterator depends on. enable makes S${START}name + K${STOP}name with both
# slots read from the target script's own directives (the convention
# measured on the device), disable rm's the S??/K?? entries in any slot and
# kills nothing (disable only removes rc.d links), and enable exits 0 when
# at least one link was made — the partial-enable trap the installer
# verifies against.
#
# A USE_PROCD script gets a one-instance procd model: `start` runs its
# start_service with procd_open_instance/procd_set_param/
# procd_close_instance stubs that record the instance's params and spawn
# the command in the background, tracking its pid via a pidfile line under
# var/run/procd-instances/<service>. `stop` kills the tracked pids and
# drops the record (procd's stop waits for exit); `status` reports from the
# record; `restart` is stop+start. A start whose tracked pid is dead
# REPLACES the registration — the fake's contract for the registered-but-
# dead corner; the device pass reconciles with real procd and we adjust if
# they differ. A script without USE_PROCD keeps legacy dispatch of its own
# functions, and `stop` also kills the tracked pids (the stock app's
# stop_service killall takes down whatever is tracked under it).
write_fake_rc_common() {
    cat > "$MOCK_ROOT/etc/rc.common" << RC_EOF
#!/bin/sh
script="\$1"
action="\${2:-boot}"
name=\$(basename "\$script")
. "\$script"
instances="$MOCK_ROOT/var/run/procd-instances/\$name"
instance_params="$MOCK_ROOT/var/run/procd-params-\$name"
kill_instances() {
    [ -f "\$instances" ] || return 0
    _killed=""
    while IFS= read -r pf; do
        [ -f "\$pf" ] || continue
        _ip=\$(cat "\$pf" 2>/dev/null)
        [ -n "\$_ip" ] && kill "\$_ip" 2>/dev/null && _killed="\$_killed \$_ip"
    done < "\$instances"
    rm -f "\$instances"
    # procd's stop returns once the instance has exited. Wait for it, up to
    # 20s, so a loaded host's slow exit is never read as a survivor.
    for _kp in \$_killed; do
        _n=0
        while kill -0 "\$_kp" 2>/dev/null && [ "\$_n" -lt 80 ]; do
            sleep 0.25
            _n=\$((_n + 1))
        done
    done
    return 0
}
procd_open_instance() {
    : > "\$instance_params"
    rm -f "\$instances"
}
procd_set_param() {
    case "\$1" in
        command) shift; printf 'command %s\n' "\$*" >> "\$instance_params" ;;
        respawn) shift; printf 'respawn %s\n' "\$*" >> "\$instance_params" ;;
        env)     shift; printf 'env %s\n' "\$*" >> "\$instance_params" ;;
    esac
    return 0
}
procd_close_instance() {
    _cmd=\$(sed -n 's/^command //p' "\$instance_params" | tail -n 1)
    [ -n "\$_cmd" ] || return 0
    "\$_cmd" >/dev/null 2>&1 &
    _pid=\$!
    _pidfile="$MOCK_ROOT/var/run/procd-\$name.pid"
    echo "\$_pid" > "\$_pidfile"
    mkdir -p "$MOCK_ROOT/var/run/procd-instances"
    echo "\$_pidfile" > "\$instances"
    return 0
}
case "\$action" in
    enable)
        mkdir -p "$MOCK_ROOT/etc/rc.d"
        [ -n "\$START" ] && ln -sfn "../init.d/\$name" "$MOCK_ROOT/etc/rc.d/S\$START\$name"
        [ -n "\$STOP" ] && ln -sfn "../init.d/\$name" "$MOCK_ROOT/etc/rc.d/K\$STOP\$name"
        exit 0
        ;;
    disable)
        rm -f "$MOCK_ROOT"/etc/rc.d/S??"\$name" "$MOCK_ROOT"/etc/rc.d/K??"\$name"
        ;;
    start)
        if [ "\$USE_PROCD" = 1 ]; then
            start_service
            exit 0
        fi
        command -v start >/dev/null 2>&1 && start
        ;;
    stop)
        if [ "\$USE_PROCD" = 1 ]; then
            kill_instances
            exit 0
        fi
        kill_instances
        command -v stop >/dev/null 2>&1 && stop
        ;;
    restart)
        if [ "\$USE_PROCD" = 1 ]; then
            kill_instances
            start_service
            exit 0
        fi
        command -v restart >/dev/null 2>&1 && restart
        ;;
    status)
        if [ "\$USE_PROCD" = 1 ]; then
            if [ -f "\$instances" ]; then
                while IFS= read -r pf; do
                    _p=\$(cat "\$pf" 2>/dev/null)
                    if [ -n "\$_p" ] && kill -0 "\$_p" 2>/dev/null; then
                        echo "running (PID \$_p)"
                        exit 0
                    fi
                done < "\$instances"
            fi
            echo "stopped"
            exit 1
        fi
        command -v status >/dev/null 2>&1 && status
        ;;
    *)
        command -v "\$action" >/dev/null 2>&1 && "\$action"
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

# Repoint an rc.common script's shebang at the fake rc.common, so a file
# written with the device spelling executes on the test host (which has no
# /etc/rc.common).
repoint_rc_common_shebang() {
    sed -i "s|#!/bin/sh /etc/rc.common|#!/bin/sh $MOCK_ROOT/etc/rc.common|" "$1"
}

# A redirected copy of the shipped init script: the shebang points at the
# fake rc.common, every absolute path it touches lands under MOCK_ROOT, and
# the PATH hardening is dropped so mocked pidof on the test PATH wins.
redirected_init_script() {
    local out="$BATS_TEST_TMPDIR/k2-webserver.init.redirected"
    sed -e "s|/usr/sbin:/usr/bin:/sbin:/bin:||" \
        -e "s|/usr/bin/web-server|$MOCK_ROOT/usr/bin/web-server|g" \
        "$INIT_SRC" > "$out"
    repoint_rc_common_shebang "$out"
    cat "$out"
}

# A fake web-server that records its launch, notes its pid, and stays alive
# long enough for lifecycle assertions. It takes a second to exit on SIGTERM,
# the way a real server does under load, so a stop that does not wait for the
# exit is caught every run instead of once in a while. It closes bats' output
# fd 3, which bats waits on before ending the file, and takes its sleep down
# with it, so an instance outliving its test costs the run nothing.
write_fake_webserver() {
    printf '#!/bin/sh\nexec 3>&-\necho $$ > "%s/webserver.pid"\necho "launched web-server" >> "%s/servers.log"\ntrap "kill \\$! 2>/dev/null; sleep 1; exit 0" TERM\nsleep 30 & wait\n' \
        "$BATS_TEST_TMPDIR" "$BATS_TEST_TMPDIR" \
        > "$MOCK_ROOT/usr/bin/web-server"
    chmod +x "$MOCK_ROOT/usr/bin/web-server"
}

# The fake web-server logs its launch line from inside the backgrounded
# child, so a count sampled the moment the caller returns can miss the
# child's write. For the same reason a test that kills or replaces an
# instance first waits for that instance's own launch line: a slow start
# killed before it logs leaves one line where the test counts two. Poll
# briefly for the expected count instead of grepping once; on timeout dump
# the log so the mismatch is readable.
await_launch_count() {
    local want="$1" got=""
    # 20s: under a full parallel bats run the restart's second launch can take
    # well over 5s. A match returns at once, so the bound costs a passing run nothing.
    for _ in $(seq 80); do
        got="$(grep -c "launched web-server" "$BATS_TEST_TMPDIR/servers.log" || true)"
        [ "$got" -eq "$want" ] && return 0
        sleep 0.25
    done
    echo "expected $want launched web-server lines, saw $got:" >&2
    cat "$BATS_TEST_TMPDIR/servers.log" >&2
    return 1
}

# Stateful pidof stand-in: reports web-server alive when a tracked
# instance pid (procd's record) or the stub's own pidfile names a live
# process — the same question real pidof answers for the guards.
mock_pidof_webserver() {
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/pidof" << PIDOF_EOF
#!/bin/sh
[ "\$1" = web-server ] || exit 1
if [ -f "$MOCK_ROOT/var/run/procd-instances/helix-k2-webserver" ]; then
    while IFS= read -r pf; do
        p=\$(cat "\$pf" 2>/dev/null)
        if [ -n "\$p" ] && kill -0 "\$p" 2>/dev/null; then
            echo "\$p"
            exit 0
        fi
    done < "$MOCK_ROOT/var/run/procd-instances/helix-k2-webserver"
fi
# A bare-launched stub (no procd record) is still visible by name to a
# real pidof; the stub's own pidfile models that.
if [ -f "$BATS_TEST_TMPDIR/webserver.pid" ]; then
    p=\$(cat "$BATS_TEST_TMPDIR/webserver.pid" 2>/dev/null)
    if [ -n "\$p" ] && kill -0 "\$p" 2>/dev/null; then
        echo "\$p"
        exit 0
    fi
fi
exit 1
PIDOF_EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/pidof"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

# Mark the running web-server as an instance the stock app service's stop
# takes down: the hook's /etc/init.d/app stop runs killall -9 over the
# stock set, ours included.
seed_app_tracks_webserver() {
    mkdir -p "$MOCK_ROOT/var/run/procd-instances"
    echo "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid" \
        > "$MOCK_ROOT/var/run/procd-instances/app"
}

# A path-redirected copy of the runtime hook: every absolute path it touches
# lands under MOCK_ROOT.
redirected_hook() {
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/usr/bin/web-server|$MOCK_ROOT/usr/bin/web-server|g" \
        "$HOOK"
}

# Run the hook's stop path the way S99helixscreen start does at every boot
# and service restart.
run_hook_stop_competing_uis() {
    local patched="$BATS_TEST_TMPDIR/hooks-k2.sh"
    redirected_hook > "$patched"
    # shellcheck disable=SC1090
    . "$patched"
    platform_stop_competing_uis
}

# An executable stock app service in the mock root. START/STOP mirror the
# stock script's own directives: enabling it on the device produces both
# S99app and K01app.
write_stock_app_service() {
    printf '#!/bin/sh /etc/rc.common\nSTART=99\nSTOP=01\nDEPEND=done\n' > "$MOCK_ROOT/etc/init.d/app"
    chmod +x "$MOCK_ROOT/etc/init.d/app"
    repoint_rc_common_shebang "$MOCK_ROOT/etc/init.d/app"
}

# The shim asset staged where install_procd_shim_k2 looks for it, its
# shebang repointed at the fake rc.common the way a deployed /etc/init.d
# script resolves on the device.
write_shim_source() {
    cp "$SHIM_SRC" "$INSTALL_DIR/config/helixscreen-k2-procd-shim.sh"
    repoint_rc_common_shebang "$INSTALL_DIR/config/helixscreen-k2-procd-shim.sh"
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
    # The disable must not be narrowed: the display stack and the AI
    # daemons stay down at boot by design; the carve-out gets its own
    # starter instead.
    grep -q '/etc/init.d/app disable' "$HOOK"
}

@test "k2: hook names the carve-out's boot starter" {
    grep -q 'helix-k2-webserver' "$HOOK"
}

# --- the init script asset: the procd contract ---

@test "k2 init script has the rc.common shebang rc.common dispatches" {
    head -1 "$INIT_SRC" | grep -q '^#!/bin/sh /etc/rc.common$'
}

@test "k2 init script declares START, STOP, DEPEND and USE_PROCD" {
    grep -q '^START=99$' "$INIT_SRC"
    grep -q '^STOP=01$' "$INIT_SRC"
    grep -q '^DEPEND="done"$' "$INIT_SRC"
    # rc.common supplies stop/restart/status for a USE_PROCD script.
    grep -q '^USE_PROCD=1$' "$INIT_SRC"
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
    # One instance, running exactly that binary.
    [ "$(grep -c '^ *procd_set_param command ' "$INIT_SRC")" -eq 1 ]
    grep -q 'procd_set_param command "$WEBSERVER_BIN"' "$INIT_SRC"
}

@test "k2 init script registers its instance with an unbounded respawn" {
    grep -q 'procd_set_param respawn' "$INIT_SRC"
    # retry 0 = respawn forever: the carve-out must outlive arbitrary
    # crashes, so a bounded retry that gives up during a long outage is
    # the wrong default.
    grep -q 'HELIX_WS_RESPAWN_RETRY:-0' "$INIT_SRC"
    # The instance env mirrors the stock app's.
    grep -q 'procd_set_param env HOME=/root' "$INIT_SRC"
}

@test "k2 init script issues no kill anywhere" {
    # Stopping is procd's: the instance-scoped stop rc.common supplies
    # under USE_PROCD. A kill issued here could reach a stock web-server
    # that an uninstall-time /etc/init.d/app start just restored. The file
    # check comes first: a missing file must read as failure, not as "no
    # kill found". Comments are excluded — the header narrates the stock
    # stop's killall.
    [ -f "$INIT_SRC" ]
    if sed '/^[[:space:]]*#/d' "$INIT_SRC" | grep -nE '(killall|kill)([[:space:]]|$)'; then
        echo "the script must not kill; procd owns stopping" >&2
        return 1
    fi
}

# --- the init script asset: behavior, via the fake rc.common ---

@test "k2 init script: start registers one instance and launches web-server once" {
    write_fake_webserver
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_pidof_webserver

    run "$dest" start
    [ "$status" -eq 0 ]
    await_launch_count 1
    local pid
    pid="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    kill -0 "$pid"
    # The instance record tracks exactly that pid's pidfile.
    [ "$(cat "$MOCK_ROOT/var/run/procd-instances/helix-k2-webserver")" = \
       "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid" ]

    # A second start that sees web-server running registers nothing new.
    run "$dest" start
    [ "$status" -eq 0 ]
    await_launch_count 1
}

@test "k2 init script: stop kills the instance process and drops the record" {
    write_fake_webserver
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_pidof_webserver

    "$dest" start
    local pid
    pid="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    kill -0 "$pid"

    run "$dest" stop
    [ "$status" -eq 0 ]
    [ ! -f "$MOCK_ROOT/var/run/procd-instances/helix-k2-webserver" ]
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        echo "stop left the instance process alive" >&2
        return 1
    fi
}

@test "k2 init script: start skips a missing binary without failing" {
    # A firmware variant without the Creality stack must boot clean, and
    # registers nothing.
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_pidof_webserver

    run "$dest" start
    [ "$status" -eq 0 ]
    [ ! -f "$BATS_TEST_TMPDIR/servers.log" ]
    [ ! -f "$MOCK_ROOT/var/run/procd-instances/helix-k2-webserver" ]
}

@test "k2 init script: status reports the tracked instance" {
    write_fake_webserver
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_pidof_webserver

    run "$dest" status
    [ "$status" -ne 0 ]
    contains "stopped" "$output"

    "$dest" start
    run "$dest" status
    [ "$status" -eq 0 ]
    contains "running" "$output"
}

@test "k2 init script: restart replaces the instance, leaving the new one up" {
    write_fake_webserver
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_pidof_webserver

    "$dest" start
    await_launch_count 1
    local old
    old="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"

    run "$dest" restart
    [ "$status" -eq 0 ]
    await_launch_count 2
    local new
    new="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    [ "$new" != "$old" ]
    kill -0 "$new"
    if kill -0 "$old" 2>/dev/null; then
        kill -9 "$old" 2>/dev/null
        echo "restart left the old instance alive" >&2
        return 1
    fi
}

@test "k2 init script: a killed registered instance is replaced by start" {
    # The fake's contract for the registered-but-dead corner: start
    # re-registers (replaces). The device pass reconciles with real procd
    # and we adjust if they differ.
    write_fake_webserver
    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_pidof_webserver

    "$dest" start
    await_launch_count 1
    local old
    old="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    kill -9 "$old" 2>/dev/null
    local tries=0
    while kill -0 "$old" 2>/dev/null && [ "$tries" -lt 50 ]; do
        tries=$((tries + 1))
        sleep 0.1
    done

    "$dest" start
    await_launch_count 2
    local new
    new="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    [ "$new" != "$old" ]
    kill -0 "$new"
}

# --- install_k2_webserver_backend ---

@test "k2 install: installs, enables and records the carve-out without starting it" {
    write_fake_webserver
    write_stock_app_service
    redirected_init_script > "$INSTALL_DIR/config/k2-webserver.init"

    run install_k2_webserver_backend k2
    [ "$status" -eq 0 ]

    local dest="$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    [ -x "$dest" ]
    grep -qF "sysv-created:$dest" "$DISABLED_SERVICES_FILE"
    # Enabled: both boot links verified by target — the S link boots it,
    # the K link is the shutdown half of the pair rc.common makes.
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helix-k2-webserver")" = "../init.d/helix-k2-webserver" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/K01helix-k2-webserver")" = "../init.d/helix-k2-webserver" ]
    # Launching is the start half's job; the install half never starts.
    [ ! -f "$BATS_TEST_TMPDIR/servers.log" ]
}

@test "k2 start half: brings the carve-out up and stays non-fatal when it cannot" {
    write_fake_webserver
    write_stock_app_service
    redirected_init_script > "$INSTALL_DIR/config/k2-webserver.init"
    install_k2_webserver_backend k2
    mock_pidof_webserver

    run start_k2_webserver_backend k2
    [ "$status" -eq 0 ]
    grep -q "launched web-server" "$BATS_TEST_TMPDIR/servers.log"

    # No-op off K2 and without the installed script.
    run start_k2_webserver_backend pi
    [ "$status" -eq 0 ]
    rm -f "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    run start_k2_webserver_backend k2
    [ "$status" -eq 0 ]

    # A failing start warns and returns 0, leaving respawn and the boot
    # entry as the recovery paths.
    redirected_init_script > "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    chmod +x "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    printf '#!/bin/sh\nexit 1\n' > "$MOCK_ROOT/etc/rc.common"
    chmod +x "$MOCK_ROOT/etc/rc.common"
    run start_k2_webserver_backend k2
    [ "$status" -eq 0 ]
    grep -q "WARN.*start failed" "$BATS_TEST_TMPDIR/log"
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

@test "k2 install: install half before start_service, start half after" {
    # Install first so the hook's first platform_stop_competing_uis finds
    # the script and the procd instance registers from launch; start after
    # because the service start's hook kills whatever web-server is live.
    awk '
        /install_k2_webserver_backend "\$platform"/ { if (!ins) ins = NR }
        /start_service "\$platform"/ { if (!svc) svc = NR }
        /start_k2_webserver_backend "\$platform"/ { if (!str) str = NR }
        END { exit !(ins && svc && str && ins < svc && svc < str) }
    ' "$MAIN_MODULE"
}

@test "k2 install: a carve-out failure warns instead of aborting the installer" {
    # The installer runs set -eu with the service already started by the
    # time the start half runs; a bare call returning 1 would kill the
    # whole install mid-tail and skip the cleanup_* tail. Both call sites
    # carry the non-fatal || contract.
    awk '
        /install_k2_webserver_backend "\$platform"/ {
            if ($0 ~ /\|\|/) ok1 = 1
            else if ((getline nxt) > 0 && nxt ~ /log_warn/) ok1 = 1
        }
        /start_k2_webserver_backend "\$platform"/ {
            if ($0 ~ /\|\|/) ok2 = 1
            else if ((getline nxt) > 0 && nxt ~ /log_warn/) ok2 = 1
        }
        END { exit !(ok1 && ok2) }' "$MAIN_MODULE"
}

@test "k2 install: a partial enable without the K01 link fails the carve-out" {
    write_fake_webserver
    write_stock_app_service
    redirected_init_script > "$INSTALL_DIR/config/k2-webserver.init"
    mock_command_script "pidof" "exit 1"

    # rc.common's enable exits 0 when at least ONE link was made; with the
    # K-link line gone it makes only the S link and still reports success.
    sed -i '\|ln -sfn.*rc\.d/K|d' "$MOCK_ROOT/etc/rc.common"

    run install_k2_webserver_backend k2
    [ "$status" -eq 1 ]
    # Precondition reached: the S half of the pair exists, so the failure
    # below is specifically the missing K half.
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helix-k2-webserver")" = "../init.d/helix-k2-webserver" ]
    grep -q "ERROR.*K01helix-k2-webserver" "$BATS_TEST_TMPDIR/log"
    grep -q "Manual fix:.*helix-k2-webserver enable" "$BATS_TEST_TMPDIR/log"
    # The ledger entry precedes the enable, and no start half runs after
    # a failed verification.
    grep -qF "sysv-created:$MOCK_ROOT/etc/init.d/helix-k2-webserver" "$DISABLED_SERVICES_FILE"
    [ ! -f "$BATS_TEST_TMPDIR/servers.log" ]
}

@test "k2 install: quoted or commented START/STOP directives still verify" {
    write_fake_webserver
    write_stock_app_service
    local src="$INSTALL_DIR/config/k2-webserver.init"
    {
        printf '#!/bin/sh /etc/rc.common\n'
        printf 'START="99" # boot slot\n'
        printf 'STOP="01" # shutdown half\n'
        printf 'start() { :; }\n'
    } > "$src"
    repoint_rc_common_shebang "$src"

    run install_k2_webserver_backend k2
    [ "$status" -eq 0 ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helix-k2-webserver")" = "../init.d/helix-k2-webserver" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/K01helix-k2-webserver")" = "../init.d/helix-k2-webserver" ]
}

@test "k2 install: a carve-out copy failure warns and skips without touching the ledger" {
    write_stock_app_service
    redirected_init_script > "$INSTALL_DIR/config/k2-webserver.init"
    # No write permission into init.d: the copy cannot land.
    chmod 555 "$MOCK_ROOT/etc/init.d"

    run install_k2_webserver_backend k2
    [ "$status" -eq 0 ]
    grep -q "WARN.*Could not install" "$BATS_TEST_TMPDIR/log"
    # No ledger entry for a file that was never written, and no boot links.
    [ ! -f "$DISABLED_SERVICES_FILE" ]
    [ ! -e "$MOCK_ROOT/etc/rc.d/S99helix-k2-webserver" ]
    # Restore the mode so bats can reap this test's tmpdir.
    chmod 755 "$MOCK_ROOT/etc/init.d"
}

# --- install_procd_shim_k2 ---

@test "k2 shim install: enables and verifies both rc.d boot links" {
    write_shim_source
    # A stale link in a different S-slot from an older install must be
    # dropped, not left beside the fresh entry.
    ln -s ../init.d/helixscreen "$MOCK_ROOT/etc/rc.d/S50helixscreen"

    run install_procd_shim_k2
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/etc/init.d/helixscreen" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helixscreen")" = "../init.d/helixscreen" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/K01helixscreen")" = "../init.d/helixscreen" ]
    [ ! -e "$MOCK_ROOT/etc/rc.d/S50helixscreen" ]
}

@test "k2 shim install: a partial enable without the S99 link fails loudly" {
    write_shim_source

    # enable exits 0 having made only the K link — the trap the link
    # verification closes.
    sed -i '\|ln -sfn.*rc\.d/S|d' "$MOCK_ROOT/etc/rc.common"

    run install_procd_shim_k2
    [ "$status" -ne 0 ]
    grep -q "ERROR.*S99helixscreen" "$BATS_TEST_TMPDIR/log"
    grep -q "Manual fix:.*init\.d/helixscreen enable" "$BATS_TEST_TMPDIR/log"
}

# The shim lives on the root overlay, which can be full during a self-update.
# A copy that dies mid-write must never leave the boot entry truncated.
@test "k2 shim install: an identical installed shim is not copied again" {
    write_shim_source
    cp "$INSTALL_DIR/config/helixscreen-k2-procd-shim.sh" "$MOCK_ROOT/etc/init.d/helixscreen"
    chmod +x "$MOCK_ROOT/etc/init.d/helixscreen"
    cp() { echo "cp $*" >> "$BATS_TEST_TMPDIR/cp-calls"; command cp "$@"; }

    run install_procd_shim_k2
    [ "$status" -eq 0 ]
    [ ! -e "$BATS_TEST_TMPDIR/cp-calls" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helixscreen")" = "../init.d/helixscreen" ]
}

@test "k2 shim install: a failed copy keeps the existing shim byte-identical" {
    write_shim_source
    printf '#!/bin/sh %s\nSTART=99\nSTOP=01\n# older shim\n' "$MOCK_ROOT/etc/rc.common" \
        > "$MOCK_ROOT/etc/init.d/helixscreen"
    chmod +x "$MOCK_ROOT/etc/init.d/helixscreen"
    command cp "$MOCK_ROOT/etc/init.d/helixscreen" "$BATS_TEST_TMPDIR/before"
    # ENOSPC: the destination is opened (truncated) and the write fails.
    cp() { : > "$2"; return 1; }

    run install_procd_shim_k2
    cmp -s "$BATS_TEST_TMPDIR/before" "$MOCK_ROOT/etc/init.d/helixscreen"
    grep -q "WARN.*kept" "$BATS_TEST_TMPDIR/log"
    [ -z "$(ls "$MOCK_ROOT/etc/init.d" | grep '\.new\.')" ]
}

@test "k2 shim install: a failed copy with no shim installed fails" {
    write_shim_source
    cp() { : > "$2"; return 1; }

    run install_procd_shim_k2
    [ "$status" -ne 0 ]
    [ ! -e "$MOCK_ROOT/etc/init.d/helixscreen" ]
    [ -z "$(ls "$MOCK_ROOT/etc/init.d" | grep '\.new\.')" ]
}

@test "k2 shim install: a fresh install copies the shim" {
    write_shim_source

    run install_procd_shim_k2
    [ "$status" -eq 0 ]
    cmp -s "$INSTALL_DIR/config/helixscreen-k2-procd-shim.sh" "$MOCK_ROOT/etc/init.d/helixscreen"
    [ -x "$MOCK_ROOT/etc/init.d/helixscreen" ]
}

@test "k2 shim install: a verification failure warns instead of aborting the install" {
    # install_service calls the shim installer after the stock UI is
    # stopped and before anything of ours starts, so an unguarded failure
    # under set -eu would leave the device with no UI at all — worse than
    # the incomplete boot entry it detected.
    awk '$0 ~ /install_procd_shim_k2 \|\|/ { ok = 1 } END { exit !ok }' "$MODULE"
}

# --- the shared rc.d helpers ---

# The shim installed with its boot links already made, the fake rc.common's
# enable recording each call, and the links' mtimes pinned so a remove and
# recreate shows up.
shim_with_current_links() {
    write_shim_source
    cp "$INSTALL_DIR/config/helixscreen-k2-procd-shim.sh" "$MOCK_ROOT/etc/init.d/helixscreen"
    chmod +x "$MOCK_ROOT/etc/init.d/helixscreen"
    ln -s ../init.d/helixscreen "$MOCK_ROOT/etc/rc.d/S99helixscreen"
    ln -s ../init.d/helixscreen "$MOCK_ROOT/etc/rc.d/K01helixscreen"
    touch -h -d 2001-01-01 "$MOCK_ROOT/etc/rc.d/S99helixscreen" "$MOCK_ROOT/etc/rc.d/K01helixscreen"
    sed -i "/^    enable)/a\\        echo enable >> \"$BATS_TEST_TMPDIR/enable-calls\"" "$MOCK_ROOT/etc/rc.common"
}

link_mtime() { stat -c %Y "$MOCK_ROOT/etc/rc.d/$1"; }

# On a full or read-only overlay the unlink can succeed while enable cannot
# recreate the links, so links that already verify are left untouched.
@test "k2 rc.d helper: links that already verify are neither removed nor re-enabled" {
    shim_with_current_links

    run enable_and_verify_rcd "$MOCK_ROOT/etc/init.d/helixscreen"
    [ "$status" -eq 0 ]
    [ ! -e "$BATS_TEST_TMPDIR/enable-calls" ]
    [ "$(link_mtime S99helixscreen)" = "$(date -d 2001-01-01 +%s)" ]
    [ "$(link_mtime K01helixscreen)" = "$(date -d 2001-01-01 +%s)" ]
    grep -q "INFO.*already current" "$BATS_TEST_TMPDIR/log"
}

@test "k2 rc.d helper: a stale duplicate link still goes through remove and enable" {
    shim_with_current_links
    ln -s ../init.d/helixscreen "$MOCK_ROOT/etc/rc.d/S80helixscreen"

    run enable_and_verify_rcd "$MOCK_ROOT/etc/init.d/helixscreen"
    [ "$status" -eq 0 ]
    [ -s "$BATS_TEST_TMPDIR/enable-calls" ]
    [ ! -e "$MOCK_ROOT/etc/rc.d/S80helixscreen" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helixscreen")" = "../init.d/helixscreen" ]
}

@test "k2 rc.d helper: a missing link still runs enable" {
    shim_with_current_links
    rm "$MOCK_ROOT/etc/rc.d/K01helixscreen"

    run enable_and_verify_rcd "$MOCK_ROOT/etc/init.d/helixscreen"
    [ "$status" -eq 0 ]
    [ -s "$BATS_TEST_TMPDIR/enable-calls" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/K01helixscreen")" = "../init.d/helixscreen" ]
}

@test "k2 rc.d helper: a link pointing elsewhere still runs enable" {
    shim_with_current_links
    ln -sfn ../init.d/other "$MOCK_ROOT/etc/rc.d/S99helixscreen"

    run enable_and_verify_rcd "$MOCK_ROOT/etc/init.d/helixscreen"
    [ "$status" -eq 0 ]
    [ -s "$BATS_TEST_TMPDIR/enable-calls" ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99helixscreen")" = "../init.d/helixscreen" ]
}

@test "k2 rc.d helper: is_rc_common_script accepts an rc.common shebang" {
    # The setup patches the module's /etc/rc.common references onto the
    # mock root, so the probe carries the repointed shebang like every
    # other rc.common fixture here.
    printf '#!/bin/sh /etc/rc.common\nSTART=99\n' > "$MOCK_ROOT/etc/init.d/rcd-probe"
    repoint_rc_common_shebang "$MOCK_ROOT/etc/init.d/rcd-probe"
    is_rc_common_script "$MOCK_ROOT/etc/init.d/rcd-probe"
}

@test "k2 rc.d helper: is_rc_common_script rejects a plain sh script" {
    # The rc.common mention sits below the shebang: only a first-line check
    # may reject this file.
    printf '#!/bin/sh\n# dispatched via /etc/rc.common at boot\n' > "$MOCK_ROOT/etc/init.d/rcd-probe"
    refute is_rc_common_script "$MOCK_ROOT/etc/init.d/rcd-probe"
}

@test "k2 rc.d helper: is_rc_common_script rejects a missing file" {
    refute is_rc_common_script "$MOCK_ROOT/etc/init.d/absent-probe"
}

@test "k2 rc.d helper: is_rc_common_script rejects an empty file" {
    # An empty first line is not a shebang; treating one as rc.common
    # would route a plain script into rc.common's disable path.
    : > "$MOCK_ROOT/etc/init.d/rcd-probe"
    refute is_rc_common_script "$MOCK_ROOT/etc/init.d/rcd-probe"
}

@test "k2 rc.d helper: a foreign script without STOP verifies S-only, with a warn" {
    # STOP is verified-when-declared, not required: the helper also runs
    # against stock firmware scripts we do not author, and rc.common
    # simply makes no K link without one.
    local foreign="$MOCK_ROOT/etc/init.d/foreign-svc"

    printf '#!/bin/sh %s/etc/rc.common\nSTART=99\n' "$MOCK_ROOT" > "$foreign"
    chmod +x "$foreign"
    run enable_and_verify_rcd "$foreign"
    [ "$status" -eq 0 ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99foreign-svc")" = "../init.d/foreign-svc" ]
    [ ! -e "$MOCK_ROOT/etc/rc.d/K01foreign-svc" ]
    grep -q "WARN.*no parseable STOP" "$BATS_TEST_TMPDIR/log"

    # A non-numeric STOP spelling lands on the same S-only path.
    printf '#!/bin/sh %s/etc/rc.common\nSTART=99\nSTOP=late\n' "$MOCK_ROOT" > "$foreign"
    chmod +x "$foreign"
    run enable_and_verify_rcd "$foreign"
    [ "$status" -eq 0 ]
    [ "$(readlink "$MOCK_ROOT/etc/rc.d/S99foreign-svc")" = "../init.d/foreign-svc" ]
    grep -q "WARN.*no parseable STOP" "$BATS_TEST_TMPDIR/log"
}

# --- the dev deploy path ---

@test "k2 deploy ships, enables, verifies and starts the carve-out" {
    grep -q 'k2-webserver.init' "$CROSSMK"
    grep -q 'helix-k2-webserver' "$CROSSMK"
    grep -q '/etc/init.d/helix-k2-webserver enable' "$CROSSMK"
    grep -q 'readlink /etc/rc.d/S99helix-k2-webserver' "$CROSSMK"
    grep -q '/etc/init.d/helix-k2-webserver start' "$CROSSMK"
}

@test "k2 deploy records the carve-out in the disabled-services ledger" {
    # A dev-deployed K2 must be uninstallable without orphaning the script
    # and its boot links: the deploy appends the same sysv-created entry
    # the installer records, with the same dedupe (prestonbrown/helixscreen#1667).
    grep -q 'grep -qF "sysv-created:/etc/init.d/helix-k2-webserver"' "$CROSSMK"
    grep -q 'sysv-created:/etc/init.d/helix-k2-webserver" >> ' "$CROSSMK"
    grep -q '\.disabled_services' "$CROSSMK"
}

@test "k2 deploy verifies the carve-out's K01 boot link like the installer" {
    grep -q 'readlink /etc/rc.d/K01helix-k2-webserver' "$CROSSMK"
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
        | grep -q 'kill_process_by_name web-server'
}

@test "k2 uninstall: stock-UI restore verifies the boot symlink before claiming success" {
    # rc.common's `enable` can exit 0 without creating any boot symlink -
    # the trap this repo documents for this exact rc.common in
    # install_procd_shim_k2 and install_k2_webserver_backend. A restore
    # that reports success on a missing link leaves the K2 booting to the
    # logo with no UI at all, so the restored claim must be gated on a
    # /etc/rc.d link actually pointing at ../init.d/app.
    local patched="$BATS_TEST_TMPDIR/uninstall.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/etc/rc\.common|$MOCK_ROOT/etc/rc.common|g" \
        -e "s|/etc/rc\.d/|$MOCK_ROOT/etc/rc.d/|g" \
        "$UNINSTALL_BUNDLE" > "$patched"

    # The restore path kills the carve-out's web-server by pid; mock pidof
    # empty so the sandbox never sees a host-addressing call.
    mock_command_script "pidof" 'exit 1'

    write_stock_app_service
    rm -f "$MOCK_ROOT"/etc/rc.d/*app 2>/dev/null || true

    # Broken shape: enable exits 0 and creates nothing. The stub also logs
    # a start dispatch, so the case can prove the restore still starts the
    # stock UI for the session despite the unverified boot entry.
    printf '#!/bin/sh\ncase "$2" in start) echo started >> "%s/app-start.log";; esac\nexit 0\n' \
        "$BATS_TEST_TMPDIR" > "$MOCK_ROOT/etc/rc.common"
    chmod +x "$MOCK_ROOT/etc/rc.common"
    local broken_out
    broken_out="$( INSTALL_DIR="$INSTALL_DIR" SUDO="" _UNINSTALL_BUNDLE_TEST=1 \
        sh -c ". '$patched'; restore_previous_ui_platform k2" 2>&1 || true )"
    echo "$broken_out" | grep -q "boot symlink missing or wrong"
    [ -f "$BATS_TEST_TMPDIR/app-start.log" ]

    # Working shape: enable creates the link (setup's fake rc.common), so
    # the restore proceeds without the warning.
    write_fake_rc_common
    rm -f "$MOCK_ROOT"/etc/rc.d/*app 2>/dev/null || true
    local good_out
    good_out="$( INSTALL_DIR="$INSTALL_DIR" SUDO="" _UNINSTALL_BUNDLE_TEST=1 \
        sh -c ". '$patched'; restore_previous_ui_platform k2" 2>&1 || true )"
    if echo "$good_out" | grep -q "boot symlink missing or wrong"; then
        echo "restore warned despite a correct boot symlink:" >&2
        echo "$good_out" >&2
        return 1
    fi
}

# --- liveness is the hook's job ---
#
# On Tina/procd, /etc/init.d/app stop inside platform_stop_competing_uis
# takes a running web-server down (killall -9 in the stock stop_service;
# `disable` only removes rc.d links). Boot dispatch of the carve-out's own
# rc.d entry is neither asserted nor relied on, so the carve-out's liveness
# must be restored at the END of the hook path — the one path that runs at
# every boot and every service restart.

@test "k2 hook: a service start with web-server running leaves the carve-out serving" {
    write_fake_webserver
    write_stock_app_service
    redirected_init_script > "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    chmod +x "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    seed_app_tracks_webserver
    mock_pidof_webserver
    mock_command_script "killall" 'exit 0'

    # Carve-out serving before the service start, as after a previous boot.
    "$MOCK_ROOT/etc/init.d/helix-k2-webserver" start
    await_launch_count 1
    local pid
    pid="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    kill -0 "$pid"

    # platform_stop_competing_uis is what S99helixscreen start runs; its
    # app stop just killed the instance procd tracked. A live successor
    # must exist when it returns.
    run_hook_stop_competing_uis

    local newpid
    newpid="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid" 2>/dev/null)"
    [ -n "$newpid" ]
    kill -0 "$newpid"
    await_launch_count 2

    kill "$newpid" 2>/dev/null || true
}

@test "k2 hook: guarded direct launch when the init script is absent" {
    # An older deploy without /etc/init.d/helix-k2-webserver must still
    # come back serving after the app stop.
    write_fake_webserver
    write_stock_app_service
    seed_app_tracks_webserver
    mock_pidof_webserver
    mock_command_script "killall" 'exit 0'

    # The carve-out's previous instance, tracked under the app service so
    # its stop kills it exactly as the stock killall would.
    "$MOCK_ROOT/usr/bin/web-server" >/dev/null 2>&1 &
    local pid=$!
    echo "$pid" > "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid"
    kill -0 "$pid"
    await_launch_count 1

    run_hook_stop_competing_uis

    await_launch_count 2
    local newpid
    newpid="$(cat "$BATS_TEST_TMPDIR/webserver.pid")"
    kill -0 "$newpid"

    kill "$newpid" 2>/dev/null || true
}

@test "k2 hook: restore reuses the init script, not a bare launch" {
    # The init script registers a procd instance; a bare fallback launch
    # does not. A rewritten instance record after the hook is the proof
    # the restore went through the service-shaped path.
    write_fake_webserver
    write_stock_app_service
    redirected_init_script > "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    chmod +x "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    seed_app_tracks_webserver
    mock_pidof_webserver
    mock_command_script "killall" 'exit 0'

    "$MOCK_ROOT/etc/init.d/helix-k2-webserver" start
    local pid
    pid="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    kill -0 "$pid"

    run_hook_stop_competing_uis

    local newpid
    newpid="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid" 2>/dev/null)"
    [ "$newpid" != "$pid" ]
    kill -0 "$newpid"

    kill "$newpid" 2>/dev/null || true
}

@test "k2 hook: a live web-server is left alone by the restore guards" {
    # With the stock app service absent, nothing kills the serving
    # instance; the restore's start sees web-server alive and registers
    # nothing, so exactly the original instance keeps serving.
    write_fake_webserver
    redirected_init_script > "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    chmod +x "$MOCK_ROOT/etc/init.d/helix-k2-webserver"
    mock_pidof_webserver
    mock_command_script "killall" 'exit 0'

    "$MOCK_ROOT/etc/init.d/helix-k2-webserver" start
    local pid
    pid="$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")"
    kill -0 "$pid"

    run_hook_stop_competing_uis

    await_launch_count 1
    [ "$(cat "$MOCK_ROOT/var/run/procd-helix-k2-webserver.pid")" = "$pid" ]
    kill -0 "$pid"

    kill "$pid" 2>/dev/null || true
}
