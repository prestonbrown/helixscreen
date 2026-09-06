#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for R2 CDN integration in scripts/lib/installer/release.sh
# Verifies manifest parsing and config defaults. The helpers themselves are
# exercised in test_download_validation, test_arch_validation and
# test_extract_release; this file no longer restates that they exist, which
# `setup()` guarantees by construction the moment it sources release.sh.

RELEASE_SH="scripts/lib/installer/release.sh"

setup() {
    # Source test helpers (stubs log_info, log_warn, etc.)
    source tests/shell/helpers.bash

    # Provide required globals that release.sh expects
    export GITHUB_REPO="prestonbrown/helixscreen"

    # Source the module under test
    source "$RELEASE_SH"
}

# --- Config defaults ---

@test "R2_BASE_URL defaults to releases.helixscreen.org" {
    unset R2_BASE_URL
    unset _HELIX_RELEASE_SOURCED
    source "$RELEASE_SH"
    [ "$R2_BASE_URL" = "https://releases.helixscreen.org" ]
}

@test "R2_BASE_URL can be overridden via environment" {
    export R2_BASE_URL="https://custom.example.com"
    unset _HELIX_RELEASE_SOURCED
    source "$RELEASE_SH"
    [ "$R2_BASE_URL" = "https://custom.example.com" ]
}

@test "R2_CHANNEL defaults to stable" {
    unset R2_CHANNEL
    unset _HELIX_RELEASE_SOURCED
    source "$RELEASE_SH"
    [ "$R2_CHANNEL" = "stable" ]
}

@test "R2_CHANNEL can be overridden via environment" {
    export R2_CHANNEL="dev"
    unset _HELIX_RELEASE_SOURCED
    source "$RELEASE_SH"
    [ "$R2_CHANNEL" = "dev" ]
}

# --- Manifest parsing ---

SAMPLE_MANIFEST='{
    "version": "0.9.5",
    "tag": "v0.9.5",
    "notes": "Test release",
    "published_at": "2025-01-15T12:00:00Z",
    "assets": {
        "pi": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz",
            "sha256": "abc123"
        },
        "pi32": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-pi32-v0.9.5.tar.gz",
            "sha256": "def456"
        },
        "ad5m": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-ad5m-v0.9.5.tar.gz",
            "sha256": "ghi789"
        },
        "k1": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-k1-v0.9.5.tar.gz",
            "sha256": "jkl012"
        }
    }
}'

@test "parse_manifest_version extracts version from valid manifest" {
    result=$(echo "$SAMPLE_MANIFEST" | parse_manifest_version)
    [ "$result" = "0.9.5" ]
}

@test "parse_manifest_version returns empty for malformed JSON" {
    result=$(echo '{"not_version": "foo"}' | parse_manifest_version)
    [ -z "$result" ]
}

@test "parse_manifest_version returns empty for empty input" {
    result=$(echo "" | parse_manifest_version)
    [ -z "$result" ]
}

@test "parse_manifest_platform_url extracts pi URL" {
    result=$(echo "$SAMPLE_MANIFEST" | parse_manifest_platform_url "pi")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_url extracts pi32 URL" {
    result=$(echo "$SAMPLE_MANIFEST" | parse_manifest_platform_url "pi32")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-pi32-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_url extracts ad5m URL" {
    result=$(echo "$SAMPLE_MANIFEST" | parse_manifest_platform_url "ad5m")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-ad5m-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_url extracts k1 URL" {
    result=$(echo "$SAMPLE_MANIFEST" | parse_manifest_platform_url "k1")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-k1-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_url returns empty for missing platform" {
    result=$(echo "$SAMPLE_MANIFEST" | parse_manifest_platform_url "windows")
    [ -z "$result" ]
}

@test "parse_manifest_platform_url returns empty for empty input" {
    result=$(echo "" | parse_manifest_platform_url "pi")
    [ -z "$result" ]
}

# --- Edge cases ---

@test "parse_manifest_version handles version with extra whitespace" {
    result=$(echo '{ "version" : "1.2.3" }' | parse_manifest_version)
    [ "$result" = "1.2.3" ]
}

@test "parse_manifest_platform_url does not confuse pi with pi32" {
    # "pi" grep should NOT match "pi32" lines - pi32 line contains "helixscreen-pi32-"
    # which does NOT match "helixscreen-pi-"
    result=$(echo "$SAMPLE_MANIFEST" | parse_manifest_platform_url "pi")
    lacks "pi32" "$result"
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz" ]
}


# --- Single-line JSON ---
#
# The GitHub API omits pretty-printing for requests that send no Accept header,
# which is every python urllib caller: our own _py_fetch, and the python wget
# shim Creality ships as /usr/bin/wget on the K2. A greedy regex resolves to the
# LAST quoted run on the line, so on a one-line payload it returned the tail of
# the release notes after their last escaped quote, and the installer then built
# a download URL and a staged filename out of 2KB of changelog.

COMPACT_RELEASE='{"tag_name":"v0.99.116","name":"v0.99.116","body":"### Fixed\n- **Time zones** (#1340) - UTC+7 was reachable only as \"Indochina\", which a reporter in Vietnam\n  never found.\n- **Graph colours come from the active theme** rather than a fixed palette.\n"}'

COMPACT_MANIFEST='{"version":"0.9.5","tag":"v0.9.5","notes":"Only as \"Indochina\", which a reporter never found.","assets":{"pi":{"url":"https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz","sha256":"abc123","zip_sha256":"zzz111"},"pi32":{"url":"https://releases.helixscreen.org/stable/helixscreen-pi32-v0.9.5.tar.gz","sha256":"def456","zip_sha256":"zzz222"},"k2":{"url":"https://releases.helixscreen.org/stable/helixscreen-k2-v0.9.5.tar.gz","sha256":"ghi789","zip_sha256":"zzz333"},"x86":{"url":"https://releases.helixscreen.org/stable/helixscreen-x86-v0.9.5.tar.gz","sha256":"jkl012","zip_sha256":"zzz444"}}}'

@test "parse_json_string_field extracts tag_name from single-line release JSON" {
    result=$(echo "$COMPACT_RELEASE" | parse_json_string_field tag_name)
    [ "$result" = "v0.99.116" ]
}

@test "parse_json_string_field does not leak release notes into the tag" {
    result=$(echo "$COMPACT_RELEASE" | parse_json_string_field tag_name)
    lacks "Vietnam" "$result"
    [[ "$result" != *"palette"* ]]
}

@test "parse_json_string_field ignores an escaped quote run in a string value" {
    # \"Indochina\" splits to the field `Indochina\`, which must not be
    # mistaken for a key, and the prose after it must not become a value.
    result=$(echo "$COMPACT_RELEASE" | parse_json_string_field Indochina)
    [ -z "$result" ]
}

@test "parse_manifest_version reads a single-line manifest" {
    result=$(echo "$COMPACT_MANIFEST" | parse_manifest_version)
    [ "$result" = "0.9.5" ]
}

@test "parse_manifest_platform_url picks the right platform in a single-line manifest" {
    result=$(echo "$COMPACT_MANIFEST" | parse_manifest_platform_url "k2")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-k2-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_url does not confuse pi with pi32 in a single-line manifest" {
    result=$(echo "$COMPACT_MANIFEST" | parse_manifest_platform_url "pi")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz" ]
    result=$(echo "$COMPACT_MANIFEST" | parse_manifest_platform_url "pi32")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-pi32-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_sha256 reads a single-line manifest" {
    result=$(echo "$COMPACT_MANIFEST" | parse_manifest_platform_sha256 "k2")
    [ "$result" = "ghi789" ]
    result=$(echo "$COMPACT_MANIFEST" | parse_manifest_platform_sha256 "k2" zip)
    [ "$result" = "zzz333" ]
}

# --- Transport fallback ---
#
# fetch_url has no caller-side retry: an empty result reads as "the CDN is down"
# and costs the manifest, and with it SHA256 verification. Newer K2 firmware
# ships a python urllib shim as /usr/bin/wget, whose default Python-urllib/x.y
# User-Agent our CDN front answers with HTTP 403, so committing to wget because
# it exists left those printers unable to reach the CDN at all.

_stub_wget() {
    STUB_BIN="$BATS_TEST_TMPDIR/bin"
    mkdir -p "$STUB_BIN"
    printf '%s\n' '#!/bin/sh' "$@" > "$STUB_BIN/wget"
    chmod +x "$STUB_BIN/wget"
    PATH="$STUB_BIN:$PATH"
}

@test "fetch_url passes the installer User-Agent to wget" {
    _stub_wget 'echo "$@"'
    _has_real_curl() { return 1; }
    result=$(fetch_url "https://example.invalid/manifest.json")
    [[ "$result" == *"-U helixscreen-installer/1.0"* ]]
}

@test "fetch_url retries wget bare when -U is rejected" {
    _stub_wget 'case "$*" in *-U*) exit 1;; esac' 'echo BARE_OK'
    _has_real_curl() { return 1; }
    result=$(fetch_url "https://example.invalid/manifest.json")
    [ "$result" = "BARE_OK" ]
}

@test "fetch_url falls through to python when wget yields nothing" {
    # The K2 shape: wget exists, but every attempt through it comes back empty.
    _stub_wget 'exit 1'
    _has_real_curl() { return 1; }
    _has_python() { return 0; }
    _py_fetch() { echo "PYTHON_PAYLOAD"; }
    result=$(fetch_url "https://example.invalid/manifest.json")
    [ "$result" = "PYTHON_PAYLOAD" ]
}

@test "fetch_url_http falls through to python when wget yields nothing" {
    _stub_wget 'exit 1'
    _has_real_curl() { return 1; }
    _has_python() { return 0; }
    _py_fetch() { echo "PYTHON_PAYLOAD"; }
    result=$(fetch_url_http "http://example.invalid/manifest.json")
    [ "$result" = "PYTHON_PAYLOAD" ]
}

@test "fetch_url reports failure when every transport comes back empty" {
    _stub_wget 'exit 1'
    _has_real_curl() { return 1; }
    _has_python() { return 1; }
    run fetch_url "https://example.invalid/manifest.json"
    [ "$status" -ne 0 ]
    [ -z "$output" ]
}
