#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end runs of the bundled install.sh (prestonbrown/helixscreen#1600).
#
# Every other installer test calls one function; these run the whole script, so
# the handoff between steps is what they pin. The bundle is rebuilt from
# scripts/lib/installer/ exactly as the release workflow builds it, then run
# under BusyBox ash inside a user + mount + pid + net namespace
# (fixtures/install_e2e_scenario.sh): tmpfs over every directory it writes, a
# stateful systemctl stub, and a fake x86 release - /bin/true for the binaries,
# the tracked config/ tree and launcher, and enough padding to pass the size check.
#
# The host is a Debian box running Klipper as root in one of two shapes. With
# Moonraker's config at /root/printer_data/config (the default), the install
# lands at /root/helixscreen with its config linked into printer_data. Bare
# (E2E_HOST=bare), it lands at /opt/helixscreen with its config inside the
# payload, which is the shape an update's backup-and-restore has to carry.
# Each test gets a fresh root.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
SCENARIO="$WORKTREE_ROOT/tests/shell/fixtures/install_e2e_scenario.sh"
INST=/root/helixscreen
USER_CFG=/root/printer_data/config/helixscreen
UNIT=/etc/systemd/system/helixscreen.service

setup_file() {
    local art="$BATS_FILE_TMPDIR/e2e"
    mkdir -p "$art/stubs" "$art/seed"
    bash "$WORKTREE_ROOT/scripts/bundle-installer.sh" -o "$art/install.sh" >/dev/null
    # The stub systemctl brings a unit up at once, so the installer's one-second
    # waits for a service to start measure nothing here and were most of this
    # file's run time. BusyBox ash runs sleep in-process, out of reach of a
    # bound stub, so this copy of the bundle shadows it with a function that
    # still waits on the real clock, briefly.
    sed -i '1a sleep() { command sleep 0.02; }' "$art/install.sh"

    _make_release "$art" 1 v1.0.0
    _make_release "$art" 2 v1.0.1
    _make_release "$art" 3 v1.1.0-beta.1
    _make_release_server "$art" 3 v1.1.0-beta.1 beta

    cp /etc/os-release "$art/seed/os-release"
    printf 'root:x:0:0:root:/root:/bin/sh\n' > "$art/seed/passwd"
    printf 'root:x:0:\n' > "$art/seed/group"
    printf '[server]\nhost: 0.0.0.0\n\n[authorization]\n' > "$art/seed/moonraker.conf"

    # Records every call and keeps enough state for is-active/is-enabled to
    # answer consistently with the starts, stops and enables before them.
    cat > "$art/stubs/systemctl" <<'STUB'
#!/bin/sh
echo "systemctl $*" >> /var/log/e2e-systemctl.log
unit=""
for a in "$@"; do case "$a" in -*) ;; *) unit="$a" ;; esac; done
case "$1" in
    start|restart) touch "/run/e2e-active-$unit" ;;
    stop) rm -f "/run/e2e-active-$unit" ;;
    is-active) [ -e "/run/e2e-active-$unit" ] || exit 3 ;;
    is-enabled) [ -e "/run/e2e-enabled-$unit" ] || exit 1 ;;
    enable)
        if [ -e "/run/e2e-fail-enable-$unit" ]; then
            echo "Failed to enable unit: Unit file $unit.service is masked." >&2
            exit 1
        fi
        touch "/run/e2e-enabled-$unit" ;;
    disable) rm -f "/run/e2e-enabled-$unit" ;;
esac
exit 0
STUB
    chmod +x "$art/stubs/systemctl"

    # Stands in for the release CDN: https://e2e.invalid/<path> and
    # http://e2e.invalid/<path> are served from /mnt/r2/<path>, anything else is
    # a 404, so a --version run reaches no real network through curl. The
    # scenario points R2_BASE_URL and HTTP_BASE_URL at that host.
    cat > "$art/stubs/curl" <<'STUB'
#!/bin/sh
[ "${1:-}" = "--version" ] && { echo "curl 8.0.0 (e2e stub)"; exit 0; }
url="" out="" wfmt=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift ;;
        -w) wfmt=$2; shift ;;
        -A | --connect-timeout | --max-time | --speed-limit | --speed-time) shift ;;
        -*) ;;
        *) url=$1 ;;
    esac
    shift
done
f=""
case "$url" in
    https://e2e.invalid/* | http://e2e.invalid/*) f="/mnt/r2/${url#*://e2e.invalid/}" ;;
esac
echo "curl $url" >> /var/log/e2e-curl.log
if [ -n "$f" ] && [ -f "$f" ]; then
    code=200
    if [ -n "$out" ]; then cp "$f" "$out"; else cat "$f"; fi
else
    code=404
fi
[ -n "$wfmt" ] && printf '%b' "$(printf '%s' "$wfmt" | sed "s/%{http_code}/$code/")"
[ "$code" = 200 ]
STUB
    chmod +x "$art/stubs/curl"
}

# Publish release <n> on the stub CDN under <channel>, with a manifest carrying
# its hash, the way the release workflow lays out R2.
_make_release_server() {
    local art=$1 n=$2 version=$3 channel=$4
    local tar="helixscreen-x86-$version.tar.gz"
    mkdir -p "$art/r2/releases/$version" "$art/r2/$channel"
    cp "$art/release-$n/$tar" "$art/r2/releases/$version/"
    printf '{"version": "%s", "assets": {"x86": {"url": "https://e2e.invalid/releases/%s/%s", "sha256": "%s"}}}\n' \
        "${version#v}" "$version" "$tar" "$(sha256sum "$art/release-$n/$tar" | cut -d' ' -f1)" \
        > "$art/r2/$channel/manifest.json"
}

# A release archive in the shipped layout, marked with its version.
_make_release() {
    local art=$1 n=$2 version=$3
    local pkg="$art/pkg$n/helixscreen"
    mkdir -p "$pkg/bin" "$pkg/assets" "$pkg/ui_xml" "$art/release-$n"
    local b
    for b in helix-screen helix-splash helix-watchdog; do
        cp /bin/true "$pkg/bin/$b"
    done
    cp "$WORKTREE_ROOT/scripts/helix-launcher.sh" "$pkg/bin/"
    # The release stages a clean checkout's config/ and ships no personal
    # config (mk/cross.mk release-x86): tracked files only, so a dev tree's
    # ignored settings.json never reaches the fake payload.
    (cd "$WORKTREE_ROOT" && git ls-files -z config | xargs -0 cp --parents -t "$pkg")
    rm -f "$pkg/config/settings.json" "$pkg/config/helixconfig.json"
    echo "$version" > "$pkg/ui_xml/e2e-release.txt"
    # The release bakes this in (mk/cross.mk write-release-info).
    printf '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"%s","asset_name":"helixscreen-x86.zip"}\n' \
        "$version" > "$pkg/release_info.json"
    # Incompressible, so the archive clears the installer's 1MB floor.
    head -c 1600000 /dev/urandom > "$pkg/assets/e2e-pad.bin"
    tar -czf "$art/release-$n/helixscreen-x86-$version.tar.gz" -C "$art/pkg$n" helixscreen
}

setup() {
    load helpers
    # The installer runs at its default verbosity, which helpers.bash turns up
    # for the unit tests; a test that needs log_info lines exports it again.
    unset HELIX_INSTALL_VERBOSE
    command -v unshare >/dev/null 2>&1 || skip "unshare not available"
    # Real root keeps real root inside the namespace, and the uninstall sweep
    # names host paths outside the tmpfs set (/srv, /usr/data, /mnt/UDISK).
    [ "$(id -u)" -ne 0 ] || skip "refuses to run as real root"
    # The same namespaces the run asks for, so a host that permits fewer skips.
    unshare --user --map-root-user --mount --pid --net --fork true 2>/dev/null \
        || skip "user+mount+pid+net namespaces not permitted"
    [ -x /usr/bin/systemctl ] || [ -x /bin/systemctl ] || [ -x /usr/sbin/systemctl ] \
        || skip "no systemctl to bind the stub over"
    [ -d /mnt ] || skip "no /mnt to reach the work dir through"

    WORK="$BATS_TEST_TMPDIR/work"
    cp -a "$BATS_FILE_TMPDIR/e2e" "$WORK"
    mkdir -p "$WORK/out"
}

# Run the scenario steps in one fresh root, leaving $status and $output; the
# scenario stops at the first step that exits non-zero.
run_scenario_status() {
    run unshare --user --map-root-user --mount --pid --net --fork bash "$SCENARIO" "$WORK" "$@"
    if [[ "$output" == *"SANDBOX_MOUNT_FAIL"* ]]; then skip "namespace mounts not permitted here"; fi
}

# Run the scenario steps in one fresh root; fails the test unless all complete.
run_scenario() {
    run_scenario_status "$@"
    [ "$status" -eq 0 ] && [[ "$output" == *"SCENARIO_DONE"* ]] \
        || fail "scenario ($*) failed, status $status:
$output"
}

# The seeded tree, as the scenario left it before its first step.
snap_seed() {
    snap 0-seed
}

# What step <n> printed: the lines between its two === STEP markers.
step_output() { # n output
    printf '%s\n' "$2" | sed -n "/^=== STEP $1: /,/^=== STEP $1: .* exit=/p" | sed '1d;$d'
}

# A transcript with what varies between runs replaced: timestamps, archive
# sizes and scratch paths.
normalize_transcript() {
    printf '%s\n' "$1" | sed -E \
        -e 's/\[[0-9]{2}:[0-9]{2}:[0-9]{2}\] ?//g' \
        -e 's/\b[0-9]+(\.[0-9]+)? ?[KMG]i?B?\b/<size>/g' \
        -e 's#/tmp/[^ ]*#<tmp>#g'
}

# Compare step <n>'s output with fixtures/install_transcripts/<name>.txt.
# HELIX_E2E_WRITE_GOLDENS=1 writes the golden instead.
matches_golden() { # name n
    local golden="$BATS_TEST_DIRNAME/fixtures/install_transcripts/$1.txt" got
    got=$(normalize_transcript "$(step_output "$2" "$output")")
    if [ "${HELIX_E2E_WRITE_GOLDENS:-0}" = 1 ]; then
        mkdir -p "$(dirname "$golden")"
        printf '%s\n' "$got" > "$golden"
    fi
    # What Mainsail and KIAUH show: no color, no cursor movement, no redraws.
    if printf '%s' "$got" | grep -q "$(printf '[\033\r]')"; then
        fail "escape or carriage-return bytes in the $1 transcript"
    fi
    diff -u "$golden" <(printf '%s\n' "$got")
}

# The systemctl calls that change something, from a snapshot's stub log.
changing_systemctl_calls() {
    grep -E '^systemctl (stop|start|enable|disable|restart|reload|mask|unmask|daemon-reload|kill)' \
        "$1/var/log/e2e-systemctl.log" 2>/dev/null || true
}

need_cdn_stub() {
    [ -x /usr/bin/curl ] || [ -x /usr/sbin/curl ] || [ -x /bin/curl ] || [ -x /sbin/curl ] \
        || skip "no curl on this host for the stub CDN to bind over"
}

# The snapshot taken after step <n>-<name>.
snap() {
    echo "$WORK/out/$1"
}

# Follow symlinks inside a snapshot: an absolute link target names a path in
# the sandbox root, not on the host.
snap_resolve() {
    local root=$1 path=$2 i target
    for i in 1 2 3 4 5 6 7 8; do
        [ -L "$root$path" ] || break
        target=$(readlink "$root$path")
        case "$target" in
            /*) path=$target ;;
            *) path="$(dirname "$path")/$target" ;;
        esac
    done
    echo "$root$path"
}

@test "install.sh e2e: a no-terminal fresh install lays down the payload, the unit and the updater entry, and prints the golden transcript" {
    run_scenario install
    local s
    s=$(snap 1-install)

    lacks "Continue?" "$output"
    matches_golden fresh 1
    contains "HelixScreen v1.0.0 is running" "$output"
    lacks "=====" "$output"
    [ "$(grep -c '^HelixScreen installer v1.0.0' <<< "$output")" -eq 1 ] \
        || fail "the installer header is not printed exactly once"
    # A --local archive is unpacked, not downloaded.
    contains "[2/6] Unpacked local archive ... ok" "$output"
    lacks "Downloaded" "$output"
    grep -q "DONE Started HelixScreen" "$s/root/printer_data/logs/helixscreen-install.log" \
        || fail "no install log in printer_data/logs"
    [ -x "$s$INST/bin/helix-screen" ] || fail "no payload binary at $INST"
    [ -x "$s$INST/bin/helix-launcher.sh" ] || fail "no launcher at $INST"
    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.0" ]

    # The unit runs the payload it was installed with.
    [ -f "$s$UNIT" ] || fail "no systemd unit"
    grep -qx "WorkingDirectory=$INST" "$s$UNIT"
    grep -qx "ExecStart=$INST/bin/helix-launcher.sh" "$s$UNIT"
    grep -qx "systemctl enable helixscreen" "$s/var/log/e2e-systemctl.log"
    grep -qx "systemctl start helixscreen" "$s/var/log/e2e-systemctl.log"

    # Moonraker's updater points at the same root.
    grep -qx "\[update_manager helixscreen\]" "$s/root/printer_data/config/moonraker.conf"
    grep -qx "path: $INST" "$s/root/printer_data/config/moonraker.conf"

    # Config lives in printer_data, and the payload's config links to it.
    [ -f "$s$USER_CFG/helixscreen.env" ] || fail "no helixscreen.env in printer_data"
    [ "$(readlink "$s$INST/config/helixscreen.env")" = "$USER_CFG/helixscreen.env" ]
}

@test "install.sh e2e: an update replaces the payload and carries config and env across" {
    run_scenario install seed-user update
    local s
    s=$(snap 3-update)

    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.1" ]
    grep -q '"e2e_user_value": "kept"' "$(snap_resolve "$s" "$INST/config/settings.json")" \
        || fail "settings.json edit lost across the update"
    grep -qx "HELIX_E2E_USER_KEY=kept" "$(snap_resolve "$s" "$INST/config/helixscreen.env")" \
        || fail "helixscreen.env edit lost across the update"

    # The service is stopped for the swap and started on the new payload.
    local calls
    calls=$(sed -n "$(($(wc -l < "$(snap 1-install)/var/log/e2e-systemctl.log") + 1)),\$p" \
        "$s/var/log/e2e-systemctl.log")
    contains "systemctl stop helixscreen" "$calls"
    contains "systemctl start helixscreen" "$calls"
    grep -qx "ExecStart=$INST/bin/helix-launcher.sh" "$s$UNIT"
    grep -qx "path: $INST" "$s/root/printer_data/config/moonraker.conf"
}

@test "install.sh e2e: a self-update keeps the running service and the local unit" {
    run_scenario install customize-unit self-update
    local s before
    s=$(snap 3-self-update)
    before=$(snap 2-customize-unit)

    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.1" ]
    cmp -s "$before$UNIT" "$s$UNIT" || fail "self-update rewrote the systemd unit"
    local calls
    calls=$(sed -n "$(($(wc -l < "$before/var/log/e2e-systemctl.log") + 1)),\$p" \
        "$s/var/log/e2e-systemctl.log")
    [[ "$calls" != *"systemctl stop helixscreen"* ]] \
        || fail "self-update stopped the service it runs under"
}

@test "install.sh e2e: uninstall removes the payload, the units and the updater entry" {
    run_scenario install uninstall
    local s out
    s=$(snap 2-uninstall)
    out=$(step_output 2 "$output")

    # At default verbosity: one header, what went, what stayed, and a close.
    [ "$(grep -c '^HelixScreen uninstaller$' <<< "$out")" -eq 1 ] \
        || fail "no single uninstaller header: $out"
    contains "    Removed ~/helixscreen" "$out"
    contains "HelixScreen uninstalled." "$out"

    [ ! -e "$s$INST" ] || fail "payload left behind at $INST"
    [ ! -e "$s$UNIT" ] || fail "systemd unit left behind"
    [ -z "$(ls "$s/etc/systemd/system" | grep -i helixscreen)" ] \
        || fail "helixscreen units left in /etc/systemd/system"
    ! grep -q "update_manager helixscreen" "$s/root/printer_data/config/moonraker.conf" \
        || fail "updater entry left in moonraker.conf"
    # The user's config is kept on purpose, where the uninstall says it is.
    contains "    User config preserved at: ~/printer_data/config/helixscreen" "$out"
}

@test "install.sh e2e (bare host): an update carries the payload's own config and env across" {
    export E2E_HOST=bare
    run_scenario install seed-user update
    local s
    s=$(snap 3-update)

    [ "$(cat "$s/opt/helixscreen/ui_xml/e2e-release.txt")" = "v1.0.1" ]
    [ ! -L "$s/opt/helixscreen/config/settings.json" ] \
        || fail "bare host config should live in the payload, not a link"
    grep -q '"e2e_user_value": "kept"' "$s/opt/helixscreen/config/settings.json" \
        || fail "settings.json edit lost across the update"
    grep -qx "HELIX_E2E_USER_KEY=kept" "$s/opt/helixscreen/config/helixscreen.env" \
        || fail "helixscreen.env edit lost across the update"
}

@test "install.sh e2e (bare host): uninstall leaves nothing behind" {
    export E2E_HOST=bare
    run_scenario install uninstall
    local s
    s=$(snap 2-uninstall)

    [ -z "$(ls -A "$s/opt")" ] || fail "left in /opt: $(ls -A "$s/opt")"
    [ -z "$(ls -A "$s/root")" ] || fail "left in /root: $(ls -A "$s/root")"
    [ ! -e "$s/var/lib/helixscreen" ] || fail "state dir left in /var/lib"
    [ -z "$(ls "$s/etc/systemd/system" | grep -i helixscreen)" ] \
        || fail "helixscreen units left in /etc/systemd/system"
}

@test "install.sh e2e: --clean over a home-directory install wipes it, config included" {
    # --clean shares uninstall's sweep list, so a $KLIPPER_HOME/helixscreen
    # install is wiped the same way an /opt one always was. "Removing" is a
    # log_info, shown only when verbose.
    export HELIX_INSTALL_VERBOSE=1
    run_scenario install seed-user clean-install
    local s
    s=$(snap 3-clean-install)

    contains "Removing $INST..." "$output"
    # One header; what --clean deletes is in the plan, not a banner.
    lacks "=====" "$output"
    lacks "CLEAN INSTALL MODE" "$output"
    contains "  Remove     ~/helixscreen, its settings and config backups" "$output"
    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.1" ]
    ! grep -q "e2e_user_value" "$(snap_resolve "$s" "$INST/config/settings.json")" \
        || fail "--clean kept the old settings.json"
}

@test "install.sh e2e: --version of a prerelease with no saved channel moves app and Moonraker to beta" {
    # The stub CDN is a curl stub bound over the host's own curl.
    [ -x /usr/bin/curl ] || [ -x /usr/sbin/curl ] || [ -x /bin/curl ] || [ -x /sbin/curl ] \
        || skip "no curl on this host for the stub CDN to bind over"
    # seed-user writes a settings.json with no update channel, so the beta
    # version decides it, after the installed config has been read.
    run_scenario install seed-user update-beta-version
    local s settings
    s=$(snap 3-update-beta-version)
    settings=$(snap_resolve "$s" "$INST/config/settings.json")

    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.1.0-beta.1" ]
    contains "SHA256 verified" "$output"
    grep -qx "channel: beta" "$s/root/printer_data/config/moonraker.conf" \
        || fail "update_manager still on $(grep '^channel:' "$s/root/printer_data/config/moonraker.conf")"
    [ "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["update"]["channel"])' "$settings")" = "1" ] \
        || fail "settings.json has no beta channel: $(cat "$settings")"
    grep -q '"e2e_user_value": "kept"' "$settings" || fail "user settings lost"
}

@test "install.sh e2e: a --local prerelease archive with no saved channel moves app and Moonraker to beta" {
    # seed-user writes a settings.json with no update channel, so the beta
    # archive decides it, as a prerelease --version does.
    run_scenario install seed-user update-beta-local
    local s settings
    s=$(snap 3-update-beta-local)
    settings=$(snap_resolve "$s" "$INST/config/settings.json")

    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.1.0-beta.1" ]
    grep -qx "channel: beta" "$s/root/printer_data/config/moonraker.conf" \
        || fail "update_manager still on $(grep '^channel:' "$s/root/printer_data/config/moonraker.conf")"
    [ "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["update"]["channel"])' "$settings")" = "1" ] \
        || fail "settings.json has no beta channel: $(cat "$settings")"
    grep -q '"e2e_user_value": "kept"' "$settings" || fail "user settings lost"
}

@test "install.sh e2e: the scenario has no route off the box" {
    # A failure path that falls through to wget or python must not reach a
    # real release server from a test. Only a loopback means no route, whether
    # or not the host itself is online.
    run_scenario net-probe
    printf '%s\n' "$output" | grep -qx "NET_IFACES: lo" \
        || fail "the scenario sees more than a loopback: $(printf '%s\n' "$output" | grep NET_IFACES)"
}

@test "install.sh e2e: --dry-run checks the release, prints the golden plan and changes nothing" {
    need_cdn_stub
    run_scenario dry-run
    local s
    s=$(snap 1-dry-run)

    matches_golden dry-run 1
    contains "Dry run, nothing changed." "$output"
    contains "SHA256 available" "$output"
    # It asked the stub CDN, so the release check really ran.
    grep -q "^curl https://e2e.invalid/beta/manifest.json" "$s/var/log/e2e-curl.log" \
        || fail "dry-run never read the manifest"
    # The stubs' own call logs are the only difference from the seeded tree.
    run diff -r -x 'e2e-*.log' "$(snap_seed)" "$s"
    [ "$status" -eq 0 ] || fail "dry-run changed the tree:
$output"
    [ -z "$(changing_systemctl_calls "$s")" ] \
        || fail "dry-run made changing systemctl calls: $(changing_systemctl_calls "$s")"
}

@test "install.sh e2e: --dry-run exits non-zero when the release does not exist" {
    need_cdn_stub
    E2E_RELEASE_VERSION=v9.9.9 run_scenario_status dry-run
    [ "$status" -ne 0 ] || fail "dry-run of a missing release exited 0:
$output"
    contains "=== STEP 1: dry-run exit=1" "$output"
    contains "No HelixScreen v9.9.9 release for x86." "$output"
    lacks "Dry run, nothing changed." "$output"
}

@test "install.sh e2e: answering n at the prompt changes nothing" {
    command -v script >/dev/null 2>&1 || skip "no script(1) for a pseudo-terminal"
    run_scenario install-tty-no
    lacks "SKIP no script" "$output"
    local s
    s=$(snap 1-install-tty-no)

    contains "Continue? [Y/n]" "$output"
    contains "Nothing changed." "$output"
    run diff -r -x 'e2e-*.log' "$(snap_seed)" "$s"
    [ "$status" -eq 0 ] || fail "answering n changed the tree:
$output"
    [ -z "$(changing_systemctl_calls "$s")" ] \
        || fail "answering n made changing systemctl calls: $(changing_systemctl_calls "$s")"
}

@test "install.sh e2e: answering y at the prompt installs" {
    command -v script >/dev/null 2>&1 || skip "no script(1) for a pseudo-terminal"
    run_scenario install-tty-yes
    lacks "SKIP no script" "$output"
    local s
    s=$(snap 1-install-tty-yes)

    contains "Continue? [Y/n]" "$output"
    [ -f "$s$UNIT" ] || fail "no systemd unit after answering y"
    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.0" ]
    grep -qx "systemctl start helixscreen" "$s/var/log/e2e-systemctl.log"
}

@test "install.sh e2e: a no-terminal update prints the golden transcript" {
    run_scenario install update
    lacks "Continue?" "$(step_output 2 "$output")"
    contains "  Update     v1.0.0 → v1.0.1 (stable)" "$(step_output 2 "$output")"
    matches_golden update 2
}

@test "install.sh e2e: a failed step reports the command, the state and the log" {
    run_scenario_status install-fail-enable
    [ "$status" -ne 0 ] || fail "a failed systemctl enable exited 0:
$output"
    contains "=== STEP 1: install-fail-enable exit=1" "$output"
    contains "[6/6] Starting HelixScreen ... FAILED" "$output"
    contains "systemctl enable helixscreen failed (exit 1):" "$output"
    contains "Failed to enable unit: Unit file helixscreen.service is masked." "$output"
    contains "re-run the installer to finish." "$output"
    contains "Full log: ~/printer_data/logs/helixscreen-install.log" "$output"
    grep -q "FAIL Starting HelixScreen" "$(snap 1-install-fail-enable)/root/printer_data/logs/helixscreen-install.log" \
        || fail "the kept log has no FAIL marker"
    matches_golden fail-enable 1
}

# The tree and the changing systemctl calls step <n>-<name> left, compared
# with the step before it.
_unchanged_since() { # before after
    run diff -r --no-dereference -x 'e2e-*.log' "$(snap "$1")" "$(snap "$2")"
    [ "$status" -eq 0 ] || fail "$2 changed the tree:
$output"
    [ "$(changing_systemctl_calls "$(snap "$1")")" = "$(changing_systemctl_calls "$(snap "$2")")" ] \
        || fail "$2 made changing systemctl calls"
}

@test "install.sh e2e: --uninstall --dry-run is refused and removes nothing" {
    run_scenario_status install uninstall-dry-run
    contains "=== STEP 2: uninstall-dry-run exit=1" "$output"
    contains "--dry-run cannot be combined with --uninstall" "$output"
    _unchanged_since 1-install 2-uninstall-dry-run
}

@test "install.sh e2e: --clean --dry-run deletes nothing" {
    run_scenario install clean-dry-run
    contains "Dry run, nothing changed." "$output"
    _unchanged_since 1-install 2-clean-dry-run
}

@test "install.sh e2e: --clean with no terminal and no --yes refuses before anything changes" {
    run_scenario_status install clean-notty
    contains "=== STEP 2: clean-notty exit=1" "$output"
    contains "Refusing to run --clean without confirmation" "$output"
    lacks "Checked system" "$(step_output 2 "$output")"
    _unchanged_since 1-install 2-clean-notty
}

@test "install.sh e2e: answering n to --clean's prompt changes nothing" {
    command -v script >/dev/null 2>&1 || skip "no script(1) for a pseudo-terminal"
    run_scenario install clean-tty-no
    lacks "SKIP no script" "$output"
    contains "Continue? [y/N]" "$output"
    contains "Nothing changed." "$output"
    lacks "Are you sure" "$output"
    _unchanged_since 1-install 2-clean-tty-no
}
