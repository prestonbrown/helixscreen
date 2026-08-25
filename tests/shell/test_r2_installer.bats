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
    [[ "$result" != *"pi32"* ]]
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz" ]
}

