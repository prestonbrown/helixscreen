#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/release-channel.sh
# Verifies the explicit RELEASE_CHANNEL -> R2 channel mapping that replaced the
# old "tag contains a hyphen" rule.

load helpers

SCRIPT="scripts/release-channel.sh"

setup() {
    TEST_DIR="$(mktemp -d)"
}

teardown() {
    rm -rf "$TEST_DIR"
}

# Write a RELEASE_CHANNEL file and return its path via $CHANNEL_FILE
write_channel() {
    CHANNEL_FILE="$TEST_DIR/RELEASE_CHANNEL"
    printf '%s\n' "$1" > "$CHANNEL_FILE"
}

@test "release-channel.sh passes shellcheck" {
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck "$SCRIPT"
}

@test "release-channel.sh has valid bash syntax" {
    bash -n "$SCRIPT"
}

# ---------------------------------------------------------------------------
# Channel mapping
# ---------------------------------------------------------------------------

@test "stable maps to the stable channel only, not a prerelease" {
    write_channel "stable"
    run "$SCRIPT" --file "$CHANNEL_FILE"
    [ "$status" -eq 0 ]
    contains "channel=stable" "$output"
    contains "channels=stable" "$output"
    [[ "$output" == *"prerelease=false"* ]]
}

@test "stable does NOT publish to dev" {
    # The dev manifest must only ever move forward. If a 1.0.x stable hotfix
    # also published to dev it would roll dev users back from 1.1.x.
    write_channel "stable"
    run "$SCRIPT" --file "$CHANNEL_FILE" --field channels
    [ "$status" -eq 0 ]
    [ "$output" = "stable" ]
}

@test "beta maps to beta plus dev and marks a prerelease" {
    write_channel "beta"
    run "$SCRIPT" --file "$CHANNEL_FILE"
    [ "$status" -eq 0 ]
    contains "channel=beta" "$output"
    contains "channels=beta dev" "$output"
    [[ "$output" == *"prerelease=true"* ]]
}

@test "dev maps to the dev channel only and marks a prerelease" {
    write_channel "dev"
    run "$SCRIPT" --file "$CHANNEL_FILE"
    [ "$status" -eq 0 ]
    contains "channel=dev" "$output"
    contains "channels=dev" "$output"
    [[ "$output" == *"prerelease=true"* ]]
}

@test "output parses as KEY=value lines" {
    write_channel "beta"
    run "$SCRIPT" --file "$CHANNEL_FILE"
    [ "$status" -eq 0 ]
    # Every line must be KEY=value; the workflow appends this to $GITHUB_OUTPUT
    while IFS= read -r line; do
        [[ "$line" =~ ^[a-z_]+=.*$ ]] || fail "not a KEY=value line: $line"
    done <<< "$output"
}

# ---------------------------------------------------------------------------
# --field selection
# ---------------------------------------------------------------------------

@test "--field channel prints just the declared channel" {
    write_channel "beta"
    run "$SCRIPT" --file "$CHANNEL_FILE" --field channel
    [ "$status" -eq 0 ]
    [ "$output" = "beta" ]
}

@test "--field prerelease prints just the prerelease flag" {
    write_channel "beta"
    run "$SCRIPT" --file "$CHANNEL_FILE" --field prerelease
    [ "$status" -eq 0 ]
    [ "$output" = "true" ]
}

@test "unknown --field is rejected" {
    write_channel "stable"
    run "$SCRIPT" --file "$CHANNEL_FILE" --field bogus
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown field"* ]]
}

@test "unknown argument is rejected" {
    run "$SCRIPT" --nope
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown argument"* ]]
}

# ---------------------------------------------------------------------------
# File parsing
# ---------------------------------------------------------------------------

@test "comments and surrounding whitespace are stripped" {
    CHANNEL_FILE="$TEST_DIR/RELEASE_CHANNEL"
    printf '# leading comment\n\n   beta   # trailing comment\n' > "$CHANNEL_FILE"
    run "$SCRIPT" --file "$CHANNEL_FILE" --field channel
    [ "$status" -eq 0 ]
    [ "$output" = "beta" ]
}

@test "missing RELEASE_CHANNEL file is a hard error" {
    run "$SCRIPT" --file "$TEST_DIR/does-not-exist"
    [ "$status" -eq 1 ]
    [[ "$output" == *"RELEASE_CHANNEL not found"* ]]
}

@test "empty RELEASE_CHANNEL file is a hard error" {
    CHANNEL_FILE="$TEST_DIR/RELEASE_CHANNEL"
    printf '# only comments\n\n' > "$CHANNEL_FILE"
    run "$SCRIPT" --file "$CHANNEL_FILE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"declares no channel"* ]]
}

@test "unknown channel name is a hard error, not a silent default" {
    write_channel "nightly"
    run "$SCRIPT" --file "$CHANNEL_FILE"
    [ "$status" -eq 1 ]
    contains "unknown channel" "$output"
    # Must not fall through to publishing anything
    [[ "$output" != *"channels="* ]]
}

# ---------------------------------------------------------------------------
# Tag cross-check
# ---------------------------------------------------------------------------

@test "prerelease-suffixed tag is rejected on the stable channel" {
    write_channel "stable"
    run "$SCRIPT" --file "$CHANNEL_FILE" --tag "v1.0.1-rc.1"
    [ "$status" -eq 1 ]
    [[ "$output" == *"prerelease suffix"* ]]
}

@test "plain tag is accepted on the stable channel" {
    write_channel "stable"
    run "$SCRIPT" --file "$CHANNEL_FILE" --tag "v1.0.1" --field channels
    [ "$status" -eq 0 ]
    [ "$output" = "stable" ]
}

@test "prerelease-suffixed tag is allowed on the beta channel" {
    write_channel "beta"
    run "$SCRIPT" --file "$CHANNEL_FILE" --tag "v1.1.0-rc.1" --field channels
    [ "$status" -eq 0 ]
    [ "$output" = "beta dev" ]
}

@test "plain tag is rejected on the beta channel" {
    write_channel "beta"
    run "$SCRIPT" --file "$CHANNEL_FILE" --tag "v1.1.0"
    [ "$status" -eq 1 ]
    [[ "$output" == *"set RELEASE_CHANNEL to stable"* ]]
}

@test "plain tag is rejected on the dev channel" {
    write_channel "dev"
    run "$SCRIPT" --file "$CHANNEL_FILE" --tag "v1.2.0"
    [ "$status" -eq 1 ]
    [[ "$output" == *"set RELEASE_CHANNEL to stable"* ]]
}

# ---------------------------------------------------------------------------
# The repo's own declaration
# ---------------------------------------------------------------------------

@test "the repo's RELEASE_CHANNEL is present and valid" {
    # A typo here would fail the release at upload time, after a ~2h build.
    [ -f "RELEASE_CHANNEL" ]
    run "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"channel="* ]]
}

# ---------------------------------------------------------------------------
# The signing key (docs/devel/RELEASE_SIGNING.md)
# ---------------------------------------------------------------------------

# dev_pub real|placeholder: a dev public key file, its path in $DEV_PUB.
dev_pub() {
    DEV_PUB="$TEST_DIR/dev.pub"
    if [ "$1" = real ]; then
        printf 'untrusted comment: minisign public key 0123456789ABCDEF\nRW%s\n' \
            "$(printf 'A%.0s' $(seq 54))" > "$DEV_PUB"
    else
        printf 'untrusted comment: PLACEHOLDER\nPLACEHOLDER_DEV_PUBLIC_KEY\n' > "$DEV_PUB"
    fi
}

# signing_for CHANNEL TAG: the signing field for that declaration and tag ("" for none).
signing_for() {
    write_channel "$1"
    local args=(--file "$CHANNEL_FILE" --dev-pubkey "$DEV_PUB" --field signing)
    [ -n "$2" ] && args+=(--tag "$2")
    run "$SCRIPT" "${args[@]}"
    [ "$status" -eq 0 ] || fail "$1 $2: $output"
    echo "$output"
}

@test "only an alpha publishing to dev alone is signed with the dev key" {
    dev_pub real
    local rows=(
        "dev|v1.3.0-alpha.1|dev"
        "dev|v1.3.0-alpha.5.20261124|dev"
        "dev||dev"
        "dev|v1.3.0-beta.1|main"
        "dev|v1.3.0-rc.1|main"
        "beta|v1.2.0-beta.3|main"
        "beta|v1.2.0-alpha.1|main"
        "beta||main"
        "stable|v1.2.0|main"
        "stable||main"
    )
    local row channel tag want got
    for row in "${rows[@]}"; do
        IFS='|' read -r channel tag want <<< "$row"
        got=$(signing_for "$channel" "$tag")
        [ "$got" = "$want" ] || fail "$channel $tag: signing=$got, want $want"
    done
}

@test "until the dev public key is committed every release signs with the release key" {
    dev_pub placeholder
    [ "$(signing_for dev v1.3.0-alpha.1)" = main ]
    rm -f "$DEV_PUB"
    [ "$(signing_for dev v1.3.0-alpha.1)" = main ]
}

@test "the KEY=value output carries the signing key" {
    dev_pub real
    write_channel "dev"
    run "$SCRIPT" --file "$CHANNEL_FILE" --dev-pubkey "$DEV_PUB" --tag v1.3.0-alpha.1
    [ "$status" -eq 0 ]
    contains "signing=dev" "$output"
}
