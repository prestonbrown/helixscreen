#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# helix-launcher.sh's automatic-update rollback guard (#1304), end to end with
# a mock watchdog and mock app in two install trees: the new one at the
# install path and the kept previous one beside it at .old. The app arms the
# guard by writing update_rollback.armed into HELIX_ROLLBACK_DIR; a launch
# that reports healthy keeps the new tree, anything else restores the old one
# and starts it.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers
    # The watcher stops the launch's real processes, so it scans the real /proc.
    export HELIX_PROC_ROOT=/proc

    mock_command_script "systemctl" 'exit 0'
    mock_command_script "killall" 'exit 0'
    mock_command_script "setterm" 'exit 0'

    export HELIX_DRI_DIR="$BATS_TEST_TMPDIR/dri"
    mkdir -p "$HELIX_DRI_DIR"
    export HELIX_NO_SPLASH=1

    export INSTALL="$BATS_TEST_TMPDIR/opt/helixscreen"
    export PREVIOUS="${INSTALL}.old"
    export HELIX_ROLLBACK_DIR="$BATS_TEST_TMPDIR/state"
    export LAUNCHES="$BATS_TEST_TMPDIR/launches"
    export HELIX_ROLLBACK_WINDOW=3
    export HELIX_ROLLBACK_POLL=1
    mkdir -p "$HELIX_ROLLBACK_DIR"

    make_tree "$INSTALL" new
    make_tree "$PREVIOUS" old
    # The previous version runs, records itself, and exits: a clean quit.
    set_app "$PREVIOUS" 'exit 0'
}

# make_tree DIR NAME: a tree with the launcher and a watchdog that runs the
# command after "--" and returns its status.
make_tree() {
    mkdir -p "$1/bin" "$1/config"
    cp "$LAUNCHER" "$1/bin/helix-launcher.sh"
    cat > "$1/bin/helix-watchdog" <<'WD'
#!/bin/sh
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do shift; done
[ "${1:-}" = "--" ] && shift
"$@"
WD
    chmod +x "$1/bin/helix-watchdog"
    echo "$2" > "$1/TREE"
}

# set_app DIR BODY: the mock helix-screen records which tree launched, then runs BODY.
set_app() {
    cat > "$1/bin/helix-screen" <<APP
#!/bin/sh
echo "\$(cat "\$(dirname "\$0")/../TREE")" >> "$LAUNCHES"
$2
APP
    chmod +x "$1/bin/helix-screen"
}

arm() {
    printf 'install_dir=%s\nprevious_dir=%s\nfrom_version=1.0.0\nto_version=1.1.0\n' \
        "$INSTALL" "${1:-$PREVIOUS}" > "$HELIX_ROLLBACK_DIR/update_rollback.armed"
}

launch() {
    timeout 60 sh "$INSTALL/bin/helix-launcher.sh" >"$BATS_TEST_TMPDIR/launcher.log" 2>&1 || true
}

reason() {
    sed -n 's/^reason=//p' "$HELIX_ROLLBACK_DIR/update_rollback.result"
}

assert_rolled_back() {
    [ "$(cat "$INSTALL/TREE")" = "old" ]
    [ ! -e "$PREVIOUS" ]
    [ ! -e "${INSTALL}.failed" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    grep -q '^from_version=1.0.0$' "$HELIX_ROLLBACK_DIR/update_rollback.result"
    grep -q '^to_version=1.1.0$' "$HELIX_ROLLBACK_DIR/update_rollback.result"
}

@test "rollback: a new version that never reports healthy is replaced when the window ends" {
    set_app "$INSTALL" 'sleep 30'
    arm
    launch
    assert_rolled_back
    [ "$(reason)" = "timeout" ]
    [ "$(cat "$LAUNCHES")" = "$(printf 'new\nold')" ]
}

@test "rollback: a new version whose launch fails is replaced at once" {
    set_app "$INSTALL" 'exit 3'
    arm
    launch
    assert_rolled_back
    [ "$(reason)" = "exit_3" ]
    [ "$(cat "$LAUNCHES")" = "$(printf 'new\nold')" ]
}

@test "rollback: the app's crash-loop request ends the wait early" {
    set_app "$INSTALL" "echo reason=crash_loop > '$HELIX_ROLLBACK_DIR/update_rollback.request'; sleep 30"
    arm
    export HELIX_ROLLBACK_WINDOW=60
    SECONDS=0
    launch
    [ "$SECONDS" -lt 30 ]
    assert_rolled_back
    [ "$(reason)" = "crash_loop" ]
}

@test "rollback: a healthy report keeps the new version and drops the previous tree" {
    set_app "$INSTALL" "touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 3; exit 0"
    arm
    launch
    [ "$(cat "$INSTALL/TREE")" = "new" ]
    [ ! -e "$PREVIOUS" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.healthy" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.result" ]
    [ "$(cat "$LAUNCHES")" = "new" ]
}

@test "rollback: under a restarting service the previous tree is kept until the restarted start is healthy" {
    set_app "$INSTALL" "touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 30"
    arm
    export HELIX_SERVICE_RESTARTS=1 HELIX_ROLLBACK_WINDOW=60
    SECONDS=0
    launch
    [ "$SECONDS" -lt 30 ]
    # First healthy start: the launch ends for the unit refresh, nothing deleted yet.
    [ "$(cat "$INSTALL/TREE")" = "new" ]
    [ -d "$PREVIOUS" ]
    [ "$(cat "$HELIX_ROLLBACK_DIR/update_rollback.phase")" = "units" ]

    # The service restarts it: healthy again, now the previous tree goes.
    set_app "$INSTALL" "touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 2; exit 0"
    launch
    [ ! -e "$PREVIOUS" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.phase" ]
    [ "$(cat "$LAUNCHES")" = "$(printf 'new\nnew')" ]
}

@test "rollback: the unit-refresh restart waits while a job holds the machine" {
    set_app "$INSTALL" "touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy' '$HELIX_ROLLBACK_DIR/update_rollback.busy'
sleep 4; rm -f '$HELIX_ROLLBACK_DIR/update_rollback.busy'; sleep 30"
    arm
    export HELIX_SERVICE_RESTARTS=1 HELIX_ROLLBACK_WINDOW=60
    SECONDS=0
    launch
    [ "$SECONDS" -ge 4 ]
    [ "$SECONDS" -lt 30 ]
    [ "$(cat "$HELIX_ROLLBACK_DIR/update_rollback.phase")" = "units" ]
}

@test "rollback: a stop during the window keeps the new version and resumes the guard next start" {
    # What helix-watchdog does when an init script stops it mid-window.
    set_app "$INSTALL" "touch '$HELIX_ROLLBACK_DIR/update_rollback.stopping'; exit 0"
    arm
    launch
    [ "$(cat "$INSTALL/TREE")" = "new" ]
    [ -d "$PREVIOUS" ]
    [ -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.result" ]
    [ "$(cat "$LAUNCHES")" = "new" ]

    set_app "$INSTALL" "touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 2; exit 0"
    launch
    [ ! -e "$PREVIOUS" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
}

@test "rollback: a shutdown signal to the launcher during the window restores nothing" {
    set_app "$INSTALL" 'sleep 30'
    arm
    export HELIX_ROLLBACK_WINDOW=60
    sh "$INSTALL/bin/helix-launcher.sh" >"$BATS_TEST_TMPDIR/launcher.log" 2>&1 &
    local pid=$!
    sleep 3
    kill -TERM "$pid"
    wait "$pid" || true
    [ "$(cat "$INSTALL/TREE")" = "new" ]
    [ -d "$PREVIOUS" ]
    [ -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.result" ]
}

@test "rollback: a broken launcher in the new tree is rolled back too" {
    # The install swaps in a launcher that cannot start anything while this
    # one runs; a rename, as install.sh's swap is, so this shell's script stays.
    set_app "$INSTALL" "printf '#!/bin/sh\\nexit 2\\n' > '$INSTALL/bin/launcher.new'
chmod +x '$INSTALL/bin/launcher.new'; mv '$INSTALL/bin/launcher.new' '$INSTALL/bin/helix-launcher.sh'
printf 'install_dir=%s\\nprevious_dir=%s\\nfrom_version=1.0.0\\nto_version=1.1.0\\n' '$INSTALL' '$PREVIOUS' > '$HELIX_ROLLBACK_DIR/update_rollback.armed'
exit 0"
    launch
    assert_rolled_back
    [ "$(reason)" = "exit_2" ]
    [ "$(cat "$LAUNCHES")" = "$(printf 'new\nold')" ]
}

@test "rollback: an install reached through a symlink rolls back by its real path" {
    local real="$BATS_TEST_TMPDIR/real"
    mkdir -p "$real"
    mv "$INSTALL" "$PREVIOUS" "$real/"
    rmdir "$BATS_TEST_TMPDIR/opt"
    ln -s "$real" "$BATS_TEST_TMPDIR/opt"
    set_app "$INSTALL" 'exit 3'
    # The app names its install by /proc/self/exe: the resolved path.
    printf 'install_dir=%s\nprevious_dir=%s\nfrom_version=1.0.0\nto_version=1.1.0\n' \
        "$real/helixscreen" "$real/helixscreen.old" > "$HELIX_ROLLBACK_DIR/update_rollback.armed"
    launch
    [ "$(cat "$real/helixscreen/TREE")" = "old" ]
    [ -L "$BATS_TEST_TMPDIR/opt" ]
    [ ! -e "$real/helixscreen.old" ]
    [ "$(reason)" = "exit_3" ]
}

@test "rollback: an unusable marker at the install restart still relaunches the new version" {
    # Before the guard, the watchdog respawned in place here; the launcher must
    # never leave the screen dark instead.
    set_app "$INSTALL" "if [ ! -e '$BATS_TEST_TMPDIR/installed' ]; then
    touch '$BATS_TEST_TMPDIR/installed'
    printf 'install_dir=/somewhere/else\\nprevious_dir=%s\\n' '$PREVIOUS' > '$HELIX_ROLLBACK_DIR/update_rollback.armed'
fi
exit 0"
    launch
    [ "$(cat "$LAUNCHES")" = "$(printf 'new\nnew')" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    # Nothing will restore the kept copy, so it does not linger.
    [ ! -e "$PREVIOUS" ]
}

@test "rollback: the install restart hands over to a guarded launch" {
    # First launch: the app installs, arms and exits for the restart. Second:
    # the new version, now guarded, reports healthy.
    set_app "$INSTALL" "if [ ! -e '$BATS_TEST_TMPDIR/installed' ]; then
    touch '$BATS_TEST_TMPDIR/installed'
    printf 'install_dir=%s\nprevious_dir=%s\nfrom_version=1.0.0\nto_version=1.1.0\n' '$INSTALL' '$PREVIOUS' > '$HELIX_ROLLBACK_DIR/update_rollback.armed'
    exit 0
fi
touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 2; exit 0"
    launch
    [ "$(cat "$LAUNCHES")" = "$(printf 'new\nnew')" ]
    [ "$(cat "$INSTALL/TREE")" = "new" ]
    [ ! -e "$PREVIOUS" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
}

# The guarded version reports healthy and the guard finishes; later, in the
# same boot, it installs the next update automatically: keeps itself at .old,
# arms a 1.1.0 -> 1.2.0 marker and exits 0 for the restart. $1: the run that
# installs; earlier runs stay up until the watcher ends them.
second_update_app() {
    local run=$1
    set_app "$INSTALL" "n=\$(cat '$BATS_TEST_TMPDIR/runs' 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > '$BATS_TEST_TMPDIR/runs'
if [ \$n = $run ]; then
  touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 4
  cp -r '$INSTALL' '$PREVIOUS'; echo v2 > '$PREVIOUS/TREE'; echo v3 > '$INSTALL/TREE'
  printf 'install_dir=%s\\nprevious_dir=%s\\nfrom_version=1.1.0\\nto_version=1.2.0\\n' '$INSTALL' '$PREVIOUS' > '$HELIX_ROLLBACK_DIR/update_rollback.armed'
  exit 0
fi
[ \$n -lt $run ] && { touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 30; }
touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 3; exit 0"
}

@test "rollback: a second automatic update after a finished guard is guarded, not rolled back" {
    second_update_app 1
    arm
    export HELIX_ROLLBACK_WINDOW=60
    launch
    [ "$(cat "$INSTALL/TREE")" = "v3" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.result" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    [ ! -e "$PREVIOUS" ]
}

@test "rollback: a second update whose marker text repeats the first is still a new install" {
    # Same versions and paths (a reinstall): only the file is new, as the app
    # writes it through a rename.
    set_app "$INSTALL" "n=\$(cat '$BATS_TEST_TMPDIR/runs' 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > '$BATS_TEST_TMPDIR/runs'
if [ \$n = 1 ]; then
  touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 4
  cp -r '$INSTALL' '$PREVIOUS'; echo v2 > '$PREVIOUS/TREE'; echo v3 > '$INSTALL/TREE'
  printf 'install_dir=%s\\nprevious_dir=%s\\nfrom_version=1.0.0\\nto_version=1.1.0\\n' '$INSTALL' '$PREVIOUS' > '$HELIX_ROLLBACK_DIR/update_rollback.armed.tmp'
  mv '$HELIX_ROLLBACK_DIR/update_rollback.armed.tmp' '$HELIX_ROLLBACK_DIR/update_rollback.armed'
  exit 0
fi
touch '$HELIX_ROLLBACK_DIR/update_rollback.healthy'; sleep 3; exit 0"
    arm
    export HELIX_ROLLBACK_WINDOW=60
    launch
    [ "$(cat "$INSTALL/TREE")" = "v3" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.result" ]
}

@test "rollback: a second automatic update after the units phase is guarded, not rolled back" {
    # Run 1 hands off for the unit refresh; run 2 finishes the guard and installs.
    second_update_app 2
    arm
    export HELIX_SERVICE_RESTARTS=1 HELIX_ROLLBACK_WINDOW=60
    launch
    launch
    [ "$(cat "$INSTALL/TREE")" = "v3" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.result" ]
}

@test "rollback: a manual install inside the window relaunches instead of leaving the screen dark" {
    # A manual Install Update replaces the kept copy (install.sh drops a stale
    # .old) and restarts; the guard can no longer restore, and the UI must
    # come back anyway.
    set_app "$INSTALL" "n=\$(cat '$BATS_TEST_TMPDIR/runs' 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > '$BATS_TEST_TMPDIR/runs'
if [ \$n = 1 ]; then rm -rf '$PREVIOUS'; exit 0; fi
exit 0"
    arm
    export HELIX_ROLLBACK_WINDOW=60
    launch
    [ "$(cat "$LAUNCHES")" = "$(printf 'new\nnew')" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
}

@test "rollback: too many guarded starts restores without launching the new version" {
    set_app "$INSTALL" 'sleep 30'
    arm
    echo 3 > "$HELIX_ROLLBACK_DIR/update_rollback.starts"
    launch
    assert_rolled_back
    [ "$(reason)" = "too_many_starts" ]
    [ "$(cat "$LAUNCHES")" = "old" ]
}

@test "rollback: a marker naming a tree that is not the kept sibling is dropped untouched" {
    local elsewhere="$BATS_TEST_TMPDIR/opt/other"
    make_tree "$elsewhere" other
    set_app "$elsewhere" 'exit 0'
    set_app "$INSTALL" 'exit 0'
    arm "$elsewhere"
    launch
    [ "$(cat "$INSTALL/TREE")" = "new" ]
    [ "$(cat "$elsewhere/TREE")" = "other" ]
    [ -d "$PREVIOUS" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    [ "$(cat "$LAUNCHES")" = "new" ]
}

@test "rollback: a marker for another install path is dropped, and its unused kept copy removed" {
    set_app "$INSTALL" 'exit 0'
    printf 'install_dir=/somewhere/else\nprevious_dir=%s\n' "$PREVIOUS" \
        > "$HELIX_ROLLBACK_DIR/update_rollback.armed"
    launch
    [ "$(cat "$INSTALL/TREE")" = "new" ]
    [ ! -e "$PREVIOUS" ]
    [ ! -e "$HELIX_ROLLBACK_DIR/update_rollback.armed" ]
    [ "$(cat "$LAUNCHES")" = "new" ]
}

@test "rollback: no watchdog, no guard advertised" {
    rm "$INSTALL/bin/helix-watchdog"
    set_app "$INSTALL" "echo \"dir=\${HELIX_ROLLBACK_DIR:-}\" > '$BATS_TEST_TMPDIR/env'; exit 0"
    launch
    [ "$(cat "$BATS_TEST_TMPDIR/env")" = "dir=" ]
}

@test "rollback: with a watchdog the guard directory reaches the app" {
    set_app "$INSTALL" "echo \"dir=\${HELIX_ROLLBACK_DIR:-}\" > '$BATS_TEST_TMPDIR/env'; exit 0"
    launch
    [ "$(cat "$BATS_TEST_TMPDIR/env")" = "dir=$HELIX_ROLLBACK_DIR" ]
}

@test "rollback: each install gets its own guard directory beside it" {
    unset HELIX_ROLLBACK_DIR
    set_app "$INSTALL" "echo \"dir=\${HELIX_ROLLBACK_DIR:-}\" > '$BATS_TEST_TMPDIR/env'; exit 0"
    launch
    [ "$(cat "$BATS_TEST_TMPDIR/env")" = "dir=$BATS_TEST_TMPDIR/opt/.helixscreen/rollback-helixscreen" ]
}
