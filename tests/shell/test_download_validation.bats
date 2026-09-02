#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for download, fetch, and tarball validation functions
# in scripts/lib/installer/release.sh

RELEASE_SH="scripts/lib/installer/release.sh"
COMMON_SH="scripts/lib/installer/common.sh"
HOST_PROFILE_SH="scripts/lib/installer/host_profile.sh"

# release.sh depends on helpers that live in common.sh (_has_python, _PY_BIN,
# ...). The bundled install.sh always carries both, so sourcing release.sh
# alone leaves `_has_python` undefined and silently drives every
# `elif _has_python` branch down its else path — the tests then pass while
# exercising code production never runs. Always load both, and clear the python
# probe cache so each test re-resolves the interpreter for its own PATH.
# host_profile.sh rides between them in the bundle's module order: common.sh's
# guards call into it, and a missing module is a 127 behind a later `|| true`.
source_installer_modules() {
    unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
    source "$COMMON_SH"
    source "$HOST_PROFILE_SH"
    source "$RELEASE_SH"
}

setup() {
    source tests/shell/helpers.bash
    export GITHUB_REPO="prestonbrown/helixscreen"

    source_installer_modules

    # Pre-set the _has_real_curl cache so mock curl scripts don't need to
    # handle --version.  Tests that exercise the "no curl" path use subshells
    # with restricted PATH and re-source release.sh, so this won't affect them.
    _REAL_CURL=yes

    # Override log_error so validate_tarball output is testable
    # (helpers.bash stubs it as a no-op, but we need the messages)
    log_error() { echo "ERROR: $*"; }
    export -f log_error

    # Isolated test environment
    export TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export CLEANUP_TMP=""

    mkdir -p "$TMP_DIR"
}

# Helper: create a valid gzip of a given size (in KB)
create_valid_gzip() {
    local dest=$1
    local size_kb=${2:-2048}
    dd if=/dev/urandom bs=1024 count="$size_kb" 2>/dev/null | gzip > "$dest"
}

# Helper: build a PATH dir containing ONLY a real python3 (no curl/wget).
# Lets us exercise the python urllib fallback in fetch_url/download_file/
# check_https_capability with no curl/wget to leak through.
make_pybin() {
    local pybin="$BATS_TEST_TMPDIR/pybin"
    mkdir -p "$pybin"
    local real
    real="$(command -v python3 2>/dev/null)" || real="$(command -v python 2>/dev/null)"
    [ -n "$real" ] || return 1
    ln -sf "$real" "$pybin/$(basename "$real")"
    echo "$pybin"
}

# =========================================================================
# fetch_url
# =========================================================================

@test "fetch_url: uses curl when available" {
    mock_command "curl" "hello from curl"
    run fetch_url "http://example.com/test"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from curl"* ]]
}

@test "fetch_url: uses wget when curl not in PATH" {
    local bin="$BATS_TEST_TMPDIR/fetch_bin"
    mkdir -p "$bin"
    cat > "$bin/wget" << 'MOCK'
#!/bin/sh
echo "hello from wget"
MOCK
    chmod +x "$bin/wget"

    # Restricted PATH: mock bin (wget only) + /usr/bin for system utilities.
    # On macOS /usr/bin/curl exists so this test skips there.
    run env PATH="$bin:/usr/bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        fetch_url "http://example.com/test"
    '
    # On macOS, /usr/bin/curl exists, so this test may still find curl.
    # Skip if curl is in /usr/bin (can't isolate on this system)
    if [ -x /usr/bin/curl ]; then
        skip "Cannot isolate from /usr/bin/curl on this system"
    fi
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from wget"* ]]
}

@test "fetch_url: returns non-zero when neither curl nor wget available" {
    local bin="$BATS_TEST_TMPDIR/empty_bin"
    mkdir -p "$bin"
    # Need basic system utils but not curl or wget
    # Symlink only the essentials we need (command is a shell built-in)
    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        fetch_url "http://example.com/test"
    '
    [ "$status" -ne 0 ]
}

@test "fetch_url: returns non-zero when curl fails" {
    mock_command_fail "curl"
    run fetch_url "http://example.com/bad"
    [ "$status" -ne 0 ]
}

@test "fetch_url: handles URL with special characters" {
    mock_command "curl" "ok"
    run fetch_url "http://example.com/path?q=hello+world&lang=en"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "fetch_url: fails gracefully with empty URL" {
    mock_command_fail "curl"
    run fetch_url ""
    [ "$status" -ne 0 ]
}

# =========================================================================
# download_file
# =========================================================================

@test "download_file: succeeds with HTTP 200 and non-empty file" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && echo "binary content" > "$dest"
        echo "200"
    '

    run download_file "http://example.com/release.tar.gz" "$TMP_DIR/test.tar.gz"
    [ "$status" -eq 0 ]
    [ -f "$TMP_DIR/test.tar.gz" ]
}

@test "download_file: fails on HTTP 404" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && echo "Not Found" > "$dest"
        echo "404"
    '

    run download_file "http://example.com/nonexistent" "$TMP_DIR/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: fails on HTTP 500" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && echo "Server Error" > "$dest"
        echo "500"
    '

    run download_file "http://example.com/error" "$TMP_DIR/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: fails when download produces empty file" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && : > "$dest"
        echo "200"
    '

    run download_file "http://example.com/empty" "$TMP_DIR/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: fails when neither curl nor wget available" {
    local bin="$BATS_TEST_TMPDIR/empty_bin"
    mkdir -p "$bin"

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        download_file "http://example.com/test" "/tmp/test.tar.gz"
    '
    [ "$status" -ne 0 ]
}

@test "download_file: fails gracefully when dest directory does not exist" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        # Writing to nonexistent dir fails silently
        [ -n "$dest" ] && echo "content" > "$dest" 2>/dev/null
        echo "200"
    '

    run download_file "http://example.com/test" "$BATS_TEST_TMPDIR/nonexistent/dir/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: uses wget when curl not in PATH" {
    local bin="$BATS_TEST_TMPDIR/dl_bin"
    mkdir -p "$bin"

    cat > "$bin/wget" << 'MOCK'
#!/bin/sh
dest=""
while [ $# -gt 0 ]; do
    case "$1" in
        -O) dest="$2"; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "$dest" ] && echo "wget content" > "$dest"
exit 0
MOCK
    chmod +x "$bin/wget"

    # Use restricted PATH with only our bin (wget) and system dirs
    # On macOS /usr/bin has curl, so skip if we can't isolate
    if [ -x /usr/bin/curl ]; then
        skip "Cannot isolate from /usr/bin/curl on this system"
    fi

    run env PATH="$bin:/usr/bin:/bin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        download_file 'http://example.com/test' '$TMP_DIR/wget_test.tar.gz'
    "
    [ "$status" -eq 0 ]
}

# =========================================================================
# validate_tarball
# =========================================================================

@test "validate_tarball: accepts valid gzip above 1MB" {
    create_valid_gzip "$TMP_DIR/good.tar.gz" 2048
    run validate_tarball "$TMP_DIR/good.tar.gz" "Test "
    [ "$status" -eq 0 ]
}

@test "validate_tarball: rejects non-gzip file" {
    echo "this is plain text, not a gzip archive" > "$TMP_DIR/fake.tar.gz"
    run validate_tarball "$TMP_DIR/fake.tar.gz" "Downloaded "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

@test "validate_tarball: rejects valid gzip that is too small" {
    echo "tiny" | gzip > "$TMP_DIR/tiny.tar.gz"
    run validate_tarball "$TMP_DIR/tiny.tar.gz" "Test "
    [ "$status" -ne 0 ]
    [[ "$output" == *"too small"* ]]
}

@test "validate_tarball: rejects empty file" {
    : > "$TMP_DIR/empty.tar.gz"
    run validate_tarball "$TMP_DIR/empty.tar.gz" "Downloaded "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

@test "validate_tarball: rejects nonexistent file" {
    run validate_tarball "$TMP_DIR/does_not_exist.tar.gz" "Test "
    [ "$status" -ne 0 ]
}

@test "validate_tarball: includes context string in error message" {
    echo "not gzip" > "$TMP_DIR/bad.tar.gz"
    run validate_tarball "$TMP_DIR/bad.tar.gz" "Downloaded "
    [ "$status" -ne 0 ]
    [[ "$output" == *"Downloaded "* ]]
}

@test "validate_tarball: rejects binary garbage (not gzip magic)" {
    dd if=/dev/urandom bs=1024 count=2048 of="$TMP_DIR/garbage.tar.gz" 2>/dev/null
    run validate_tarball "$TMP_DIR/garbage.tar.gz" "Test "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

# =========================================================================
# validate_archive: .zip path (prestonbrown/helixscreen#993)
#
# Release archives ship as .zip, and `unzip -t` support depends on the
# firmware's BusyBox vintage. Verified on-device:
#
#   BusyBox 1.29.3 (FlashForge AD5M)    no -t -> "invalid option -- 't'"
#   BusyBox 1.31.1 (Creality K1)        no -t -> "invalid option -- 't'"
#   BusyBox 1.36.1 (Elegoo Centauri)    -t present and correct
#   info-zip 6.00  (Debian/Pi/desktop)  -t present and correct
#
# Preferring `unzip -t` therefore failed every AD5M and K1 update on a
# perfectly good download. The python zipfile check needs zlib as well as
# zipfile -- the AD5M's python3.7 has none -- so it must degrade to the
# structural check rather than declaring a good archive corrupt.
# =========================================================================

# Build a zip larger than validate_archive's 1MB floor. Args: dest
create_valid_zip() {
    local dest=$1
    python3 - "$dest" << 'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("bin/helix-screen", b"\x7fELF" + os.urandom(1536 * 1024))
PY
}

# Build a zip whose compressed payload has a flipped byte, so the stored CRC no
# longer matches. Structurally intact: only a real CRC test catches this.
create_crc_corrupt_zip() {
    local dest=$1
    create_valid_zip "$dest"
    python3 - "$dest" << 'PY'
import sys
p = sys.argv[1]
data = bytearray(open(p, "rb").read())
data[len(data) // 2] ^= 0xFF
open(p, "wb").write(bytes(data))
PY
}

# Write a fake `unzip` emulating a BusyBox/info-zip flavour into a PATH dir.
# Args: bindir flavour(busybox131|busybox136)
make_fake_unzip() {
    local bindir=$1 flavour=$2
    mkdir -p "$bindir"
    case "$flavour" in
        busybox131)
            # BusyBox 1.31: -t is not a recognised option at all.
            cat > "$bindir/unzip" << 'MOCK'
#!/bin/sh
for arg in "$@"; do
    case "$arg" in
        -*t*) echo "unzip: invalid option -- 't'" >&2; exit 1 ;;
    esac
done
exec /usr/bin/unzip "$@"
MOCK
            ;;
        busybox136)
            # BusyBox 1.36: -t is accepted and always succeeds, testing nothing.
            cat > "$bindir/unzip" << 'MOCK'
#!/bin/sh
for arg in "$@"; do
    case "$arg" in
        -*t*) exit 0 ;;
    esac
done
exec /usr/bin/unzip "$@"
MOCK
            ;;
    esac
    chmod +x "$bindir/unzip"
}

@test "validate_archive: accepts valid zip" {
    create_valid_zip "$TMP_DIR/good.zip"
    run validate_archive "$TMP_DIR/good.zip" "Test "
    [ "$status" -eq 0 ]
}

@test "validate_archive: rejects CRC-corrupt zip" {
    create_crc_corrupt_zip "$TMP_DIR/crc.zip"
    run validate_archive "$TMP_DIR/crc.zip" "Downloaded "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid zip archive"* ]]
}

@test "validate_archive: rejects non-zip file named .zip" {
    dd if=/dev/urandom bs=1024 count=2048 of="$TMP_DIR/garbage.zip" 2>/dev/null
    run validate_archive "$TMP_DIR/garbage.zip" "Test "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid zip archive"* ]]
}

@test "validate_archive: rejects truncated zip (central directory lost)" {
    create_valid_zip "$TMP_DIR/trunc.zip"
    # Lop off the tail, destroying the end-of-central-directory record.
    python3 - "$TMP_DIR/trunc.zip" << 'PY'
import sys
p = sys.argv[1]
data = open(p, "rb").read()
open(p, "wb").write(data[: len(data) // 2])
PY
    run validate_archive "$TMP_DIR/trunc.zip" "Test "
    [ "$status" -ne 0 ]
}

@test "validate_archive: rejects valid zip that is too small" {
    python3 - "$TMP_DIR/tiny.zip" << 'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("a.txt", "tiny")
PY
    run validate_archive "$TMP_DIR/tiny.zip" "Test "
    [ "$status" -ne 0 ]
    [[ "$output" == *"too small"* ]]
}

@test "validate_archive: rejects nonexistent zip" {
    run validate_archive "$TMP_DIR/does_not_exist.zip" "Test "
    [ "$status" -ne 0 ]
}

# --- BusyBox flavour regressions -----------------------------------------

@test "validate_archive: accepts valid zip when unzip rejects -t (BusyBox 1.31 / K1, 1.29 / AD5M)" {
    # The #993 regression: every K1 and AD5M in-app update and Moonraker
    # install failed here with "file is not a valid zip archive" on an intact
    # download.
    local bin="$BATS_TEST_TMPDIR/bb131"
    make_fake_unzip "$bin" busybox131
    create_valid_zip "$TMP_DIR/good.zip"

    run env PATH="$bin:$PATH" /bin/bash -c '
        source tests/shell/helpers.bash
        log_error() { echo "ERROR: $*"; }
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        validate_archive "$1" "Test "
    ' _ "$TMP_DIR/good.zip"
    [ "$status" -eq 0 ]
    [[ "$output" != *"not a valid zip archive"* ]]
}

@test "validate_archive: rejects corrupt zip even when unzip -t always succeeds" {
    # Defensive: some firmware could ship a -t that reports success without
    # testing anything. The real CRC check must not be delegated to it.
    local bin="$BATS_TEST_TMPDIR/bb136"
    make_fake_unzip "$bin" busybox136
    create_crc_corrupt_zip "$TMP_DIR/crc.zip"

    run env PATH="$bin:$PATH" /bin/bash -c '
        source tests/shell/helpers.bash
        log_error() { echo "ERROR: $*"; }
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        validate_archive "$1" "Downloaded "
    ' _ "$TMP_DIR/crc.zip"
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid zip archive"* ]]
}

@test "validate_archive: falls back to unzip -l when python is unavailable" {
    # python-less system: the structural check must still accept a good zip
    # (never false-fail) while rejecting garbage.
    # (See also the AD5M case below: python present but built without zlib.)
    local bin="$BATS_TEST_TMPDIR/nopy"
    make_fake_unzip "$bin" busybox131
    for t in dd du cut sed grep head basename; do
        real=$(command -v "$t" 2>/dev/null) && ln -sf "$real" "$bin/$t"
    done
    ln -sf /bin/sh "$bin/sh"
    create_valid_zip "$TMP_DIR/good.zip"
    dd if=/dev/urandom bs=1024 count=2048 of="$TMP_DIR/garbage.zip" 2>/dev/null

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        log_error() { echo "ERROR: $*"; }
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        validate_archive "$1" "Test "
    ' _ "$TMP_DIR/good.zip"
    [ "$status" -eq 0 ]

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        log_error() { echo "ERROR: $*"; }
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        validate_archive "$1" "Test "
    ' _ "$TMP_DIR/garbage.zip"
    [ "$status" -ne 0 ]
}

@test "validate_archive: accepts valid zip when python has no zlib (AD5M)" {
    # The AD5M's python3.7 is built without zlib, so zipfile.ZipFile() raises
    # "Compression requires the (missing) zlib module" on a deflated release
    # zip. Reading that as corruption would swap one broken platform for
    # another, so the check must degrade to the structural probe instead.
    local bin="$BATS_TEST_TMPDIR/nozlib"
    make_fake_unzip "$bin" busybox131
    for t in dd du cut sed grep head basename; do
        real=$(command -v "$t" 2>/dev/null) && ln -sf "$real" "$bin/$t"
    done
    ln -sf /bin/sh "$bin/sh"

    # python3 shim whose zlib import always fails, mimicking the AD5M build.
    cat > "$bin/python3" << MOCK
#!/bin/sh
exec $(command -v python3) -c 'import sys; sys.modules["zlib"] = None; del sys.modules["zlib"]
import builtins
_real = builtins.__import__
def _blocked(name, *a, **k):
    if name == "zlib":
        raise ImportError("No module named zlib")
    return _real(name, *a, **k)
builtins.__import__ = _blocked
_argv = sys.argv[1:]
if _argv and _argv[0] == "-c":
    sys.argv = ["-c"] + _argv[2:]
    exec(_argv[1])
else:
    sys.argv = _argv or ["-"]
    exec(sys.stdin.read())
' "\$@"
MOCK
    chmod +x "$bin/python3"

    create_valid_zip "$TMP_DIR/good.zip"

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        log_error() { echo "ERROR: $*"; }
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        validate_archive "$1" "Test "
    ' _ "$TMP_DIR/good.zip"
    [ "$status" -eq 0 ]
    [[ "$output" != *"not a valid zip archive"* ]]
}

@test "validate_archive: errors when neither python nor unzip can validate" {
    local bin="$BATS_TEST_TMPDIR/notools"
    mkdir -p "$bin"
    for t in dd du cut sed grep head basename; do
        real=$(command -v "$t" 2>/dev/null) && ln -sf "$real" "$bin/$t"
    done
    ln -sf /bin/sh "$bin/sh"
    create_valid_zip "$TMP_DIR/good.zip"

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        log_error() { echo "ERROR: $*"; }
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        validate_archive "$1" "Test "
    ' _ "$TMP_DIR/good.zip"
    [ "$status" -ne 0 ]
    [[ "$output" == *"neither unzip nor python3"* ]]
}

# =========================================================================
# check_https_capability
# =========================================================================

@test "check_https_capability: returns non-zero with no curl or wget" {
    local bin="$BATS_TEST_TMPDIR/empty_bin"
    mkdir -p "$bin"

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        check_https_capability
    '
    [ "$status" -ne 0 ]
}

@test "check_https_capability: returns non-zero when curl exists but HTTPS fails" {
    local bin="$BATS_TEST_TMPDIR/https_bin"
    mkdir -p "$bin"

    # curl that always fails (simulating no SSL support)
    printf '#!/bin/sh\nexit 1\n' > "$bin/curl"
    chmod +x "$bin/curl"

    # wget whose --help does not mention https, and also fails on https URLs
    cat > "$bin/wget" << 'MOCK'
#!/bin/sh
case "$1" in
    --help) echo "BusyBox wget" ;;
    *) exit 1 ;;
esac
MOCK
    chmod +x "$bin/wget"

    # Need grep for the wget --help check inside check_https_capability
    ln -sf /usr/bin/grep "$bin/grep" 2>/dev/null || true

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        check_https_capability
    '
    [ "$status" -ne 0 ]
}

# =========================================================================
# python urllib fallback (no curl/wget in PATH)
# =========================================================================

@test "fetch_url: falls back to python when curl and wget absent" {
    local pybin
    pybin=$(make_pybin) || skip "no python interpreter available"

    local f="$BATS_TEST_TMPDIR/fetch_body.txt"
    printf 'py-fallback-fetch-7788\n' > "$f"

    run env PATH="$pybin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_RELEASE_SOURCED _PY_BIN _PY_PROBED _REAL_CURL
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        _has_no_new_privs() { return 1; }
        source scripts/lib/installer/release.sh
        fetch_url 'file://$f'
    "
    [ "$status" -eq 0 ]
    [[ "$output" == *"py-fallback-fetch-7788"* ]]
}

@test "download_file: falls back to python when curl and wget absent" {
    local pybin
    pybin=$(make_pybin) || skip "no python interpreter available"

    local src="$BATS_TEST_TMPDIR/dl_src.bin"
    local dest="$TMP_DIR/dl_py.bin"
    printf 'py-fallback-download-9900\n' > "$src"

    run env PATH="$pybin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_RELEASE_SOURCED _PY_BIN _PY_PROBED _REAL_CURL
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        _has_no_new_privs() { return 1; }
        source scripts/lib/installer/release.sh
        download_file 'file://$src' '$dest'
    "
    [ "$status" -eq 0 ]
    [ -f "$dest" ]
    grep -q "py-fallback-download-9900" "$dest"
}

@test "check_https_capability: returns 0 when only python available" {
    local pybin
    pybin=$(make_pybin) || skip "no python interpreter available"

    run env PATH="$pybin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_RELEASE_SOURCED _PY_BIN _PY_PROBED _REAL_CURL
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        _has_no_new_privs() { return 1; }
        source scripts/lib/installer/release.sh
        check_https_capability
    "
    [ "$status" -eq 0 ]
}

# =========================================================================
# use_local_tarball
# =========================================================================

@test "use_local_tarball: creates symlink for valid tarball" {
    create_valid_gzip "$BATS_TEST_TMPDIR/local_release.tar.gz" 2048

    run use_local_tarball "$BATS_TEST_TMPDIR/local_release.tar.gz"
    [ "$status" -eq 0 ]
    [ -e "$TMP_DIR/helixscreen.tar.gz" ]
}

@test "use_local_tarball: exits with error for missing file" {
    run use_local_tarball "$BATS_TEST_TMPDIR/no_such_file.tar.gz"
    [ "$status" -ne 0 ]
}

@test "use_local_tarball: exits with error for invalid gzip" {
    echo "not a real tarball" > "$BATS_TEST_TMPDIR/bad_local.tar.gz"
    run use_local_tarball "$BATS_TEST_TMPDIR/bad_local.tar.gz"
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

@test "use_local_tarball: skips symlink when src equals dest" {
    mkdir -p "$TMP_DIR"
    create_valid_gzip "$TMP_DIR/helixscreen.tar.gz" 2048

    run use_local_tarball "$TMP_DIR/helixscreen.tar.gz"
    [ "$status" -eq 0 ]
    # File should still be a regular file, not a symlink to itself
    [ -f "$TMP_DIR/helixscreen.tar.gz" ]
}

@test "use_local_tarball: falls back to copy when symlink fails" {
    create_valid_gzip "$BATS_TEST_TMPDIR/copy_test.tar.gz" 2048

    # Shadow ln with a failing script so ln -sf fails
    mock_command_fail "ln"

    run use_local_tarball "$BATS_TEST_TMPDIR/copy_test.tar.gz"
    [ "$status" -eq 0 ]
    [ -f "$TMP_DIR/helixscreen.tar.gz" ]
}

# =========================================================================
# parse_manifest_version
# =========================================================================

@test "parse_manifest_version: extracts version from valid JSON" {
    result=$(echo '{"version": "0.9.5", "assets": []}' | parse_manifest_version)
    [ "$result" = "0.9.5" ]
}

@test "parse_manifest_version: returns empty for missing version field" {
    result=$(echo '{"tag": "v1.0.0", "notes": "release"}' | parse_manifest_version)
    [ -z "$result" ]
}

@test "parse_manifest_version: returns first match with multiple version fields" {
    result=$(printf '{"version": "1.0.0"}\n{"version": "2.0.0"}\n' | parse_manifest_version)
    [ "$result" = "1.0.0" ]
}

@test "parse_manifest_version: returns empty for empty input" {
    result=$(echo "" | parse_manifest_version)
    [ -z "$result" ]
}

# =========================================================================
# parse_manifest_platform_url
# =========================================================================

MANIFEST_WITH_ASSETS='{
    "version": "0.9.5",
    "assets": {
        "pi": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz",
            "sha256": "abc123"
        },
        "ad5m": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-ad5m-v0.9.5.tar.gz",
            "sha256": "def456"
        },
        "k1": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-k1-v0.9.5.tar.gz",
            "sha256": "ghi789"
        }
    }
}'

@test "parse_manifest_platform_url: extracts correct URL for platform" {
    result=$(echo "$MANIFEST_WITH_ASSETS" | parse_manifest_platform_url "ad5m")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-ad5m-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_url: returns empty for missing platform" {
    result=$(echo "$MANIFEST_WITH_ASSETS" | parse_manifest_platform_url "windows")
    [ -z "$result" ]
}

@test "parse_manifest_platform_url: returns empty for malformed JSON" {
    result=$(echo 'not json at all {{{' | parse_manifest_platform_url "pi")
    [ -z "$result" ]
}

@test "parse_manifest_platform_url: returns correct URL among multiple platforms" {
    result=$(echo "$MANIFEST_WITH_ASSETS" | parse_manifest_platform_url "k1")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-k1-v0.9.5.tar.gz" ]
}

# =========================================================================
# Release integrity: SHA256 verification and the plain-HTTP fail-closed rule
#
# The archive is extracted over the live install as root, so a gzip/zip CRC
# ("the bytes arrived intact") is not the same question as "these are the bytes
# we published". HTTP_BASE_URL exists because BusyBox wget on K1/AD5M has no
# TLS at all, so the answer cannot be "force HTTPS" — it has to be a hash.
# =========================================================================

MANIFEST_WITH_HASHES='{
    "version": "0.99.103",
    "tag": "v0.99.103",
    "assets": {
        "pi": {
            "url": "https://releases.helixscreen.org/releases/v0.99.103/helixscreen-pi-v0.99.103.tar.gz",
            "sha256": "1111111111111111111111111111111111111111111111111111111111111111",
            "size": 12345,
            "zip_url": "https://releases.helixscreen.org/releases/v0.99.103/helixscreen-pi.zip",
            "zip_sha256": "2222222222222222222222222222222222222222222222222222222222222222",
            "zip_size": 12346
        },
        "pi32": {
            "url": "https://releases.helixscreen.org/releases/v0.99.103/helixscreen-pi32-v0.99.103.tar.gz",
            "sha256": "3333333333333333333333333333333333333333333333333333333333333333"
        },
        "k1": {
            "url": "https://releases.helixscreen.org/releases/v0.99.103/helixscreen-k1-v0.99.103.tar.gz",
            "sha256": "4444444444444444444444444444444444444444444444444444444444444444"
        },
        "k2": {
            "zip_url": "https://releases.helixscreen.org/releases/v0.99.103/helixscreen-k2.zip",
            "zip_sha256": "5555555555555555555555555555555555555555555555555555555555555555"
        }
    }
}'

# --- parse_manifest_platform_sha256 --------------------------------------

@test "parse_manifest_platform_sha256: extracts the tar.gz hash" {
    result=$(echo "$MANIFEST_WITH_HASHES" | parse_manifest_platform_sha256 "pi")
    [ "$result" = "1111111111111111111111111111111111111111111111111111111111111111" ]
}

@test "parse_manifest_platform_sha256: extracts the zip hash separately" {
    result=$(echo "$MANIFEST_WITH_HASHES" | parse_manifest_platform_sha256 "pi" zip)
    [ "$result" = "2222222222222222222222222222222222222222222222222222222222222222" ]
}

@test "parse_manifest_platform_sha256: 'sha256' does not match 'zip_sha256'" {
    # pi32 has no zip_sha256 — leaking pi's would verify the wrong artifact.
    result=$(echo "$MANIFEST_WITH_HASHES" | parse_manifest_platform_sha256 "pi32" zip)
    [ -z "$result" ]
}

@test "parse_manifest_platform_sha256: a zip-only platform yields no tar.gz hash" {
    # k2 ships only zip_sha256. Returning it for the tar.gz would check the
    # tarball against the zip's digest — a guaranteed false mismatch that
    # aborts every install on that platform.
    result=$(echo "$MANIFEST_WITH_HASHES" | parse_manifest_platform_sha256 "k2")
    [ -z "$result" ]
    result=$(echo "$MANIFEST_WITH_HASHES" | parse_manifest_platform_sha256 "k2" zip)
    [ "$result" = "5555555555555555555555555555555555555555555555555555555555555555" ]
}

@test "parse_manifest_platform_sha256: 'pi' does not match the 'pi32' block" {
    result=$(echo "$MANIFEST_WITH_HASHES" | parse_manifest_platform_sha256 "pi32")
    [ "$result" = "3333333333333333333333333333333333333333333333333333333333333333" ]
}

@test "parse_manifest_platform_sha256: empty for an unknown platform" {
    result=$(echo "$MANIFEST_WITH_HASHES" | parse_manifest_platform_sha256 "windows")
    [ -z "$result" ]
}

@test "parse_manifest_platform_sha256: empty for malformed JSON" {
    result=$(echo 'not json at all {{{' | parse_manifest_platform_sha256 "pi")
    [ -z "$result" ]
}

# --- _manifest_covers_version --------------------------------------------

@test "_manifest_covers_version: true when the channel manifest is the target version" {
    _R2_MANIFEST="$MANIFEST_WITH_HASHES"
    run _manifest_covers_version "v0.99.103"
    [ "$status" -eq 0 ]
}

@test "_manifest_covers_version: false for a pinned older --version" {
    # The channel manifest only ever describes the LATEST release. Checking an
    # old build against it would mismatch every candidate and fail the install.
    _R2_MANIFEST="$MANIFEST_WITH_HASHES"
    run _manifest_covers_version "v0.99.50"
    [ "$status" -ne 0 ]
}

@test "_manifest_covers_version: false with no manifest" {
    _R2_MANIFEST=""
    run _manifest_covers_version "v0.99.103"
    [ "$status" -ne 0 ]
}

# --- _sha256_file --------------------------------------------------------

@test "_sha256_file: matches the known digest of a known file" {
    printf 'helixscreen' > "$TMP_DIR/hashme"
    # echo -n helixscreen | sha256sum
    expected=$(printf 'helixscreen' | { sha256sum 2>/dev/null || shasum -a 256; } | awk '{print $1}')
    result=$(_sha256_file "$TMP_DIR/hashme")
    [ "$result" = "$expected" ]
    [ -n "$result" ]
}

@test "_sha256_file: empty (not a bogus digest) when no hashing tool exists" {
    # A missing tool must read as "unverifiable", never as "verified" — an old
    # BusyBox without the sha256sum applet and no python must not silently
    # produce a digest that then "matches".
    #
    # Build a PATH holding only the utilities _sha256_file legitimately needs,
    # with every hashing tool (sha256sum/shasum/openssl/python3) left out.
    local bin="$BATS_TEST_TMPDIR/nohash"
    mkdir -p "$bin"
    local tool
    for tool in awk tr cat head sed grep mktemp; do
        local real
        real=$(command -v "$tool") && ln -sf "$real" "$bin/$tool"
    done
    printf 'x' > "$TMP_DIR/hashme"

    # Absolute /bin/bash so the restricted PATH doesn't have to carry a shell.
    #
    # HELIX_TEST_REAL_SYSTEMCTL=1 keeps helpers.bash from installing its
    # systemctl shim in here: the shim runs mkdir and chmod at source time, and
    # this PATH deliberately carries neither, so it would write "command not
    # found" onto stderr and `run` would fold that into $output. The assertion
    # below is about what _sha256_file prints, so nothing else may print.
    run env PATH="$bin" HELIX_TEST_REAL_SYSTEMCTL=1 /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        source scripts/lib/installer/release.sh
        _sha256_file '$TMP_DIR/hashme'
    "
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# --- _verify_archive_hash ------------------------------------------------

@test "_verify_archive_hash: accepts a file whose digest matches" {
    printf 'payload' > "$TMP_DIR/a.zip"
    local sha
    sha=$(_sha256_file "$TMP_DIR/a.zip")
    run _verify_archive_hash "$TMP_DIR/a.zip" "$sha" https
    [ "$status" -eq 0 ]
}

@test "_verify_archive_hash: rejects a tampered file over HTTPS" {
    printf 'payload' > "$TMP_DIR/a.zip"
    local sha
    sha=$(_sha256_file "$TMP_DIR/a.zip")
    printf 'payload-with-a-backdoor' > "$TMP_DIR/a.zip"

    run _verify_archive_hash "$TMP_DIR/a.zip" "$sha" https
    [ "$status" -ne 0 ]
    [[ "$output" == *"SHA256 MISMATCH"* ]]
}

@test "_verify_archive_hash: rejects a tampered file over plain HTTP" {
    printf 'payload' > "$TMP_DIR/a.tar.gz"
    local sha
    sha=$(_sha256_file "$TMP_DIR/a.tar.gz")
    printf 'payload-with-a-backdoor' > "$TMP_DIR/a.tar.gz"

    run _verify_archive_hash "$TMP_DIR/a.tar.gz" "$sha" http
    [ "$status" -ne 0 ]
    [[ "$output" == *"SHA256 MISMATCH"* ]]
}

@test "_verify_archive_hash: no hash over HTTPS is accepted (TLS authenticated it)" {
    printf 'payload' > "$TMP_DIR/a.zip"
    run _verify_archive_hash "$TMP_DIR/a.zip" "" https
    [ "$status" -eq 0 ]
}

@test "_verify_archive_hash: no hash over plain HTTP fails closed" {
    printf 'payload' > "$TMP_DIR/a.zip"
    run _verify_archive_hash "$TMP_DIR/a.zip" "" http
    [ "$status" -ne 0 ]
    [[ "$output" == *"Refusing an unverified plain-HTTP download"* ]]
}

@test "download_release: an all-unverified HTTP cascade explains the override" {
    # The zip candidate legitimately has no hash on ZIP_EXCLUDE_PLATFORMS
    # (helixscreen#993), so the per-candidate refusal is a warning; only the
    # terminal "nothing worked" message names HELIX_ALLOW_UNVERIFIED_HTTP.
    create_valid_gzip "$BATS_TEST_TMPDIR/any.tar.gz" 2048
    export FAKE_BODY="$BATS_TEST_TMPDIR/any.tar.gz"
    _R2_MANIFEST=""                  # no hashes for anything
    _ensure_manifest() { return 1; }
    download_file()      { return 1; }   # HTTPS transports unreachable
    download_file_http() { cp "$FAKE_BODY" "$2"; }

    run download_release "v9.9.9" "ad5m"
    [ "$status" -ne 0 ]
    [[ "$output" == *"HELIX_ALLOW_UNVERIFIED_HTTP=1"* ]]
}

@test "download_release: a plain 404 cascade does NOT mention the override" {
    # Keep the security advice off unrelated failures — it would read as
    # "set this env var to fix your install" for a simple typo'd --version.
    _R2_MANIFEST=""
    _ensure_manifest() { return 1; }
    download_file()      { return 1; }
    download_file_http() { return 1; }

    run download_release "v9.9.9" "ad5m"
    [ "$status" -ne 0 ]
    refute_sh '[[ "$output" == *"HELIX_ALLOW_UNVERIFIED_HTTP"* ]]'
}

@test "_verify_archive_hash: HELIX_ALLOW_UNVERIFIED_HTTP=1 is the documented escape hatch" {
    # Needed for a pinned --version on HTTPS-incapable firmware (K1/AD5M),
    # where the channel manifest carries no hash for that build.
    printf 'payload' > "$TMP_DIR/a.zip"
    HELIX_ALLOW_UNVERIFIED_HTTP=1 run _verify_archive_hash "$TMP_DIR/a.zip" "" http
    [ "$status" -eq 0 ]
    [[ "$output" == *"UNVERIFIED"* ]]
}

# --- _try_download_candidate integration ---------------------------------

# Mock wget that writes $FAKE_BODY to -O's destination. download_file_http
# prefers wget, so this drives the plain-HTTP transport end to end.
mock_http_wget() {
    mock_command_script "wget" '
dest=""
while [ $# -gt 0 ]; do
    case "$1" in
        -O) dest="$2"; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "$dest" ] && cp "$FAKE_BODY" "$dest"
exit 0
'
}

@test "_try_download_candidate: accepts an HTTP download matching the manifest hash" {
    create_valid_gzip "$BATS_TEST_TMPDIR/genuine.tar.gz" 2048
    export FAKE_BODY="$BATS_TEST_TMPDIR/genuine.tar.gz"
    mock_http_wget
    local sha
    sha=$(_sha256_file "$FAKE_BODY")

    run _try_download_candidate "http://dl.example/x.tar.gz" "$TMP_DIR/dl.tar.gz" http "$sha"
    [ "$status" -eq 0 ]
}

@test "_try_download_candidate: rejects a swapped HTTP payload that is still valid gzip" {
    # The MITM case the gzip CRC cannot catch: a well-formed archive that is
    # simply not the one we published.
    create_valid_gzip "$BATS_TEST_TMPDIR/genuine.tar.gz" 2048
    create_valid_gzip "$BATS_TEST_TMPDIR/attacker.tar.gz" 2048
    local sha
    sha=$(_sha256_file "$BATS_TEST_TMPDIR/genuine.tar.gz")
    export FAKE_BODY="$BATS_TEST_TMPDIR/attacker.tar.gz"
    mock_http_wget

    # Sanity: the swapped archive is itself a perfectly valid gzip.
    run gunzip -t "$BATS_TEST_TMPDIR/attacker.tar.gz"
    [ "$status" -eq 0 ]

    run _try_download_candidate "http://dl.example/x.tar.gz" "$TMP_DIR/dl.tar.gz" http "$sha"
    [ "$status" -ne 0 ]
}

@test "_try_download_candidate: rejects an unhashed HTTP download" {
    create_valid_gzip "$BATS_TEST_TMPDIR/genuine.tar.gz" 2048
    export FAKE_BODY="$BATS_TEST_TMPDIR/genuine.tar.gz"
    mock_http_wget

    run _try_download_candidate "http://dl.example/x.tar.gz" "$TMP_DIR/dl.tar.gz" http ""
    [ "$status" -ne 0 ]
}

@test "download_release: every candidate is handed an expected SHA256" {
    # The integrity gate is only as good as its wiring: a candidate invoked
    # without the 4th argument silently downgrades to "no hash", which on the
    # plain-HTTP transport is the whole vulnerability again.
    local unwired
    unwired=$(grep -n '_try_download_candidate "\$' "$RELEASE_SH" \
        | grep -vE '(https|http) "\$(zip|tar)_sha"' || true)
    [ -z "$unwired" ] || fail "candidate(s) called without an expected sha256:
$unwired"
}

@test "download_release: aborts the whole install when every transport serves a tampered archive" {
    # Full-chain check: a MITM on the plain-HTTP mirror hands back a perfectly
    # valid .tar.gz that simply is not ours. Every candidate must be rejected
    # and download_release must abort rather than extract it over the install.
    create_valid_gzip "$BATS_TEST_TMPDIR/genuine.tar.gz" 2048
    create_valid_gzip "$BATS_TEST_TMPDIR/attacker.tar.gz" 2048
    local good_sha
    good_sha=$(_sha256_file "$BATS_TEST_TMPDIR/genuine.tar.gz")
    export FAKE_BODY="$BATS_TEST_TMPDIR/attacker.tar.gz"

    _R2_MANIFEST="{
        \"version\": \"9.9.9\",
        \"assets\": {
            \"ad5m\": {
                \"url\": \"https://cdn.example/releases/v9.9.9/helixscreen-ad5m-v9.9.9.tar.gz\",
                \"sha256\": \"$good_sha\"
            }
        }
    }"
    _R2_MANIFEST_TRANSPORT="http"
    _ensure_manifest() { return 0; }   # manifest already in hand; no network

    # Both transports "succeed" at the socket level and hand back the swap.
    download_file()      { cp "$FAKE_BODY" "$2"; }
    download_file_http() { cp "$FAKE_BODY" "$2"; }

    run download_release "v9.9.9" "ad5m"
    [ "$status" -ne 0 ]
    [[ "$output" == *"SHA256 MISMATCH"* ]]
    [[ "$output" == *"Failed to download release"* ]]
}

@test "download_release: accepts the genuine archive through the same chain" {
    # Same wiring, honest mirror — proves the previous test fails for the right
    # reason and that verification does not reject a legitimate download.
    create_valid_gzip "$BATS_TEST_TMPDIR/genuine.tar.gz" 2048
    local good_sha
    good_sha=$(_sha256_file "$BATS_TEST_TMPDIR/genuine.tar.gz")
    export FAKE_BODY="$BATS_TEST_TMPDIR/genuine.tar.gz"

    _R2_MANIFEST="{
        \"version\": \"9.9.9\",
        \"assets\": {
            \"ad5m\": {
                \"url\": \"https://cdn.example/releases/v9.9.9/helixscreen-ad5m-v9.9.9.tar.gz\",
                \"sha256\": \"$good_sha\"
            }
        }
    }"
    _R2_MANIFEST_TRANSPORT="http"
    _ensure_manifest() { return 0; }
    download_file()      { cp "$FAKE_BODY" "$2"; }
    download_file_http() { cp "$FAKE_BODY" "$2"; }

    run download_release "v9.9.9" "ad5m"
    [ "$status" -eq 0 ]
    [[ "$output" == *"SHA256 verified"* ]]
}
