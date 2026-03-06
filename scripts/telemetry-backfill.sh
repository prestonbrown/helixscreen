#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Backfill Analytics Engine from R2 event data.
# Reads events via the admin API and POSTs them to the backfill endpoint.
# Only backfills last 90 days (Analytics Engine retention limit).
#
# Requires HELIX_TELEMETRY_ADMIN_KEY env var.

set -euo pipefail

# Auto-load credentials from project root if available
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="$SCRIPT_DIR/../.env.telemetry"
if [[ -f "$ENV_FILE" ]] && [[ -z "${HELIX_TELEMETRY_ADMIN_KEY:-}" ]]; then
    # shellcheck source=/dev/null
    source "$ENV_FILE"
fi

API_BASE="https://telemetry.helixscreen.org"
BATCH_SIZE=250  # Events per backfill POST (keep request bodies reasonable)

# Defaults
SINCE=""
UNTIL=""
DRY_RUN=false

usage() {
    cat <<'EOF'
Usage: telemetry-backfill.sh [OPTIONS]

Backfill Analytics Engine from existing R2 telemetry data.
Reads events via admin API and writes them to Analytics Engine.

Requires HELIX_TELEMETRY_ADMIN_KEY environment variable.

Options:
  --since YYYY-MM-DD   Start date (default: 90 days ago)
  --until YYYY-MM-DD   End date (default: today)
  --dry-run            Show what would be backfilled without doing it
  -h, --help           Show this help message

Environment:
  HELIX_TELEMETRY_ADMIN_KEY   Admin API key (required)
EOF
    exit 0
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --since) [[ -n "${2:-}" ]] || die "--since requires YYYY-MM-DD"; SINCE="$2"; shift 2 ;;
        --until) [[ -n "${2:-}" ]] || die "--until requires YYYY-MM-DD"; UNTIL="$2"; shift 2 ;;
        --dry-run) DRY_RUN=true; shift ;;
        -h|--help) usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

# Prerequisites
command -v curl >/dev/null 2>&1 || die "curl is required"
command -v jq >/dev/null 2>&1 || die "jq is required (brew install jq)"
[[ -n "${HELIX_TELEMETRY_ADMIN_KEY:-}" ]] || die "HELIX_TELEMETRY_ADMIN_KEY is required"

# Cross-platform date helpers
date_to_epoch() {
    if date -d "$1" +%s >/dev/null 2>&1; then date -d "$1" +%s
    else date -j -f "%Y-%m-%d" "$1" +%s; fi
}
epoch_to_ymd() {
    if date -d "@$1" +%Y-%m-%d >/dev/null 2>&1; then date -d "@$1" +%Y-%m-%d
    else date -j -r "$1" +%Y-%m-%d; fi
}

# Resolve defaults (90 days max for Analytics Engine)
if [[ -z "$SINCE" ]]; then
    if date -d "90 days ago" +%Y-%m-%d >/dev/null 2>&1; then
        SINCE=$(date -d "90 days ago" +%Y-%m-%d)
    else
        SINCE=$(date -j -v-90d +%Y-%m-%d)
    fi
fi
[[ -z "$UNTIL" ]] && UNTIL=$(date +%Y-%m-%d)

since_epoch=$(date_to_epoch "$SINCE")
until_epoch=$(date_to_epoch "$UNTIL")
[[ "$since_epoch" -gt "$until_epoch" ]] && die "--since ($SINCE) is after --until ($UNTIL)"

echo "Backfill: $SINCE to $UNTIL"
$DRY_RUN && echo "(dry run)"
echo ""

# Auth check
http_code=$(curl -s -o /dev/null -w "%{http_code}" \
    -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
    "$API_BASE/v1/events/list?prefix=events/&limit=1")
[[ "$http_code" == "401" ]] && die "Admin API key rejected (HTTP 401)"
[[ "$http_code" != "200" ]] && die "API check failed (HTTP $http_code)"

# Generate date list
dates=()
current_epoch="$since_epoch"
while [[ "$current_epoch" -le "$until_epoch" ]]; do
    dates+=("$(epoch_to_ymd "$current_epoch")")
    current_epoch=$((current_epoch + 86400))
done

total_events=0
total_files=0
total_batches=0
total_errors=0

# Accumulator for batching events across files (use temp files to avoid arg-list-too-long)
BATCH_FILE=$(mktemp)
echo '[]' > "$BATCH_FILE"
batch_count=0
trap 'rm -f "$BATCH_FILE"' EXIT

flush_batch() {
    if [[ "$batch_count" -eq 0 ]]; then return; fi
    if $DRY_RUN; then
        echo "  Would backfill batch of $batch_count events"
        total_batches=$((total_batches + 1))
        echo '[]' > "$BATCH_FILE"
        batch_count=0
        return
    fi

    local body_file
    body_file=$(mktemp)
    jq -n --slurpfile events "$BATCH_FILE" '{ events: $events[0] }' > "$body_file"

    local http_code
    http_code=$(curl -s -o /dev/null -w "%{http_code}" \
        -X POST \
        -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
        -H "Content-Type: application/json" \
        -d @"$body_file" \
        "$API_BASE/v1/admin/backfill")

    rm -f "$body_file"

    if [[ "$http_code" == "200" ]]; then
        total_batches=$((total_batches + 1))
    else
        echo "  WARNING: Backfill batch failed (HTTP $http_code)"
        total_errors=$((total_errors + 1))
    fi

    echo '[]' > "$BATCH_FILE"
    batch_count=0
}

for d in "${dates[@]}"; do
    prefix="events/${d:0:4}/${d:5:2}/${d:8:2}/"
    echo "Processing $prefix ..."

    # List all R2 keys for this date (paginated)
    all_keys=""
    cursor=""
    while true; do
        list_url="$API_BASE/v1/events/list?prefix=$prefix&limit=1000"
        [[ -n "$cursor" ]] && list_url="${list_url}&cursor=${cursor}"

        list_response=$(curl -sf \
            -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
            "$list_url" 2>&1) || {
            echo "  WARNING: Failed to list $prefix"
            total_errors=$((total_errors + 1))
            break
        }

        page_keys=$(echo "$list_response" | jq -r '.keys[].key // empty' 2>/dev/null) || page_keys=""
        if [[ -n "$page_keys" ]]; then
            [[ -n "$all_keys" ]] && all_keys="$all_keys"$'\n'"$page_keys" || all_keys="$page_keys"
        fi

        truncated=$(echo "$list_response" | jq -r '.truncated // false' 2>/dev/null)
        if [[ "$truncated" == "true" ]]; then
            cursor=$(echo "$list_response" | jq -r '.cursor // empty' 2>/dev/null)
            [[ -z "$cursor" ]] && break
        else
            break
        fi
    done

    [[ -z "$all_keys" ]] && { echo "  No events."; continue; }

    file_count=$(echo "$all_keys" | wc -l | tr -d ' ')
    echo "  Found $file_count file(s)."

    while IFS= read -r key; do
        [[ -z "$key" ]] && continue
        total_files=$((total_files + 1))

        # Download events from R2
        encoded_key=$(python3 -c "import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=''))" "$key")
        events_json=$(curl -sf \
            -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
            "$API_BASE/v1/events/get?key=$encoded_key" 2>/dev/null) || {
            echo "  WARNING: Failed to download $key"
            total_errors=$((total_errors + 1))
            continue
        }

        # Count events in this file
        file_event_count=$(echo "$events_json" | jq 'length' 2>/dev/null) || file_event_count=0
        [[ "$file_event_count" -eq 0 ]] && continue

        # Merge into batch (use temp files to avoid shell arg-list-too-long)
        tmp_merge=$(mktemp)
        echo "$events_json" | jq -s --slurpfile existing "$BATCH_FILE" '$existing[0] + .[0]' > "$tmp_merge"
        mv "$tmp_merge" "$BATCH_FILE"
        batch_count=$((batch_count + file_event_count))
        total_events=$((total_events + file_event_count))

        # Flush if batch is full
        if [[ "$batch_count" -ge "$BATCH_SIZE" ]]; then
            flush_batch
        fi
    done <<< "$all_keys"
done

# Flush any remaining events
flush_batch

echo ""
echo "--- Summary ---"
echo "Files processed:   $total_files"
echo "Events backfilled: $total_events"
echo "Batches sent:      $total_batches"
[[ "$total_errors" -gt 0 ]] && echo "Errors:            $total_errors"
$DRY_RUN && echo "(dry run — nothing was actually written)"
