#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A UI restart that overlaps an exiting launcher. Z-Mod restarts HelixScreen on
# every Klipper start by killing helix-screen by name and running its own
# S80helixscreen while the old launcher is still in its exit trap. Three rules
# keep the new instance alive:
#   - a launcher's exit sweep stops only the processes it started;
#   - a launcher waits, bounded, for an older launcher to exit;
#   - the Z-Mod platform hooks wait for an exiting launcher, but not for a
#     healthy one.
#
# Every process here is real (fake binaries are sh scripts) so the launcher's
# /proc reads run for real. Each test hands the launcher its own proc root:
# real /proc where only the marker can match, or a directory of symlinks to the
# PIDs the test owns, so launchers from parallel tests never show up.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"
HOOKS_DIR="$WORKTREE_ROOT/assets/config/platform"

setup() {
    load helpers

    # Records any by-name sweep instead of performing it: a real killall here
    # would reach other sessions' helix-screen processes.
    mock_command_script "killall" "echo \"\$*\" >> \"$BATS_TEST_TMPDIR/killall.log\""
    mock_command_script "systemctl" 'exit 0'
    mock_command_script "setterm" 'exit 0'

    # No DRM card, so no helix-screen --probe-egl lands in the screens log,
    # whatever /dev/dri this host has.
    export HELIX_DRI_DIR="$BATS_TEST_TMPDIR/dri"
    mkdir -p "$HELIX_DRI_DIR"

    MI="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$MI/bin" "$MI/config"
    cp "$LAUNCHER" "$MI/bin/helix-launcher.sh"
    FAKE_PROC="$BATS_TEST_TMPDIR/fakeproc"
    mkdir -p "$FAKE_PROC"
    OWNED_PIDS=""
}

teardown() {
    # A launcher defers its TERM until its foreground child exits, so the
    # residents go first. Each recorded its PID as it started, so this holds
    # even when the test failed before reading the PIDs back.
    if [ -f "$BATS_TEST_TMPDIR/residents" ]; then
        while read -r p; do
            kill "$p" 2>/dev/null || true
        done < "$BATS_TEST_TMPDIR/residents"
    fi
    for p in $OWNED_PIDS; do
        kill "$p" 2>/dev/null || true
    done
}

# A fake binary that records "<HELIX_LAUNCHER_PID> <own pid>" in $2 and then
# stays up until signalled. Short sleeps, so a TERM leaves no long orphan.
install_resident() {
    cat > "$1" <<EOF
#!/bin/sh
echo "\$\$" >> "$BATS_TEST_TMPDIR/residents"
echo "\${HELIX_LAUNCHER_PID:-none} \$\$" >> "$2"
trap 'exit 0' TERM
while :; do sleep 0.2; done
EOF
    chmod +x "$1"
}

# A process whose argv names helix-launcher.sh (an exiting launcher, as the
# scan sees it) that lives for $1 tenths of a second. Prints its PID.
start_fake_launcher() {
    mkdir -p "$BATS_TEST_TMPDIR/old/bin"
    cat > "$BATS_TEST_TMPDIR/old/bin/helix-launcher.sh" <<'EOF'
#!/bin/sh
i=0
while [ "$i" -lt "$1" ]; do sleep 0.1; i=$((i + 1)); done
EOF
    sh "$BATS_TEST_TMPDIR/old/bin/helix-launcher.sh" "$1" >/dev/null 2>&1 3>&- &
    local pid=$! i=0
    # Until the fork execs, its argv is still this shell's.
    while [ "$i" -lt 50 ]; do
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q 'helix-launcher.sh' && break
        sleep 0.05
        i=$((i + 1))
    done
    echo "$pid"
}

# Expose one real PID in the fake proc root.
expose() {
    ln -s "/proc/$1" "$FAKE_PROC/$1"
}

# Wait until file $1 has at least $2 lines. The cap is generous because a
# launcher's start under a loaded machine can take seconds.
wait_lines() {
    local i=0
    while [ "$i" -lt 200 ]; do
        [ -f "$1" ] && [ "$(wc -l < "$1")" -ge "$2" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    fail "timed out waiting for $2 lines in $1"
}

wait_gone() {
    local i=0
    while [ "$i" -lt 30 ]; do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

@test "a successor started while the old launcher exits survives its cleanup" {
    install_resident "$MI/bin/helix-screen" "$BATS_TEST_TMPDIR/screens"

    HELIX_PROC_ROOT=/proc HELIX_LAUNCHER_PREDECESSOR_WAIT=0 \
        sh "$MI/bin/helix-launcher.sh" > "$BATS_TEST_TMPDIR/l1.log" 2>&1 3>&- &
    l1=$!
    OWNED_PIDS="$OWNED_PIDS $l1"
    wait_lines "$BATS_TEST_TMPDIR/screens" 1

    HELIX_PROC_ROOT=/proc HELIX_LAUNCHER_PREDECESSOR_WAIT=0 \
        sh "$MI/bin/helix-launcher.sh" > "$BATS_TEST_TMPDIR/l2.log" 2>&1 3>&- &
    l2=$!
    OWNED_PIDS="$OWNED_PIDS $l2"
    wait_lines "$BATS_TEST_TMPDIR/screens" 2

    h1=$(awk -v l="$l1" '$1 == l {print $2}' "$BATS_TEST_TMPDIR/screens")
    h2=$(awk -v l="$l2" '$1 == l {print $2}' "$BATS_TEST_TMPDIR/screens")
    [ -n "$h1" ] && [ -n "$h2" ]
    OWNED_PIDS="$OWNED_PIDS $h1 $h2"

    # Z-Mod's killall reaches helix-screen; the old launcher then exits and
    # runs its trap with the successor already up.
    kill "$h1"
    wait "$l1" || true

    kill -0 "$h2" || fail "old launcher's cleanup killed the successor's helix-screen"
    if grep -q 'helix-' "$BATS_TEST_TMPDIR/killall.log" 2>/dev/null; then
        fail "cleanup swept by name: $(cat "$BATS_TEST_TMPDIR/killall.log")"
    fi
}

@test "cleanup stops a child of its own left behind by its watchdog" {
    install_resident "$MI/bin/helix-splash" "$BATS_TEST_TMPDIR/splashes"
    # helix-screen leaves a splash running and exits, the shape of an orphan
    # whose supervisor died.
    cat > "$MI/bin/helix-screen" <<EOF
#!/bin/sh
"$MI/bin/helix-splash" >/dev/null 2>&1 &
exit 0
EOF
    chmod +x "$MI/bin/helix-screen"

    HELIX_PROC_ROOT=/proc HELIX_LAUNCHER_PREDECESSOR_WAIT=0 HELIX_NO_SPLASH=1 \
        sh "$MI/bin/helix-launcher.sh" > "$BATS_TEST_TMPDIR/l1.log" 2>&1 3>&- || true
    wait_lines "$BATS_TEST_TMPDIR/splashes" 1
    splash=$(awk '{print $2}' "$BATS_TEST_TMPDIR/splashes")
    OWNED_PIDS="$OWNED_PIDS $splash"

    wait_gone "$splash" || fail "launcher left its own helix-splash (PID $splash) running"
}

@test "launcher waits for an exiting predecessor before starting anything" {
    cat > "$MI/bin/helix-screen" <<EOF
#!/bin/sh
if kill -0 "\$OLD_PID" 2>/dev/null; then echo alive; else echo gone; fi > "$BATS_TEST_TMPDIR/at_start"
EOF
    chmod +x "$MI/bin/helix-screen"

    old=$(start_fake_launcher 15)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"

    OLD_PID="$old" HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=10 \
        run sh "$MI/bin/helix-launcher.sh"

    contains "Waiting up to 10s for exiting launcher (PID $old)" "$output"
    contains "Previous launcher exited after" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/at_start")" = "gone" ]
}

@test "launcher gives up on a predecessor after the bound and starts" {
    cat > "$MI/bin/helix-screen" <<EOF
#!/bin/sh
if kill -0 "\$OLD_PID" 2>/dev/null; then echo alive; else echo gone; fi > "$BATS_TEST_TMPDIR/at_start"
EOF
    chmod +x "$MI/bin/helix-screen"

    old=$(start_fake_launcher 600)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"

    start=$(date +%s)
    OLD_PID="$old" HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=2 \
        run sh "$MI/bin/helix-launcher.sh"
    elapsed=$(( $(date +%s) - start ))

    contains "Launcher (PID $old) still running after 2s; proceeding" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/at_start")" = "alive" ]
    [ "$elapsed" -lt 10 ]
}

@test "a launcher running the query is not its own predecessor" {
    # A launcher-named parent asks the question, as the launcher does when it
    # runs a platform hook that calls --wait-for-predecessor.
    mkdir -p "$BATS_TEST_TMPDIR/parent/bin"
    cat > "$BATS_TEST_TMPDIR/parent/bin/helix-launcher.sh" <<EOF
#!/bin/sh
ln -s "/proc/\$\$" "$FAKE_PROC/\$\$"
HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=5 \
    sh "$MI/bin/helix-launcher.sh" --wait-for-predecessor
EOF
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"

    start=$(date +%s)
    run sh "$BATS_TEST_TMPDIR/parent/bin/helix-launcher.sh"
    elapsed=$(( $(date +%s) - start ))

    [ "$status" -eq 0 ]
    [[ "$output" != *"Waiting"* ]] || fail "query waited on its own parent: $output"
    [ "$elapsed" -lt 3 ]
}

# Runs platform_pre_start from hook file $1 with the side effects neutralized.
run_hook_pre_start() {
    run sh -c '
        . "$1"
        mkdir() { :; }
        touch() { :; }
        LAUNCHER="$2"
        platform_pre_start
        echo "waited=${HELIX_PREDECESSOR_WAITED:-}"
    ' sh "$1" "$MI/bin/helix-launcher.sh"
}

@test "zmod hooks do not wait when a healthy instance is up" {
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"
    install_resident "$BATS_TEST_TMPDIR/helix-watchdog" "$BATS_TEST_TMPDIR/watchdogs"
    sh "$BATS_TEST_TMPDIR/helix-watchdog" >/dev/null 2>&1 3>&- &
    wd=$!
    OWNED_PIDS="$OWNED_PIDS $wd"
    old=$(start_fake_launcher 600)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"
    expose "$wd"

    for hook in hooks-ad5x.sh hooks-ad5m-zmod.sh; do
        start=$(date +%s)
        HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=5 \
            run_hook_pre_start "$HOOKS_DIR/$hook"
        elapsed=$(( $(date +%s) - start ))
        [ "$status" -eq 0 ]
        [[ "$output" != *"Waiting"* ]] || fail "$hook waited on a healthy instance: $output"
        [ "$elapsed" -lt 3 ] || fail "$hook took ${elapsed}s with a healthy instance up"
    done
}

@test "zmod hooks wait for an exiting launcher with no UI under it" {
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"

    for hook in hooks-ad5x.sh hooks-ad5m-zmod.sh; do
        old=$(start_fake_launcher 15)
        OWNED_PIDS="$OWNED_PIDS $old"
        expose "$old"
        HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=10 \
            run_hook_pre_start "$HOOKS_DIR/$hook"
        [ "$status" -eq 0 ]
        contains "Waiting up to 10s for exiting launcher (PID $old)" "$output"
        contains "waited=1" "$output"
        kill -0 "$old" 2>/dev/null && fail "$hook returned while launcher $old was alive"
        rm -f "$FAKE_PROC/$old"
    done
}

@test "the query stops waiting when the UI comes back" {
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"
    old=$(start_fake_launcher 600)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"
    install_resident "$BATS_TEST_TMPDIR/helix-watchdog" "$BATS_TEST_TMPDIR/watchdogs"
    # The predecessor's watchdog respawns a second into the wait.
    ( sleep 1; sh "$BATS_TEST_TMPDIR/helix-watchdog" >/dev/null 2>&1 & echo $! > "$BATS_TEST_TMPDIR/wd.pid"
      sleep 0.2; ln -s "/proc/$(cat "$BATS_TEST_TMPDIR/wd.pid")" "$FAKE_PROC/$(cat "$BATS_TEST_TMPDIR/wd.pid")" ) 3>&- &

    start=$(date +%s)
    HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=8 \
        run sh "$MI/bin/helix-launcher.sh" --wait-for-predecessor
    elapsed=$(( $(date +%s) - start ))
    OWNED_PIDS="$OWNED_PIDS $(cat "$BATS_TEST_TMPDIR/wd.pid")"

    contains "UI came back after" "$output"
    [ "$elapsed" -lt 6 ] || fail "query waited ${elapsed}s after the UI came back"
}

@test "a launcher started after a completed query does not wait again, and does not pass it on" {
    cat > "$MI/bin/helix-screen" <<EOF
#!/bin/sh
echo "waited=\${HELIX_PREDECESSOR_WAITED:-}" > "$BATS_TEST_TMPDIR/child_env"
EOF
    chmod +x "$MI/bin/helix-screen"
    old=$(start_fake_launcher 600)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"

    start=$(date +%s)
    HELIX_PREDECESSOR_WAITED=1 HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=5 \
        run sh "$MI/bin/helix-launcher.sh"
    elapsed=$(( $(date +%s) - start ))

    [[ "$output" != *"Waiting"* ]] || fail "launcher waited again: $output"
    [ "$elapsed" -lt 3 ]
    [ "$(cat "$BATS_TEST_TMPDIR/child_env")" = "waited=" ]
}

# Fake one launcher-shaped process under $FAKE_PROC with start time $2.
fake_launcher_proc() {
    mkdir -p "$FAKE_PROC/$1"
    echo sh > "$FAKE_PROC/$1/comm"
    printf 'sh\0/x/bin/helix-launcher.sh\0' > "$FAKE_PROC/$1/cmdline"
    printf '%s (sh) S 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %s 0 0\n' "$1" "$2" > "$FAKE_PROC/$1/stat"
}

@test "a start time that overflows or does not parse is not a predecessor" {
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"
    fake_launcher_proc 999999990 99999999999999999999
    fake_launcher_proc 999999991 garbage
    # Control: an older start time is a predecessor, which proves the
    # launcher read its own start time.
    fake_launcher_proc 999999992 1
    # exec keeps the PID, so the launcher finds itself in the proc root.
    cat > "$BATS_TEST_TMPDIR/wrap.sh" <<EOF
ln -s "/proc/\$\$" "$FAKE_PROC/\$\$"
exec sh "$MI/bin/helix-launcher.sh"
EOF

    HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=1 \
        run sh "$BATS_TEST_TMPDIR/wrap.sh"

    contains "for exiting launcher (PID 999999992)" "$output"
}

@test "start-time comparison is exact past 2^31 and refuses non-numbers" {
    awk '/^helix_uint_ge\(\) \{/ {f = 1} f {print} f && /^}/ {exit}' "$LAUNCHER" > "$BATS_TEST_TMPDIR/fn.sh"
    run sh -c '
        . "$1"
        r() { rc=0; helix_uint_ge "$2" "$3" || rc=$?; [ "$rc" -eq "$1" ] || echo "BAD $2 $3 -> $rc"; }
        r 0 3000000000 2999999999
        r 1 2999999999 3000000000
        r 0 3000000000 3000000000
        r 0 99999999999999999999 10000000000000000000
        r 1 10000000000000000000 99999999999999999999
        r 0 100000000000000000001 99999999999999999999
        r 1 5 12
        r 2 garbage 1
        r 2 1 ""
        echo end
    ' sh "$BATS_TEST_TMPDIR/fn.sh"
    [ "$output" = "end" ] || fail "$output"
}

@test "init start drops an inherited launcher tag and marks the wait as done" {
    local init="$WORKTREE_ROOT/config/helixscreen.init"
    T="$BATS_TEST_TMPDIR/init"
    mkdir -p "$T/bin"
    awk '/^start\(\) \{/ {f = 1} f {print} f && /^}/ {exit}' "$init" \
        | sed "s|/var/run|$T|g" > "$T/start.sh"
    printf '#!/bin/sh\necho "marker=${HELIX_LAUNCHER_PID:-}" > "%s/splash_env"\n' "$T" > "$T/bin/helix-splash"
    printf '#!/bin/sh\necho "$* waited=${HELIX_PREDECESSOR_WAITED:-}" >> "%s/launcher_calls"\n[ "$1" = --wait-for-predecessor ] || sleep 2\n' "$T" > "$T/bin/helix-launcher.sh"
    : > "$T/bin/helix-screen"
    chmod +x "$T/bin/"*

    run env HELIX_LAUNCHER_PID=4242 T="$T" sh -c '
        LOGFILE="$T/log" DESC=x NAME=helixscreen DAEMON_DIR="$T" PIDFILE="$T/pid"
        DAEMON="$T/bin/helix-screen" SPLASH="$T/bin/helix-splash" LAUNCHER="$T/bin/helix-launcher.sh"
        for f in platform_pre_start platform_stop_competing_uis platform_enable_backlight \
            platform_wait_for_services platform_wait_for_boot_complete stop_splash; do
            eval "$f() { :; }"
        done
        pidof() { return 1; }
        . "$T/start.sh"
        start
    '
    OWNED_PIDS="$OWNED_PIDS $(cat "$T/pid" 2>/dev/null)"
    wait_lines "$T/launcher_calls" 2

    [ "$(cat "$T/splash_env")" = "marker=" ]
    [ "$(sed -n 1p "$T/launcher_calls")" = "--wait-for-predecessor waited=" ]
    [ "$(sed -n 2p "$T/launcher_calls")" = " waited=1" ]
}
