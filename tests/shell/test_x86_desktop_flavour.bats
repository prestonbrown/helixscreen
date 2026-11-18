#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The x86 desktop flavour (scripts/lib/installer/desktop.sh): how a run on an
# x86 box settles desktop vs kiosk, what a desktop install writes, and what its
# uninstall is allowed to remove. Every path the code reads (HOME, the systemd
# unit dir, init.d, /home, /proc, the known install roots) points into the
# test's own tmp dir.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$WORKTREE_ROOT/scripts/lib/installer"

setup() {
    load helpers
    # Command stubs that log every call, so a test can prove the desktop path
    # never reached for privileges, packages or the init system.
    STUBBIN="$BATS_TEST_TMPDIR/stubbin"
    mkdir -p "$STUBBIN"
    for c in sudo apt-get apt apt-cache dpkg-query pkexec udevadm pidof curl wget; do
        printf '#!/bin/sh\necho "%s $*" >> "%s/calls.log"\nexit 1\n' "$c" "$BATS_TEST_TMPDIR" > "$STUBBIN/$c"
        chmod +x "$STUBBIN/$c"
    done
    PATH="$STUBBIN:$PATH"
    # systemctl answers nothing active or enabled unless a test says so.
    set_systemctl ''
    # An ordinary user unless a test says root, whoever runs the suite.
    set_uid 1000

    export HOME="$BATS_TEST_TMPDIR/home/alice"
    mkdir -p "$HOME"
    unset XDG_DATA_HOME XDG_STATE_HOME KLIPPER_HOME DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE INSTALL_DIR TMP_DIR
    export HELIX_SYSTEMD_UNIT_DIR="$BATS_TEST_TMPDIR/etc/systemd/system"
    export HELIX_INIT_D_DIR="$BATS_TEST_TMPDIR/etc/init.d"
    export HELIX_HOME_ROOT="$BATS_TEST_TMPDIR/home"
    export HELIX_PROC_ROOT="$BATS_TEST_TMPDIR/no-proc"
    export _HELIX_KNOWN_INSTALL_DIRS="$BATS_TEST_TMPDIR/opt/helixscreen"
    export HELIX_TTY_DEVICE=/nonexistent
    export HELIX_INSTALL_TTY=0
    export SUDO=""
    mkdir -p "$HELIX_SYSTEMD_UNIT_DIR" "$HELIX_INIT_D_DIR"

    local bats_traps
    bats_traps="$(trap -p EXIT INT TERM HUP)"
    for m in common logo host_profile platform permissions requirements competing_uis desktop release service helix_print_conf moonraker kiauh printer_seed plan uninstall main; do
        . "$LIB/$m.sh"
    done
    trap - ERR EXIT INT TERM HUP
    eval "$bats_traps"

    platform=x86
    X86_FLAVOUR_FLAG=""
    update_mode=false
    uninstall_mode=false
    DRY_RUN=false
    ASSUME_YES=false
    DESK="$HOME/.local/share/helixscreen"
}

# systemctl's answer to everything: is-enabled prints $1's line when asked
# about a unit named in it ("gdm3=enabled"), else nothing and exit 1.
set_systemctl() {
    mock_command_script systemctl "
echo \"systemctl \$*\" >> '$BATS_TEST_TMPDIR/calls.log'
case \"\$1\" in
    is-enabled) case '$1' in \"\$2=\"*) echo '${1#*=}'; exit 0 ;; esac; exit 1 ;;
    *) exit 1 ;;
esac"
}

# `id -u` answers UID; every other form of id runs the real one.
set_uid() {
    mock_command_script id "[ \"\$1\" = -u ] && [ \$# -eq 1 ] && { echo $1; exit 0; }
exec /usr/bin/id \"\$@\""
}

# A desktop payload: binary plus the release_info.json the release bakes.
make_payload() { # dir asset
    mkdir -p "$1/bin" "$1/assets/images" "$1/ui_xml" "$1/config"
    create_fake_elf "$1/bin/helix-screen" 02 3e
    chmod +x "$1/bin/helix-screen"
    : > "$1/assets/images/helix-icon-256.png"
    printf '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"v1.2.3","asset_name":"%s"}\n' \
        "$2" > "$1/release_info.json"
}

make_desktop_install() { make_payload "$DESK" helixscreen-x86-desktop.zip; }

# An x86 install that predates the desktop flavour: no release_info.json.
make_kiosk_install() { # dir
    mkdir -p "$1/bin"
    printf '#!/bin/sh\n' > "$1/bin/helix-screen"
    chmod +x "$1/bin/helix-screen"
}

called() { grep -qs "^$1" "$BATS_TEST_TMPDIR/calls.log"; }

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------

@test "--desktop and --kiosk set the flavour flag" {
    parse_installer_args --desktop
    [ "$X86_FLAVOUR_FLAG" = desktop ]
    parse_installer_args --kiosk
    [ "$X86_FLAVOUR_FLAG" = kiosk ]
    parse_installer_args
    [ -z "$X86_FLAVOUR_FLAG" ]
}

@test "--desktop and --kiosk together are refused" {
    run parse_installer_args --desktop --kiosk
    [ "$status" -eq 1 ]
    contains "cannot be combined" "$output"
    run parse_installer_args --kiosk --desktop
    [ "$status" -eq 1 ]
}

@test "--desktop off x86 is refused, naming the platform" {
    platform=pi
    X86_FLAVOUR_FLAG=desktop
    run resolve_x86_flavour
    [ "$status" -eq 1 ]
    contains "--desktop applies to x86_64 Linux only" "$output"
    contains "pi system" "$output"
}

@test "--kiosk off x86 is refused" {
    platform=k1
    X86_FLAVOUR_FLAG=kiosk
    run resolve_x86_flavour
    [ "$status" -eq 1 ]
}

@test "off x86 with no flag the platform and plan are untouched" {
    platform=pi
    export DISPLAY=:0
    resolve_x86_flavour
    [ "$platform" = pi ]
    [ -z "$X86_FLAVOUR_REASON" ]
    [ -z "$(x86_flavour_plan_text)" ]
}

@test "usage lists --desktop and --kiosk" {
    run usage
    contains "--desktop" "$output"
    contains "--kiosk" "$output"
}

# ---------------------------------------------------------------------------
# Precedence: flag > existing install > ask or infer
# ---------------------------------------------------------------------------

@test "a flag outranks the install on disk, both ways" {
    make_desktop_install
    X86_FLAVOUR_FLAG=kiosk
    resolve_x86_flavour
    [ "$platform" = x86 ]
    [ "$X86_FLAVOUR_REASON" = "--kiosk" ]

    rm -rf "$DESK"
    make_kiosk_install "$HOME/helixscreen"
    platform=x86
    X86_FLAVOUR_FLAG=desktop
    resolve_x86_flavour
    [ "$platform" = x86-desktop ]
    [ "$X86_FLAVOUR_REASON" = "--desktop" ]
}

@test "an existing desktop install stays desktop with no desktop session to see" {
    make_desktop_install
    update_mode=true
    resolve_x86_flavour
    [ "$platform" = x86-desktop ]
    [ "$X86_FLAVOUR_REASON" = "existing install" ]
}

@test "an unmarked x86 install stays kiosk on --update, desktop session or not" {
    make_kiosk_install "$HOME/helixscreen"
    export DISPLAY=:0
    update_mode=true
    resolve_x86_flavour
    [ "$platform" = x86 ]
    [ "$X86_FLAVOUR_REASON" = "existing install" ]
}

@test "the kiosk service alone marks a kiosk install the user cannot read" {
    : > "$HELIX_SYSTEMD_UNIT_DIR/helixscreen.service"
    export DISPLAY=:0
    resolve_x86_flavour
    [ "$platform" = x86 ]
    [ "$X86_FLAVOUR_REASON" = "existing install" ]
}

@test "a kiosk install under another user's home counts" {
    make_kiosk_install "$HELIX_HOME_ROOT/pi/helixscreen"
    export DISPLAY=:0
    resolve_x86_flavour
    [ "$platform" = x86 ]
}

@test "a desktop tarball extracted by hand into ~/helixscreen is no kiosk install" {
    make_payload "$HOME/helixscreen" helixscreen-x86-desktop.zip
    export DISPLAY=:0
    resolve_x86_flavour
    [ "$platform" = x86-desktop ]
    [ "$X86_FLAVOUR_REASON" = "desktop session found" ]
}

@test "a directory at the desktop root without the desktop payload is not a desktop install" {
    make_payload "$DESK" helixscreen-x86.zip
    export DISPLAY=:0
    resolve_x86_flavour
    # Not ours, so this reads as a fresh box with a desktop session.
    [ "$X86_FLAVOUR_REASON" = "desktop session found" ]
}

@test "an in-app update is the kiosk, whatever else is on disk" {
    make_desktop_install
    make_kiosk_install "$HOME/helixscreen"
    export DISPLAY=:0
    HELIX_SELF_UPDATE=1 resolve_x86_flavour
    [ "$platform" = x86 ]
    [ "$X86_FLAVOUR_REASON" = "in-app update" ]
}

@test "both flavours installed and no flag is refused" {
    make_desktop_install
    make_kiosk_install "$HOME/helixscreen"
    run resolve_x86_flavour
    [ "$status" -eq 1 ]
    contains "--desktop or --kiosk" "$output"
}

@test "a fresh install with no desktop session is kiosk without asking" {
    printf 'y\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    resolve_x86_flavour < /dev/null
    [ "$platform" = x86 ]
    [ "$X86_FLAVOUR_REASON" = "no desktop session found" ]
}

@test "a fresh install in a desktop session with no terminal infers desktop" {
    export DISPLAY=:0
    resolve_x86_flavour < /dev/null
    [ "$platform" = x86-desktop ]
    [ "$X86_FLAVOUR_REASON" = "desktop session found" ]
}

@test "an enabled display manager is a desktop, even run from ssh" {
    set_systemctl 'gdm3=enabled'
    resolve_x86_flavour < /dev/null
    [ "$platform" = x86-desktop ]
}

@test "a display manager that is installed but disabled is no desktop" {
    set_systemctl 'gdm3=disabled'
    resolve_x86_flavour < /dev/null
    [ "$platform" = x86 ]
}

@test "a fresh install in a desktop session asks, and n is kiosk" {
    export DISPLAY=:0
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    resolve_x86_flavour < /dev/null 2> "$BATS_TEST_TMPDIR/err"
    [ "$platform" = x86 ]
    [ "$X86_FLAVOUR_REASON" = "you chose" ]
    grep -q "Run HelixScreen in a window on this desktop?" "$BATS_TEST_TMPDIR/err"
}

@test "end of input at the flavour question changes nothing" {
    export DISPLAY=:0
    : > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run resolve_x86_flavour < /dev/null
    [ "$status" -eq 1 ]
    contains "Nothing changed" "$output"
    [ "$platform" = x86 ]
}

@test "a root run on a desktop box with no install is the kiosk, unasked" {
    set_uid 0
    set_systemctl 'gdm3=enabled'
    export DISPLAY=:0
    ASSUME_YES=true
    resolve_x86_flavour
    [ "$platform" = x86 ]
    [ "$X86_FLAVOUR_REASON" = "running as root" ]
}

@test "a root run still finds an existing desktop install, and is refused" {
    make_desktop_install
    set_uid 0
    resolve_x86_flavour
    [ "$platform" = x86-desktop ]
    run check_permissions x86-desktop
    [ "$status" -eq 1 ]
    contains "without sudo" "$output"
}

@test "an empty answer takes the window" {
    export DISPLAY=:0
    printf '\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    resolve_x86_flavour < /dev/null 2>/dev/null
    [ "$platform" = x86-desktop ]
    [ "$X86_FLAVOUR_REASON" = "you chose" ]
}

@test "--update in a desktop session with nothing installed never asks" {
    export DISPLAY=:0
    update_mode=true
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    resolve_x86_flavour < /dev/null
    [ "$platform" = x86-desktop ]
}

@test "--dry-run does not ask, and its plan says a real run would" {
    export DISPLAY=:0
    DRY_RUN=true
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    resolve_x86_flavour < /dev/null
    [ "$platform" = x86-desktop ]
    [ "$(x86_flavour_plan_text)" = "window on this desktop (desktop session found; a real run asks)" ]
}

@test "uninstall with no desktop install on disk is the kiosk uninstall" {
    export DISPLAY=:0
    uninstall_mode=true
    resolve_x86_flavour
    [ "$platform" = x86 ]
}

@test "the plan names the kiosk flavour and why" {
    resolve_x86_flavour
    [ "$(x86_flavour_plan_text)" = "kiosk, takes over the screen (no desktop session found)" ]
}

# ---------------------------------------------------------------------------
# Paths, download and validation
# ---------------------------------------------------------------------------

@test "the desktop root is under the XDG data dir, never ~/helixscreen" {
    [ "$(desktop_install_root)" = "$HOME/.local/share/helixscreen" ]
    XDG_DATA_HOME="$BATS_TEST_TMPDIR/xdg"
    [ "$(desktop_install_root)" = "$BATS_TEST_TMPDIR/xdg/helixscreen" ]
    # The spec says a relative XDG_DATA_HOME is ignored.
    XDG_DATA_HOME="relative/data"
    [ "$(desktop_install_root)" = "$HOME/.local/share/helixscreen" ]
}

@test "a home that needs quoting has no desktop root" {
    HOME="$BATS_TEST_TMPDIR/home/al ice"
    run desktop_install_root
    [ "$status" -ne 0 ]
    HOME=""
    run desktop_install_root
    [ "$status" -ne 0 ]
}

@test "set_install_paths keeps the desktop root even beside a kiosk install" {
    make_kiosk_install "$BATS_TEST_TMPDIR/opt/helixscreen"
    _USER_INSTALL_DIR=""
    set_install_paths x86-desktop
    [ "$INSTALL_DIR" = "$DESK" ]
    [ -z "$INIT_SCRIPT_DEST" ]
    [ -z "$KLIPPER_USER" ]
    [ "$(install_log_dest)" = "$HOME/.local/state/helixscreen/helixscreen-install.log" ]
}

@test "the desktop downloads the x86-desktop artifacts" {
    [ "$(get_download_platform x86-desktop)" = x86-desktop ]
    [ "$(helix_self_update_asset x86-desktop)" = helixscreen-x86-desktop.zip ]
    _ensure_manifest() { return 1; }
    local zip_filename tar_filename zip_r2 tar_r2 zip_gh tar_gh tar_http zip_sha tar_sha manifest_ok
    _release_candidates v1.2.3 "$(get_release_platform x86-desktop)"
    [ "$zip_filename" = helixscreen-x86-desktop.zip ]
    [ "$tar_filename" = helixscreen-x86-desktop-v1.2.3.tar.gz ]
    contains "/releases/download/v1.2.3/helixscreen-x86-desktop.zip" "$zip_gh"
    # The kiosk keeps its own.
    [ "$(helix_self_update_asset x86)" = helixscreen-x86.zip ]
}

@test "the desktop binary is validated as x86_64" {
    create_fake_elf "$BATS_TEST_TMPDIR/x64" 02 3e
    create_fake_aarch64_elf "$BATS_TEST_TMPDIR/a64"
    run validate_binary_architecture "$BATS_TEST_TMPDIR/x64" x86-desktop
    [ "$status" -eq 0 ]
    contains "x86_64 64-bit" "$output"
    run validate_binary_architecture "$BATS_TEST_TMPDIR/a64" x86-desktop
    [ "$status" -eq 1 ]
    contains "Architecture mismatch" "$output"
}

@test "a desktop run as root is refused; as a user it needs no sudo" {
    set_uid 0
    run check_permissions x86-desktop
    [ "$status" -eq 1 ]
    contains "without sudo" "$output"
    contains "--kiosk" "$output"
    set_uid 1000
    SUDO=sudo
    check_permissions x86-desktop
    [ -z "$SUDO" ]
}

@test "the desktop plan counts four steps whatever the kiosk would find" {
    platform=x86-desktop
    MISSING_UNZIP_PKG=unzip MISSING_RUNTIME_DEPS=libfoo COMPETING_UIS_FOUND=KlipperScreen
    plan_count_steps
    [ "$STEP_TOTAL" -eq 4 ]
    platform=x86
    plan_count_steps
    [ "$STEP_TOTAL" -eq 8 ]
}

@test "the desktop finds nothing to disable, register or apt-install" {
    mkdir -p "$HOME/kiauh/kiauh/extensions"
    platform=x86
    detect_kiauh
    [ -n "$KIAUH_DIR" ]
    platform=x86-desktop
    detect_kiauh
    [ -z "$KIAUH_DIR" ]
    printf '[server]\nhost: 0.0.0.0\n' > "$BATS_TEST_TMPDIR/moonraker.conf"
    MOONRAKER_CONF_PATHS="$BATS_TEST_TMPDIR/moonraker.conf"
    detect_moonraker_integration x86
    contains update-manager "$MOONRAKER_ADDS"
    detect_moonraker_integration x86-desktop
    [ -z "$MOONRAKER_ADDS" ]
    INIT_SYSTEM=systemd
    set_systemctl 'KlipperScreen=enabled'
    platform=x86
    detect_competing_uis
    [ "$COMPETING_UIS_FOUND" = KlipperScreen ]
    platform=x86-desktop
    detect_competing_uis
    [ -z "$COMPETING_UIS_FOUND" ]
    PATH="$STUBBIN" detect_missing_unzip
    [ -z "$MISSING_UNZIP_PKG" ]
    [ "$(plan_adds_line)" = "app menu entry" ]
}

# ---------------------------------------------------------------------------
# The whole install, from a --local archive
# ---------------------------------------------------------------------------

# Run main() as an x86 box whose init system is whatever the test host has.
run_main() {
    detect_platform() { echo x86; }
    detect_init_system() { INIT_SYSTEM=systemd; }
    run main "$@"
}

make_archive() { # asset name
    local pkg="$BATS_TEST_TMPDIR/pkg-$1/helixscreen"
    make_payload "$pkg" "$1"
    head -c 1100000 /dev/urandom > "$pkg/assets/pad.bin"
    tar -czf "$BATS_TEST_TMPDIR/$2" -C "$BATS_TEST_TMPDIR/pkg-$1" helixscreen
}

@test "a desktop install writes the payload and the menu entry, and nothing system-wide" {
    make_archive helixscreen-x86-desktop.zip helixscreen-x86-desktop-v1.2.3.tar.gz
    run_main --desktop --local "$BATS_TEST_TMPDIR/helixscreen-x86-desktop-v1.2.3.tar.gz"
    [ "$status" -eq 0 ] || fail "main failed ($status): $output"

    [ -x "$DESK/bin/helix-screen" ]
    desktop_root_is_ours "$DESK"
    entry="$HOME/.local/share/applications/helixscreen.desktop"
    grep -qx "Exec=$DESK/bin/helix-screen" "$entry"
    grep -qx "Icon=$DESK/assets/images/helix-icon-256.png" "$entry"
    grep -qx "Terminal=false" "$entry"
    grep -qx "Categories=Utility;" "$entry"
    [ ! -e "$HOME/.config/autostart/helixscreen.desktop" ]
    [ -f "$HOME/.local/state/helixscreen/helixscreen-install.log" ]

    [ -z "$(ls -A "$HELIX_SYSTEMD_UNIT_DIR")" ]
    [ -z "$(ls -A "$HELIX_INIT_D_DIR")" ]
    for c in sudo apt-get systemctl udevadm pkexec; do
        if called "$c"; then fail "the desktop install ran $c: $(cat "$BATS_TEST_TMPDIR/calls.log")"; fi
    done
    contains "window on this desktop (--desktop)" "$output"
    # Every step the plan counted closes, and no more.
    contains "[4/4] Added HelixScreen to the app menu" "$output"
    lacks "/4] Set up service" "$output"
    contains "--moonraker ws://PRINTER_IP:7125" "$output"
    contains "re-running this installer" "$output"
}

@test "a desktop install refuses a foreign directory at its root, before changing anything" {
    mkdir -p "$DESK/notes"
    echo keep > "$DESK/notes/mine.txt"
    make_archive helixscreen-x86-desktop.zip helixscreen-x86-desktop-v1.2.3.tar.gz
    run_main --desktop --local "$BATS_TEST_TMPDIR/helixscreen-x86-desktop-v1.2.3.tar.gz"
    [ "$status" -eq 1 ]
    contains "is not a HelixScreen desktop install" "$output"
    [ "$(cat "$DESK/notes/mine.txt")" = keep ]
    [ ! -e "$DESK.old" ]
    [ ! -e "$HOME/.local/share/applications/helixscreen.desktop" ]
}

@test "an --update with no flag finds the desktop install and keeps its settings" {
    make_archive helixscreen-x86-desktop.zip helixscreen-x86-desktop-v1.2.3.tar.gz
    run_main --desktop --local "$BATS_TEST_TMPDIR/helixscreen-x86-desktop-v1.2.3.tar.gz"
    [ "$status" -eq 0 ] || fail "install failed ($status): $output"
    printf '{"config_version":3,"printer":{"moonraker_host":"10.0.0.9"}}\n' > "$DESK/config/settings.json"
    echo v2 > "$BATS_TEST_TMPDIR/pkg-helixscreen-x86-desktop.zip/helixscreen/ui_xml/marker.txt"
    tar -czf "$BATS_TEST_TMPDIR/helixscreen-x86-desktop-v1.2.4.tar.gz" \
        -C "$BATS_TEST_TMPDIR/pkg-helixscreen-x86-desktop.zip" helixscreen
    export DISPLAY=:0

    run_main --update --local "$BATS_TEST_TMPDIR/helixscreen-x86-desktop-v1.2.4.tar.gz"
    [ "$status" -eq 0 ] || fail "update failed ($status): $output"
    contains "window on this desktop (existing install)" "$output"
    [ "$(cat "$DESK/ui_xml/marker.txt")" = v2 ]
    grep -q 10.0.0.9 "$DESK/config/settings.json"
    [ ! -e "$DESK.old" ]
}

@test "--dry-run states the desktop flavour and changes nothing" {
    make_archive helixscreen-x86-desktop.zip helixscreen-x86-desktop-v1.2.3.tar.gz
    export DISPLAY=:0
    run_main --dry-run --local "$BATS_TEST_TMPDIR/helixscreen-x86-desktop-v1.2.3.tar.gz"
    [ "$status" -eq 0 ] || fail "dry run failed ($status): $output"
    contains "Mode       window on this desktop (desktop session found)" "$output"
    contains "app menu entry" "$output"
    [ ! -e "$DESK" ]
    [ ! -e "$HOME/.local/share/applications" ]
}

@test "a kiosk archive handed to a desktop install is refused before it lands" {
    make_desktop_install
    echo keep > "$DESK/config/settings.json"
    make_archive helixscreen-x86.zip helixscreen-x86-v1.2.4.tar.gz
    run_main --desktop --local "$BATS_TEST_TMPDIR/helixscreen-x86-v1.2.4.tar.gz"
    [ "$status" -ne 0 ]
    contains "not the x86 desktop build" "$output"
    [ "$(cat "$DESK/config/settings.json")" = keep ]
    desktop_root_is_ours "$DESK"
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------

@test "--uninstall finds the desktop install with no flags and removes only it" {
    make_desktop_install
    make_kiosk_install "$BATS_TEST_TMPDIR/opt/helixscreen"
    rm -rf "$BATS_TEST_TMPDIR/opt"
    INSTALL_DIR="$DESK"
    write_desktop_entry
    mkdir -p "$HOME/.local/state/helixscreen"
    echo log > "$HOME/.local/state/helixscreen/helixscreen-install.log"
    echo mine > "$HOME/notes.txt"

    run_main --uninstall
    [ "$status" -eq 0 ] || fail "uninstall failed ($status): $output"
    [ ! -e "$DESK" ]
    [ ! -e "$HOME/.local/share/applications/helixscreen.desktop" ]
    [ ! -e "$HOME/.local/state/helixscreen" ]
    [ "$(cat "$HOME/notes.txt")" = mine ]
    for c in sudo systemctl; do
        if called "$c"; then fail "the desktop uninstall ran $c"; fi
    done
}

@test "the desktop uninstall refuses a root that is not provably ours" {
    make_payload "$DESK" helixscreen-x86.zip
    INSTALL_DIR="$DESK"
    run uninstall_desktop
    [ "$status" -eq 1 ]
    contains "Refusing to remove" "$output"
    [ -x "$DESK/bin/helix-screen" ]

    rm "$DESK/release_info.json"
    run uninstall_desktop
    [ "$status" -eq 1 ]
    [ -d "$DESK" ]
}

@test "the desktop uninstall refuses a root reached through a symlink" {
    make_payload "$BATS_TEST_TMPDIR/elsewhere/helixscreen" helixscreen-x86-desktop.zip
    mkdir -p "$HOME/.local/share"
    ln -s "$BATS_TEST_TMPDIR/elsewhere/helixscreen" "$DESK"
    run remove_desktop_root "$DESK"
    [ "$status" -eq 1 ]
    [ -x "$BATS_TEST_TMPDIR/elsewhere/helixscreen/bin/helix-screen" ]
}

@test "the desktop uninstall leaves a menu entry it did not write" {
    make_desktop_install
    mkdir -p "$HOME/.local/share/applications"
    printf '[Desktop Entry]\nExec=/usr/bin/helix-screen\n' > "$HOME/.local/share/applications/helixscreen.desktop"
    INSTALL_DIR="$DESK"
    run uninstall_desktop
    [ "$status" -eq 0 ]
    [ ! -e "$DESK" ]
    [ -f "$HOME/.local/share/applications/helixscreen.desktop" ]
}

@test "the desktop root is never on the uninstall sweep list" {
    case " $HELIX_INSTALL_DIRS " in
        *".local/share"*) fail "HELIX_INSTALL_DIRS names a home path: $HELIX_INSTALL_DIRS" ;;
    esac
}

@test "the standalone uninstaller removes a desktop install and nothing else" {
    make_desktop_install
    INSTALL_DIR="$DESK"
    write_desktop_entry
    unset INSTALL_DIR
    mock_command_script uname 'case "$1" in -m) echo x86_64 ;; *) echo 6.1.0 ;; esac'
    run sh "$UNINSTALL_BUNDLE" --force
    [ "$status" -eq 0 ] || fail "standalone uninstall failed ($status): $output"
    [ ! -e "$DESK" ]
    [ ! -e "$HOME/.local/share/applications/helixscreen.desktop" ]
    if called systemctl; then fail "the standalone desktop uninstall ran systemctl"; fi
}

@test "the standalone uninstaller takes --desktop when both flavours are installed" {
    make_desktop_install
    make_kiosk_install "$HOME/helixscreen"
    mock_command_script uname 'case "$1" in -m) echo x86_64 ;; *) echo 6.1.0 ;; esac'
    run sh "$UNINSTALL_BUNDLE" --force
    [ "$status" -eq 1 ]
    contains "--desktop or --kiosk" "$output"
    run sh "$UNINSTALL_BUNDLE" --force --desktop
    [ "$status" -eq 0 ] || fail "standalone --desktop uninstall failed ($status): $output"
    [ ! -e "$DESK" ]
    [ -x "$HOME/helixscreen/bin/helix-screen" ]
}
