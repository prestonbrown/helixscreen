#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A K1 that loses SSH has to leave a record of when it went missing: the
# install log says whether dropbear was up at each step of an install, and
# hooks-k1.sh appends a timestamped line to a log under /usr/data at every
# launch and stop. Both read dropbear's pids and whether port 22 listens.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export MOCK_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$MOCK_ROOT/usr/data" "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/sbin"
    export DROPBEAR_FLAG="$BATS_TEST_TMPDIR/dropbear.running"

    # dropbear is "running" while the flag file exists; port 22 listens with it.
    mock_command_script pidof '[ "$1" = dropbear ] && [ -f "$DROPBEAR_FLAG" ] && { echo 4242; exit 0; }; exit 1'
    mock_command_script netstat '[ -f "$DROPBEAR_FLAG" ] && echo "tcp 0 0 0.0.0.0:22 0.0.0.0:* LISTEN"; exit 0'

    HOOK="$BATS_TEST_TMPDIR/hooks-k1.sh"
    sed -e "s|/usr/data/|$MOCK_ROOT/usr/data/|g" \
        -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/usr/sbin/dropbear|$MOCK_ROOT/usr/sbin/dropbear|g" \
        -e "s|/usr/bin/dropbear|$MOCK_ROOT/usr/bin/dropbear|g" \
        -e "s| /sbin/dropbear| $MOCK_ROOT/sbin/dropbear|g" \
        "$WORKTREE_ROOT/assets/config/platform/hooks-k1.sh" > "$HOOK"
    SSH_LOG="$MOCK_ROOT/usr/data/helixscreen-state/logs/ssh.log"
}

# An init script whose start brings dropbear up.
write_dropbear_init() {
    printf '#!/bin/sh\n[ "$1" = start ] && touch "%s"\nexit 0\n' "$DROPBEAR_FLAG" \
        > "$MOCK_ROOT/etc/init.d/S50dropbear"
    chmod +x "$MOCK_ROOT/etc/init.d/S50dropbear"
}

run_hook() { # function
    run sh -c '. "$1"; "$2"' _ "$HOOK" "$1"
}

@test "k1 ssh log: a launch with dropbear up records its pid and port 22" {
    touch "$DROPBEAR_FLAG"
    run_hook _ensure_ssh_running
    [ "$status" -eq 0 ]
    run cat "$SSH_LOG"
    [ "${#lines[@]}" -eq 1 ]
    [[ "${lines[0]}" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}\ [0-9:]{8}\ launch:\ dropbear=4242\ port22=yes$ ]] || fail "$output"
}

@test "k1 ssh log: a launch that finds SSH down records it, then how it came back" {
    write_dropbear_init
    run_hook _ensure_ssh_running
    [ "$status" -eq 0 ]
    run cat "$SSH_LOG"
    [ "${#lines[@]}" -eq 2 ]
    [[ "${lines[0]}" == *"launch: dropbear=none port22=no" ]] || fail "$output"
    [[ "${lines[1]}" == *"launch: started via $MOCK_ROOT/etc/init.d/S50dropbear: dropbear=4242 port22=yes" ]] || fail "$output"
}

@test "k1 ssh log: a launch with no init script and no binary says SSH stays down" {
    run_hook _ensure_ssh_running
    [ "$status" -eq 0 ]
    run cat "$SSH_LOG"
    [[ "${lines[1]}" == *"launch: no dropbear binary found, SSH stays down" ]] || fail "$output"
}

@test "k1 ssh log: a stop records SSH state" {
    touch "$DROPBEAR_FLAG"
    run_hook platform_post_stop
    [ "$status" -eq 0 ]
    run cat "$SSH_LOG"
    [[ "${lines[0]}" == *"stop: dropbear=4242 port22=yes" ]] || fail "$output"
}

@test "k1 ssh log: an oversized log keeps its newest half and the new line" {
    mkdir -p "$(dirname "$SSH_LOG")"
    { i=0; while [ $i -lt 2000 ]; do echo "old line $i padding padding padding"; i=$((i+1)); done; } > "$SSH_LOG"
    [ "$(wc -c < "$SSH_LOG")" -gt 65536 ]
    run_hook platform_post_stop
    [ "$(wc -c < "$SSH_LOG")" -le 65536 ]
    run tail -n 2 "$SSH_LOG"
    [[ "${lines[0]}" == "old line 1999 padding padding padding" ]] || fail "$output"
    [[ "${lines[1]}" == *"stop: dropbear=none port22=no" ]] || fail "$output"
}

@test "k1 ssh log: the installer writes SSH state into the install log" {
    unset _HELIX_COMMON_SOURCED _HELIX_COMPETING_UIS_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh"
    INSTALL_LOG="$BATS_TEST_TMPDIR/install.log"
    : > "$INSTALL_LOG"
    log_k1_ssh_state "install start"
    touch "$DROPBEAR_FLAG"
    log_k1_ssh_state "after ensure"
    run cat "$INSTALL_LOG"
    [[ "${lines[0]}" == *"INFO SSH state (install start): dropbear=none port22=no" ]] || fail "$output"
    [[ "${lines[1]}" == *"INFO SSH state (after ensure): dropbear=4242 port22=yes" ]] || fail "$output"
}
