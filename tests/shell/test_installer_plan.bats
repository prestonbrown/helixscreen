#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The installer's read-only pass: the plan it builds, how it asks, and the
# --dry-run and --verbose flags.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$WORKTREE_ROOT/scripts/lib/installer"

# Inert stand-ins for every command that could reach the host's package
# manager, init system or privilege prompt. Each exits 1 and logs its call;
# a test that needs other behaviour overwrites its stub.
stub() { # name body
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls.log"\n%s\n' \
        "$1" "$BATS_TEST_TMPDIR" "${2:-exit 1}" > "$STUBBIN/$1"
    chmod +x "$STUBBIN/$1"
}

setup() {
    STUBBIN="$BATS_TEST_TMPDIR/stubbin"
    mkdir -p "$STUBBIN"
    for c in systemctl sudo apt-get apt apt-cache dpkg-query pkexec udevadm pidof config-manager curl wget; do
        stub "$c"
    done
    PATH="$STUBBIN:$PATH"

    load helpers
    skip_line_tracking
    # From here helpers.bash owns systemctl: its inert shim, scripted with
    # mock_command_script. The rest of the stubs go back in front of
    # everything, the test sandbox included.
    rm "$STUBBIN/systemctl"
    PATH="$STUBBIN:$PATH"
    export HELIX_INSTALL_TTY=0
    export SUDO=""
    # main.sh installs its own EXIT/signal/ERR traps at source time; put
    # bats' back so a failing test still reports.
    local bats_traps
    bats_traps="$(trap -p EXIT INT TERM HUP)"
    for m in common logo host_profile platform permissions requirements competing_uis desktop release service helix_print_conf moonraker kiauh plan uninstall main; do
        . "$LIB/$m.sh"
    done
    trap - ERR EXIT INT TERM HUP
    eval "$bats_traps"
}

@test "--dry-run and --verbose are parsed" {
    parse_installer_args --dry-run --verbose
    [ "$DRY_RUN" = true ]
    [ "$HELIX_INSTALL_VERBOSE" = 1 ]
    parse_installer_args -v
    [ "$HELIX_INSTALL_VERBOSE" = 1 ]
}

@test "DRY_RUN defaults to false" {
    parse_installer_args
    [ "$DRY_RUN" = false ]
}

@test "tty_confirm: ASSUME_YES answers yes without reading" {
    ASSUME_YES=true
    run tty_confirm "Continue?" n < /dev/null
    [ "$status" -eq 0 ]
}

@test "tty_confirm: no terminal and no /dev/tty returns the default" {
    HELIX_TTY_DEVICE=/nonexistent
    run tty_confirm "Continue?" y < /dev/null
    [ "$status" -eq 0 ]
    run tty_confirm "Continue?" n < /dev/null
    [ "$status" -eq 1 ]
}

@test "tty_confirm reads its answer from the tty device, not stdin" {
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run tty_confirm "Continue?" y <<EOF
y
EOF
    [ "$status" -eq 1 ]
}

@test "confirm_clean_install takes a yes from the tty device" {
    printf 'y\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    ASSUME_YES=false
    run confirm_clean_install < /dev/null
    [ "$status" -eq 0 ]
}

@test "confirm_clean_install refuses --clean with no terminal to ask" {
    HELIX_TTY_DEVICE=/nonexistent
    ASSUME_YES=false
    run confirm_clean_install < /dev/null
    [ "$status" -eq 1 ]
    contains "Refusing to run --clean without confirmation" "$output"
}

@test "plan prints one aligned line per set key, in order" {
    plan_set Printer "Raspberry Pi 4 · Kalico · systemd"
    plan_set Install "v1.1.0-beta.4 (beta)"
    run print_plan
    [ "${lines[0]}" = "  Printer    Raspberry Pi 4 · Kalico · systemd" ]
    [ "${lines[1]}" = "  Install    v1.1.0-beta.4 (beta)" ]
}

@test "plan_count_steps adds a step for packages and one for competing UIs" {
    MISSING_RUNTIME_DEPS="" MISSING_UNZIP_PKG="" COMPETING_UIS_FOUND=""
    plan_count_steps
    [ "$STEP_TOTAL" -eq 6 ]
    MISSING_UNZIP_PKG=unzip COMPETING_UIS_FOUND=KlipperScreen
    plan_count_steps
    [ "$STEP_TOTAL" -eq 8 ]
}

@test "detect_missing_runtime_deps installs nothing" {
    stub apt-get 'echo "apt-get $*" >> "'"$BATS_TEST_TMPDIR"'/apt.log"'
    detect_missing_runtime_deps pi
    [ -n "$MISSING_RUNTIME_DEPS" ]
    [ ! -f "$BATS_TEST_TMPDIR/apt.log" ]
}

@test "detect_missing_runtime_deps finds nothing to do off the Pi" {
    detect_missing_runtime_deps ad5m
    [ -z "$MISSING_RUNTIME_DEPS" ]
}

@test "detect_missing_unzip names unzip when apt can install it, and installs nothing" {
    _has_no_new_privs() { return 1; }
    # Only the stubs on PATH: no unzip, and an apt-get that logs any call.
    PATH="$STUBBIN" detect_missing_unzip
    [ "$MISSING_UNZIP_PKG" = unzip ]
    if grep -qs 'apt-get' "$BATS_TEST_TMPDIR/calls.log"; then fail "detection ran apt-get"; fi
    rm "$STUBBIN/apt-get"
    PATH="$STUBBIN" detect_missing_unzip
    [ -z "$MISSING_UNZIP_PKG" ]
}

@test "install_missing_unzip installs the package detection named" {
    MISSING_UNZIP_PKG=unzip
    stub apt-get 'exit 0'
    run install_missing_unzip
    grep -q 'apt-get install -y --no-install-recommends unzip' "$BATS_TEST_TMPDIR/calls.log" \
        || fail "apt-get install was not run"
}

@test "detect_competing_uis lists an active KlipperScreen unit without stopping it" {
    mock_command_script systemctl 'echo "systemctl $*" >> "'"$BATS_TEST_TMPDIR"'/calls.log"
case "$*" in *is-active*KlipperScreen*) exit 0 ;; esac
exit 1'
    INIT_SYSTEM=systemd
    detect_competing_uis
    [ "${COMPETING_UIS_FOUND#*KlipperScreen}" != "$COMPETING_UIS_FOUND" ]
    ! grep -E 'systemctl (stop|disable|mask|daemon-reload)' "$BATS_TEST_TMPDIR/calls.log"
}

@test "detect_moonraker_integration reports what would be added" {
    KLIPPER_CONFIG_DIR="$BATS_TEST_TMPDIR/printer_data/config"
    conf="$KLIPPER_CONFIG_DIR/moonraker.conf"
    mkdir -p "$KLIPPER_CONFIG_DIR"
    printf '[server]\n' > "$conf"
    : > "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc"
    detect_moonraker_integration pi
    [ "${MOONRAKER_ADDS#*update-manager}" != "$MOONRAKER_ADDS" ]
    [ "${MOONRAKER_ADDS#*allowlist}" != "$MOONRAKER_ADDS" ]
    [ "$(cat "$conf")" = "[server]" ]
    [ ! -s "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc" ]
}

@test "detect_moonraker_integration adds nothing that is already there" {
    KLIPPER_CONFIG_DIR="$BATS_TEST_TMPDIR/printer_data/config"
    mkdir -p "$KLIPPER_CONFIG_DIR"
    printf '[update_manager helixscreen]\n' > "$KLIPPER_CONFIG_DIR/moonraker.conf"
    printf 'helixscreen\n' > "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc"
    detect_moonraker_integration pi
    [ -z "$MOONRAKER_ADDS" ]
}

@test "detect_moonraker_integration plans no stanza when moonraker.conf includes one" {
    KLIPPER_CONFIG_DIR="$BATS_TEST_TMPDIR/printer_data/config"
    mkdir -p "$KLIPPER_CONFIG_DIR"
    printf '[server]\n[include mainsail.conf]\n' > "$KLIPPER_CONFIG_DIR/moonraker.conf"
    printf '[update_manager helixscreen]\n' > "$KLIPPER_CONFIG_DIR/mainsail.conf"
    printf 'helixscreen\n' > "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc"
    detect_moonraker_integration pi
    [ -z "$MOONRAKER_ADDS" ]
}

@test "detect_kiauh finds the extensions dir unless registration is skipped" {
    HOME="$BATS_TEST_TMPDIR/home"
    mkdir -p "$HOME/kiauh/kiauh/extensions"
    skip_kiauh_registration=false
    detect_kiauh
    [ "$KIAUH_DIR" = "$HOME/kiauh/kiauh/extensions" ]
    skip_kiauh_registration=true
    detect_kiauh
    [ -z "$KIAUH_DIR" ]
}

@test "detect_competing_uis lists the K1 stock display processes the stop kills" {
    K1_FIRMWARE=stock_klipper INIT_SYSTEM=sysv
    stub pidof 'case "$1" in Monitor) echo 123 ;; *) exit 1 ;; esac'
    detect_competing_uis
    [ "$COMPETING_UIS_FOUND" = Creality-UI ]
    K1_FIRMWARE=simple_af
    detect_competing_uis
    [ -z "$COMPETING_UIS_FOUND" ]
}

@test "detect_competing_uis lists a Klipper Mod KlipperScreen python process" {
    AD5M_FIRMWARE=klipper_mod INIT_SYSTEM=sysv
    stub ps 'echo "root 321 1 0 00:00 ? 00:00:01 python3 /root/KlipperScreen/screen.py"; exit 0'
    detect_competing_uis
    [ "$COMPETING_UIS_FOUND" = KlipperScreen ]
    if grep -qs 'kill' "$BATS_TEST_TMPDIR/calls.log"; then fail "detection killed something"; fi
}

@test "detect_missing_runtime_deps prints no warning above the plan" {
    HELIX_INSTALL_VERBOSE=0
    run detect_missing_runtime_deps pi
    lacks "WARN" "$output"
}

@test "install_runtime_deps detects first when detection has not run" {
    unset MISSING_RUNTIME_DEPS
    _has_no_new_privs() { return 1; }
    stub apt-get 'exit 0'
    run install_runtime_deps pi
    grep -q 'apt-get install' "$BATS_TEST_TMPDIR/calls.log" \
        || fail "nothing was installed: $output"
}

@test "payload_legacy_prompt_adopt takes its answer from the terminal device" {
    printf 'y\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    ASSUME_YES=false
    run payload_legacy_prompt_adopt /opt/helixscreen < /dev/null
    [ "$status" -eq 0 ]
}

@test "payload_legacy_prompt_adopt declines when nothing can ask, --yes included" {
    HELIX_TTY_DEVICE=/nonexistent
    ASSUME_YES=true
    run payload_legacy_prompt_adopt /opt/helixscreen < /dev/null
    [ "$status" -eq 1 ]
    payload_legacy_prompt_adopt /opt/helixscreen < /dev/null || true
    [ "$ASSUME_YES" = true ]
}

@test "detect_tmp_dir probes writability without sudo" {
    SUDO=sudo
    stub sudo 'exit 0'
    mkdir -p "$BATS_TEST_TMPDIR/ro" "$BATS_TEST_TMPDIR/home"
    chmod a-w "$BATS_TEST_TMPDIR/ro"
    INSTALL_DIR="$BATS_TEST_TMPDIR/ro/helixscreen"
    HOME="$BATS_TEST_TMPDIR/home"
    TMP_DIR=""
    detect_tmp_dir
    chmod u+w "$BATS_TEST_TMPDIR/ro"
    [ "$TMP_DIR" = "$HOME/.helixscreen-install" ]
    if grep -qs '^sudo' "$BATS_TEST_TMPDIR/calls.log"; then fail "detection ran sudo"; fi
}

@test "set_install_paths leaves the AD5M gcodes root alone" {
    AD5M_GCODES_ROOT="$BATS_TEST_TMPDIR/data"
    mkdir -p "$AD5M_GCODES_ROOT"
    : > "$AD5M_GCODES_ROOT/helixscreen-ad5m-v1.0.0.zip"
    TMP_DIR="$BATS_TEST_TMPDIR/helixscreen-install"
    set_install_paths ad5m forge_x
    [ -f "$AD5M_GCODES_ROOT/helixscreen-ad5m-v1.0.0.zip" ]
}

@test "check_disk_space defers a write probe sudo would have to ask for" {
    SUDO=sudo
    stub sudo 'exit 1'
    INSTALL_DIR="/nonexistent-helix-root/helixscreen"
    HELIX_DATA_MOUNT_CANDIDATES="/nonexistent-helix-data"
    DISK_CHECK_DEFERRED=""
    check_disk_space ad5x
    [ "$DISK_CHECK_DEFERRED" = 1 ]
    if grep -qs 'sudo dd' "$BATS_TEST_TMPDIR/calls.log"; then fail "probe ran sudo without -n"; fi
}

@test "probe_release HEADs the archive and downloads nothing" {
    TMP_DIR="$BATS_TEST_TMPDIR/scratch"
    R2_BASE_URL=https://r2.test HTTP_BASE_URL=http://mirror.test R2_CHANNEL=stable
    local_tarball=""
    stub curl 'case "$*" in
  --version*) echo "curl 8.0.0" ;;
  *manifest.json*) printf "{\"version\":\"1.2.3\",\"assets\":{\"pi\":{\"zip_sha256\":\"ab\"}}}" ;;
  *r2.test/releases/v1.2.3/helixscreen-pi.zip*) printf 200 ;;
  *) exit 22 ;;
esac'
    probe_release v1.2.3 pi
    [ "$PROBE_SIZE_TEXT" = "SHA256 available" ]
    [ ! -e "$TMP_DIR" ]
    grep -q 'r2.test/releases/v1.2.3/helixscreen-pi.zip' "$BATS_TEST_TMPDIR/calls.log"
}

_probe_setup() {
    TMP_DIR="$BATS_TEST_TMPDIR/scratch"
    R2_BASE_URL=https://r2.test HTTP_BASE_URL=http://mirror.test R2_CHANNEL=stable
    local_tarball=""
}

@test "probe_release stops when every candidate answers 404" {
    _probe_setup
    stub curl 'case "$*" in --version*) echo "curl 8.0.0" ;; *http_code*) printf 404 ;; *) exit 22 ;; esac'
    run probe_release v9.9.9 pi
    [ "$status" -eq 1 ]
    contains "No HelixScreen v9.9.9 release for pi" "$output"
}

@test "probe_release continues unchecked when a candidate cannot be reached" {
    _probe_setup
    stub curl 'case "$*" in
  --version*) echo "curl 8.0.0" ;;
  *http_code*r2.test*) printf 000; exit 28 ;;
  *http_code*) printf 404 ;;
  *) exit 22 ;;
esac'
    run probe_release v1.2.3 pi
    [ "$status" -eq 0 ]
    probe_release v1.2.3 pi
    contains "release not checked" "$PROBE_SIZE_TEXT"
}

@test "probe_release sizes a --local archive the way the step does" {
    local_tarball="$BATS_TEST_TMPDIR/helixscreen-pi-v1.2.3.tar.gz"
    head -c 1700000 /dev/zero > "$local_tarball"
    probe_release v1.2.3 pi
    [ "$PROBE_SIZE_TEXT" = "local file, $(file_size_text "$local_tarball")" ] \
        || fail "plan says '$PROBE_SIZE_TEXT', step says '$(file_size_text "$local_tarball")'"
}

@test "probe_release HEAD requests are time-limited" {
    _probe_setup
    stub curl 'case "$*" in --version*) echo "curl 8.0.0" ;; *http_code*) printf 200 ;; *) exit 22 ;; esac'
    probe_release v1.2.3 pi
    grep 'http_code' "$BATS_TEST_TMPDIR/calls.log" | grep -q -- '--max-time' \
        || fail "HEAD without --max-time"
}

@test "_url_probe through wget writes no file and reads 404 as missing" {
    _has_real_curl() { return 1; }
    stub wget 'case "$*" in --help*) echo "  --spider" ;; *) echo "wget: server returned error: HTTP/1.1 404 Not Found" >&2; exit 8 ;; esac'
    cd "$BATS_TEST_TMPDIR"
    [ "$(_url_probe http://mirror.test/x.zip)" = missing ]
    grep -q -- '--spider' "$BATS_TEST_TMPDIR/calls.log"
    grep -q -- '-O /dev/null' "$BATS_TEST_TMPDIR/calls.log"
}

@test "_url_probe does not trust a wget that does not offer --spider" {
    _has_real_curl() { return 1; }
    stub wget 'case "$*" in --help*) echo "usage: wget URL" ;; *) exit 0 ;; esac'
    [ "$(_url_probe http://mirror.test/x.zip)" = unknown ]
    if grep -q 'mirror.test' "$BATS_TEST_TMPDIR/calls.log"; then fail "the shim was asked to fetch"; fi
}

@test "probe_release sizes a --local archive without the network" {
    local_tarball="$BATS_TEST_TMPDIR/helixscreen-pi.zip"
    head -c 4096 /dev/zero > "$local_tarball"
    probe_release local pi
    contains "local file" "$PROBE_SIZE_TEXT"
    if grep -qs '^curl' "$BATS_TEST_TMPDIR/calls.log"; then fail "probed the network"; fi
}

_cp_setup() {
    TMP_DIR="$BATS_TEST_TMPDIR/scratch/helixscreen-install"
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    update_mode=false DRY_RUN=false ASSUME_YES=false SUDO=""
    INIT_SYSTEM=systemd KLIPPER_USER=pi UI_UTF8=0
    MISSING_RUNTIME_DEPS="" MISSING_UNZIP_PKG="" COMPETING_UIS_FOUND=""
    MOONRAKER_ADDS="" KIAUH_DIR="" PROBE_SIZE_TEXT="" DISK_CHECK_DEFERRED=""
    KIAUH_SUDO="" KIAUH_WRITE_SKIPPED=""
}

@test "confirm_point on a dry run shows the plan and exits 0 with nothing written" {
    _cp_setup
    DRY_RUN=true
    COMPETING_UIS_FOUND=KlipperScreen
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "Install    v1.2.3 (stable)" "$output"
    contains "Disable    KlipperScreen" "$output"
    contains "Dry run, nothing changed." "$output"
    lacks "Checked system" "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "confirm_point with no terminal continues, closes Checked system and opens the log" {
    _cp_setup
    UI_TTY=0
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "Checked system ... ok (pi, systemd, user pi)" "$output"
    grep -q 'PLAN Install: v1.2.3' "$TMP_DIR/install.log"
}

@test "confirm_point asks on a terminal, and n changes nothing" {
    _cp_setup
    UI_TTY=1
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "Nothing changed." "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "confirm_point never asks on --update and shows the version change" {
    _cp_setup
    UI_TTY=1
    update_mode=true
    mkdir -p "$INSTALL_DIR"
    printf '{"project_name":"helixscreen","version":"v1.2.2"}' > "$INSTALL_DIR/release_info.json"
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "v1.2.2 -> v1.2.3" "$output"
    [ -f "$TMP_DIR/install.log" ]
}

@test "an --update plan reads Update <installed> -> <target>" {
    _cp_setup
    update_mode=true
    mkdir -p "$INSTALL_DIR"
    printf '{"project_name":"helixscreen","version":"v1.2.2"}' > "$INSTALL_DIR/release_info.json"
    run confirm_point pi v1.2.3
    contains "  Update     v1.2.2 -> v1.2.3 (stable)" "$output"
    lacks "  Install " "$output"
}

@test "an --update plan never runs the installed binary to learn its version" {
    # A binary that hangs (a broken library, a wrapper waiting on a tty) would
    # stop the installer before the plan, --dry-run included.
    _cp_setup
    update_mode=true
    mkdir -p "$INSTALL_DIR/bin"
    printf '#!/bin/sh\ntouch "%s/ran"\necho "HelixScreen v1.2.1 (abc123)"\n' \
        "$BATS_TEST_TMPDIR" > "$INSTALL_DIR/bin/helix-screen"
    chmod +x "$INSTALL_DIR/bin/helix-screen"
    run confirm_point pi v1.2.3
    [ ! -e "$BATS_TEST_TMPDIR/ran" ] || fail "the plan ran $INSTALL_DIR/bin/helix-screen"
    contains "  Install    v1.2.3 (stable)" "$output"
}

@test "an --update plan with no readable installed version keeps the Install line" {
    _cp_setup
    update_mode=true
    mkdir -p "$INSTALL_DIR"
    run confirm_point pi v1.2.3
    contains "  Install    v1.2.3 (stable)" "$output"
    lacks "Update " "$output"
}

@test "confirm_point asks sudo for its password once, before any step" {
    _cp_setup
    _has_no_new_privs() { return 1; }
    UI_TTY=0
    SUDO=sudo
    stub sudo 'case "$1" in -n) exit 1 ;; -v) exit 0 ;; esac; exit 1'
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    [ "$(grep -c '^sudo -v' "$BATS_TEST_TMPDIR/calls.log")" -eq 1 ]
}

@test "confirm_point stops with nothing changed when sudo is refused" {
    _cp_setup
    _has_no_new_privs() { return 1; }
    UI_TTY=0
    SUDO=sudo
    stub sudo 'exit 1'
    run confirm_point pi v1.2.3
    [ "$status" -eq 1 ]
    contains "nothing changed" "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "the payload root is recorded after the confirm point, not by mod_payload_mode_block" {
    HOST_MOD_ROOT="$BATS_TEST_TMPDIR/usr/data/config/mod"
    HELIX_MOD_PAYLOAD=1 HOST_SERVICE_MECHANISM=mod-managed uninstall_mode=false
    MOD_PAYLOAD_ROOT="" MOD_PAYLOAD_FLAG_GIVEN="" HOST_LEGACY_INSTALL_ROOT=""
    INSTALL_DIR="$BATS_TEST_TMPDIR/usr/data/config/mod_data/helixscreen"
    mkdir -p "$(host_mod_data)"
    mod_payload_mode_block
    [ ! -e "$(host_payload_root_record)" ]
    record_payload_root_if_payload
    [ "$(cat "$(host_payload_root_record)")" = "$INSTALL_DIR" ]
}

@test "confirm_point continues under NoNewPrivileges, where sudo cannot work" {
    _cp_setup
    UI_TTY=0
    SUDO=sudo
    _has_no_new_privs() { return 0; }
    stub sudo 'exit 1'
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "Checked system" "$output"
}

@test "tty_confirm: end of input at the prompt is a no, an empty line the default" {
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    ASSUME_YES=false
    printf '' > "$HELIX_TTY_DEVICE"
    run tty_confirm "Continue?" y < /dev/null
    [ "$status" -eq 1 ]
    printf '\n' > "$HELIX_TTY_DEVICE"
    run tty_confirm "Continue?" y < /dev/null
    [ "$status" -eq 0 ]
}

_klipper_down() {
    stub ps 'exit 0'
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    printf 'n\n' > "$HELIX_TTY_DEVICE"
    DRY_RUN=false ASSUME_YES=false
}

@test "check_klipper_ecosystem asks through the terminal device" {
    _klipper_down
    run check_klipper_ecosystem k1 < /dev/null
    [ "$status" -eq 1 ]
    contains "Installation cancelled." "$output"
}

@test "check_klipper_ecosystem never asks on a dry run" {
    _klipper_down
    DRY_RUN=true
    run check_klipper_ecosystem k1 < /dev/null
    [ "$status" -eq 0 ]
    contains "Klipper does not appear to be running" "$output"
}

@test "check_klipper_ecosystem never asks under --yes" {
    _klipper_down
    ASSUME_YES=true
    run check_klipper_ecosystem k1 < /dev/null
    [ "$status" -eq 0 ]
}

# The body of function $1 in file $2, comments stripped.
_fn_body() {
    awk -v fn="$1" '$0 ~ "^"fn"\\(\\) *\\{" {c=1} c{print} c&&/^\}/{exit}' "$2" | sed 's/#.*//'
}

@test "install.sh --uninstall sweeps the AD5M gcodes root" {
    _fn_body uninstall "$LIB/uninstall.sh" \
        | grep -q '"$platform" = "ad5m" ].*cleanup_ad5m_gcodes_root' \
        || fail "uninstall() does not sweep the gcodes root on ad5m"
}

@test "the bundled uninstaller sweeps the AD5M gcodes root" {
    _fn_body remove_installation "$WORKTREE_ROOT/scripts/bundle-uninstaller.sh" \
        | grep -q '"${platform:-}" = "ad5m" ].*cleanup_ad5m_gcodes_root' \
        || fail "remove_installation() does not sweep the gcodes root on ad5m"
}

@test "the AD5M gcodes sweep in uninstall runs for ad5m only" {
    AD5M_GCODES_ROOT="$BATS_TEST_TMPDIR/data"
    mkdir -p "$AD5M_GCODES_ROOT"
    : > "$AD5M_GCODES_ROOT/helixscreen-ad5m-v1.0.0.zip"
    line=$(_fn_body uninstall "$LIB/uninstall.sh" | grep 'cleanup_ad5m_gcodes_root')
    platform=k1; eval "$line"
    [ -f "$AD5M_GCODES_ROOT/helixscreen-ad5m-v1.0.0.zip" ]
    platform=ad5m; eval "$line"
    [ ! -e "$AD5M_GCODES_ROOT/helixscreen-ad5m-v1.0.0.zip" ]
}

@test "--dry-run with --uninstall is refused, since the uninstall never reaches the plan" {
    run parse_installer_args --uninstall --dry-run
    [ "$status" -eq 1 ]
    contains "--dry-run cannot be combined with --uninstall" "$output"
    run parse_installer_args --dry-run --uninstall
    [ "$status" -eq 1 ]
}

@test "--clean --dry-run shows the Remove line and stops without asking" {
    _cp_setup
    clean_mode=true DRY_RUN=true UI_TTY=0
    HELIX_TTY_DEVICE=/nonexistent
    run confirm_point pi v1.2.3 < /dev/null
    [ "$status" -eq 0 ]
    contains "  Remove     " "$output"
    contains "Dry run, nothing changed." "$output"
    lacks "Refusing" "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "--clean: the Continue? prompt is the consent and defaults to no" {
    _cp_setup
    clean_mode=true UI_TTY=1
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    printf '\n' > "$HELIX_TTY_DEVICE"
    run confirm_point pi v1.2.3 < /dev/null
    [ "$status" -eq 0 ]
    contains "Continue? [y/N]" "$output"
    contains "Nothing changed." "$output"
    lacks "Are you sure" "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "--clean: y at the prompt goes ahead, with no second question" {
    _cp_setup
    clean_mode=true UI_TTY=1
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    printf 'y\n' > "$HELIX_TTY_DEVICE"
    run confirm_point pi v1.2.3 < /dev/null
    [ "$status" -eq 0 ]
    [ "$(grep -c 'Continue?' <<< "$output")" -eq 1 ]
    contains "Checked system" "$output"
}

@test "--clean with no terminal and no --yes refuses at the plan, before anything changes" {
    _cp_setup
    clean_mode=true UI_TTY=0
    HELIX_TTY_DEVICE=/nonexistent
    run confirm_point pi v1.2.3 < /dev/null
    [ "$status" -eq 1 ]
    contains "Refusing to run --clean without confirmation" "$output"
    lacks "Checked system" "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "--clean --yes goes ahead with no terminal" {
    _cp_setup
    clean_mode=true UI_TTY=0 ASSUME_YES=true
    HELIX_TTY_DEVICE=/nonexistent
    run confirm_point pi v1.2.3 < /dev/null
    [ "$status" -eq 0 ]
    contains "Checked system" "$output"
}

@test "clean_old_installation does not ask again: confirm_point holds the consent" {
    if _fn_body clean_old_installation "$LIB/uninstall.sh" | grep -q 'confirm_clean_install'; then
        fail "clean_old_installation still asks, after the download and the UI stop"
    fi
}

@test "tty_can_ask and tty_confirm survive a terminal that cannot open under dash and BusyBox ash" {
    local ran=0 sh
    for sh in dash "busybox ash"; do
        $sh -c : 2>/dev/null || continue
        ran=1
        run $sh -c "set -eu; . '$LIB/common.sh'; HELIX_TTY_DEVICE=/nonexistent ASSUME_YES=false
            tty_can_ask < /dev/null || echo cannot-ask
            tty_confirm 'Continue?' n < /dev/null || echo default-no"
        [ "$status" -eq 0 ] || fail "$sh ended the script: $output"
        contains "cannot-ask" "$output"
        contains "default-no" "$output"
    done
    [ "$ran" -eq 1 ] || skip "neither dash nor BusyBox ash is installed"
}

@test "detect_missing_runtime_deps under NoNewPrivileges plans no libraries and says what to install" {
    _has_no_new_privs() { return 0; }
    run detect_missing_runtime_deps pi
    contains "sudo apt-get install libdrm2" "$output"
    detect_missing_runtime_deps pi 2>/dev/null
    [ -z "$MISSING_RUNTIME_DEPS" ]
    MISSING_UNZIP_PKG="" COMPETING_UIS_FOUND=""
    plan_count_steps
    [ "$STEP_TOTAL" -eq 6 ]
}

@test "install_runtime_deps returns non-zero when apt cannot install the libraries" {
    _has_no_new_privs() { return 1; }
    MISSING_RUNTIME_DEPS="libdrm2"
    stub apt-get 'exit 1'
    run install_runtime_deps pi
    [ "$status" -ne 0 ]
}

@test "the libraries step fails, and the install continues, when apt cannot install them" {
    _has_no_new_privs() { return 1; }
    platform=pi UI_TTY=0 MISSING_UNZIP_PKG="" MISSING_RUNTIME_DEPS="libdrm2"
    stub apt-get 'exit 1'
    run install_libraries_step
    [ "$status" -eq 0 ]
    contains "Installing libraries ... FAILED (continuing; see above)" "$output"
    lacks "Installed libraries" "$output"
}

@test "the libraries step fails when apt cannot install unzip, though python can unzip" {
    _has_no_new_privs() { return 1; }
    _py_has_module() { return 0; }
    platform=x86 UI_TTY=0 MISSING_UNZIP_PKG=unzip MISSING_RUNTIME_DEPS=""
    stub apt-get 'exit 1'
    run install_libraries_step
    [ "$status" -eq 0 ]
    contains "Installing libraries ... FAILED" "$output"
}

@test "the libraries step names what apt installed" {
    _has_no_new_privs() { return 1; }
    platform=pi UI_TTY=0 MISSING_UNZIP_PKG="" MISSING_RUNTIME_DEPS="libdrm2"
    stub apt-get 'exit 0'
    run install_libraries_step
    [ "$status" -eq 0 ]
    contains "Installed libraries ... ok (libdrm2)" "$output"
}

@test "the plan's sudo row lists libraries only when some are missing" {
    _cp_setup
    _has_no_new_privs() { return 1; }
    DRY_RUN=true SUDO=sudo
    run confirm_point pi v1.2.3
    contains "  sudo       needed for: service, udev and polkit rules" "$output"
    MISSING_RUNTIME_DEPS="libdrm2"
    run confirm_point pi v1.2.3
    contains "  sudo       needed for: service, libraries, udev and polkit rules" "$output"
}

@test "the plan names a KIAUH it will write with sudo, and one it has to skip" {
    _cp_setup
    _has_no_new_privs() { return 1; }
    DRY_RUN=true SUDO=sudo
    KIAUH_DIR=/home/mks/kiauh/kiauh/extensions KIAUH_SUDO=1
    run confirm_point pi v1.2.3
    contains "  Add        KIAUH extension" "$output"
    contains "  sudo       needed for: service, KIAUH extension, udev and polkit rules" "$output"
    lacks "  KIAUH      found" "$output"

    KIAUH_DIR="" KIAUH_SUDO="" KIAUH_WRITE_SKIPPED=/home/mks/kiauh/kiauh/extensions SUDO=""
    run confirm_point pi v1.2.3
    lacks "KIAUH extension" "$output"
    contains "  KIAUH      found at /home/mks/kiauh/kiauh/extensions, not writable without sudo: extension skipped" "$output"
}

@test "the plan has no sudo row under NoNewPrivileges, where sudo cannot run" {
    _cp_setup
    _has_no_new_privs() { return 0; }
    DRY_RUN=true SUDO=sudo
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    lacks "  sudo " "$output"
}

@test "check_klipper_ecosystem never asks on --update" {
    _klipper_down
    update_mode=true
    run check_klipper_ecosystem k1 < /dev/null
    [ "$status" -eq 0 ]
    lacks "Continue anyway?" "$output"
    contains "Klipper does not appear to be running" "$output"
}

@test "a payload install counts the step it always opens after Moonraker" {
    MISSING_RUNTIME_DEPS="" MISSING_UNZIP_PKG="" COMPETING_UIS_FOUND=""
    HOST_SERVICE_MECHANISM=mod-managed
    plan_count_steps
    [ "$STEP_TOTAL" -eq 6 ]
    if _fn_body apply_install "$LIB/main.sh" | grep -q 'plan_starts_ui; then step_done; else step_skip'; then
        fail "the payload path skips a step the count includes"
    fi
}

@test "detect_kiauh adds nothing when the extension is already installed" {
    HOME="$BATS_TEST_TMPDIR/home"
    mkdir -p "$HOME/kiauh/kiauh/extensions/helixscreen"
    skip_kiauh_registration=false
    detect_kiauh
    [ -z "$KIAUH_DIR" ]
    [ -z "$(plan_adds_line)" ]
}
