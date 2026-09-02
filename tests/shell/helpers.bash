# SPDX-License-Identifier: GPL-3.0-or-later
# Shared test helpers for bats tests
# Source this from setup() in each .bats file

# Stub out logging functions (not available outside installer)
log_info() { :; }
log_warn() { :; }
log_error() { :; }
log_success() { :; }
export -f log_info log_warn log_error log_success

# Ensure BATS_TEST_TMPDIR exists (added in bats 1.4.1, Ubuntu 22.04 ships 1.2.1)
# Each bats test runs in a subshell, so this creates a fresh dir per test.
if [ -z "$BATS_TEST_TMPDIR" ]; then
    BATS_TEST_TMPDIR=$(mktemp -d "${BATS_TMPDIR:-/tmp}/bats-test-XXXXXX")
fi

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

    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\n# Inert inside bats: helpers.bash shadows systemctl by default.\nexit 0\n' \
        > "$BATS_TEST_TMPDIR/bin/systemctl"
    chmod +x "$BATS_TEST_TMPDIR/bin/systemctl"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

[ -z "${HELIX_TEST_REAL_SYSTEMCTL:-}" ] && install_systemctl_shim
