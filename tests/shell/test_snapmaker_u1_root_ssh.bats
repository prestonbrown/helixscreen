#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for ensure_root_ssh in hooks-snapmaker-u1.sh.
#
# On stock Snapmaker U1 firmware, /etc/init.d/S50dropbear exits without starting
# dropbear on a retail build unless it is invoked as `start --force`, and the
# only caller of `--force` is the stock UI (/usr/bin/gui), which does so at
# startup when its Settings > Maintenance > Root Access toggle has stored
# "open_root": 1 under "system" in gui_config.json. HelixScreen disables
# /usr/bin/gui, so the hook carries that duty. PAXX Extended Firmware comments
# the --force gate out of S50dropbear and decides SSH from its own config, so
# the hook leaves it alone there.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-snapmaker-u1.sh"

setup() {
    load helpers

    export HELIX_GUI_CONFIG="$BATS_TEST_TMPDIR/gui_config.json"
    export HELIX_DROPBEAR_INIT="$BATS_TEST_TMPDIR/S50dropbear"
    export DROPBEAR_LOG="$BATS_TEST_TMPDIR/dropbear_calls.log"

    mkdir -p "$BATS_TEST_TMPDIR/bin"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    mock_pidof_dropbear ""
}

# Stub pidof: prints PIDS for `pidof dropbear` (empty = not running).
mock_pidof_dropbear() {
    local pids="$1"
    cat > "$BATS_TEST_TMPDIR/bin/pidof" <<EOF
#!/bin/sh
if [ "\$1" = dropbear ] && [ -n "$pids" ]; then
    echo "$pids"
    exit 0
fi
exit 1
EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/pidof"
}

# Stock S50dropbear: the retail gate that only `--force` bypasses. Records
# every invocation's arguments instead of starting anything.
seed_stock_dropbear() {
    cat > "$HELIX_DROPBEAR_INIT" <<EOF
#!/bin/sh
echo "\$*" >> "$DROPBEAR_LOG"
if [ "\$2" != "--force" ] ; then
	[ x"\$(custom_misc vertype 2>/dev/null)" = x"dbg" ] || exit 0
fi
exit 0
EOF
    chmod +x "$HELIX_DROPBEAR_INIT"
}

# PAXX S50dropbear: the --force gate commented out, SSH decided by
# extended2.cfg's `ssh:` key inside start().
seed_paxx_dropbear() {
    cat > "$HELIX_DROPBEAR_INIT" <<EOF
#!/bin/sh
echo "\$*" >> "$DROPBEAR_LOG"
# if [ "\$2" != "--force" ] ; then
# 	[ x"\$(custom_misc vertype 2>/dev/null)" = x"dbg" ] || exit 0
# fi
exit 0
EOF
    chmod +x "$HELIX_DROPBEAR_INIT"
}

# gui_config.json as the stock UI writes it (cJSON_Print, tab-indented).
seed_gui_config() {
    local open_root="$1"
    printf '{\n\t"system":\t{\n\t\t"guide_pass":\t1,\n\t\t"language":\t"en",\n\t\t"wifi_opened":\t1,\n\t\t"open_root":\t%s,\n\t\t"top_cover_guide":\t1\n\t},\n\t"calibration":\t{\n\t\t"offset_pass":\t1\n\t}\n}\n' \
        "$open_root" > "$HELIX_GUI_CONFIG"
}

run_ensure_root_ssh() {
    ( . "$HOOK"; ensure_root_ssh )
}

@test "u1 root ssh: Root Access on, dropbear down -> S50dropbear start --force" {
    seed_stock_dropbear
    seed_gui_config 1

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]

    [ -f "$DROPBEAR_LOG" ]
    [ "$(cat "$DROPBEAR_LOG")" = "start --force" ]
}

@test "u1 root ssh: compact JSON and a boolean true are honoured too" {
    seed_stock_dropbear
    printf '{"system":{"guide_pass":1,"open_root":true}}' > "$HELIX_GUI_CONFIG"

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ "$(cat "$DROPBEAR_LOG")" = "start --force" ]

    rm -f "$DROPBEAR_LOG"
    printf '{ "system" : { "open_root" : 1 } }' > "$HELIX_GUI_CONFIG"
    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ "$(cat "$DROPBEAR_LOG")" = "start --force" ]
}

@test "u1 root ssh: Root Access off (0) -> S50dropbear not invoked" {
    seed_stock_dropbear
    seed_gui_config 0

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ ! -e "$DROPBEAR_LOG" ]
}

@test "u1 root ssh: false, or open_root absent -> S50dropbear not invoked" {
    seed_stock_dropbear
    printf '{"system":{"open_root":false}}' > "$HELIX_GUI_CONFIG"
    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ ! -e "$DROPBEAR_LOG" ]

    # A key that merely ends in open_root is not the toggle.
    printf '{"system":{"guide_pass":1,"close_open_root":1}}' > "$HELIX_GUI_CONFIG"
    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ ! -e "$DROPBEAR_LOG" ]
}

@test "u1 root ssh: no gui_config.json -> not invoked, no error" {
    seed_stock_dropbear
    [ ! -e "$HELIX_GUI_CONFIG" ]

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ ! -e "$DROPBEAR_LOG" ]
}

@test "u1 root ssh: dropbear already running -> not invoked" {
    seed_stock_dropbear
    seed_gui_config 1
    mock_pidof_dropbear 1234

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ ! -e "$DROPBEAR_LOG" ]
}

@test "u1 root ssh: PAXX S50dropbear (no --force gate) -> not invoked, ssh: setting rules" {
    seed_paxx_dropbear
    seed_gui_config 1

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
    [ ! -e "$DROPBEAR_LOG" ]
}

@test "u1 root ssh: no S50dropbear -> no error" {
    seed_gui_config 1
    [ ! -e "$HELIX_DROPBEAR_INIT" ]

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
}

@test "u1 root ssh: a failing S50dropbear does not fail the hook" {
    seed_gui_config 1
    cat > "$HELIX_DROPBEAR_INIT" <<'EOF'
#!/bin/sh
if [ "$2" != "--force" ] ; then
	[ x"$(custom_misc vertype 2>/dev/null)" = x"dbg" ] || exit 0
fi
exit 1
EOF
    chmod +x "$HELIX_DROPBEAR_INIT"

    run run_ensure_root_ssh
    [ "$status" -eq 0 ]
}

@test "u1 root ssh: platform_pre_start runs ensure_root_ssh" {
    seed_stock_dropbear
    seed_gui_config 1

    # Neutralize the other pre_start duties so only the SSH step is observed.
    run bash -c '
        . "'"$HOOK"'"
        ensure_lmd_running() { :; }
        ensure_wifi_associated() { :; }
        start_remote_screen() { :; }
        platform_pre_start
    '
    [ "$status" -eq 0 ]
    [ "$(cat "$DROPBEAR_LOG")" = "start --force" ]
}
