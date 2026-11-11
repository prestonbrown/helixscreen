#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Proves the host sandbox in setup_suite.bash is active and catches an escape
# across each boundary a test can cross.
#
# Every escape here is deliberate, so each one redirects the ledger to a private
# file. A blocked call recorded in the suite ledger fails the run, and these
# tests must not do that.

bats_require_minimum_version 1.5.0

SANDBOX_COMMANDS_DOC="killall pkill pidof reboot shutdown halt poweroff telinit launchctl crontab mount umount diskutil mkfs addr2line systemctl jobpool"

setup() {
    load helpers
    PRIVATE_LEDGER="$BATS_TEST_TMPDIR/ledger.tsv"
    : > "$PRIVATE_LEDGER"
}

# Run a deliberate escape without touching the suite ledger.
escape() {
    HELIX_TEST_SANDBOX_LEDGER="$PRIVATE_LEDGER" "$@"
}

@test "sandbox: the suite installed a shim directory and a ledger" {
    [ -n "$HELIX_TEST_SANDBOX_BIN" ]
    [ -d "$HELIX_TEST_SANDBOX_BIN" ]
    [ -n "$HELIX_TEST_SANDBOX_LEDGER" ]
    case ":$PATH:" in
        *":$HELIX_TEST_SANDBOX_BIN:"*) ;;
        *) fail "sandbox bin is not on PATH: $PATH" ;;
    esac
}

@test "sandbox: makes the suite runs bypass the host's build pool" {
    [ "${JOBPOOL:-}" = 0 ]
}

@test "sandbox: every documented command has a shim on PATH" {
    local cmd
    for cmd in $SANDBOX_COMMANDS_DOC; do
        [ -x "$HELIX_TEST_SANDBOX_BIN/$cmd" ] || fail "no shim for $cmd"
    done
}

@test "sandbox: the shim list in setup_suite matches this test's copy" {
    local declared
    declared=$(grep '^HELIX_SANDBOX_COMMANDS=' "$BATS_TEST_DIRNAME/setup_suite.bash" \
        | cut -d'"' -f2)
    [ "$declared" = "$SANDBOX_COMMANDS_DOC" ] \
        || fail "setup_suite lists [$declared], this test lists [$SANDBOX_COMMANDS_DOC]"
}

@test "sandbox: a suite-run make is GNU make 4.2+ whenever gmake exists" {
    # macOS ships GNU make 3.81 as /usr/bin/make (Homebrew's 4.x installs as
    # gmake), and the repo Makefile hard-errors below 4.2, so install tests
    # that run make would fail on the environment alone. setup_suite forwards
    # make to gmake on such a host; a Linux make (or the jobpool shim, which
    # execs a modern make) parses as 4.2+ and takes the passthrough.
    command -v gmake >/dev/null 2>&1 || skip "no gmake on this host"
    local ver major minor
    ver="$(make --version 2>/dev/null | head -1)"
    major="$(printf '%s\n' "$ver" | sed -n 's/^GNU Make \([0-9]*\)\..*$/\1/p')"
    minor="$(printf '%s\n' "$ver" | sed -n 's/^GNU Make [0-9]*\.\([0-9]*\).*$/\1/p')"
    [ -n "$major" ] || fail "make --version unreadable: $ver"
    if [ "$major" -eq 4 ]; then
        [ "${minor:-0}" -ge 2 ] || fail "suite make is $ver; the gmake forwarder did not engage"
    elif [ "$major" -lt 4 ]; then
        fail "suite make is $ver; the gmake forwarder did not engage"
    fi
}

@test "sandbox: a test's own make mock wins over the gmake forwarder" {
    # The forwarder also installs a bash function, and a function outranks
    # PATH; several tests script make through a mock, so the forwarder must
    # hand control back to it.
    command -v gmake >/dev/null 2>&1 || skip "no gmake on this host"
    mock_command_script make 'echo MOCK-MAKE "$@"'
    run make install PLATFORM_TARGET=pi
    [ "$output" = "MOCK-MAKE install PLATFORM_TARGET=pi" ]
}

# --- make stays out of the tree under test ----------------------------------

@test "sandbox: the suite sees none of an enclosing make's state" {
    local v
    for v in MAKEFLAGS MFLAGS GNUMAKEFLAGS MAKELEVEL MAKEOVERRIDES BATS_REQUIRED BATS_BIN \
        ABI_STAMPED_FOR HELIX_PATCHES_FROM_CLEAN HELIX_FROM_CLEAN_SENTINEL HELIX_TESTS_NO_STALE_CHECK; do
        [ -z "${!v+set}" ] || fail "$v reached the suite: ${!v}"
    done
}

# Each shape here reaches the tree on purpose, so each test carries the
# lint's reviewed-exception marker.
@test "sandbox: a make in the tree under test is blocked and recorded" {
    # REAL_TREE_MAKE_OK: deliberate escapes, each recorded in a private ledger
    # Started from outside the tree, so only the directory make is given counts.
    cd "$BATS_TEST_TMPDIR" || return 1
    local root="$HELIX_TEST_REPO_ROOT" n=0 shape
    run escape bash -c 'cd "$1" && make -n help' _ "$root"
    [ "$status" -ne 0 ]
    contains "blocked make in the tree under test" "$output"
    n=1
    for shape in "-C $root" "-C$root" "--directory=$root/mk" "--directory $root" \
        "--dir=$root" "--di $root" "-sC $root" "-sC$root" "-ksC $root" "-f Makefile -C $root"; do
        # shellcheck disable=SC2086  # each shape is its own word list
        run escape make $shape -n help
        [ "$status" -ne 0 ] || fail "make $shape -n help was not blocked"
        n=$((n + 1))
    done
    run escape sh -c 'cd /tmp && make -C"$1" -n help' _ "$root"
    [ "$status" -ne 0 ]
    n=$((n + 1))
    [ "$(grep -c 'make .*-n help' "$PRIVATE_LEDGER")" -eq "$n" ]
}

@test "sandbox: an option argument that only looks like -C is not a directory" {
    # REAL_TREE_MAKE_OK: the -f argument names a file in the tree, the run is not there
    run escape make -C "$(scratch_tree)" --no-print-directory -f "$HELIX_TEST_REPO_ROOT/Makefile" -W -C print-var-LVGL_DIR
    [ "$status" -eq 0 ]
    [ ! -s "$PRIVATE_LEDGER" ]
}

@test "sandbox: gmake in the tree under test is blocked too" {
    # REAL_TREE_MAKE_OK: deliberate escape, recorded in a private ledger
    # Started from outside the tree, so only the directory make is given counts.
    cd "$BATS_TEST_TMPDIR" || return 1
    [ -x "$HELIX_TEST_SANDBOX_BIN/gmake" ] || skip "no gmake on this host"
    run escape gmake -C "$HELIX_TEST_REPO_ROOT" -n help
    [ "$status" -ne 0 ]
    run escape sh -c 'gmake -C "$1" -n help' _ "$HELIX_TEST_REPO_ROOT"
    [ "$status" -ne 0 ]
    [ "$(grep -c 'gmake .*-n help' "$PRIVATE_LEDGER")" -eq 2 ]
}

@test "sandbox: a make in the tree survives a callee that REPLACES PATH" {
    # REAL_TREE_MAKE_OK: deliberate escapes, recorded in a private ledger
    # Started from outside the tree, so only the directory make is given counts.
    cd "$BATS_TEST_TMPDIR" || return 1
    run escape env PATH="/usr/bin:/bin" bash -c 'cd "$1" && make -n help' _ "$HELIX_TEST_REPO_ROOT"
    [ "$status" -ne 0 ]
    # path_without_jobpool keeps the guard while it drops the jobpool blocker.
    run escape env PATH="$(path_without_jobpool)" make -C "$HELIX_TEST_REPO_ROOT" -n help
    [ "$status" -ne 0 ]
    PATH="$(path_without_jobpool)" run type -P jobpool
    [ "$status" -ne 0 ]
    [ "$(grep -c 'make .*-n help' "$PRIVATE_LEDGER")" -eq 2 ]
}

@test "sandbox: a make in scratch_tree, or asking make about itself, runs" {
    # REAL_TREE_MAKE_OK: make --version in the tree is allowed, and asserted here
    run --separate-stderr escape make -C "$(scratch_tree)" --no-print-directory print-var-LVGL_DIR
    echo "stdout: [$output]"
    echo "stderr: [$stderr]"
    [ "$status" -eq 0 ]
    # Parsing the Makefile can talk on either stream before the recipe runs:
    # $(shell) diagnostics on stderr, and $(info) notes such as the clang++ ->
    # g++ fallback on stdout. The value is the last line of stdout.
    [ "${lines[${#lines[@]}-1]}" = lib/lvgl ]
    run escape bash -c 'cd "$HELIX_TEST_REPO_ROOT" && make --version'
    [ "$status" -eq 0 ]
    contains "GNU Make" "$output"
    [ ! -s "$PRIVATE_LEDGER" ]
}

@test "sandbox: scratch_tree has a build/ of its own and reads the rest from the tree" {
    local tree
    tree="$(scratch_tree)"
    [ -d "$tree/build" ] && [ ! -L "$tree/build" ]
    [ ! -e "$tree/.git" ]
    [ "$(readlink "$tree/Makefile")" = "$HELIX_TEST_REPO_ROOT/Makefile" ]
    if [ -e "$HELIX_TEST_REPO_ROOT/.fonts.stamp" ]; then
        [ -f "$tree/.fonts.stamp" ] && [ ! -L "$tree/.fonts.stamp" ]
        [ "$(stat_mtime "$tree/.fonts.stamp")" = "$(stat_mtime "$HELIX_TEST_REPO_ROOT/.fonts.stamp")" ]
    fi
}

@test "sandbox: tests racing for scratch_tree all get the whole tree" {
    local i
    # A file directory of its own, so the tree is built here, under the race.
    BATS_FILE_TMPDIR="$BATS_TEST_TMPDIR/race-file"
    mkdir -p "$BATS_FILE_TMPDIR"
    for i in 1 2 3 4 5 6 7 8; do
        (t="$(scratch_tree)" && [ -e "$t/Makefile" ] && [ -d "$t/build" ] && echo ok) \
            > "$BATS_TEST_TMPDIR/race.$i" 2>&1 &
    done
    wait
    [ "$(cat "$BATS_TEST_TMPDIR"/race.* | grep -c '^ok$')" -eq 8 ]
    [ "$(find "$BATS_FILE_TMPDIR" -maxdepth 1 -name 'scratch-tree.*' | wc -l)" -eq 1 ]
}

# The guard is a PATH shim plus a bash function, so `env PATH=<no sandbox>
# make` run from a directory named by a variable holding the tree escapes both.
# This catches that shape in the source. A reviewed exception writes
# `# REAL_TREE_MAKE_OK: <reason>` inside the @test that needs it, which exempts
# that test's body and nothing else: the exemption ends at the body's closing
# `}`, so a helper defined after it is still checked. The lint is
# real_tree_make_offenders in scripts/code_lint.sh; qc_test_lint runs it over
# this directory (lint_bats_real_tree_make), and the cases below pin it.
@test "sandbox: the lint's exemption ends with the marked test's body" {
    local f="$BATS_TEST_TMPDIR/synthetic.bats"
    # Split words keep the fixture's lines from matching the lint over this file.
    local mk='ma''ke' ok='REAL_TREE_MAKE''_OK:'
    printf '%s\n' \
        '@test "marked" {' \
        "    # $ok reviewed" \
        "    (cd \"\$REPO\" && $mk -n help)" \
        '}' \
        '' \
        'helper_after() {' \
        "    (cd \"\$REPO\" && $mk -n help)" \
        '}' \
        '' \
        'semicolon() {' \
        "    cd \"\$ROOT\"; $mk -n help" \
        '}' > "$f"
    . "$BATS_TEST_DIRNAME/../../scripts/code_lint.sh"
    run real_tree_make_offenders "$f"
    lacks "synthetic.bats:3:" "$output"
    contains "synthetic.bats:7:" "$output"
    contains "synthetic.bats:11:" "$output"

    # The lint reads every .bats file and helpers.bash under tests/shell.
    mkdir -p "$BATS_TEST_TMPDIR/root/tests/shell"
    cp "$f" "$BATS_TEST_TMPDIR/root/tests/shell/"
    : > "$BATS_TEST_TMPDIR/root/tests/shell/helpers.bash"
    cd "$BATS_TEST_TMPDIR/root" || return 1
    run lint_bats_real_tree_make
    [ "$status" -eq 1 ]
    contains "tests/shell/synthetic.bats:7:" "$output"
}

@test "sandbox: the lint's exemption reaches neither the next @test nor the next file" {
    . "$BATS_TEST_DIRNAME/../../scripts/code_lint.sh"
    local mk='ma''ke' ok='REAL_TREE_MAKE''_OK:'
    # A marker outside any function, then a @test: the @test is not exempt.
    printf '%s\n' \
        "# $ok a note above the test" \
        '@test "unmarked" {' \
        "    (cd \"\$REPO\" && $mk -n help)" \
        '}' > "$BATS_TEST_TMPDIR/a.bats"
    # A marker still open at the end of a file, then a second file.
    printf '%s\n' "# $ok left open" > "$BATS_TEST_TMPDIR/b.bats"
    printf '%s\n' "(cd \"\$ROOT\" && $mk -n help)" > "$BATS_TEST_TMPDIR/c.bats"
    run real_tree_make_offenders "$BATS_TEST_TMPDIR/a.bats" "$BATS_TEST_TMPDIR/b.bats" "$BATS_TEST_TMPDIR/c.bats"
    contains "a.bats:3:" "$output"
    contains "c.bats:1:" "$output"
}

# --- the deliberate escapes -------------------------------------------------

@test "sandbox: a bare killall is blocked and recorded" {
    run escape killall helix-screen
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: an escape from a non-bash callee is blocked by the PATH shim" {
    # sh does not import bash's exported functions, so only PATH covers this.
    cat > "$BATS_TEST_TMPDIR/escape.sh" <<'ESC'
killall helix-screen
ESC
    run escape sh "$BATS_TEST_TMPDIR/escape.sh"
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: an escape survives a callee that REPLACES PATH" {
    # The exported function layer is what holds here: the shim directory is gone
    # from PATH, which is what a script hardening PATH with the stock system
    # directories first also does.
    run escape env PATH="/usr/bin:/bin" bash -c 'killall helix-screen'
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: pidof is blocked, so name-based process lookup finds nothing" {
    run escape pidof helix-screen
    [ "$status" -ne 0 ]
    grep -q 'pidof helix-screen' "$PRIVATE_LEDGER"
}

# --- the sandbox must not get in a legitimate test's way --------------------

@test "sandbox: a test's own mock wins and is never recorded" {
    mock_command_script "killall" 'exit 0'
    run escape killall helix-screen
    [ "$status" -eq 0 ]
    [ ! -s "$PRIVATE_LEDGER" ]
}

@test "sandbox: kill on a pid the test owns is not intercepted" {
    sleep 30 &
    local pid=$!
    run kill "$pid"
    [ "$status" -eq 0 ]
    wait "$pid" 2>/dev/null || true
    [ ! -s "$PRIVATE_LEDGER" ]
}

# --- the residual gap, pinned shut ------------------------------------------

@test "sandbox: nothing lands in the gap between the two layers" {
    # The PATH layer is lost at a callee that REPLACES PATH instead of
    # prepending to it. The function layer is lost where bash functions do not
    # travel: a non-bash callee, or a scrubbed environment. A line that does
    # both at once is the one shape neither layer covers.
    #
    # A reviewed exception writes the marker below inside the @test that needs
    # it, which exempts that test and nothing else.
    local offenders
    offenders=$(awk '
        FNR == 1 || /^@test/ { reviewed = 0 }
        /sandbox-gap-reviewed/ { reviewed = 1 }
        /(^|[^A-Za-z_])PATH=/ {
            if ($0 ~ /^[[:space:]]*#/) next
            if ($0 ~ /\$\{?PATH|HELIX_TEST_SANDBOX_BIN/) next
            if ($0 !~ /env -i|(^|[^-A-Za-z_\/])(sh|dash|busybox)[[:space:]]/) next
            if (reviewed) next
            printf "%s:%d:%s\n", FILENAME, FNR, $0
        }
    ' "$BATS_TEST_DIRNAME"/*.bats "$BATS_TEST_DIRNAME/helpers.bash")
    [ -z "$offenders" ] || fail "PATH replaced for a callee that also loses the function layer:
$offenders"
}

@test "sandbox: systemctl daemon-reload is blocked without helpers, so polkit is never asked" {
    # Both PATHs leave out helpers.bash's per-test systemctl shim, which is
    # what a test that never loads helpers runs with.
    run escape env PATH="/usr/bin:/bin" bash -c 'systemctl daemon-reload'
    [ "$status" -ne 0 ]
    grep -q 'systemctl daemon-reload' "$PRIVATE_LEDGER"
    run escape env PATH="$HELIX_TEST_SANDBOX_BIN:/usr/bin:/bin" sh -c 'systemctl daemon-reload'
    [ "$status" -ne 0 ]
    [ "$(grep -c 'systemctl daemon-reload' "$PRIVATE_LEDGER")" -eq 2 ]
}
