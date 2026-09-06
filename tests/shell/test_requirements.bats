#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for check_requirements(), check_disk_space(), detect_init_system(),
# and install_runtime_deps() in scripts/lib/installer/requirements.sh

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Override the no-op log stubs so we can assert on output
    log_error()   { echo "ERROR: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_error log_warn log_info log_success

    # common.sh provides _has_python(), which check_requirements() now calls.
    # Must be sourced BEFORE requirements.sh or the call hits an undefined fn.
    # host_profile.sh rides along in the bundle's module order (common.sh,
    # then host_profile.sh) and defaults the HOST_* globals the chroot-aware
    # verify_binary_deps branch reads.
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"

    # requirements.sh references _has_no_new_privs (defined in service.sh).
    _has_no_new_privs() { return 1; }
    export -f _has_no_new_privs

    # Reset source guard so we can re-source per test
    unset _HELIX_REQUIREMENTS_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/requirements.sh"

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export INIT_SYSTEM=""
}

# ---------------------------------------------------------------------------
# Helper: create a restricted PATH with only the named commands available.
# This is the cleanest way to hide real commands from check_requirements(),
# which relies on `command -v`.
# ---------------------------------------------------------------------------
make_restricted_path() {
    local bin="$BATS_TEST_TMPDIR/restricted_bin"
    mkdir -p "$bin"
    for cmd in "$@"; do
        local real
        real="$(command -v "$cmd" 2>/dev/null)" || continue
        ln -sf "$real" "$bin/$cmd"
    done
    echo "$bin"
}

# Helper: run check_requirements in a subshell with a restricted PATH.
# log_* functions are redefined to produce visible output since helpers.bash
# stubs them as no-ops.
run_check_requirements_with_path() {
    local restricted_path="$1"
    run bash -c "
        log_error()   { echo \"ERROR: \$*\"; }
        log_info()    { echo \"INFO: \$*\"; }
        log_success() { echo \"OK: \$*\"; }
        export -f log_error log_info log_success
        export PATH='$restricted_path'
        unset _HELIX_COMMON_SOURCED _HELIX_REQUIREMENTS_SOURCED _PY_BIN _PY_PROBED
        . '$WORKTREE_ROOT/scripts/lib/installer/common.sh'
        . '$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh'
        _has_no_new_privs() { return 1; }
        . '$WORKTREE_ROOT/scripts/lib/installer/requirements.sh'
        check_requirements
    "
}

# ===========================================================================
# check_requirements
# ===========================================================================

@test "check_requirements: succeeds when all tools present" {
    run check_requirements
    [ "$status" -eq 0 ]
    [[ "$output" == *"All required commands available"* ]]
}

@test "check_requirements: fails when curl AND wget both missing" {
    local rbin
    rbin=$(make_restricted_path tar gunzip)

    run_check_requirements_with_path "$rbin"
    [ "$status" -ne 0 ]
    [[ "$output" == *"curl, wget, or python3"* ]]
}

@test "check_requirements: fails when tar missing" {
    local rbin
    rbin=$(make_restricted_path curl gunzip)

    run_check_requirements_with_path "$rbin"
    [ "$status" -ne 0 ]
    [[ "$output" == *"tar"* ]]
}

@test "check_requirements: fails when gunzip missing" {
    local rbin
    rbin=$(make_restricted_path curl tar)

    run_check_requirements_with_path "$rbin"
    [ "$status" -ne 0 ]
    [[ "$output" == *"gunzip"* ]]
}

@test "check_requirements: succeeds with curl but no wget" {
    local rbin
    rbin=$(make_restricted_path curl tar gunzip unzip)

    run_check_requirements_with_path "$rbin"
    [ "$status" -eq 0 ]
}

@test "check_requirements: succeeds with wget but no curl" {
    local rbin
    rbin=$(make_restricted_path tar gunzip unzip)
    # wget may not exist on macOS; create a stub
    printf '#!/bin/sh\nexit 0\n' > "$rbin/wget"
    chmod +x "$rbin/wget"

    run_check_requirements_with_path "$rbin"
    [ "$status" -eq 0 ]
}

@test "check_requirements: error lists all missing tools" {
    local rbin
    rbin=$(make_restricted_path)  # nothing available

    run_check_requirements_with_path "$rbin"
    [ "$status" -ne 0 ]
    contains "curl, wget, or python3" "$output"
    contains "tar" "$output"
    [[ "$output" == *"gunzip"* ]]
}

@test "check_requirements: all tools missing exits non-zero" {
    local rbin
    rbin=$(make_restricted_path)

    run_check_requirements_with_path "$rbin"
    [ "$status" -ne 0 ]
}

# --- python3 download/extraction fallback ---

@test "check_requirements: succeeds with python3 but no curl/wget/unzip" {
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"
    local rbin
    # python3 covers BOTH download (urllib) and zip extraction (zipfile).
    rbin=$(make_restricted_path tar gunzip python3)

    run_check_requirements_with_path "$rbin"
    [ "$status" -eq 0 ]
}

@test "check_requirements: fails when curl/wget/python all missing" {
    local rbin
    # tar/gunzip/unzip present, but no downloader at all (no python3 either).
    rbin=$(make_restricted_path tar gunzip unzip)

    run_check_requirements_with_path "$rbin"
    [ "$status" -ne 0 ]
    [[ "$output" == *"curl, wget, or python3"* ]]
}

@test "check_requirements: unzip not required when python3 present" {
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"
    local rbin
    # curl for download, python3 covers zip extraction — no unzip, no apt-get.
    rbin=$(make_restricted_path curl tar gunzip python3)

    run_check_requirements_with_path "$rbin"
    [ "$status" -eq 0 ]
    [[ "$output" != *"unzip"* ]]
}

@test "check_requirements: unzip required when no python and no apt" {
    local rbin
    # curl for download, but no unzip, no python fallback, and no apt-get.
    rbin=$(make_restricted_path curl tar gunzip)

    run_check_requirements_with_path "$rbin"
    [ "$status" -ne 0 ]
    [[ "$output" == *"unzip"* ]]
}

# ===========================================================================
# check_disk_space
# ===========================================================================

@test "check_disk_space: succeeds with adequate space (GNU df)" {
    mkdir -p "$BATS_TEST_TMPDIR/opt"

    mock_command_script "df" '
echo "Filesystem     1M-blocks  Used Available Use% Mounted on"
echo "/dev/sda1          1000   900       100  90% /"
'

    run check_disk_space "pi"
    [ "$status" -eq 0 ]
    [[ "$output" == *"100MB available"* ]]
}

@test "check_disk_space: exits when space insufficient (GNU df)" {
    mkdir -p "$BATS_TEST_TMPDIR/opt"

    mock_command_script "df" '
echo "Filesystem     1M-blocks  Used Available Use% Mounted on"
echo "/dev/sda1          1000   990        10  99% /"
'

    run check_disk_space "pi"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Insufficient disk space"* ]]
}

@test "check_disk_space: walks up to parent when INSTALL_DIR does not exist" {
    # INSTALL_DIR doesn't exist, but its parent does
    mkdir -p "$BATS_TEST_TMPDIR/opt"

    mock_command_script "df" '
echo "Filesystem     1M-blocks  Used Available Use% Mounted on"
echo "/dev/sda1          1000   800       200  80% /"
'

    run check_disk_space "pi"
    [ "$status" -eq 0 ]
}

@test "check_disk_space: walks up deeply nested non-existent path" {
    # No intermediate directories created
    export INSTALL_DIR="$BATS_TEST_TMPDIR/a/b/c/d/helixscreen"

    mock_command_script "df" '
echo "Filesystem     1M-blocks  Used Available Use% Mounted on"
echo "/dev/sda1          1000   800       200  80% /"
'

    run check_disk_space "pi"
    [ "$status" -eq 0 ]
}

@test "check_disk_space: graceful when df fails entirely" {
    mkdir -p "$BATS_TEST_TMPDIR/opt"

    # df produces no output and exits non-zero
    mock_command_script "df" 'exit 1'

    # available_mb will be empty, so the integer comparison is skipped
    run check_disk_space "pi"
    [ "$status" -eq 0 ]
}

@test "check_disk_space: BusyBox df format with ad5m platform" {
    mkdir -p "$BATS_TEST_TMPDIR/opt"

    # BusyBox df: KB blocks. 102400 KB = 100MB available
    mock_command_script "df" '
echo "Filesystem           1K-blocks      Used Available Use% Mounted on"
echo "/dev/mmcblk0p1         1048576    945152    102400  90% /"
'

    run check_disk_space "ad5m"
    [ "$status" -eq 0 ]
    [[ "$output" == *"100MB available"* ]]
}

@test "check_disk_space: BusyBox df with insufficient space" {
    mkdir -p "$BATS_TEST_TMPDIR/opt"

    # BusyBox df: 10240 KB = 10MB available (below 50MB minimum)
    mock_command_script "df" '
echo "Filesystem           1K-blocks      Used Available Use% Mounted on"
echo "/dev/mmcblk0p1         1048576   1038336     10240  99% /"
'

    run check_disk_space "ad5m"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Insufficient disk space"* ]]
}

@test "check_disk_space: uses default /opt/helixscreen when INSTALL_DIR unset" {
    unset INSTALL_DIR

    mock_command_script "df" '
echo "Filesystem     1M-blocks  Used Available Use% Mounted on"
echo "/dev/sda1          1000   800       200  80% /"
'

    run check_disk_space "pi"
    [ "$status" -eq 0 ]
}

# --- The install path does not exist and its ancestors stop at "/" -----------
#
# A stock AD5X installs to /srv/helixscreen and /srv is not on the read-only
# squashfs "/", so the dirname walk pins "/" as the df target. df "/" measures
# that 12.5M squashfs -- effectively full -- while the bytes actually land on
# the writable data partition (4.7G free on the same box). The check must
# measure the data mount, never "/".

@test "check_disk_space: install path under a missing top-level dir measures the data mount, not /" {
    # No ancestor of INSTALL_DIR exists: the walk runs out at "/".
    export INSTALL_DIR="/no-such-mount-point/helixscreen"
    local data="$BATS_TEST_TMPDIR/usr/data"
    mkdir -p "$data"
    export HELIX_DATA_MOUNT_CANDIDATES="$data"

    # df answers "full" for "/" and roomy for everything else: if the check
    # df'd "/", it would refuse; measuring the data mount passes.
    mock_command_script "df" '
case "$*" in
  */) echo "/dev/mmcblk0p5 12800 12800 0 100% /" ;;
  *)  echo "/dev/mmcblk0p7 4831838 0 4831838 0% $*" ;;
esac
'

    run check_disk_space "ad5x"
    [ "$status" -eq 0 ]
    contains "${data}" "$output"      # measured the data mount
    [[ "$output" != *"Insufficient"* ]]
}

@test "check_disk_space: insufficient space on the data mount still refuses" {
    export INSTALL_DIR="/no-such-mount-point/helixscreen"
    local data="$BATS_TEST_TMPDIR/usr/data"
    mkdir -p "$data"
    export HELIX_DATA_MOUNT_CANDIDATES="$data"

    mock_command_script "df" '
echo "/dev/mmcblk0p7 4831838 4816000 10240 99% $2"
'

    run check_disk_space "ad5x"
    [ "$status" -ne 0 ]
    contains "Insufficient disk space" "$output"
    [[ "$output" == *"${data}"* ]]      # names the partition that is full
}

@test "check_disk_space: with no data mount either, falls back to a real-write probe" {
    export INSTALL_DIR="/no-such-mount-point/helixscreen"
    export HELIX_DATA_MOUNT_CANDIDATES="/no/such/data/mount"

    # Record what the probe targeted instead of writing anything.
    dd() {
        for arg in "$@"; do
            case "$arg" in of=*) echo "${arg#of=}" > "$BATS_TEST_TMPDIR/probe-target" ;; esac
        done
        return 0
    }
    export -f dd

    run check_disk_space "ad5x"
    [ "$status" -eq 0 ]
    # The deepest existing ancestor is "/" -- the probe must go there, not
    # trust a df number for it.
    grep -q "^/\.helixscreen-space-probe\." "$BATS_TEST_TMPDIR/probe-target" \
        || fail "probe targeted $(cat "$BATS_TEST_TMPDIR/probe-target")"
}

@test "check_disk_space: the real-write probe refusal is fatal when / is unwritable" {
    export INSTALL_DIR="/no-such-mount-point/helixscreen"
    export HELIX_DATA_MOUNT_CANDIDATES="/no/such/data/mount"
    dd() { return 1; }
    export -f dd

    run check_disk_space "ad5x"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Cannot write"* ]]
}

# ===========================================================================
# detect_init_system
# ===========================================================================

@test "detect_init_system: detects systemd when both indicators present" {
    mock_command "systemctl" ""
    if [ ! -d /run/systemd/system ]; then
        skip "Requires /run/systemd/system (Linux with systemd)"
    fi

    detect_init_system
    [ "$INIT_SYSTEM" = "systemd" ]
}

@test "detect_init_system: systemctl without /run/systemd/system falls through" {
    mock_command "systemctl" ""
    # On macOS, /run/systemd/system does not exist
    if [ -d /run/systemd/system ]; then
        skip "Test requires /run/systemd/system to NOT exist"
    fi

    if [ -d /etc/init.d ]; then
        detect_init_system
        [ "$INIT_SYSTEM" = "sysv" ]
    else
        # Neither systemd dir nor init.d -- expect failure
        run detect_init_system
        [ "$status" -ne 0 ]
    fi
}

@test "detect_init_system: no systemctl, /etc/init.d exists -> sysv" {
    mock_command_fail "systemctl"
    if [ ! -d /etc/init.d ]; then
        skip "/etc/init.d not present on this system"
    fi
    # detect_init_system checks 'command -v systemctl' (which finds the mock)
    # AND '[ -d /run/systemd/system ]'. If the latter exists, it picks systemd
    # regardless of mock. Skip on systems with real systemd runtime dir.
    if [ -d /run/systemd/system ]; then
        skip "Cannot hide /run/systemd/system on this system (systemd runner)"
    fi

    detect_init_system
    [ "$INIT_SYSTEM" = "sysv" ]
}

@test "detect_init_system: neither systemctl nor /etc/init.d -> error" {
    mock_command_fail "systemctl"
    if [ -d /etc/init.d ]; then
        skip "/etc/init.d exists on this system"
    fi

    run detect_init_system
    [ "$status" -ne 0 ]
    [[ "$output" == *"Could not detect init system"* ]]
}

@test "detect_init_system: systemd wins over sysv when both present" {
    mock_command "systemctl" ""
    if [ ! -d /run/systemd/system ]; then
        skip "Requires /run/systemd/system (Linux with systemd)"
    fi
    # /etc/init.d may also exist, but systemd should win

    detect_init_system
    [ "$INIT_SYSTEM" = "systemd" ]
}

# ===========================================================================
# install_runtime_deps
# ===========================================================================

@test "install_runtime_deps: returns immediately for ad5m platform" {
    run install_runtime_deps "ad5m"
    [ "$status" -eq 0 ]
    # Should not attempt to check or install packages
    [[ "$output" != *"Checking runtime dependencies"* ]]
}

@test "install_runtime_deps: returns immediately for k1 platform" {
    run install_runtime_deps "k1"
    [ "$status" -eq 0 ]
    [[ "$output" != *"Checking runtime dependencies"* ]]
}

@test "install_runtime_deps: checks deps for pi platform" {
    # Mock dpkg-query to report all packages as installed
    mock_command_script "dpkg-query" 'echo "install ok installed"'

    run install_runtime_deps "pi"
    [ "$status" -eq 0 ]
    contains "Checking runtime dependencies" "$output"
    [[ "$output" == *"already installed"* ]]
}

# ===========================================================================
# verify_binary_deps
# ===========================================================================

# Helper: set up a fake binary and mock ldd for verify_binary_deps tests
setup_verify_binary() {
    mkdir -p "$INSTALL_DIR/bin"
    printf '#!/bin/sh\nexit 0\n' > "$INSTALL_DIR/bin/helix-screen"
    chmod +x "$INSTALL_DIR/bin/helix-screen"
}

@test "verify_binary_deps: succeeds when all libs found" {
    setup_verify_binary
    mock_command_script "ldd" '
echo "	linux-vdso.so.1 (0x7fff12345000)"
echo "	libdrm.so.2 => /usr/lib/aarch64-linux-gnu/libdrm.so.2 (0x7f1234000000)"
echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x7f1230000000)"
'

    run verify_binary_deps "pi"
    [ "$status" -eq 0 ]
    [[ "$output" == *"All shared library dependencies satisfied"* ]]
}

@test "verify_binary_deps: skips when ldd not available" {
    setup_verify_binary
    mock_command_fail "ldd"

    run verify_binary_deps "pi"
    [ "$status" -eq 0 ]
}

@test "verify_binary_deps: skips when binary not found" {
    # Provide ldd but don't create the binary
    mock_command "ldd" ""

    run verify_binary_deps "pi"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Binary not found"* ]]
}

@test "verify_binary_deps: detects missing libssl.so.1.1 and installs compat" {
    setup_verify_binary

    # First ldd call: libssl missing. Second call (after install): all good.
    local call_count_file="$BATS_TEST_TMPDIR/ldd_calls"
    echo "0" > "$call_count_file"

    mock_command_script "ldd" "
count=\$(cat '$call_count_file')
count=\$((count + 1))
echo \$count > '$call_count_file'
if [ \$count -eq 1 ]; then
    echo '	libssl.so.1.1 => not found'
    echo '	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x7f1230000000)'
else
    echo '	libssl.so.1.1 => /usr/lib/aarch64-linux-gnu/libssl.so.1.1 (0x7f1234000000)'
    echo '	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x7f1230000000)'
fi
"

    # apt-cache says libssl1.1 is available
    mock_command_script "apt-cache" 'echo "Package: libssl1.1"'
    # apt-get install succeeds
    mock_command "apt-get" ""

    run verify_binary_deps "pi"
    [ "$status" -eq 0 ]
    contains "libssl.so.1.1 not found" "$output"
    contains "Installing libssl1.1" "$output"
    [[ "$output" == *"resolved"* ]]
}

@test "verify_binary_deps: fails when libssl1.1 package not available" {
    setup_verify_binary
    mock_command_script "ldd" '
echo "	libssl.so.1.1 => not found"
echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x7f1230000000)"
'
    # apt-cache says no such package
    mock_command_fail "apt-cache"

    run verify_binary_deps "pi"
    [ "$status" -ne 0 ]
    contains "libssl1.1 package not available" "$output"
    [[ "$output" == *"OpenSSL 3"* ]]
}

@test "verify_binary_deps: fails when non-ssl library missing on pi" {
    setup_verify_binary
    mock_command_script "ldd" '
echo "	libfoo.so.42 => not found"
echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x7f1230000000)"
'

    run verify_binary_deps "pi"
    [ "$status" -ne 0 ]
    contains "Missing shared libraries" "$output"
    contains "libfoo.so.42" "$output"
    [[ "$output" == *"Could not resolve"* ]]
}

@test "verify_binary_deps: warns but continues on non-pi platforms" {
    setup_verify_binary
    mock_command_script "ldd" '
echo "	libfoo.so.1 => not found"
'

    run verify_binary_deps "ad5m"
    [ "$status" -eq 0 ]
    contains "Missing shared libraries" "$output"
    [[ "$output" == *"may not start correctly"* ]]
}

@test "verify_binary_deps: works for pi32 platform same as pi" {
    setup_verify_binary
    mock_command_script "ldd" '
echo "	libssl.so.1.1 => not found"
'
    mock_command_fail "apt-cache"

    run verify_binary_deps "pi32"
    [ "$status" -ne 0 ]
    [[ "$output" == *"libssl1.1 package not available"* ]]
}

@test "verify_binary_deps: multiple missing libs all reported" {
    setup_verify_binary
    mock_command_script "ldd" '
echo "	libssl.so.1.1 => not found"
echo "	libcrypto.so.1.1 => not found"
echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x7f1230000000)"
'

    # apt-cache says libssl1.1 is available, but libcrypto stays missing
    mock_command_script "apt-cache" 'echo "Package: libssl1.1"'
    mock_command "apt-get" ""
    # After "install", ldd still shows libcrypto missing
    # (We can't easily change mock mid-test, so the re-check will still show both)

    run verify_binary_deps "pi"
    contains "Missing shared libraries" "$output"
    contains "libssl.so.1.1" "$output"
    [[ "$output" == *"libcrypto.so.1.1"* ]]
}

# ===========================================================================
# verify_binary_deps outside a mod chroot
#
# On a Forge-X AD5X the installer runs on the host rootfs while the binary is
# built for the mod's chroot (a different Buildroot/glibc). Host-side ldd
# resolves against the host's libc and reports the chroot's libraries as
# "not found" -- false errors for a binary that runs fine where the mod runs
# it. Outside such a chroot the check must verify against the chroot and only
# ever WARN, never fail (audit item 8).
# ===========================================================================

# Sandbox a Forge-X host: chroot tree at $CHROOT, install root under a
# usr/data-shaped sandbox path, and the host->chroot bind map pointed at the
# sandbox (production default: /usr/data=/opt).
setup_mod_chroot_host() {
    SANDBOX="$BATS_TEST_TMPDIR/host"
    CHROOT="$SANDBOX/usr/data/.mod/.forge-x"
    INSTALL_DIR="$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    HOST_CHROOT_STATE="outside:$CHROOT"
    export HELIX_CHROOT_BINDS="$SANDBOX/usr/data=/opt"
    # The mod binds its data partition at /opt inside the chroot, so the
    # host's <sandbox>/usr/data/... install root is /opt/... in there.
    mkdir -p "$CHROOT/opt/config/mod/.bin/helixscreen/bin"
    printf '\x7fELF-mips-fake\n' > "$CHROOT/opt/config/mod/.bin/helixscreen/bin/helix-screen"
    setup_verify_binary
}

# chroot(1) stand-in: records the chroot dir and in-chroot path to a file
# (its stdout is swallowed by the $(...) capture in the implementation), then
# answers like a chroot-side ldd would. The answer rides in a global: a local
# of this helper dies when it returns, and the mock's closure over it would
# answer every call with an empty line.
mock_chroot_ldd() {
    _MOCK_CHROOT_LDD_ANSWER="$1"
    export _MOCK_CHROOT_LDD_ANSWER
    : > "$BATS_TEST_TMPDIR/chroot-calls"
    chroot() {
        echo "chroot $1 ldd $3" >> "$BATS_TEST_TMPDIR/chroot-calls"
        printf '%s\n' "$_MOCK_CHROOT_LDD_ANSWER"
    }
    export -f chroot
}

@test "verify_binary_deps: outside a mod chroot, verifies inside the chroot, not host-side" {
    setup_mod_chroot_host
    mock_chroot_ldd '	libc.so.0 => /lib/libc.so.0 (0x40000000)'

    # Host-side ldd reports the chroot's glibc as missing -- the false error
    # this branch exists to ignore.
    mock_command_script "ldd" '
echo "	libc.so.0 => not found"
'

    run verify_binary_deps "ad5x"
    [ "$status" -eq 0 ]
    # The chroot-side ldd ran, against the chroot's /opt spelling of the
    # host's /usr/data install root.
    grep -q "chroot $CHROOT ldd /opt/config/mod/.bin/helixscreen/bin/helix-screen" \
        "$BATS_TEST_TMPDIR/chroot-calls" \
        || fail "chroot-side ldd not invoked: $(cat "$BATS_TEST_TMPDIR/chroot-calls")"
    contains "verified inside the mod chroot" "$output"
    [[ "$output" != *"not found"* ]]
}

@test "verify_binary_deps: missing libs INSIDE the chroot warn but never fail" {
    setup_mod_chroot_host
    mock_chroot_ldd '	libc.so.0 => not found'

    run verify_binary_deps "ad5x"
    [ "$status" -eq 0 ]
    grep -q "chroot $CHROOT ldd" "$BATS_TEST_TMPDIR/chroot-calls" \
        || fail "chroot-side ldd not invoked"
    [[ "$output" == *"libc.so.0"* ]]
}

@test "verify_binary_deps: no in-chroot spelling of the binary leaves a WARN with the hint" {
    setup_mod_chroot_host
    # The chroot does not expose the install tree at either spelling.
    rm -rf "$CHROOT/opt"
    mock_chroot_ldd 'unreachable'

    # Host ldd's false "not found" must not surface; the WARN names the chroot.
    mock_command_script "ldd" 'echo "libstdc++.so.6 => not found"'

    run verify_binary_deps "ad5x"
    [ "$status" -eq 0 ]
    [ ! -s "$BATS_TEST_TMPDIR/chroot-calls" ]  # nothing to verify against
    lacks "libstdc++.so.6" "$output"     # host-side false error suppressed
    [[ "$output" == *"chroot"* ]]             # the hint names the chroot
}

@test "verify_binary_deps: a plain host still uses host-side ldd (control)" {
    setup_verify_binary
    HOST_CHROOT_STATE="none"
    mock_command_script "ldd" '
echo "	libdrm.so.2 => /usr/lib/aarch64-linux-gnu/libdrm.so.2 (0x7f1234000000)"
'

    run verify_binary_deps "pi"
    [ "$status" -eq 0 ]
    contains "All shared library dependencies satisfied" "$output"
    [[ "$output" != *"CHROOT-LDD"* ]]
}
