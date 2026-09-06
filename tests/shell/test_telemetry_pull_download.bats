#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/telemetry-pull.sh — dry run, caching, force, multi-day, summary, output
# Split from test_telemetry_pull.bats for parallel execution.

SCRIPT="scripts/telemetry-pull.sh"

setup() {
    source tests/shell/helpers.bash

    export HELIX_TELEMETRY_ADMIN_KEY="test-key-abc123"

    # Create a fake project structure so the script can resolve PROJECT_ROOT
    export TEST_PROJECT="$BATS_TEST_TMPDIR/project"
    mkdir -p "$TEST_PROJECT/scripts"
    cp "$SCRIPT" "$TEST_PROJECT/scripts/telemetry-pull.sh"
    chmod +x "$TEST_PROJECT/scripts/telemetry-pull.sh"

    SCRIPT_PATH="$TEST_PROJECT/scripts/telemetry-pull.sh"
}

# --- Helper: create a mock curl that simulates the API ---

# Build a curl mock that returns 200 for auth check and configurable list responses.
# Usage: setup_mock_api [key_lines]
#   key_lines: newline-separated keys to return from list endpoint (empty = no events)
setup_mock_api() {
    local keys="${1:-}"
    mkdir -p "$BATS_TEST_TMPDIR/bin"

    # Build the JSON response for list endpoint
    local json_keys=""
    if [ -n "$keys" ]; then
        local first=true
        while IFS= read -r k; do
            [ -z "$k" ] && continue
            if $first; then
                json_keys="{\"key\":\"$k\"}"
                first=false
            else
                json_keys="$json_keys,{\"key\":\"$k\"}"
            fi
        done <<< "$keys"
    fi

    cat > "$BATS_TEST_TMPDIR/bin/curl" << 'MOCK_OUTER'
#!/bin/bash
KEYS_JSON='KEYS_PLACEHOLDER'
# Parse args to figure out what curl is being asked to do
output_file=""
write_out=""
silent=false
url=""
args=("$@")
i=0
while [ $i -lt ${#args[@]} ]; do
    case "${args[$i]}" in
        -o) output_file="${args[$((i+1))]}"; i=$((i+2)) ;;
        -w) write_out="${args[$((i+1))]}"; i=$((i+2)) ;;
        -s|-f|-sf|-fs) i=$((i+1)) ;;
        -H) i=$((i+2)) ;;  # skip header
        *) url="${args[$i]}"; i=$((i+1)) ;;
    esac
done

# Auth check: returns http_code via -w
if [[ "$write_out" == *"http_code"* ]]; then
    if [ -n "$output_file" ]; then
        echo -n "" > "$output_file"
    fi
    echo -n "200"
    exit 0
fi

# List endpoint
if [[ "$url" == *"/v1/events/list"* ]]; then
    echo "{\"keys\":[$KEYS_JSON],\"truncated\":false}"
    exit 0
fi

# Get endpoint (download)
if [[ "$url" == *"/v1/events/get"* ]]; then
    if [ -n "$output_file" ]; then
        echo '{"event":"test","ts":1234567890}' > "$output_file"
    else
        echo '{"event":"test","ts":1234567890}'
    fi
    exit 0
fi

echo "MOCK_CURL: unhandled request: $url" >&2
exit 1
MOCK_OUTER

    # Substitute the keys JSON into the mock
    sed -i.bak "s|KEYS_PLACEHOLDER|$json_keys|" "$BATS_TEST_TMPDIR/bin/curl"
    rm -f "$BATS_TEST_TMPDIR/bin/curl.bak"
    chmod +x "$BATS_TEST_TMPDIR/bin/curl"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

# ===== Dry run =====

@test "dry run shows would-download message without creating files" {
    setup_mock_api "events/2025/01/15/1705312000000-abc123.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15" --dry-run
    [ "$status" -eq 0 ]
    contains "dry run" "$output"
    contains "Would download" "$output"
    contains "nothing was actually downloaded" "$output"

    # No files should have been created in the data dir
    [ ! -d "$TEST_PROJECT/.telemetry-data/events/2025/01/15" ]
}

@test "dry run reports correct download count" {
    setup_mock_api "events/2025/01/15/1705312000000-aaa111.json
events/2025/01/15/1705312000000-bbb222.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15" --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"Downloaded:        2"* ]]
}

# ===== Incremental caching =====

@test "skips existing non-empty files without --force" {
    setup_mock_api "events/2025/01/15/1705312000000-abc123.json"

    # Pre-create the file with content
    local_dir="$TEST_PROJECT/.telemetry-data/events/2025/01/15"
    mkdir -p "$local_dir"
    echo '{"cached":"true"}' > "$local_dir/1705312000000-abc123.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15"
    [ "$status" -eq 0 ]
    contains "Skipped (cached):  1" "$output"

    # File should still have original content (not overwritten)
    local content
    content=$(cat "$local_dir/1705312000000-abc123.json")
    [[ "$content" == *"cached"* ]]
}

@test "does not skip empty files" {
    setup_mock_api "events/2025/01/15/1705312000000-abc123.json"

    # Pre-create an empty file
    local_dir="$TEST_PROJECT/.telemetry-data/events/2025/01/15"
    mkdir -p "$local_dir"
    touch "$local_dir/1705312000000-abc123.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15"
    [ "$status" -eq 0 ]
    contains "Downloaded:        1" "$output"
    [[ "$output" == *"Skipped (cached):  0"* ]]
}

# ===== Force mode =====

@test "force re-downloads existing files" {
    setup_mock_api "events/2025/01/15/1705312000000-abc123.json"

    # Pre-create the file with old content
    local_dir="$TEST_PROJECT/.telemetry-data/events/2025/01/15"
    mkdir -p "$local_dir"
    echo '{"old":"data"}' > "$local_dir/1705312000000-abc123.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15" --force
    [ "$status" -eq 0 ]
    contains "Downloaded:        1" "$output"
    # Should NOT show skipped
    contains "Skipped (cached):  0" "$output"

    # File should be overwritten with mock data
    local content
    content=$(cat "$local_dir/1705312000000-abc123.json")
    [[ "$content" == *"test"* ]]
}

# ===== Default dates =====

@test "default dates use last 30 days to today" {
    setup_mock_api

    run bash "$SCRIPT_PATH"
    [ "$status" -eq 0 ]

    # Should contain today's date as the until date
    local today
    today=$(date +%Y-%m-%d)
    contains "$today" "$output"

    # Compute expected since date (30 days ago)
    local since
    if date -d "30 days ago" +%Y-%m-%d >/dev/null 2>&1; then
        since=$(date -d "30 days ago" +%Y-%m-%d)
    else
        since=$(date -j -v-30d +%Y-%m-%d)
    fi
    [[ "$output" == *"$since"* ]]
}

# ===== Multi-day range =====

@test "multi-day range checks each date" {
    setup_mock_api

    run bash "$SCRIPT_PATH" --since "2025-01-14" --until "2025-01-16"
    [ "$status" -eq 0 ]
    contains "events/2025/01/14/" "$output"
    contains "events/2025/01/15/" "$output"
    [[ "$output" == *"events/2025/01/16/"* ]]
}

# ===== Summary output =====

@test "summary shows total files found" {
    setup_mock_api "events/2025/01/15/1705312000000-aaa111.json
events/2025/01/15/1705312000000-bbb222.json
events/2025/01/15/1705312000000-ccc333.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15"
    [ "$status" -eq 0 ]
    contains "Total files found: 3" "$output"
    [[ "$output" == *"Downloaded:        3"* ]]
}

@test "no events found shows appropriate message" {
    setup_mock_api ""

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15"
    [ "$status" -eq 0 ]
    contains "No events found" "$output"
    [[ "$output" == *"Total files found: 0"* ]]
}

# ===== Output directory =====

@test "creates output directory structure from key paths" {
    setup_mock_api "events/2025/01/15/1705312000000-abc123.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15"
    [ "$status" -eq 0 ]

    # The directory should have been created
    [ -d "$TEST_PROJECT/.telemetry-data/events/2025/01/15" ]
    # The file should exist with content
    [ -s "$TEST_PROJECT/.telemetry-data/events/2025/01/15/1705312000000-abc123.json" ]
}

# ===== Flag combinations =====

@test "force and dry-run together shows would-download for existing files" {
    setup_mock_api "events/2025/01/15/1705312000000-abc123.json"

    # Pre-create file
    local_dir="$TEST_PROJECT/.telemetry-data/events/2025/01/15"
    mkdir -p "$local_dir"
    echo '{"cached":"true"}' > "$local_dir/1705312000000-abc123.json"

    run bash "$SCRIPT_PATH" --since "2025-01-15" --until "2025-01-15" --force --dry-run
    [ "$status" -eq 0 ]
    contains "Would download" "$output"
    contains "nothing was actually downloaded" "$output"

    # Original file should be untouched
    local content
    content=$(cat "$local_dir/1705312000000-abc123.json")
    [[ "$content" == *"cached"* ]]
}

@test "same day for --since and --until is valid" {
    setup_mock_api
    run bash "$SCRIPT_PATH" --since "2025-03-01" --until "2025-03-01"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Telemetry pull: 2025-03-01 to 2025-03-01"* ]]
}
