#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installer and Moonraker-update refresh gating for the QIDI .3mf
# plate-thumbnail helper (prestonbrown/helixscreen#1713). Both paths run the
# same shipped script, config/qidi-3mf-thumbs-units.sh, whose capability gate
# is generate_thumb_path in Moonraker's file_manager metadata.py; uninstall
# removes the units again.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    # The payload the installer templates from: the real units + helper.
    cp "$WORKTREE_ROOT/config/helixscreen-3mf-thumbs.path" "$INSTALL_DIR/config/"
    cp "$WORKTREE_ROOT/config/helixscreen-3mf-thumbs.service" "$INSTALL_DIR/config/"
    cp "$WORKTREE_ROOT/config/qidi_3mf_thumbs.py" "$INSTALL_DIR/config/"

    export MOCK_ROOT="$BATS_TEST_TMPDIR/qidi"
    export ETC_SYSTEMD="$MOCK_ROOT/etc/systemd/system"
    export GCODES_DIR="$MOCK_ROOT/home/qidi/printer_data/gcodes"
    mkdir -p "$ETC_SYSTEMD" "$GCODES_DIR"
    # The shipped gate+install script, with /etc redirected into the fixture.
    # It sits at its real payload location so its own IDIR discovery works.
    sed -e "s|/etc/systemd/system|$ETC_SYSTEMD|g" \
        "$WORKTREE_ROOT/config/qidi-3mf-thumbs-units.sh" \
        > "$INSTALL_DIR/config/qidi-3mf-thumbs-units.sh"
    chmod +x "$INSTALL_DIR/config/qidi-3mf-thumbs-units.sh"

    export DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"
    export TMP_DIR="$BATS_TEST_TMPDIR/stage"
    export SYSTEMD_LOG="$BATS_TEST_TMPDIR/systemctl.log"
    : > "$SYSTEMD_LOG"
    mock_command_script systemctl 'echo "$*" >> "$SYSTEMD_LOG"; exit 0'

    INIT_SYSTEM="systemd"
    SUDO=""
    KLIPPER_USER="qidi"
    KLIPPER_GROUP="qidi"
    KLIPPER_HOME="$MOCK_ROOT/home/qidi"
    # Keep the moonraker source scan inside the fixture: the metadata probe
    # checks KLIPPER_HOME/moonraker first, so the fixture lives there.
    MOONRAKER_SRC_PATHS=""
    export MOONRAKER_SRC_PATHS

    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/systemd/system|$ETC_SYSTEMD|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh" > "$patched"
    unset _HELIX_COMPETING_UIS_SOURCED
    # shellcheck disable=SC1090
    . "$patched"
}

# A Moonraker tree whose file_manager/metadata.py contains $1's contents.
write_moonraker_metadata() {
    local meta="$MOCK_ROOT/home/qidi/moonraker/moonraker/components/file_manager/metadata.py"
    mkdir -p "$(dirname "$meta")"
    printf '%s\n' "$1" > "$meta"
}

@test "qidi thumbs: installs the units when moonraker hardcodes the thumb path" {
    write_moonraker_metadata 'def generate_thumb_path(input_filepath, root_path, plant_index):
    return ".thumbs/plate_1.png"'

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "thumbnail helper units"

    [ -f "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
    [ -f "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]

    # Every placeholder templated, real paths substituted in.
    ! grep -q '@@' "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path"
    ! grep -q '@@' "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service"
    grep -qF "PathChanged=$GCODES_DIR" "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path"
    grep -qF "User=qidi" "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service"
    grep -qF "Group=qidi" "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service"
    grep -qF "config/qidi_3mf_thumbs.py $GCODES_DIR" "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service"

    # Enabled (boot backfill) and watching now.
    grep -q "enable helixscreen-3mf-thumbs.path" "$SYSTEMD_LOG"
    grep -q "start helixscreen-3mf-thumbs.path" "$SYSTEMD_LOG"
    grep -q "start helixscreen-3mf-thumbs.service" "$SYSTEMD_LOG"
}

@test "qidi thumbs: unchanged units are not rewritten, only enablement is repaired" {
    write_moonraker_metadata "def generate_thumb_path(): pass"

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    local first
    first=$(cat "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service")

    : > "$SYSTEMD_LOG"
    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    [ "$(cat "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service")" = "$first" ]
    # Cheap idempotent state repair runs on the no-change path too...
    grep -q "enable helixscreen-3mf-thumbs.service" "$SYSTEMD_LOG"
    grep -q "enable helixscreen-3mf-thumbs.path" "$SYSTEMD_LOG"
    grep -q "start helixscreen-3mf-thumbs.path" "$SYSTEMD_LOG"
    # ...but nothing heavier: no reload, no backfill re-run.
    ! grep -q "daemon-reload" "$SYSTEMD_LOG"
    ! grep -q "start helixscreen-3mf-thumbs.service" "$SYSTEMD_LOG"
}

@test "qidi thumbs: skips when metadata.py lacks generate_thumb_path" {
    write_moonraker_metadata "def extract_3mf(zf, filename): pass"

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "extracts .3mf thumbnails itself"
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]
}

@test "qidi thumbs: skips when no moonraker metadata.py is found" {
    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "no Moonraker metadata.py"
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
}

@test "qidi thumbs: skips when the gcodes directory is missing" {
    write_moonraker_metadata "def generate_thumb_path(): pass"
    rm -rf "$GCODES_DIR"

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "gcodes directory not found"
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
}

@test "qidi thumbs: never falls back to running as root" {
    write_moonraker_metadata "def generate_thumb_path(): pass"
    KLIPPER_USER="root"
    KLIPPER_GROUP="root"

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "non-root service user"
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]
}

@test "qidi thumbs: uninstall stops, disables and removes the units" {
    local patched="$BATS_TEST_TMPDIR/uninstall.sh"
    sed -e "s|/etc/systemd/system|$ETC_SYSTEMD|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" > "$patched"
    unset _HELIX_UNINSTALL_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    cat > "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" <<'EOF'
[Unit]
Description=watch
EOF
    cat > "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" <<'EOF'
[Unit]
Description=extract
EOF

    run uninstall_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]
    grep -q "stop helixscreen-3mf-thumbs.path" "$SYSTEMD_LOG"
    grep -q "disable helixscreen-3mf-thumbs.path" "$SYSTEMD_LOG"
}

@test "qidi thumbs: uninstall is a no-op when nothing is installed" {
    local patched="$BATS_TEST_TMPDIR/uninstall.sh"
    sed -e "s|/etc/systemd/system|$ETC_SYSTEMD|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" > "$patched"
    unset _HELIX_UNINSTALL_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    run uninstall_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
}

@test "qidi thumbs: uninstall() in the module wires the reversal in" {
    # Defined once plus called at least once: a refactor that drops the call
    # site leaves the units behind on every uninstall.
    [ "$(grep -c "uninstall_qidi_3mf_thumbs" "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh")" -ge 2 ]
}

@test "qidi thumbs: main install flow wires the step in" {
    grep -q "install_qidi_3mf_thumbs" "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

# The Moonraker web-update refresh: an in-app update runs install.sh under
# NoNewPrivileges (no sudo), so this root-privileged path is what installs the
# units on a machine updating from a release that lacked them.
setup_refresh_fixture() {
    sed -e "s|/etc/systemd/system|$ETC_SYSTEMD|g" \
        "$WORKTREE_ROOT/config/refresh-service-units.sh" \
        > "$INSTALL_DIR/config/refresh-service-units.sh"
    # A stable install dir so the refresh's settle-wait returns immediately.
    mkdir -p "$INSTALL_DIR/bin"
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/helix-screen"
    printf '{}' > "$INSTALL_DIR/release_info.json"
    # The identity source the refresh reads before overwriting the service:
    # User= there is the Klipper user on these single-user installs.
    cat > "$ETC_SYSTEMD/helixscreen.service" <<EOF
[Service]
User=qidi
Group=qidi
EOF
}

@test "qidi thumbs: update refresh installs the units on a host that had none" {
    setup_refresh_fixture
    write_moonraker_metadata "def generate_thumb_path(): pass"
    # The refresh learns the home the way it does in production would be via
    # passwd; the fixture's home is not in the real passwd, so name it.
    export HELIX_QIDI_HOME="$MOCK_ROOT/home/qidi"

    run sh "$INSTALL_DIR/config/refresh-service-units.sh"
    [ "$status" -eq 0 ]

    [ -f "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
    [ -f "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]
    grep -qF "PathChanged=$GCODES_DIR" "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path"
    grep -qF "User=qidi" "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service"
    grep -q "enable helixscreen-3mf-thumbs.path" "$SYSTEMD_LOG"
}

@test "qidi thumbs: update refresh does nothing when the gate fails" {
    setup_refresh_fixture
    write_moonraker_metadata "def extract_3mf(zf, filename): pass"
    export HELIX_QIDI_HOME="$MOCK_ROOT/home/qidi"

    run sh "$INSTALL_DIR/config/refresh-service-units.sh"
    [ "$status" -eq 0 ]

    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]
    echo "$output" | grep -q "generate_thumb_path"
}
