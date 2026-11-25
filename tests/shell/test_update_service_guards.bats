#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the ExecStart script of config/helixscreen-update.service the way
# systemd would: continuation lines joined, `$$` unescaped, placeholders
# substituted. Each guard must skip with exit 0, so the unit reads as
# succeeded, and must not refresh the units or restart anything.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
UNIT="$WORKTREE_ROOT/config/helixscreen-update.service"

setup() {
    load helpers

    PARENT="$BATS_TEST_TMPDIR/opt"
    IDIR="$PARENT/helixscreen"
    CALLS="$BATS_TEST_TMPDIR/calls"
    # The legacy sentinel's real path is host-wide: a C++ test or a real update
    # on this host would otherwise decide what these cases see.
    LEGACY_SENTINEL="$BATS_TEST_TMPDIR/helixscreen_self_restart"
    mkdir -p "$IDIR/config" "$PARENT/.helixscreen"
    : > "$CALLS"

    printf '#!/bin/sh\necho refresh >> "%s"\n' "$CALLS" > "$IDIR/config/refresh-service-units.sh"
    chmod +x "$IDIR/config/refresh-service-units.sh"
    mock_command_script "systemctl" "echo \"systemctl \$*\" >> \"$CALLS\""
    mock_command_script "sleep" "echo \"sleep \$*\" >> \"$CALLS\""
    UPTIME=1000
}

# The ExecStart script as the shell will run it. An absolute systemctl path
# names the host binary and passes every PATH mock, so it is reduced to the
# bare name the mock answers.
_unit_script() {
    local script
    script=$(awk '
        /^ExecStart=/ { on = 1; sub(/^ExecStart=/, "") }
        on {
            cont = sub(/\\$/, "")
            line = line $0
            if (!cont) { print line; exit }
        }
    ' "$UNIT")
    script=${script#"/bin/sh -c '"}
    script=${script%"'"}
    script=$(printf '%s' "$script" | sed -e 's/\$\$/$/g' \
        -e "s|@@INSTALL_PARENT@@|$PARENT|g" -e "s|@@INSTALL_DIR@@|$IDIR|g" \
        -e "s|/tmp/helixscreen_self_restart|$LEGACY_SENTINEL|g" \
        -e 's#^/\(usr/\)\{0,1\}s\{0,1\}bin/systemctl#systemctl#' \
        -e 's#\([[:space:];&|(]\)/\(usr/\)\{0,1\}s\{0,1\}bin/systemctl#\1systemctl#g')
    printf '%s\n' "$script"
}

# Uptime is read through `cut`, so the stub answers with $UPTIME.
_run_unit() {
    mock_command_script "cut" "echo $UPTIME"
    local script
    script=$(_unit_script)
    run sh -c "$script"
}

@test "the harness never runs systemctl by an absolute path" {
    UNIT="$BATS_TEST_TMPDIR/abs.service"
    printf '[Service]\nExecStart=/bin/systemctl restart helixscreen\n' > "$UNIT"
    [ "$(_unit_script)" = "systemctl restart helixscreen" ]
    printf '[Service]\nExecStart=/usr/sbin/systemctl restart helixscreen\n' > "$UNIT"
    [ "$(_unit_script)" = "systemctl restart helixscreen" ]
    printf '[Service]\nExecStart=true; /bin/systemctl stop x\n' > "$UNIT"
    [ "$(_unit_script)" = "true; systemctl stop x" ]
    # Only a stock system directory is rewritten; any other path is kept whole.
    printf '[Service]\nExecStart=/opt/x/bin/systemctl restart helixscreen\n' > "$UNIT"
    [ "$(_unit_script)" = "/opt/x/bin/systemctl restart helixscreen" ]
}

@test "the update unit keeps no ExecStartPre or ExecCondition guards" {
    refute_grep '^ExecStartPre=' "$UNIT"
    refute_grep '^ExecCondition=' "$UNIT"
}

@test "an update past the boot grace refreshes units and restarts both units" {
    _run_unit
    [ "$status" -eq 0 ]
    [ "$(cat "$CALLS")" = "sleep 10
refresh
systemctl restart helixscreen
systemctl restart helixscreen-update.path" ]
}

@test "within the boot grace the unit skips with exit 0 and restarts nothing" {
    UPTIME=120
    _run_unit
    [ "$status" -eq 0 ]
    contains "Skipping: system booted 120s ago" "$output"
    [ ! -s "$CALLS" ]
}

@test "a self-update sentinel skips with exit 0 after the extraction wait" {
    touch "$PARENT/.helixscreen/self_restart_sentinel"
    _run_unit
    [ "$status" -eq 0 ]
    contains "Skipping: self-update restart handled by watchdog" "$output"
    [ "$(cat "$CALLS")" = "sleep 10" ]
}

@test "the harness never reads the host's legacy sentinel" {
    grep -q '/tmp/helixscreen_self_restart' "$UNIT"
    refute_grep '/tmp/helixscreen_self_restart' <(_unit_script)
}

@test "a legacy /tmp sentinel skips with exit 0 after the extraction wait" {
    touch "$LEGACY_SENTINEL"
    _run_unit
    [ "$status" -eq 0 ]
    contains "Skipping: self-update restart handled by watchdog ($LEGACY_SENTINEL)" "$output"
    [ "$(cat "$CALLS")" = "sleep 10" ]
}

@test "an uninstall in progress skips with exit 0 and restarts nothing" {
    touch "$PARENT/.helixscreen/.uninstalling"
    _run_unit
    [ "$status" -eq 0 ]
    contains "Skipping: uninstall in progress" "$output"
    [ "$(cat "$CALLS")" = "sleep 10" ]
}

@test "a failed unit refresh fails the unit without restarting helixscreen" {
    printf '#!/bin/sh\nexit 3\n' > "$IDIR/config/refresh-service-units.sh"
    _run_unit
    [ "$status" -eq 3 ]
    refute_grep 'systemctl' "$CALLS"
}
