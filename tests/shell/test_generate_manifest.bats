#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/generate-manifest.sh
# Verifies manifest JSON generation from release zips.

load helpers

SCRIPT="scripts/generate-manifest.sh"

# A stand-in release zip whose bytes differ per name, so digests tell them apart.
make_zip() {
    printf 'PK\003\004%s' "$1" > "$2"
}

run_manifest() {
    run bash "$SCRIPT" \
        --version "${2:-0.9.5}" --tag "v${2:-0.9.5}" --notes "Test release" \
        --dir "$1" \
        --base-url "https://releases.helixscreen.org/dev" \
        --output "$1/manifest.json"
}

setup() {
    TEST_DIR="$BATS_TEST_TMPDIR/release"
    mkdir -p "$TEST_DIR"
    for plat in pi pi32 ad5m k1; do
        make_zip "$plat" "$TEST_DIR/helixscreen-${plat}.zip"
    done
}

@test "generate-manifest.sh passes shellcheck" {
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck "$SCRIPT"
}

@test "generate-manifest.sh has valid bash syntax" {
    bash -n "$SCRIPT"
}

@test "generate-manifest.sh --help shows usage" {
    run bash "$SCRIPT" --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"Usage"* ]]
}

@test "generates valid JSON with version, tag and notes" {
    run_manifest "$TEST_DIR"
    [ "$status" -eq 0 ] || fail "generate-manifest.sh exited $status: $output"

    run jq -r '.version' "$TEST_DIR/manifest.json"
    [ "$output" = "0.9.5" ]
    run jq -r '.tag' "$TEST_DIR/manifest.json"
    [ "$output" = "v0.9.5" ]
    run jq -r '.notes' "$TEST_DIR/manifest.json"
    [ "$output" = "Test release" ]
}

@test "manifest lists every platform zip in the directory" {
    run_manifest "$TEST_DIR"
    run jq -r '.assets | keys | join(" ")' "$TEST_DIR/manifest.json"
    [ "$output" = "ad5m k1 pi pi32" ] || fail "platforms: $output"
}

@test "each platform's zip fields describe the zip" {
    run_manifest "$TEST_DIR"
    local zip="$TEST_DIR/helixscreen-pi.zip"

    run jq -r '.assets.pi.zip_url' "$TEST_DIR/manifest.json"
    [ "$output" = "https://releases.helixscreen.org/dev/helixscreen-pi.zip" ] || fail "zip_url: $output"
    run jq -r '.assets.pi.zip_sha256' "$TEST_DIR/manifest.json"
    [ "$output" = "$(sha256sum "$zip" | cut -d' ' -f1)" ] || fail "zip_sha256: $output"
    run jq -r '.assets.pi.zip_size' "$TEST_DIR/manifest.json"
    [ "$output" = "$(wc -c < "$zip" | tr -d ' ')" ] || fail "zip_size: $output"
}

@test "url, sha256 and size mirror the zip fields" {
    # The installer looks a platform up by url, and pre-zip readers of the
    # schema know no other field; with no tarball, the zip is all there is.
    run_manifest "$TEST_DIR"
    for plat in pi pi32 ad5m k1; do
        run jq -e --arg p "$plat" \
            '.assets[$p] | .url == .zip_url and .sha256 == .zip_sha256 and .size == .zip_size' \
            "$TEST_DIR/manifest.json"
        [ "$status" -eq 0 ] || fail "$plat legacy fields differ from its zip fields"
    done
}

@test "a platform entry carries its fields in a fixed order" {
    run_manifest "$TEST_DIR"
    run jq -rc '.assets.pi | keys_unsorted | join(",")' "$TEST_DIR/manifest.json"
    [ "$output" = "url,sha256,size,zip_url,zip_sha256,zip_size" ] || fail "field order: $output"
}

@test "every platform is offered its zip, BusyBox platforms included" {
    for plat in ad5x cc1 k2 snapmaker-u1; do
        make_zip "$plat" "$TEST_DIR/helixscreen-${plat}.zip"
    done
    run_manifest "$TEST_DIR"
    for plat in ad5m ad5x cc1 k1 k2 snapmaker-u1; do
        run jq -re --arg p "$plat" '.assets[$p].zip_url' "$TEST_DIR/manifest.json"
        [ "$output" = "https://releases.helixscreen.org/dev/helixscreen-${plat}.zip" ] \
            || fail "$plat zip_url: $output"
    done
}

@test "a tarball in the directory adds no platform and no field" {
    dd if=/dev/zero bs=1024 count=1 2>/dev/null | gzip > "$TEST_DIR/helixscreen-pi-v0.9.5.tar.gz"
    dd if=/dev/zero bs=1024 count=1 2>/dev/null | gzip > "$TEST_DIR/helixscreen-x86-v0.9.5.tar.gz"
    run_manifest "$TEST_DIR"
    [ "$status" -eq 0 ] || fail "generate-manifest.sh exited $status: $output"

    run jq -r '.assets | keys | join(" ")' "$TEST_DIR/manifest.json"
    [ "$output" = "ad5m k1 pi pi32" ] || fail "platforms: $output"
    run jq -r '.. | strings | select(endswith(".tar.gz"))' "$TEST_DIR/manifest.json"
    [ -z "$output" ] || fail "manifest names a tarball: $output"
}

@test "the ESP32 firmware zip is listed as esp32-ktouch with url, sha256 and size" {
    make_zip esp32 "$TEST_DIR/helixscreen-esp32-ktouch-v0.9.5.zip"
    run_manifest "$TEST_DIR"

    run jq -r '.assets | keys | join(" ")' "$TEST_DIR/manifest.json"
    [ "$output" = "ad5m esp32-ktouch k1 pi pi32" ] || fail "manifest platforms: $output"

    run jq -r '.assets["esp32-ktouch"].url' "$TEST_DIR/manifest.json"
    [ "$output" = "https://releases.helixscreen.org/dev/helixscreen-esp32-ktouch-v0.9.5.zip" ] || fail "url: $output"

    expected=$(sha256sum "$TEST_DIR/helixscreen-esp32-ktouch-v0.9.5.zip" | cut -d' ' -f1)
    run jq -r '.assets["esp32-ktouch"].sha256' "$TEST_DIR/manifest.json"
    [ "$output" = "$expected" ] || fail "sha256: $output"

    run jq -r '.assets["esp32-ktouch"].size' "$TEST_DIR/manifest.json"
    [ "$output" = "$(wc -c < "$TEST_DIR/helixscreen-esp32-ktouch-v0.9.5.zip" | tr -d ' ')" ] || fail "size: $output"

    # An ESP32 image is flashed, never self-installed: no zip_* fields that an
    # updater could mistake for an installable package.
    run jq -e '.assets["esp32-ktouch"] | has("zip_url")' "$TEST_DIR/manifest.json"
    [ "$status" -ne 0 ]
}

@test "the existing platforms are unchanged by an ESP32 zip being present" {
    bash "$SCRIPT" --version "0.9.5" --tag "v0.9.5" --notes "Test" \
        --dir "$TEST_DIR" --base-url "https://releases.helixscreen.org/dev" \
        --output "$BATS_TEST_TMPDIR/before.json"
    make_zip esp32 "$TEST_DIR/helixscreen-esp32-ktouch-v0.9.5.zip"
    bash "$SCRIPT" --version "0.9.5" --tag "v0.9.5" --notes "Test" \
        --dir "$TEST_DIR" --base-url "https://releases.helixscreen.org/dev" \
        --output "$BATS_TEST_TMPDIR/after.json"

    run jq -n --slurpfile b "$BATS_TEST_TMPDIR/before.json" --slurpfile a "$BATS_TEST_TMPDIR/after.json" \
        '($b[0].assets) == ($a[0].assets | del(.["esp32-ktouch"]))'
    [ "$output" = "true" ]
}

@test "an ESP32 zip of another version is not listed" {
    make_zip esp32 "$TEST_DIR/helixscreen-esp32-ktouch-v0.9.4.zip"
    run_manifest "$TEST_DIR"
    run jq -e '.assets | has("esp32-ktouch")' "$TEST_DIR/manifest.json"
    [ "$status" -ne 0 ]
}

@test "a directory holding only an ESP32 zip is still an error" {
    local esp_dir="$BATS_TEST_TMPDIR/esp-only"
    mkdir -p "$esp_dir"
    make_zip esp32 "$esp_dir/helixscreen-esp32-ktouch-v0.9.5.zip"
    run_manifest "$esp_dir"
    [ "$status" -ne 0 ]
}

@test "a directory holding only tarballs is an error" {
    local tar_dir="$BATS_TEST_TMPDIR/tar-only"
    mkdir -p "$tar_dir"
    dd if=/dev/zero bs=1024 count=1 2>/dev/null | gzip > "$tar_dir/helixscreen-pi-v0.9.5.tar.gz"
    run_manifest "$tar_dir"
    [ "$status" -ne 0 ] || fail "a manifest was produced from tarballs alone"
}

@test "a directory with no release asset exits non-zero" {
    local bare_dir="$BATS_TEST_TMPDIR/bare"
    mkdir -p "$bare_dir"
    # Files that are not release assets must not be mistaken for one.
    echo unrelated > "$bare_dir/README.txt"
    echo unrelated > "$bare_dir/helixscreen-notes.txt"
    run_manifest "$bare_dir"
    [ "$status" -ne 0 ] || fail "a manifest was produced from no assets"
}

@test "manifest includes published_at timestamp" {
    run_manifest "$TEST_DIR"
    run jq -re '.published_at' "$TEST_DIR/manifest.json"
    [ "$status" -eq 0 ]
    [[ "$output" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T ]]
}

@test "handles subset of platforms" {
    local sub_dir="$BATS_TEST_TMPDIR/subset"
    mkdir -p "$sub_dir"
    make_zip pi "$sub_dir/helixscreen-pi.zip"
    make_zip pi32 "$sub_dir/helixscreen-pi32.zip"
    run_manifest "$sub_dir"
    run jq -r '.assets | keys | join(" ")' "$sub_dir/manifest.json"
    [ "$output" = "pi pi32" ] || fail "platforms: $output"
}

@test "fails with missing --version" {
    run bash "$SCRIPT" \
        --tag "v0.9.5" --notes "Test" \
        --dir "$TEST_DIR" \
        --base-url "https://releases.helixscreen.org/dev" \
        --output "$TEST_DIR/manifest.json"
    [ "$status" -ne 0 ]
}

@test "fails with missing --dir" {
    run bash "$SCRIPT" \
        --version "0.9.5" --tag "v0.9.5" --notes "Test" \
        --base-url "https://releases.helixscreen.org/dev" \
        --output "$TEST_DIR/manifest.json"
    [ "$status" -ne 0 ]
}

@test "fails with missing --base-url" {
    run bash "$SCRIPT" \
        --version "0.9.5" --tag "v0.9.5" --notes "Test" \
        --dir "$TEST_DIR" \
        --output "$TEST_DIR/manifest.json"
    [ "$status" -ne 0 ]
}

@test "fails with missing --output" {
    run bash "$SCRIPT" \
        --version "0.9.5" --tag "v0.9.5" --notes "Test" \
        --dir "$TEST_DIR" \
        --base-url "https://releases.helixscreen.org/dev"
    [ "$status" -ne 0 ]
}

@test "hyphenated platform keys reach the manifest whole" {
    # There is no version half to split off a zip name, so the whole key,
    # hyphens included, is the platform.
    local zip_dir="$BATS_TEST_TMPDIR/hyphens"
    mkdir -p "$zip_dir"
    for plat in snapmaker-u1 k1-dynamic x86-desktop; do
        make_zip "$plat" "$zip_dir/helixscreen-${plat}.zip"
    done
    run_manifest "$zip_dir" "1.1.0-beta.1"
    [ "$status" -eq 0 ] || fail "generate-manifest.sh exited $status: $output"

    run jq -r '.assets | keys | join(",")' "$zip_dir/manifest.json"
    [ "$output" = "k1-dynamic,snapmaker-u1,x86-desktop" ] || fail "platform keys: [$output]"
}

@test "a platform key does not take a longer key's zip" {
    # k1-dynamic shares the helixscreen-k1 prefix: each entry must describe its
    # own bytes.
    local trap_dir="$BATS_TEST_TMPDIR/prefix"
    mkdir -p "$trap_dir"
    make_zip k1-bytes "$trap_dir/helixscreen-k1.zip"
    make_zip dynamic-bytes "$trap_dir/helixscreen-k1-dynamic.zip"
    run_manifest "$trap_dir" "1.1.0"
    [ "$status" -eq 0 ] || fail "generate-manifest.sh exited $status: $output"

    run jq -r '.assets.k1.zip_sha256' "$trap_dir/manifest.json"
    [ "$output" = "$(sha256sum "$trap_dir/helixscreen-k1.zip" | cut -d' ' -f1)" ] || fail "k1 digest"
    run jq -r '.assets["k1-dynamic"].zip_sha256' "$trap_dir/manifest.json"
    [ "$output" = "$(sha256sum "$trap_dir/helixscreen-k1-dynamic.zip" | cut -d' ' -f1)" ] || fail "k1-dynamic digest"
}

@test "unified mips aliases: k1/ad5x entries match the mips zip byte-for-byte" {
    # The unified MIPS release ships one archive copied under three names, so
    # binaries reporting retired keys keep updating from the same bytes.
    local alias_dir="$BATS_TEST_TMPDIR/aliases"
    mkdir -p "$alias_dir"
    make_zip mips "$alias_dir/helixscreen-mips.zip"
    cp "$alias_dir/helixscreen-mips.zip" "$alias_dir/helixscreen-k1.zip"
    cp "$alias_dir/helixscreen-mips.zip" "$alias_dir/helixscreen-ad5x.zip"
    run_manifest "$alias_dir" "1.1.0"
    [ "$status" -eq 0 ] || fail "generate-manifest.sh exited $status: $output"

    local mips_sha
    mips_sha=$(jq -re '.assets.mips.zip_sha256' "$alias_dir/manifest.json")
    for key in k1 ad5x; do
        run jq -re --arg k "$key" '.assets[$k].zip_sha256' "$alias_dir/manifest.json"
        [ "$status" -eq 0 ] || fail "$key entry missing from the manifest"
        [ "$output" = "$mips_sha" ] || fail "$key digest differs from mips"
    done
}
