#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the pure-Python download/extraction fallback in
# scripts/lib/installer/common.sh (_has_python) and
# scripts/lib/installer/release.sh (_py_fetch, _py_download,
# _py_unzip_test, _py_unzip_extract).
#
# These exercise the fallback used on platforms that lack curl/wget/unzip
# but ship python3 (e.g. recent Creality K2 Tina/OpenWrt firmware).

COMMON_SH="scripts/lib/installer/common.sh"
HOST_PROFILE_SH="scripts/lib/installer/host_profile.sh"
RELEASE_SH="scripts/lib/installer/release.sh"

setup() {
    source tests/shell/helpers.bash
    export GITHUB_REPO="prestonbrown/helixscreen"

    # Source common.sh (provides _has_python) then release.sh (provides _py_*).
    # host_profile.sh sits between them in the bundle's module order: common.sh's
    # guards call into it, and a missing module is a 127 behind a later `|| true`.
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    source "$COMMON_SH"
    source "$HOST_PROFILE_SH"

    # release.sh references _has_no_new_privs (defined in service.sh); stub it.
    _has_no_new_privs() { return 1; }

    unset _HELIX_RELEASE_SOURCED
    source "$RELEASE_SH"

    # helpers.bash stubs log_* as no-ops; echo so we can assert on messages.
    log_error() { echo "ERROR: $*"; }
    log_info()  { echo "INFO: $*"; }
    export -f log_error log_info

    export TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    mkdir -p "$TMP_DIR"

    # These tests require a python interpreter with ssl + urllib (CI has one).
    if ! command -v python3 >/dev/null 2>&1 && ! command -v python >/dev/null 2>&1; then
        skip "no python interpreter available"
    fi
}

# Pick whichever python is present (CI standard is python3).
PY() { if command -v python3 >/dev/null 2>&1; then echo python3; else echo python; fi; }

# Build a flat zip with controlled permission bits via python's zipfile, so
# external_attr carries the unix mode. bin/helix-screen is stored 0644
# (deliberately NON-executable) to exercise the bin/ exec-forcing logic.
make_zip() {
    local zip=$1
    "$(PY)" - "$zip" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
def add(name, data, mode):
    zi = zipfile.ZipInfo(name)
    zi.external_attr = (mode & 0o7777) << 16
    z.writestr(zi, data)
add("bin/helix-screen", "#!/bin/sh\n", 0o644)   # deliberately NON-executable
add("config/settings.json", "{}", 0o644)
z.close()
PY
}

# =========================================================================
# _has_python
# =========================================================================

@test "_has_python: returns 0 when a python interpreter is available" {
    run _has_python
    [ "$status" -eq 0 ]
}

@test "_has_python: returns non-zero with empty PATH" {
    local empty="$BATS_TEST_TMPDIR/empty"
    mkdir -p "$empty"
    # Invoke bash by absolute path so the shell itself runs; with an empty PATH
    # `command -v python3/python` find nothing, so _has_python must fail.
    run env PATH="$empty" /bin/bash -c '
        unset _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        source scripts/lib/installer/host_profile.sh
        _has_python
    '
    [ "$status" -ne 0 ]
}

# =========================================================================
# _py_fetch
# =========================================================================

@test "_py_fetch: streams file:// body to stdout" {
    local f="$TMP_DIR/body.txt"
    printf 'helix-fetch-marker-12345\n' > "$f"
    run _py_fetch "file://$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"helix-fetch-marker-12345"* ]]
}

@test "_py_fetch: non-zero for missing file://" {
    run _py_fetch "file://$TMP_DIR/does_not_exist_xyz"
    [ "$status" -ne 0 ]
}

# =========================================================================
# _py_download
# =========================================================================

@test "_py_download: writes file:// source to dest" {
    local src="$TMP_DIR/src.bin"
    local dest="$TMP_DIR/dest.bin"
    printf 'download-payload-abcdef\n' > "$src"
    run _py_download "file://$src" "$dest"
    [ "$status" -eq 0 ]
    [ -f "$dest" ]
    grep -q "download-payload-abcdef" "$dest"
}

@test "_py_download: non-zero for missing source" {
    local dest="$TMP_DIR/dest_missing.bin"
    run _py_download "file://$TMP_DIR/no_such_source" "$dest"
    [ "$status" -ne 0 ]
    # Dest must not be left as a non-empty file from a failed transfer.
    [ ! -s "$dest" ]
}

# =========================================================================
# _py_unzip_test
# =========================================================================

@test "_py_unzip_test: accepts a valid zip" {
    local zip="$TMP_DIR/good.zip"
    make_zip "$zip"
    run _py_unzip_test "$zip"
    [ "$status" -eq 0 ]
}

@test "_py_unzip_test: rejects a non-zip file" {
    local f="$TMP_DIR/garbage.zip"
    echo "this is not a zip archive" > "$f"
    run _py_unzip_test "$f"
    [ "$status" -ne 0 ]
}

# =========================================================================
# _py_unzip_extract
# =========================================================================

@test "_py_unzip_extract: extracts entries into destdir" {
    local zip="$TMP_DIR/extract1.zip"
    local dest="$TMP_DIR/out1"
    make_zip "$zip"
    mkdir -p "$dest"
    run _py_unzip_extract "$zip" "$dest"
    [ "$status" -eq 0 ]
    [ -f "$dest/bin/helix-screen" ]
    [ -f "$dest/config/settings.json" ]
}

@test "_py_unzip_extract: forces bin/ file executable even when stored 0644" {
    # KEY REGRESSION TEST: make_zip stores bin/helix-screen as 0644 (no exec).
    # After extract it MUST be owner-executable so the service can run it.
    local zip="$TMP_DIR/extract2.zip"
    local dest="$TMP_DIR/out2"
    make_zip "$zip"
    mkdir -p "$dest"
    _py_unzip_extract "$zip" "$dest"
    [ -f "$dest/bin/helix-screen" ]
    [ -x "$dest/bin/helix-screen" ]
}

@test "_py_unzip_extract: non-bin file does NOT gain exec bit" {
    local zip="$TMP_DIR/extract3.zip"
    local dest="$TMP_DIR/out3"
    make_zip "$zip"
    mkdir -p "$dest"
    _py_unzip_extract "$zip" "$dest"
    [ -f "$dest/config/settings.json" ]
    [ ! -x "$dest/config/settings.json" ]
}
