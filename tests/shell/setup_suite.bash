# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host sandbox for the whole bats suite.
#
# bats loads this file automatically for a directory run, before any test file.
# Nothing in a .bats file has to opt in, so a new test is sandboxed on the day
# it is written and a test that forgets to `load helpers` is covered anyway.
#
# What it blocks: commands that address the HOST by name rather than by a handle
# the test owns. `killall helix-screen` and `pidof dropbear` name a process this
# test did not start, so they reach whatever else is running on the machine -
# another bats file's app, or the developer's own. `kill "$pid"` is not in the
# list: a pid is a handle the test already holds, so it is scoped by
# construction. That distinction is the whole rule.
#
# A blocked call is recorded in the ledger with the test that made it and then
# fails; teardown_suite fails the run and prints the ledger. A test that needs
# one of these commands mocks it explicitly - mock_command* writes into
# $BATS_TEST_TMPDIR/bin, which is prepended later and therefore wins, and a
# shell function beats both. Mocked calls never reach the shim, so they never
# appear in the ledger.
#
# Two layers, because neither survives every boundary a test crosses:
#
#   PATH shims       survive a non-bash callee (installer scripts are #!/bin/sh,
#                    and sh does not import bash's exported functions), and are
#                    lost when a callee REPLACES PATH rather than prepending to
#                    it - `env PATH="$bin" ...`, or a script that hardens PATH
#                    by putting the stock system directories first.
#   Exported shell   survive PATH replacement and PATH hardening, and are lost
#   functions        at a non-bash callee.
#
# The union covers every boundary this suite actually crosses. The residual gap
# is a callee that is BOTH non-bash AND replaces PATH; test_sandbox_gate.bats
# pins that gap closed by failing any PATH assignment in tests/shell that drops
# the sandbox directory.

# Commands that address the host by name. Keep this list and the gate test's
# copy in step - the gate fails if they diverge.
HELIX_SANDBOX_COMMANDS="killall pkill pidof reboot shutdown halt poweroff telinit launchctl crontab mount umount diskutil mkfs addr2line systemctl jobpool"

setup_suite() {
    # The installer bundles are generated, never committed. Build them once per
    # run from scripts/lib/installer/, so no test reads a bundle older than the
    # modules it is meant to check.
    export INSTALL_BUNDLE="$BATS_SUITE_TMPDIR/installer/install.sh"
    export UNINSTALL_BUNDLE="$BATS_SUITE_TMPDIR/installer/uninstall.sh"
    mkdir -p "$BATS_SUITE_TMPDIR/installer"
    local repo
    repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    sh "$repo/scripts/bundle-installer.sh" -o "$INSTALL_BUNDLE" >/dev/null
    sh "$repo/scripts/bundle-uninstaller.sh" -o "$UNINSTALL_BUNDLE" >/dev/null

    export HELIX_TEST_SANDBOX_BIN="$BATS_SUITE_TMPDIR/sandbox-bin"
    export HELIX_TEST_SANDBOX_LEDGER="$BATS_SUITE_TMPDIR/escapes.tsv"
    # "enforce" fails the blocked call; "permissive" lets it return success.
    # Neither ever runs the real command.
    export HELIX_TEST_SANDBOX_MODE="${HELIX_TEST_SANDBOX_MODE:-enforce}"

    mkdir -p "$HELIX_TEST_SANDBOX_BIN"
    : > "$HELIX_TEST_SANDBOX_LEDGER"

    local cmd
    for cmd in $HELIX_SANDBOX_COMMANDS; do
        # A single short printf append is atomic, which matters because bats
        # runs files in parallel and they share one ledger.
        cat > "$HELIX_TEST_SANDBOX_BIN/$cmd" <<SHIM
#!/bin/sh
printf '%s\t%s\t%s %s\n' \
    "\${BATS_TEST_FILENAME##*/}" "\${BATS_TEST_NAME:-(file setup)}" "$cmd" "\$*" \
    >> "\$HELIX_TEST_SANDBOX_LEDGER"
printf 'helix sandbox: blocked host-reaching command: %s %s\n' "$cmd" "\$*" >&2
[ "\$HELIX_TEST_SANDBOX_MODE" = permissive ] && exit 0
exit 1
SHIM
        chmod +x "$HELIX_TEST_SANDBOX_BIN/$cmd"
    done

    export PATH="$HELIX_TEST_SANDBOX_BIN:$PATH"

    # The suite runs the same inside a make as outside one. `make test-shell`,
    # full-test-run and land-gate hand every test their jobserver, their
    # command-line variables (BATS_REQUIRED=1 among them, through MAKEFLAGS
    # and the plain environment alike) and the variables the Makefile exports,
    # and a make a test runs would read all of it as its own.
    unset MAKEFLAGS MFLAGS GNUMAKEFLAGS MAKELEVEL MAKEOVERRIDES BATS_REQUIRED BATS_BIN \
        BATS_GATE BATS_ALL BATS_BASE BATS_APP \
        ABI_STAMPED_FOR HELIX_PATCHES_FROM_CLEAN HELIX_FROM_CLEAN_SENTINEL HELIX_TESTS_NO_STALE_CHECK

    # make never runs in the tree under test. A gate runs this suite while its
    # own lanes build there, and a second make in that tree deletes the test
    # binary they are about to run (prune-orphan-test-objs). A dry run is no
    # safer: make still runs every recipe line that names $(MAKE), and parsing
    # the Makefile writes stamps under build/. A test runs make in
    # scratch_tree (helpers.bash) or in a directory of its own; one whose
    # directory, after any -C, is this tree or inside it is blocked and
    # recorded like the commands above.
    #
    # A macOS host's /usr/bin/make is GNU make 3.81 (Homebrew's 4.x installs
    # as gmake), and the repo Makefile hard-errors below 4.2, so the guard
    # hands such a host's make calls to gmake. A Linux make, and the jobpool
    # shim (which execs a modern make), parse as 4.2+ and are used as found.
    export HELIX_TEST_REPO_ROOT
    HELIX_TEST_REPO_ROOT="$(cd "$repo" && pwd -P)"
    # The guard execs the first make on PATH that is neither itself nor a
    # jobpool shim: such a shim execs the next make on PATH, which is the guard
    # again. JOBPOOL=0 below makes a shim a passthrough anyway.
    local mk_ver mk_major mk_minor real_make="" d
    local IFS=:
    for d in ${PATH#"$HELIX_TEST_SANDBOX_BIN:"}; do
        [ -n "$d" ] && [ -x "$d/make" ] && [ ! -d "$d/make" ] || continue
        head -n 2 "$d/make" 2>/dev/null | grep -q '^# jobpool shim' && continue
        real_make="$d/make"
        break
    done
    unset IFS
    mk_ver="$("$real_make" --version 2>/dev/null | head -1)"
    mk_major="$(printf '%s\n' "$mk_ver" | sed -n 's/^GNU Make \([0-9]*\)\..*$/\1/p')"
    mk_minor="$(printf '%s\n' "$mk_ver" | sed -n 's/^GNU Make [0-9]*\.\([0-9]*\).*$/\1/p')"
    if { [ -z "$mk_major" ] || [ "$mk_major" -lt 4 ] ||
         { [ "$mk_major" -eq 4 ] && [ "${mk_minor:-0}" -lt 2 ]; }; } &&
        command -v gmake >/dev/null 2>&1; then
        real_make="$(command -v gmake)"
    fi
    # One guard behind both names a test can call make by. It finds the
    # directory make would run in from -C (alone, or last in a cluster such as
    # -sC), --directory and any prefix of it make accepts (--dir=), each taken
    # relative to the one before.
    export HELIX_TEST_MAKE_GUARD="$BATS_SUITE_TMPDIR/make-guard.sh"
    cat > "$HELIX_TEST_MAKE_GUARD" <<'GUARD'
#!/bin/sh
# make-guard.sh <real make> <name it was called by> [make args...]
real=$1 name=$2
shift 2
dir=$(pwd -P)
info= want= skip=
chdir() { dir=$(cd "$dir" 2>/dev/null && cd "$1" 2>/dev/null && pwd -P) || dir=; }
for a in "$@"; do
    if [ -n "$want" ]; then chdir "$a"; want=; continue; fi
    if [ -n "$skip" ]; then skip=; continue; fi
    case "$a" in
        --) break ;;
        --version | --help) info=1 ;;
        --di*=*) case --directory in "${a%%=*}"*) chdir "${a#*=}" ;; esac ;;
        --di*) case --directory in "$a"*) want=1 ;; esac ;;
        --*) ;;
        -?*)
            c=${a#-}
            while [ -n "$c" ]; do
                f=${c%"${c#?}"}
                c=${c#?}
                case "$f" in
                    C) if [ -n "$c" ]; then chdir "$c"; else want=1; fi; break ;;
                    v | h) info=1 ;;
                    # Options whose argument is the rest of the cluster or the next word.
                    [fIoWE]) [ -n "$c" ] || skip=1; break ;;
                esac
            done ;;
    esac
done
case "$dir/" in
    "$HELIX_TEST_REPO_ROOT"/*)
        if [ -z "$info" ]; then
            printf '%s\t%s\t%s %s (in %s)\n' \
                "${BATS_TEST_FILENAME##*/}" "${BATS_TEST_NAME:-(file setup)}" "$name" "$*" "$dir" \
                >> "$HELIX_TEST_SANDBOX_LEDGER"
            printf 'helix sandbox: blocked %s in the tree under test (%s): run it in scratch_tree\n' "$name" "$dir" >&2
            [ "$HELIX_TEST_SANDBOX_MODE" = permissive ] && exit 0
            exit 1
        fi ;;
esac
exec "$real" "$@"
GUARD
    chmod +x "$HELIX_TEST_MAKE_GUARD"
    printf '#!/bin/sh\nexec "$HELIX_TEST_MAKE_GUARD" %s make "$@"\n' "'$real_make'" \
        > "$HELIX_TEST_SANDBOX_BIN/make"
    chmod +x "$HELIX_TEST_SANDBOX_BIN/make"
    # The function layer, for the same reason as the blocked commands: a
    # callee that REPLACES PATH loses the shim.
    make() { _helix_sandbox_dispatch make "$@"; }
    export -f make
    local real_gmake
    real_gmake="$(PATH="${PATH#"$HELIX_TEST_SANDBOX_BIN:"}" command -v gmake)" || real_gmake=""
    if [ -n "$real_gmake" ]; then
        printf '#!/bin/sh\nexec "$HELIX_TEST_MAKE_GUARD" %s gmake "$@"\n' "'$real_gmake'" \
            > "$HELIX_TEST_SANDBOX_BIN/gmake"
        chmod +x "$HELIX_TEST_SANDBOX_BIN/gmake"
        gmake() { _helix_sandbox_dispatch gmake "$@"; }
        export -f gmake
    fi

    # The host's build pool is shared with every other session on the box. A
    # make the suite runs bypasses it (JOBPOOL=0 turns the shim into a plain
    # passthrough), and jobpool itself is blocked above; a test that exercises
    # pool handling names a fake through HELIX_JOBPOOL and unsets JOBPOOL.
    export JOBPOOL=0

    # The developer's build and test hosts (scripts/lib/build_hosts.sh) never
    # reach a test: not their config file, and not variables they exported. A
    # test that wants a host names a fake one.
    export HELIX_BUILD_HOSTS_FILE="${BATS_RUN_TMPDIR:-/tmp}/no-build-hosts.env"
    # shellcheck disable=SC2046  # one key per word, from the parser's allowlist
    unset $("$repo/scripts/lib/build_hosts.sh" --keys)

    # The function layer, which exists for the case where PATH no longer holds
    # the shim. bash resolves a function before PATH, so it would otherwise
    # outrank a test's own mock; the dispatcher hands control back whenever PATH
    # resolves to something under the bats run directory, which is where every
    # mock in this suite is written and nowhere a system binary can be.
    # shellcheck disable=SC2329  # reached through the per-command wrappers below
    _helix_sandbox_dispatch() {
        local cmd="$1"; shift
        local resolved
        resolved=$(type -P "$cmd" 2>/dev/null) || resolved=""
        if [ -n "$resolved" ] && [ "$resolved" != "$HELIX_TEST_SANDBOX_BIN/$cmd" ]; then
            case "$resolved" in
                "$BATS_RUN_TMPDIR"/*) "$resolved" "$@"; return $? ;;
            esac
        fi
        "$HELIX_TEST_SANDBOX_BIN/$cmd" "$@"
    }
    export -f _helix_sandbox_dispatch

    for cmd in $HELIX_SANDBOX_COMMANDS; do
        eval "$cmd() { _helix_sandbox_dispatch $cmd \"\$@\"; }"
        # shellcheck disable=SC2163  # the name is the loop variable, by design
        export -f "$cmd"
    done

    # A runaway tool that chews the machine is the other way a test does damage,
    # and no command shim catches it. addr2line is in the list above because
    # resolving one address at a time against an unstripped binary exhausts
    # memory; a CPU-time cap is the portable backstop for the rest. An address
    # space cap is deliberately not set: allocators and sanitizers reserve far
    # more virtual memory than they use, so it fails builds without bounding
    # real usage.
    ulimit -t 900 2>/dev/null || true
}

teardown_suite() {
    [ -s "${HELIX_TEST_SANDBOX_LEDGER:-/nonexistent}" ] || return 0

    {
        echo "Tests reached outside the sandbox for a host-reaching command."
        echo "Each line is: test file, test name, blocked command."
        echo
        sort -u "$HELIX_TEST_SANDBOX_LEDGER" | sed 's/^/    /'
        echo
        echo "Mock the command in that test's setup() - mock_command_script"
        echo "\"killall\" 'exit 0' - or call it on a pid the test owns instead."
        echo "A make in the tree under test runs in scratch_tree (helpers.bash) instead."
    } >&2

    [ "$HELIX_TEST_SANDBOX_MODE" = permissive ] && return 0
    return 1
}
