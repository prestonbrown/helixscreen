#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installer gating for the QIDI .3mf plate-thumbnail helper
# (prestonbrown/helixscreen#1713): the helper units are installed only on a
# QIDI-class host whose Moonraker fork hardcodes the .thumbs path
# (generate_thumb_path in file_manager/metadata.py), and uninstall removes
# them again.

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

be_qidi() { _is_qidi_class_sbc() { return 0; }; }
not_qidi() { _is_qidi_class_sbc() { return 1; }; }

@test "qidi thumbs: installs the units when qidi-class and moonraker hardcodes the thumb path" {
    be_qidi
    write_moonraker_metadata 'def generate_thumb_path(input_filepath, root_path, plant_index):
    return ".thumbs/plate_1.png"'

    log_info() { echo "INFO: $*"; }
    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "thumbnail helper"

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

@test "qidi thumbs: install is idempotent (second run touches no systemd state)" {
    be_qidi
    write_moonraker_metadata "def generate_thumb_path(): pass"

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    local first
    first=$(cat "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service")

    # systemctl failing loudly proves the second run never reaches it.
    mock_command_script systemctl 'echo "systemctl must not run again" >&2; exit 1'
    : > "$SYSTEMD_LOG"
    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    [ -z "$(cat "$SYSTEMD_LOG")" ]
    [ "$(cat "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service")" = "$first" ]
}

@test "qidi thumbs: skips when metadata.py lacks generate_thumb_path" {
    be_qidi
    write_moonraker_metadata "def extract_3mf(zf, filename): pass"

    log_info() { echo "INFO: $*"; }
    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    echo "$output" | grep -qi "skipping.*thumbnail"
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]
}

@test "qidi thumbs: skips when no moonraker metadata.py is found" {
    be_qidi

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
}

@test "qidi thumbs: skips on a non-qidi host" {
    not_qidi
    write_moonraker_metadata "def generate_thumb_path(): pass"

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.service" ]
}

@test "qidi thumbs: skips when the gcodes directory is missing" {
    be_qidi
    write_moonraker_metadata "def generate_thumb_path(): pass"
    rm -rf "$GCODES_DIR"

    log_warn() { echo "WARN: $*"; }
    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
    [ ! -e "$ETC_SYSTEMD/helixscreen-3mf-thumbs.path" ]
}

@test "qidi thumbs: never falls back to running as root" {
    be_qidi
    write_moonraker_metadata "def generate_thumb_path(): pass"
    KLIPPER_USER="root"
    KLIPPER_GROUP="root"

    run install_qidi_3mf_thumbs
    [ "$status" -eq 0 ]
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
