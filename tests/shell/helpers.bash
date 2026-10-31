# SPDX-License-Identifier: GPL-3.0-or-later
# Shared test helpers for bats tests
# Source this from setup() in each .bats file

# Stub out logging functions (not available outside installer)
log_info() { :; }
log_warn() { :; }
log_error() { :; }
log_success() { :; }
log_note() { :; }
export -f log_info log_warn log_error log_success log_note
# The installer's log_info prints to the terminal only when verbose; tests
# assert on its text, so they run verbose unless a test unsets this.
export HELIX_INSTALL_VERBOSE=1

# Ensure BATS_TEST_TMPDIR exists (added in bats 1.4.1, Ubuntu 22.04 ships 1.2.1)
# Each bats test runs in a subshell, so this creates a fresh dir per test.
if [ -z "$BATS_TEST_TMPDIR" ]; then
    BATS_TEST_TMPDIR=$(mktemp -d "${BATS_TMPDIR:-/tmp}/bats-test-XXXXXX")
fi
export BATS_TEST_TMPDIR

# helix-claim treats two claims carrying one session id as one owner. A suite
# run from inside a session would otherwise make every claim a test takes the
# same owner, and each "another session holds it" case would pass the take. A
# test that needs a session id sets it on the command.
unset HELIX_SESSION_ID CLAUDE_CODE_SESSION_ID

# Where the installer writes systemd drop-ins. A test run as root (the CI and
# zeus containers) would otherwise write into the real /etc/systemd/system.
export HELIX_SYSTEMD_UNIT_DIR="$BATS_TEST_TMPDIR/etc/systemd/system"
# A per-test build-hosts file (scripts/lib/build_hosts.sh), absent until a test writes it.
export HELIX_BUILD_HOSTS_FILE="$BATS_TEST_TMPDIR/build-hosts.env"

# Where the installer reads process ancestry. Absent until mock_proc fills it,
# so the desktop session a developer runs bats from never reads as the
# installer's own. Source time runs builtins only: some tests source this
# file under a PATH with no coreutils.
export HELIX_PROC_ROOT="$BATS_TEST_TMPDIR/proc"

# Where the installer opens the controlling terminal to ask a question. A
# developer running bats from a terminal would otherwise be prompted, and the
# test would wait on them. A test that wants an answer writes this file.
export HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/no-tty"

# Fake one process under HELIX_PROC_ROOT. The comm carries a space and a ')'
# because the real field can, and the parser must split after the LAST ')'.
# Args: $1 = pid, $2 = ppid, $3.. = environment entries (KEY=value)
mock_proc() {
    local pid="$1" ppid="$2"
    shift 2
    mkdir -p "$HELIX_PROC_ROOT/$pid"
    printf '%s (sh (x) y) S %s 1 1 0 -1\n' "$pid" "$ppid" > "$HELIX_PROC_ROOT/$pid/stat"
    if [ $# -gt 0 ]; then
        printf '%s\0' "$@" > "$HELIX_PROC_ROOT/$pid/environ"
    else
        : > "$HELIX_PROC_ROOT/$pid/environ"
    fi
}

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Release-target derivation from mk/cross.mk
# ---------------------------------------------------------------------------

# Real release packaging targets: every `release-<plat>:` target minus
# aggregates (release-all, release-clean*) minus pure aliases - a target whose
# recipe is only `release-x: release-y` delegates to a target this list
# already covers. Shared by test_release_packaging_artifacts.bats and
# test_symbol_ci.bats; a second hand-written copy is how the two drift.
# Args: $1 = path to mk/cross.mk (default: mk/cross.mk)
release_targets() {
    local cross_mk="${1:-mk/cross.mk}"
    grep -oE '^release-[a-z0-9-]+:' "$cross_mk" \
        | sed 's/:$//' \
        | grep -vxE 'release-all|release-clean|release-clean-assets' \
        | while IFS= read -r t; do
            grep -qE "^${t}: *release-[a-z0-9-]+$" "$cross_mk" || echo "$t"
        done \
        | sort -u
}

# Platforms release.yml builds: the plan job's FULL_MATRIX, one per line.
# Shared by test_symbol_ci.bats and test_update_platform_coverage.bats.
# Args: $1 = path to release.yml (default: .github/workflows/release.yml)
release_matrix_platforms() {
    python3 - "${1:-.github/workflows/release.yml}" <<'PY'
import json, sys, yaml
with open(sys.argv[1]) as fh:
    doc = yaml.safe_load(fh)
for step in doc["jobs"]["plan"]["steps"]:
    matrix = (step.get("env") or {}).get("FULL_MATRIX")
    if matrix:
        for entry in json.loads(matrix):
            print(entry["platform"])
PY
}

# Mocking a command: which mechanism, and where each one stops
#
# The suite has two, and they fail at different boundaries. Pick by asking what
# runs the command, not by what reads better:
#
#   mock_command* (this file)   A file on PATH. Reaches a callee in any
#                               language, which is what installer scripts need
#                               because they are #!/bin/sh and sh does not
#                               import bash functions. Lost the moment a callee
#                               REPLACES PATH rather than prepending to it -
#                               `env PATH="$bin" ...`, or a script that hardens
#                               PATH by putting the stock system directories
#                               first, as install.sh does.
#
#   name() { :; }; export -f    A bash function. Outranks PATH, so it survives
#                               both of those. Lost at a non-bash callee, and
#                               at `env -i`.
#
# Neither is a safety net. tests/shell/setup_suite.bash blocks the commands that
# address the host by name for the whole suite, in both mechanisms at once, and
# fails the run when a test reaches one. A mock here is how a test opts out of
# that block for a command it means to exercise: it is found first, so the
# sandbox shim never runs and nothing is recorded against the test.
# ---------------------------------------------------------------------------

# Create a mock command that outputs specific text
# Usage: mock_command "systemctl" "User=biqu"
mock_command() {
    local cmd="$1" output="$2"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/$cmd" << MOCK_EOF
#!/bin/sh
echo "$output"
MOCK_EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/$cmd"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

# Create a mock command that fails (exits non-zero)
mock_command_fail() {
    local cmd="$1"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\nexit 1\n' > "$BATS_TEST_TMPDIR/bin/$cmd"
    chmod +x "$BATS_TEST_TMPDIR/bin/$cmd"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

# Create a mock command with custom script body
# Usage: mock_command_script "systemctl" 'case "$1" in show) echo "User=biqu";; *) exit 1;; esac'
mock_command_script() {
    local cmd="$1" body="$2"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/$cmd" << MOCK_EOF
#!/bin/sh
$body
MOCK_EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/$cmd"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

# A clock for code that waits by `date +%s` or by /proc/uptime (read from
# HELIX_UPTIME_FILE, which this exports), with sleep mocked: `sleep N` moves
# it N seconds and both read it, so a wait of minutes runs at once. Every
# other date call reaches the real date. $1, when set, is shell run after each
# tick with the new time in $now (a process that exits at a given second). The
# clock file is $BATS_TEST_TMPDIR/clock, starting at 1000.
mock_fake_clock() {
    local clock="$BATS_TEST_TMPDIR/clock" real_date
    real_date=$(command -v date)
    [ "$real_date" != "$BATS_TEST_TMPDIR/bin/date" ] || real_date=/bin/date
    echo 1000 > "$clock"
    export HELIX_UPTIME_FILE="$clock"
    mock_command_script "sleep" "now=\$((\$(cat '$clock') + \${1%%.*}))
echo \"\$now\" > '$clock'
${1:-}"
    mock_command_script "date" "[ \"\$*\" = +%s ] && exec cat '$clock'
exec '$real_date' \"\$@\""
}

# A curl proper that stands in for Moonraker's HTTP API, its state held in
# files under $1: up (it answers at all), auth (every request is answered
# 401), print_state (print_stats.state, default standby), version (what
# /server/helix/status reports; without it a 404), new_version (the version a
# service restart loads, and an in-place restart when none is loaded yet),
# inert (a service restart is answered 200 and does nothing, as Moonraker
# does when its service manager cannot act), refuse (every POST is answered
# 403), system_info
# (the body of /machine/system_info; without it one Moonraker running as the
# "moonraker" service). An accepted
# restart makes the next request go unanswered, as Moonraker does while it
# restarts. Every request is logged to $1/log as "METHOD URL".
# Starts up and idle, with the fake clock (mock_fake_clock) running.
mock_moonraker_http() {
    local d="$1"
    mkdir -p "$d"
    : > "$d/up"
    local body
    body="d='$d'
$(cat <<'MOCK'
case "$1" in --version) echo 'curl 8.5.0 (x86_64-pc-linux-gnu)'; exit 0 ;; esac
method=GET url='' fmt='' fail=''
while [ $# -gt 0 ]; do
    case "$1" in
        -X) method=$2; shift ;;
        -w) fmt=$2; shift ;;
        --max-time | --connect-timeout | -o | -A | -m) shift ;;
        -*f*) fail=1 ;;
        -*) ;;
        *) url=$1 ;;
    esac
    shift
done
printf '%s %s\n' "$method" "$url" >> "$d/log"
if [ -e "$d/bounce" ]; then
    rm -f "$d/bounce"
    [ -z "$fmt" ] || printf '\n000'
    exit 7
fi
if [ ! -e "$d/up" ]; then
    [ -z "$fmt" ] || printf '\n000'
    exit 7
fi
code=200 out='{"result":{}}'
if [ -e "$d/auth" ]; then
    code=401 out=''
elif [ "$method" = POST ]; then
    if [ -e "$d/refuse" ]; then
        code=403 out=''
    else
        out='{"result":"ok"}'
        case "$url" in
            */machine/services/restart*)
                if [ ! -e "$d/inert" ]; then
                    : > "$d/bounce"
                    [ ! -e "$d/new_version" ] || cp "$d/new_version" "$d/version"
                fi ;;
            */server/restart)
                : > "$d/bounce"
                [ -e "$d/version" ] || [ ! -e "$d/new_version" ] || cp "$d/new_version" "$d/version" ;;
        esac
    fi
else
    case "$url" in
        */server/helix/status)
            if [ -e "$d/version" ]; then
                out="{\"result\":{\"enabled\":true,\"version\":\"$(cat "$d/version")\"}}"
            else
                code=404 out=''
            fi ;;
        *print_stats*)
            out="{\"result\":{\"status\":{\"print_stats\":{\"state\":\"$(cat "$d/print_state" 2>/dev/null || echo standby)\"}}}}" ;;
        */server/info) out='{"result":{"klippy_state":"ready","components":["file_manager"]}}' ;;
        */machine/system_info)
            out=$(cat "$d/system_info" 2>/dev/null ||
                echo '{"result":{"system_info":{"provider":"systemd_dbus","available_services":["klipper","moonraker"],"instance_ids":{"moonraker":"moonraker","klipper":"klipper"}}}}') ;;
    esac
fi
printf '%s' "$out"
[ -z "$fmt" ] || printf '\n%s' "$code"
[ -z "$fail" ] || [ "$code" -lt 400 ] || exit 22
exit 0
MOCK
)"
    mock_command_script "curl" "$body"
    mock_fake_clock
}

# Create temp directory structure mimicking a Pi system
setup_mock_pi() {
    export MOCK_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$MOCK_ROOT/home/biqu/printer_data/config"
    mkdir -p "$MOCK_ROOT/opt/helixscreen/config"
}

# SUDO stub (no-op for tests)
SUDO=""
export SUDO

# Create a fake ELF binary with specific architecture
# Usage: create_fake_elf output_path class machine
#   class: 01=32-bit, 02=64-bit
#   machine: 0x28=ARM, 0xb7=AARCH64
# Creates a minimal 20-byte ELF header
create_fake_elf() {
    local output=$1
    local class=$2      # "01" or "02"
    local machine=$3    # "28" or "b7"

    # ELF header: magic(4) + class(1) + data(1) + version(1) + osabi(1)
    # + padding(8) + type(2) + machine(2) = 20 bytes
    # Using little-endian (data=01)
    printf '\x7fELF' > "$output"                          # magic
    printf "\\x$(printf '%02x' "0x$class")" >> "$output"  # class
    printf '\x01' >> "$output"                             # data (LE)
    printf '\x01' >> "$output"                             # version
    printf '\x00' >> "$output"                             # osabi
    printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$output"  # padding
    printf '\x02\x00' >> "$output"                         # type (ET_EXEC)
    printf "\\x$(printf '%02x' "0x$machine")" >> "$output" # machine lo
    printf '\x00' >> "$output"                             # machine hi
}

# Create a fake ARM 32-bit ELF binary
create_fake_arm32_elf() {
    create_fake_elf "$1" "01" "28"
}

# Create a fake MIPS 32-bit ELF binary (little-endian, e_machine=0x08)
create_fake_mips_elf() {
    create_fake_elf "$1" "01" "08"
}

# Create a fake AARCH64 64-bit ELF binary
create_fake_aarch64_elf() {
    create_fake_elf "$1" "02" "b7"
}

# ---------------------------------------------------------------------------
# BSD/macOS portability
#
# The installer scripts ship to Linux devices, so their GNU idioms (sed -i EXPR,
# stat -c) are correct there. Only the test host may differ, so the shims live
# here rather than in scripts/lib/installer/.
# ---------------------------------------------------------------------------

# Put a GNU-compatible `sed` on PATH when the host sed is BSD (macOS).
#
# BSD sed reads the argument after -i as a backup suffix, so GNU-style
# `sed -i EXPR FILE` either errors ("sed: -e: No such file or directory") or
# consumes EXPR as the suffix and then blocks reading stdin. Call this from
# setup() in any test that runs `sed -i` itself or sources installer code
# that does.
install_gnu_sed_shim() {
    [ -n "${_HELIX_GNU_SED_SHIM:-}" ] && return 0

    local real
    real=$(command -v sed) || return 1
    # Probe the REAL sed before the shim can shadow it on PATH.
    if "$real" --version >/dev/null 2>&1; then
        _HELIX_GNU_SED_SHIM=gnu   # already GNU, nothing to do
        return 0
    fi

    mkdir -p "$BATS_TEST_TMPDIR/gnubin"
    cat > "$BATS_TEST_TMPDIR/gnubin/sed" <<SHIM
#!/bin/sh
# Translate GNU 'sed -i EXPR...' into BSD 'sed -i "" EXPR...'.
if [ "\$1" = "-i" ]; then
    shift
    exec $real -i '' "\$@"
fi
exec $real "\$@"
SHIM
    chmod +x "$BATS_TEST_TMPDIR/gnubin/sed"
    export PATH="$BATS_TEST_TMPDIR/gnubin:$PATH"
    _HELIX_GNU_SED_SHIM=shim
}

# Modification time in epoch seconds (GNU `stat -c %Y`, BSD `stat -f %m`).
stat_mtime() {
    stat -c %Y "$1" 2>/dev/null || stat -f %m "$1"
}

# Put a GNU-compatible `stat` on PATH when the host stat is BSD (macOS).
#
# Installer code uses `stat -c '%d'` (device id, for mountpoint detection).
# BSD stat rejects -c outright, so on macOS the substitution fails, the branch
# silently takes its "unknown" path, and under bats' errexit the test body
# aborts before it can assert anything. Call this from setup() in any test that
# sources installer code doing `stat -c`.
install_gnu_stat_shim() {
    [ -n "${_HELIX_GNU_STAT_SHIM:-}" ] && return 0

    local real
    real=$(command -v stat) || return 1
    # Probe the REAL stat before the shim can shadow it on PATH.
    if "$real" -c '%d' . >/dev/null 2>&1; then
        _HELIX_GNU_STAT_SHIM=gnu   # already GNU, nothing to do
        return 0
    fi

    mkdir -p "$BATS_TEST_TMPDIR/gnubin"
    cat > "$BATS_TEST_TMPDIR/gnubin/stat" <<SHIM
#!/bin/sh
# Translate the GNU format specifiers the installer uses into BSD ones.
if [ "\$1" = "-c" ]; then
    fmt="\$2"; shift 2
    case "\$fmt" in
        '%d') bfmt='%d' ;;
        '%Y') bfmt='%m' ;;
        '%s') bfmt='%z' ;;
        *)    bfmt="\$fmt" ;;
    esac
    exec $real -f "\$bfmt" "\$@"
fi
exec $real "\$@"
SHIM
    chmod +x "$BATS_TEST_TMPDIR/gnubin/stat"
    export PATH="$BATS_TEST_TMPDIR/gnubin:$PATH"
    _HELIX_GNU_STAT_SHIM=shim
}

# Last element of bats' $lines array. bash 3.2 (macOS) has no negative
# subscripts, so ${lines[-1]} is a "bad array subscript" error there.
last_line() {
    printf '%s' "${lines[$((${#lines[@]} - 1))]}"
}

# ---------------------------------------------------------------------------
# Negative assertions
#
# POSIX makes the `!` reserved word exempt from errexit, and bats runs each
# @test body under `set -e`. So a bare `! some_command` that is NOT the final
# statement of the body is a silent no-op: when the assertion should fail, the
# non-zero status is swallowed and whatever runs last decides the test result.
# 65 such assertions were proving nothing before this helper landed.
#
#     ! grep -q oops "$conf"        # swallowed unless it is the last line
#     refute grep -q oops "$conf"   # fails the test, as intended
#
# Use `refute` for a simple command, `refute_sh` when you need a pipeline or
# other shell syntax, and `refute_grep` for the common file case (it dumps the
# file on failure, which is usually what you want to see).
# ---------------------------------------------------------------------------

# Abort the current test with a message. bats-core does not ship `fail` (it
# comes from the optional bats-assert library), so a test body calling it dies
# with "fail: command not found" — which still fails the test, but reports the
# wrong reason and hides the assertion's own message.
fail() {
    printf '%s\n' "$*" >&2
    return 1
}

# Assert that a command FAILS. Returns non-zero if it unexpectedly succeeds.
refute() {
    if "$@"; then
        printf 'refute: expected failure, but succeeded: %s\n' "$*" >&2
        return 1
    fi
}

# refute for a pipeline or anything else needing shell parsing.
refute_sh() {
    if eval "$1"; then
        printf 'refute_sh: expected failure, but succeeded: %s\n' "$1" >&2
        return 1
    fi
}

# Assert a pattern is ABSENT from a file. Dumps the file on failure.
refute_grep() {
    local pattern="$1" file="$2"
    if grep -q "$pattern" "$file"; then
        printf 'refute_grep: unexpectedly matched /%s/ in %s\n' "$pattern" "$file" >&2
        cat "$file" >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Positive assertions
#
# bash 3.2 is what macOS ships, and it does not apply errexit to a failing
# `[[ ]]` or `(( ))`. bats runs each @test body under `set -e`, so a mid-body
# `[[ ]]` assertion is inert on a Mac: when it should fail, the status is
# swallowed and whichever statement runs LAST decides the test result. Under
# bash 5 in CI the same line fails the test, so the two hosts disagree about
# what a test pins, and the Mac is the one that reports green.
#
#     [[ "$output" == *"ready"* ]]     # inert unless it is the last statement
#     contains "ready" "$output"       # fails the test, as intended
#
# Going through a function makes the failure a failing simple command, which
# every shell honours. A `[[ ]]` that genuinely IS the last statement of its
# body works everywhere and needs no wrapper.
#
# The needle is matched as a LITERAL substring - the case patterns quote it, so
# a `*` or `?` inside it means itself. An assertion that needs a real glob
# (anchored at either end, or several ordered segments) or any other compound
# condition keeps its `[[ ]]` and appends `|| fail "..."`, which ends the list
# in a simple command and is therefore honoured too.
# ---------------------------------------------------------------------------

# Assert that $2 contains the literal substring $1.
contains() {
    case "$2" in
        *"$1"*) return 0 ;;
        *) printf 'expected to find: %s\nin:\n%s\n' "$1" "$2" >&2; return 1 ;;
    esac
}

# Assert that $2 does NOT contain the literal substring $1.
lacks() {
    case "$2" in
        *"$1"*) printf 'expected NOT to find: %s\nin:\n%s\n' "$1" "$2" >&2; return 1 ;;
        *) return 0 ;;
    esac
}

# --- GNU sed on a BSD host (macOS) ----------------------------------------
#
# BSD sed differs from GNU/BusyBox sed in two ways this suite trips over: it
# needs `-i ''` where the others take a bare `-i`, and it does not expand `\n`
# in a replacement. Both failures are loud but misleading -- "invalid command
# code f" names the file, not the flag -- and one class of them is silent:
# `sed -i "/tag/d" "$conf" 2>/dev/null || true` inside an installer module
# returns 0 having changed nothing, so the test fails on an assertion about the
# file rather than on sed.
#
# Rewriting the call sites is not the fix, and for half of them it is not even
# possible. Tests that drive a DEVICE script are testing that script's real
# `sed -i`, which is correct for its only target (BusyBox/Linux on a printer).
# device-env-set-remote.sh in particular is REQUIRED to edit in place: a
# temp-file+mv would replace the inode and turn a symlinked helixscreen.env into
# a regular file, which is the exact regression one of its tests pins. The
# remaining call sites are tests editing their own fixtures, and a portable
# wrapper would still not give them BSD `\n` support, so they take the same
# route rather than a second, weaker one.
#
# So: give the test a real GNU sed instead. Linux CI already has one and this is
# a no-op there. On macOS, gnu-sed installs it as `gsed`, which we shim onto
# PATH under the name `sed` for the duration of the test -- that reaches the
# device script's own invocation too, since it calls a bare `sed`. With neither
# available the test skips with an actionable message rather than failing on a
# platform it was never written for.
require_gnu_sed() {
    if sed --version 2>/dev/null | grep -q GNU; then
        return 0
    fi
    if gsed --version 2>/dev/null | grep -q GNU; then
        mkdir -p "$BATS_TEST_TMPDIR/bin"
        printf '#!/bin/sh\nexec %s "$@"\n' "$(command -v gsed)" > "$BATS_TEST_TMPDIR/bin/sed"
        chmod +x "$BATS_TEST_TMPDIR/bin/sed"
        export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
        return 0
    fi
    skip "needs GNU sed (BSD sed differs on -i and \\n): brew install gnu-sed"
}

# ---------------------------------------------------------------------------
# Host systemctl is shadowed for every test, by default
#
# Installer code reaches systemctl through paths a test never names: the
# update-unit stop/disable sweep is not gated on INIT_SYSTEM, and
# detect_init_system answers "systemd" on any dev desktop. Headless CI has no
# polkit agent, so there each call is denied instantly and the installer's
# trailing "|| true" hides it - the suite stays green while doing something
# that must never happen. On a desktop the same call raises an auth dialog the
# test cannot answer, once per systemctl. Shadowing the binary by default
# makes both impossible; a test that wants scripted systemctl behaviour still
# calls mock_command* afterwards, whose later write to the same PATH slot
# wins. HELIX_TEST_REAL_SYSTEMCTL=1 restores the host binary for debugging
# the helpers themselves - no test should need it.
# ---------------------------------------------------------------------------
install_systemctl_shim() {
    [ -n "${_HELIX_SYSTEMCTL_SHIM:-}" ] && return 0
    _HELIX_SYSTEMCTL_SHIM=1

    # helpers.bash is also sourced as a library inside synthetic environments
    # built on a minimal PATH (no mkdir/chmod) that assert on total silence:
    # install where possible, degrade to no shadow without a word otherwise.
    if mkdir -p "$BATS_TEST_TMPDIR/bin" 2>/dev/null \
       && printf '#!/bin/sh\n# Inert inside bats: helpers.bash shadows systemctl by default.\nexit 0\n' \
              > "$BATS_TEST_TMPDIR/bin/systemctl" 2>/dev/null \
       && chmod +x "$BATS_TEST_TMPDIR/bin/systemctl" 2>/dev/null; then
        export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    fi
    return 0
}

[ -z "${HELIX_TEST_REAL_SYSTEMCTL:-}" ] && install_systemctl_shim

# ---------------------------------------------------------------------------
# The shared app lock: serialising the files that own a live helix-screen
#
# bats runs FILES in parallel, and only one of them may own a running app at a
# time - the instance lock in the config dir is per-config-dir, so it does not
# stop a sibling file from stopping, starting or killing an app the other file
# is driving. Tests that bring an instance up hold this lock for as long as
# they drive it.
#
# The lock is a DIRECTORY. mkdir is atomic on every POSIX filesystem, needs no
# binary outside the shell, and behaves identically on a developer Mac and in
# CI, so the path CI exercises is the path a developer runs. The alternatives
# each lose a host: `flock` is util-linux and absent from macOS, and the
# `exec {fd}>file` form that opens a descriptor for it needs bash 4.1 while
# macOS ships bash 3.2; `shlock` is the mirror image, present on macOS and
# absent from most Linux distributions, and it polls rather than blocks, so it
# needs this same wait loop around it anyway. A perl wrapper would get a
# kernel-backed lock released automatically when the holder dies, but holding
# one across setup() -> test -> teardown() needs a background co-process and a
# readiness handshake, and bash 3.2 has no `coproc` to build it with.
#
# What a directory costs is that a holder killed outright leaves it behind, so
# the owner's pid inside it is the recovery handle: a waiter that finds a dead
# owner breaks the lock and retries.
# ---------------------------------------------------------------------------
HELIX_APP_LOCK_DIR="${TMPDIR:-/tmp}/helix-bats-app.lock.d"
HELIX_APP_LOCK_HELD=""

# Drop a lock directory whose owner is gone.
#
# Serialised through a second mkdir so two waiters cannot both decide to clear
# the same lock, and the ownership test is repeated inside that guard: by the
# time a waiter gets here the pid may belong to a live process that acquired
# the lock while it waited.
helix_app_lock_break() {
    local breaker="${HELIX_APP_LOCK_DIR}.break" owner
    mkdir "$breaker" 2>/dev/null || return 0
    owner="$(cat "$HELIX_APP_LOCK_DIR/pid" 2>/dev/null || true)"
    if [ -z "$owner" ] || ! kill -0 "$owner" 2>/dev/null; then
        rm -rf "$HELIX_APP_LOCK_DIR"
    fi
    rmdir "$breaker" 2>/dev/null || true
    return 0
}

# Take the shared app lock, waiting up to $1 seconds (default 600).
# Returns 0 holding it, 1 on timeout.
helix_app_lock_acquire() {
    local timeout="${1:-600}"
    local deadline owner blank=0

    [ -n "$HELIX_APP_LOCK_HELD" ] && return 0
    deadline=$(( $(date +%s) + timeout ))

    while :; do
        if mkdir "$HELIX_APP_LOCK_DIR" 2>/dev/null; then
            echo $$ > "$HELIX_APP_LOCK_DIR/pid"
            HELIX_APP_LOCK_HELD=1
            return 0
        fi

        owner="$(cat "$HELIX_APP_LOCK_DIR/pid" 2>/dev/null || true)"
        if [ -n "$owner" ]; then
            blank=0
            kill -0 "$owner" 2>/dev/null || helix_app_lock_break
        else
            # An owner that has taken the directory but not yet written its pid
            # is indistinguishable from one that died in that window. The write
            # follows the mkdir immediately, so only the second is still blank
            # after a couple of seconds of polling.
            blank=$(( blank + 1 ))
            [ "$blank" -ge 20 ] && helix_app_lock_break
        fi

        if [ "$(date +%s)" -ge "$deadline" ]; then
            echo "helix_app_lock_acquire: gave up after ${timeout}s waiting for" \
                 "$HELIX_APP_LOCK_DIR (owner=${owner:-unknown})" >&2
            return 1
        fi
        sleep 0.1
    done
}

# Skip unless the app binary at $1 is built. The Shell Tests CI job never
# builds it, so build.yml's compile job runs every file that calls this against
# the binary it just linked. A file that skips on a missing app any other way
# runs in neither job, which test_code_lint.bats refuses.
require_app() {
    [ -x "$1" ] || skip "helix-screen not built (run 'make -j')"
}

# Give the shared app lock back. Safe to call from a file-level teardown that
# runs after tests which never took it, and it drops only a lock this process
# still owns, so a stale-breaker that already handed it on is not disturbed.
helix_app_lock_release() {
    [ -n "$HELIX_APP_LOCK_HELD" ] || return 0
    local owner
    owner="$(cat "$HELIX_APP_LOCK_DIR/pid" 2>/dev/null || true)"
    [ "$owner" = "$$" ] && rm -rf "$HELIX_APP_LOCK_DIR"
    HELIX_APP_LOCK_HELD=""
    return 0
}

# A SUDO stand-in that runs everything for real except an rm outside $1.
#
# clean_old_installation rm -f's real /etc paths (polkit and udev rules) that a
# test user cannot touch and that no test here asserts on. Letting those through
# fails the run on permissions; dropping every rm would also drop the sandbox
# writes the tests do assert on. So an rm whose arguments name a path under the
# given root runs, any other rm is a no-op, and everything else runs.
#
# The dispatch is `exec "$@"`, never `exec rm "$@"`: the incoming argv already
# begins with rm, so naming it again passes rm its own name as the first
# operand. GNU rm permutes options that follow an operand and -f suppresses the
# nonexistent one, which hides the duplicate on Linux; BSD rm stops option
# parsing at the first operand, so -rf is read as a filename and the directory
# survives.
install_sudo_rm_shim() {
    local root="$1"
    local shim="$BATS_TEST_TMPDIR/sudo-rm-neutral"
    cat > "$shim" <<SHIM
#!/bin/sh
if [ "\$1" = "rm" ]; then
    case " \$* " in
        *"$root"/*) exec "\$@" ;;
        *) exit 0 ;;
    esac
fi
exec "\$@"
SHIM
    chmod +x "$shim"
    SUDO="$shim"
    export SUDO
}

# ---------------------------------------------------------------------------
# The quality gate is scripts/quality-checks.sh (the driver) plus one file per
# gate under scripts/qc/: the gate's qc_<gate> function and its
# QC_TRIGGER_qc_<gate> staged-path regex. Helpers every gate calls live in
# scripts/qc/_lib.sh, so a test sources that before the gate it drives.

# Prints a gate's staged-path trigger regex, read by sourcing the gate file.
# Empty means the gate runs on every commit.
qc_trigger() {
    bash -c '. scripts/qc/_lib.sh && . "scripts/qc/$1.sh" && v="QC_TRIGGER_qc_$1" && printf "%s\n" "${!v:-}"' _ "$1"
}

# A gate body lifted out and run standalone needs qc_note/qc_count in scope, or
# its pass verdict is status 127 under `set -e` and the block dies mid-gate.
#
# Emits the real definitions rather than stubs, so assertions about what a gate
# prints on its pass path stay honest. Callers eval the output, or prepend it to
# an extracted block.
qc_verdict_defs() {
    local script="${1:-scripts/qc/_lib.sh}"
    local defs
    defs="$(grep -E '^qc_(note|count)\(\) \{' "$script")"
    [ "$(printf '%s\n' "$defs" | wc -l)" -eq 2 ] \
        || fail "expected one-line qc_note and qc_count definitions in $script, got: $defs"
    printf '%s\n' "$defs"
}

# ---------------------------------------------------------------------------
# Print one platform's branch of set_install_paths.
#
# The branches are an if/elif chain, so a fixed `grep -A N` window stops
# covering a branch the moment it grows a line, and the assertion inside it
# then passes or fails for the wrong reason. This reads to the next branch
# instead, whatever the length.
platform_branch() {
    local id="$1"
    local script="${2:-scripts/lib/installer/platform.sh}"
    awk -v start='[ "$platform" = "'"$1"'" ]' '
        index($0, start) { inside = 1; next }
        inside && /\[ "\$platform" = "/ && /\]; then/ { inside = 0 }
        inside
    ' "$script"
}

# ---------------------------------------------------------------------------
# make, kept out of the tree under test.
#
# setup_suite blocks a make whose directory is the tree (it races a gate's own
# lanes, and even a dry run writes there), so a test that needs the real
# Makefile runs it here instead.

# Prints this file's stand-in for the repo: every top-level entry symlinked,
# except build/, which is its own and starts empty, .git, left out so nothing
# reaches the tree's index through it, and .fonts.stamp, copied with its mtime
# so the fonts read as current exactly when they do in the tree.
#
# It is safe for parsing, for print-style targets, and for recipes that write
# only under build/ or to paths the test overrides. Everything else is a plain
# link a recipe writes straight through into the tree: compile_commands.json,
# src/ (the default TRANS_GEN_DIR), .venv, assets/.
#
# Built beside its final name and published with one symlink creation, so
# tests running in parallel in one file see either no tree or a whole one.
# What it prints is the physical path, which is what pwd -P and make report.
scratch_tree() {
    local link="$BATS_FILE_TMPDIR/scratch-tree" root e tmp
    if [ ! -L "$link" ]; then
        root="${HELIX_TEST_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)}"
        tmp="$(mktemp -d "$BATS_FILE_TMPDIR/scratch-tree.XXXXXX")"
        mkdir "$tmp/build"
        for e in "$root"/* "$root"/.[!.]*; do
            case "${e##*/}" in
                build | .git | .worktrees) continue ;;
                .fonts.stamp) cp -p "$e" "$tmp/" ;;
                *) ln -s "$e" "$tmp/${e##*/}" ;;
            esac
        done
        ln -sn "$tmp" "$link" 2>/dev/null || rm -rf "$tmp"
    fi
    (cd "$link" && pwd -P)
}

# scratch_make [make args...]: make in scratch_tree.
scratch_make() {
    make -C "$(scratch_tree)" --no-print-directory "$@"
}

# A PATH with no jobpool on it, for a test of a box that never installed one.
# The sandbox's own directory holds a jobpool blocker, so it is swapped, in
# place, for a copy of the sandbox without that one entry: the make guard and
# every other blocker stay on PATH.
path_without_jobpool() {
    local d p="" keep="$BATS_TEST_TMPDIR/sandbox-no-jobpool" f
    if [ -n "${HELIX_TEST_SANDBOX_BIN:-}" ] && [ ! -d "$keep" ]; then
        mkdir -p "$keep"
        for f in "$HELIX_TEST_SANDBOX_BIN"/*; do
            [ "${f##*/}" = jobpool ] || ln -s "$f" "$keep/${f##*/}"
        done
    fi
    local IFS=:
    for d in $PATH; do
        if [ -n "${HELIX_TEST_SANDBOX_BIN:-}" ] && [ "$d" = "$HELIX_TEST_SANDBOX_BIN" ]; then
            d=$keep
        elif [ -e "$d/jobpool" ]; then
            continue
        fi
        p=${p:+$p:}$d
    done
    printf '%s' "$p"
}

# make_db [make args...]: the Makefile's database, parsed in scratch_tree. The
# goal is `help`, whose recipe only echoes: with no goal a dry run walks `all`,
# whose recipe names $(MAKE) and so really runs.
make_db() {
    scratch_make -pn help "$@" 2>/dev/null
}

# mk_fragment <mk file> <first-line-regex> <target>: the lines of <mk file>
# from the first match through <target>'s recipe.
mk_fragment() {
    awk -v first="$2" -v tgt="^$3:" '
        $0 ~ first { on = 1 }
        on { print }
        on && $0 ~ tgt { intgt = 1; next }
        intgt && /^$/ { exit }' "$1"
}

# run_mk_fragment <mk file> <first-line-regex> <target> [make args...]: runs
# make on that fragment alone, as a scratch Makefile under BATS_TEST_TMPDIR,
# so a recipe is checked without the rest of the build.
run_mk_fragment() {
    local dir="$BATS_TEST_TMPDIR/mkfrag"
    mkdir -p "$dir"
    { printf 'ECHO := @echo\nQ := @\n'; mk_fragment "$1" "$2" "$3"; } >"$dir/Makefile"
    grep -q "^$3:" "$dir/Makefile" || { echo "no $3 recipe in the fragment" >&2; return 1; }
    shift 3
    (cd "$dir" && make --no-print-directory -f Makefile "$@")
}
