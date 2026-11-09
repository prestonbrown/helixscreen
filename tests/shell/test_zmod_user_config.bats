#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# On Z-Mod (AD5X, AD5M) the install root /srv/helixscreen is the path Moonraker's
# type:web entry rmtree()s, and there is no printer_data helixscreen/ directory to
# link user files into. restore-user-config.sh keeps them in the state root beside
# the payload instead and links config/<file> to them, the same per-file links
# the printer_data layouts use, so an update unlinks links and leaves the files.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
RESTORE="$WORKTREE_ROOT/config/restore-user-config.sh"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers
    umask 022

    INSTALL_DIR="$BATS_TEST_TMPDIR/srv/helixscreen"
    STATE="$BATS_TEST_TMPDIR/srv/helixscreen-state/config"
    mkdir -p "$INSTALL_DIR/config"

    export HELIX_ZMOD_PAYLOAD_ROOT="$INSTALL_DIR"
    export HELIX_ZMOD_USER_CONFIG_DIR="$STATE"

    HELIX_RESTORE_LIB_ONLY=1 . "$RESTORE"
    IDIR="$INSTALL_DIR"
    # No printer_data on Z-Mod: the scan finds nothing.
    discover_pd_helix() { :; }
}

# What Moonraker leaves behind: a fresh tree holding only what the zip ships.
simulate_web_update() {
    rm -rf "$INSTALL_DIR"
    mkdir -p "$INSTALL_DIR/config"
    echo '{"config_version":0}' > "$INSTALL_DIR/config/settings.json"
    echo 'HELIX_LOG_LEVEL=packaged' > "$INSTALL_DIR/config/helixscreen.env"
    echo '{}' > "$INSTALL_DIR/release_info.json"
}

write_user_files() {
    echo '{"config_version":33}' > "$INSTALL_DIR/config/settings.json"
    echo '{"spools":1}' > "$INSTALL_DIR/config/tool_spools.json"
    echo '[1]' > "$INSTALL_DIR/config/crash_history.json"
    echo '{"uuid":"u-1","salt":"s-1"}' > "$INSTALL_DIR/config/telemetry_device.json"
}

# Every entry under DIR with its mtime and content hash, so a rerun that
# rewrites a file shows up even when the bytes are the same.
snapshot() {
    (cd "$1" && find . -mindepth 1 | LC_ALL=C sort | while IFS= read -r p; do
        if [ -L "$p" ]; then
            printf '%s -> %s\n' "$p" "$(readlink "$p")"
        else
            printf '%s %s %s\n' "$p" "$(stat -c %Y "$p")" "$(cksum < "$p" 2>/dev/null)"
        fi
    done)
}

@test "user files move to the state dir once and a second run is a no-op" {
    write_user_files

    run restore_config_symlinks
    [ "$status" -eq 0 ]
    contains "moved tool_spools.json" "$output"

    for f in settings.json tool_spools.json crash_history.json telemetry_device.json; do
        [ -L "$INSTALL_DIR/config/$f" ] || fail "$f is not a link"
        [ "$(readlink "$INSTALL_DIR/config/$f")" = "$STATE/$f" ] || fail "$f links elsewhere"
        [ -f "$STATE/$f" ] && [ ! -L "$STATE/$f" ] || fail "$f is not a real file in the state dir"
    done
    [ "$(cat "$STATE/tool_spools.json")" = '{"spools":1}' ]

    before_state=$(snapshot "$STATE")
    before_install=$(snapshot "$INSTALL_DIR/config")
    run restore_config_symlinks
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "second run acted: $output"
    [ "$(snapshot "$STATE")" = "$before_state" ]
    [ "$(snapshot "$INSTALL_DIR/config")" = "$before_install" ]
}

@test "every move is logged with a timestamp" {
    write_user_files
    run restore_config_symlinks
    [ "$status" -eq 0 ]
    printf '%s\n' "$output" | grep -Eq \
        '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} restore-user-config: moved telemetry_device.json to ' \
        || fail "no timestamped move line in: $output"
}

@test "a newer file already in the state dir is never overwritten" {
    mkdir -p "$STATE"
    echo '{"spools":"newer"}' > "$STATE/tool_spools.json"
    echo '{"spools":"older"}' > "$INSTALL_DIR/config/tool_spools.json"
    touch -d '2026-01-01 00:00:00' "$INSTALL_DIR/config/tool_spools.json"
    echo '{}' > "$INSTALL_DIR/release_info.json"
    touch -d '2026-06-01 00:00:00' "$INSTALL_DIR/release_info.json"

    run restore_config_symlinks
    [ "$status" -eq 0 ]
    [ "$(cat "$STATE/tool_spools.json")" = '{"spools":"newer"}' ]
    [ -L "$INSTALL_DIR/config/tool_spools.json" ]
}

@test "an install-dir copy written after the release is kept beside the state copy" {
    mkdir -p "$STATE"
    echo '{"spools":"state"}' > "$STATE/tool_spools.json"
    echo '{}' > "$INSTALL_DIR/release_info.json"
    touch -d '2026-01-01 00:00:00' "$INSTALL_DIR/release_info.json"
    echo '{"spools":"install"}' > "$INSTALL_DIR/config/tool_spools.json"

    run restore_config_symlinks
    [ "$status" -eq 0 ]
    [ "$(cat "$STATE/tool_spools.json")" = '{"spools":"state"}' ]
    [ "$(cat "$STATE/tool_spools.json.pre-restore")" = '{"spools":"install"}' ]
}

@test "the device id survives a simulated rmtree of the install root" {
    write_user_files
    restore_config_symlinks >/dev/null

    simulate_web_update
    [ ! -e "$INSTALL_DIR/config/telemetry_device.json" ]

    run restore_config_symlinks
    [ "$status" -eq 0 ]
    [ -L "$INSTALL_DIR/config/telemetry_device.json" ]
    [ "$(cat "$INSTALL_DIR/config/telemetry_device.json")" = '{"uuid":"u-1","salt":"s-1"}' ]
    # The packaged settings.json does not replace the user's.
    [ "$(cat "$INSTALL_DIR/config/settings.json")" = '{"config_version":33}' ]
    [ "$(cat "$INSTALL_DIR/config/tool_spools.json")" = '{"spools":1}' ]
}

@test "a move interrupted after the copy completes on the next run without loss" {
    mkdir -p "$STATE"
    echo '[1]' > "$INSTALL_DIR/config/crash_history.json"
    cp -p "$INSTALL_DIR/config/crash_history.json" "$STATE/crash_history.json"

    run restore_config_symlinks
    [ "$status" -eq 0 ]
    [ -L "$INSTALL_DIR/config/crash_history.json" ]
    [ "$(cat "$STATE/crash_history.json")" = '[1]' ]
}

@test "a printer_data layout keeps using printer_data and never creates the state dir" {
    PD="$BATS_TEST_TMPDIR/printer_data/config/helixscreen"
    mkdir -p "$PD"
    discover_pd_helix() { printf '%s' "$PD"; }
    write_user_files

    run restore_config_symlinks
    [ "$status" -eq 0 ]
    [ ! -e "$STATE" ]
    # Unchanged rule there: only runtime-only files move on a service start.
    [ -L "$INSTALL_DIR/config/telemetry_device.json" ]
    [ "$(readlink "$INSTALL_DIR/config/telemetry_device.json")" = "$PD/telemetry_device.json" ]
    [ -f "$INSTALL_DIR/config/tool_spools.json" ] && [ ! -L "$INSTALL_DIR/config/tool_spools.json" ]
}

@test "an install root other than Z-Mod's gets no state dir" {
    export HELIX_ZMOD_PAYLOAD_ROOT="$BATS_TEST_TMPDIR/elsewhere"
    write_user_files

    run restore_config_symlinks
    [ "$status" -eq 0 ]
    [ ! -e "$STATE" ]
    [ ! -L "$INSTALL_DIR/config/tool_spools.json" ]
}

@test "launcher: relinks the Z-Mod user files before it reads helixscreen.env" {
    mock_command_script "systemctl" 'exit 0'
    mock_command_script "killall" 'exit 0'
    mock_command_script "setterm" 'exit 0'
    export HELIX_DRI_DIR="$BATS_TEST_TMPDIR/dri"
    mkdir -p "$HELIX_DRI_DIR" "$STATE"

    simulate_web_update
    mkdir -p "$INSTALL_DIR/bin"
    cp "$LAUNCHER" "$INSTALL_DIR/bin/helix-launcher.sh"
    cp "$RESTORE" "$INSTALL_DIR/config/restore-user-config.sh"
    cat > "$INSTALL_DIR/bin/helix-screen" <<'EOF'
#!/bin/sh
[ "$1" = "--probe-egl" ] && exit 1
echo "${HELIX_LOG_LEVEL:-<unset>}" > "$(dirname "$0")/../seen_log_level"
exit 0
EOF
    printf '#!/bin/sh\nexit 0\n' > "$INSTALL_DIR/bin/helix-splash"
    chmod +x "$INSTALL_DIR/bin/helix-screen" "$INSTALL_DIR/bin/helix-splash"
    echo 'HELIX_LOG_LEVEL=debug' > "$STATE/helixscreen.env"
    echo '{"uuid":"u-1","salt":"s-1"}' > "$STATE/telemetry_device.json"
    unset HELIX_LOG_LEVEL HELIX_DEBUG

    sh "$INSTALL_DIR/bin/helix-launcher.sh" >/dev/null 2>&1 || true

    [ -L "$INSTALL_DIR/config/telemetry_device.json" ]
    [ -L "$INSTALL_DIR/config/helixscreen.env" ]
    [ "$(cat "$INSTALL_DIR/seen_log_level")" = "debug" ]
}
