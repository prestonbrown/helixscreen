#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch and display debug bundles from crash.helixscreen.org
#
# Retrieve a single bundle:
#   ./scripts/debug-bundle.sh <share_code>          # Pretty-print bundle
#   ./scripts/debug-bundle.sh <share_code> --raw     # Raw JSON output
#   ./scripts/debug-bundle.sh <share_code> --save     # Save to file
#   ./scripts/debug-bundle.sh <share_code> --summary  # One-line summary
#
# List bundles (admin only):
#   ./scripts/debug-bundle.sh --list                              # Last 20 bundles
#   ./scripts/debug-bundle.sh --list --since 2026-02-27           # Since date
#   ./scripts/debug-bundle.sh --list --until 2026-02-26           # Until date
#   ./scripts/debug-bundle.sh --list --match "0.13.8"             # Match version/model/platform
#   ./scripts/debug-bundle.sh --list --match "pi" --limit 10      # Combined filters
#
# Auth: Set HELIX_ADMIN_KEY env var, or create .env.telemetry with HELIX_TELEMETRY_ADMIN_KEY=...

set -euo pipefail

CRASH_URL="https://crash.helixscreen.org/v1/debug-bundle"

# Find admin key from .env.telemetry
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="$PROJECT_ROOT/.env.telemetry"

if [[ -z "${HELIX_ADMIN_KEY:-}" ]]; then
    if [[ -f "$ENV_FILE" ]]; then
        HELIX_ADMIN_KEY=$(grep -E '^HELIX_TELEMETRY_ADMIN_KEY=' "$ENV_FILE" | cut -d= -f2 || true)
    fi
fi

if [[ -z "${HELIX_ADMIN_KEY:-}" ]]; then
    echo "Error: No admin key found." >&2
    echo "Set HELIX_ADMIN_KEY or create $ENV_FILE with HELIX_TELEMETRY_ADMIN_KEY=..." >&2
    exit 1
fi

usage() {
    echo "Usage: $(basename "$0") <share_code> [--raw|--save|--summary]"
    echo "       $(basename "$0") --list [--since DATE] [--until DATE] [--match PATTERN] [--limit N]"
    echo ""
    echo "Options:"
    echo "  --raw       Output raw JSON (for piping to jq, etc.)"
    echo "  --save      Save JSON to debug-bundle-<code>.json in the current dir (run from /tmp, not the repo)"
    echo "  --summary   Print one-line summary"
    echo ""
    echo "List options:"
    echo "  --list      List debug bundles"
    echo "  --since     Filter bundles uploaded on or after DATE (YYYY-MM-DD)"
    echo "  --until     Filter bundles uploaded on or before DATE (YYYY-MM-DD)"
    echo "  --match     Filter by version, printer model, or platform (substring)"
    echo "  --limit     Max bundles to return (default 20, max 100)"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") ZYZCAT4L"
    echo "  $(basename "$0") ZYZCAT4L --summary"
    echo "  $(basename "$0") ZYZCAT4L --raw | jq '.printer'"
    echo "  $(basename "$0") --list"
    echo "  $(basename "$0") --list --since 2026-02-27"
    echo "  $(basename "$0") --list --match pi --limit 10"
    exit 1
}

# --- List mode ---
if [[ "${1:-}" == "--list" ]]; then
    shift
    LIST_SINCE=""
    LIST_UNTIL=""
    LIST_MATCH=""
    LIST_LIMIT=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --since) LIST_SINCE="$2"; shift 2 ;;
            --until) LIST_UNTIL="$2"; shift 2 ;;
            --match) LIST_MATCH="$2"; shift 2 ;;
            --limit) LIST_LIMIT="$2"; shift 2 ;;
            *) echo "Unknown list option: $1" >&2; usage ;;
        esac
    done

    # Build query string
    QUERY=""
    [[ -n "$LIST_SINCE" ]] && QUERY="${QUERY}&since=${LIST_SINCE}"
    [[ -n "$LIST_UNTIL" ]] && QUERY="${QUERY}&until=${LIST_UNTIL}"
    [[ -n "$LIST_MATCH" ]] && QUERY="${QUERY}&match=${LIST_MATCH}"
    [[ -n "$LIST_LIMIT" ]] && QUERY="${QUERY}&limit=${LIST_LIMIT}"
    # Remove leading &
    QUERY="${QUERY#&}"
    [[ -n "$QUERY" ]] && QUERY="?${QUERY}"

    TMPFILE=$(mktemp)
    trap 'rm -f "$TMPFILE"' EXIT

    HTTP_CODE=$(curl -s \
        -H "X-Admin-Key: $HELIX_ADMIN_KEY" \
        -o "$TMPFILE" \
        -w "%{http_code}" \
        "${CRASH_URL}${QUERY}")

    BODY=$(cat "$TMPFILE")

    if [[ "$HTTP_CODE" != "200" ]]; then
        echo "Error: HTTP $HTTP_CODE" >&2
        echo "$BODY" >&2
        exit 1
    fi

    echo "$BODY" | python3 -c "
import json, sys

data = json.load(sys.stdin)
bundles = data.get('bundles', [])

if not bundles:
    print('No bundles found.')
    sys.exit(0)

# Header
fmt = '{:<12s} {:>8s}   {:<22s} {:>9s}   {:<16s} {:>8s}'
print(fmt.format('Share Code', 'Size', 'Uploaded', 'Version', 'Printer', 'Platform'))
print('-' * 85)

for b in bundles:
    code = b['share_code']
    size_kb = b['size'] / 1024
    if size_kb >= 1024:
        size_str = f'{size_kb/1024:.1f} MB'
    else:
        size_str = f'{size_kb:.0f} KB'
    uploaded = b.get('uploaded', '')[:19].replace('T', ' ')
    if uploaded:
        uploaded += ' UTC'
    meta = b.get('metadata', {})
    version = meta.get('version', '') or ''
    printer = meta.get('printer_model', '') or ''
    platform = meta.get('platform', '') or ''
    # Truncate long printer names
    if len(printer) > 16:
        printer = printer[:15] + '…'
    line = fmt.format(code, size_str, uploaded, version, printer, platform)
    note = meta.get('user_note', '') or ''
    if note:
        note_preview = note[:60] + ('…' if len(note) > 60 else '')
        line += f'  \U0001f4dd {note_preview}'
    print(line)

if data.get('truncated'):
    print(f'\\n... more results available (use --limit to increase)')
"
    exit 0
fi

if [[ $# -lt 1 ]]; then
    usage
fi

CODE="$1"
MODE="${2:---pretty}"

# Fetch the bundle
TMPFILE=$(mktemp)
TMPJSON=$(mktemp)
trap 'rm -f "$TMPFILE" "$TMPJSON"' EXIT

HTTP_CODE=$(curl -s \
    -H "X-Admin-Key: $HELIX_ADMIN_KEY" \
    -o "$TMPFILE" \
    -w "%{http_code}" \
    "$CRASH_URL/$CODE")

# Decompress if gzipped, otherwise use as-is
if file "$TMPFILE" | grep -q gzip; then
    gunzip -c "$TMPFILE" > "$TMPJSON"
else
    cp "$TMPFILE" "$TMPJSON"
fi

BODY=$(cat "$TMPJSON")

if [[ "$HTTP_CODE" != "200" ]]; then
    echo "Error: HTTP $HTTP_CODE" >&2
    echo "$BODY" >&2
    exit 1
fi

# The commit a bundle's binary was built from, printed only when this clone
# does not have it, which marks a build from someone's fork (#2131). Read from
# the bundle's git_hash field, or else from the startup line
# "HelixScreen <version> (<hash>)" in its log tail or launcher log.
bundle_fork_hash() {
    local hash
    hash=$(python3 -c "
import json, re, sys
d = json.load(open(sys.argv[1]))
h = d.get('git_hash') or ''
if not h:
    log = ''
    for key in ('log_tail', 'launcher_log'):
        part = d.get(key) or ''
        if isinstance(part, list):
            part = '\\n'.join(str(x) for x in part)
        log += str(part) + '\\n'
    m = re.search(r'HelixScreen \\S+ \\(([0-9a-f]{7,40})\\)', log)
    h = m.group(1) if m else ''
print(h if re.fullmatch(r'[0-9a-f]{7,40}', h) else '')
" "$1" 2>/dev/null) || return 0
    [[ -n "$hash" ]] || return 0
    git -C "$PROJECT_ROOT" cat-file -e "${hash}^{commit}" 2>/dev/null && return 0
    echo "$hash"
}

BUNDLE_FORK_HASH="$(bundle_fork_hash "$TMPJSON")"
export BUNDLE_FORK_HASH

case "$MODE" in
    --raw)
        echo "$BODY"
        ;;
    --save)
        OUTFILE="debug-bundle-${CODE}.json"
        echo "$BODY" | python3 -m json.tool > "$OUTFILE"
        echo "Saved to $OUTFILE"
        ;;
    --summary)
        export BUNDLE_CODE="$CODE"
        echo "$BODY" | python3 -c "
import json, sys, os
code = os.environ.get('BUNDLE_CODE', '?')
d = json.load(sys.stdin)
p = d.get('printer', {})
s = d.get('system', {})
v = d.get('version', '?')
ts = d.get('timestamp', '?')
model = p.get('model', '?')
state = p.get('klippy_state', '?')
plat = s.get('platform', '?')
ram = s.get('total_ram_mb', '?')
lang = d.get('settings', {}).get('language', '?')
uptime_h = round(s.get('uptime_seconds', 0) / 3600, 1)
note = d.get('user_note', '')
summary = f'[{code}] {ts} | v{v} | {model} | {plat} {ram}MB | klippy={state} | lang={lang} | uptime={uptime_h}h'
fork = os.environ.get('BUNDLE_FORK_HASH', '')
if fork:
    summary += f' | FORK BUILD: commit {fork} is not in our repo'
if note:
    summary += f' | note={note[:80]}'
print(summary)
"
        ;;
    --pretty|*)
        echo "$BODY" | python3 -c "
import json, os, sys

d = json.load(sys.stdin)
p = d.get('printer', {})
s = d.get('system', {})
st = d.get('settings', {})
v = d.get('version', '?')
ts = d.get('timestamp', '?')

uptime_h = round(s.get('uptime_seconds', 0) / 3600, 1)
uptime_d = round(uptime_h / 24, 1)

print(f'=== Debug Bundle: $CODE ===')
print(f'Timestamp:  {ts}')
print(f'Version:    {v}')
fork = os.environ.get('BUNDLE_FORK_HASH', '')
if fork:
    print(f'FORK BUILD: commit {fork} is not in our repo (git fetch first if it may be newer than this clone)')
note = d.get('user_note', '')
if note:
    print(f'User Note:  {note}')
print()
print(f'--- System ---')
print(f'Platform:   {s.get(\"platform\", \"?\")}')
print(f'CPU cores:  {s.get(\"cpu_cores\", \"?\")}')
print(f'RAM:        {s.get(\"total_ram_mb\", \"?\")} MB')
print(f'Uptime:     {uptime_h}h ({uptime_d}d)')
print()
print(f'--- Printer ---')
print(f'Model:      {p.get(\"model\", \"?\")}')
print(f'Name:       {st.get(\"printer\", {}).get(\"name\", \"?\")}')
print(f'Klipper:    {p.get(\"klipper_version\", \"?\")}')
print(f'State:      {p.get(\"connection_state\", \"?\")}/{p.get(\"klippy_state\", \"?\")}')
print()
print(f'--- Settings ---')
print(f'Language:   {st.get(\"language\", \"?\")}')
print(f'Dark mode:  {st.get(\"dark_mode\", \"?\")}')
print(f'Theme:      preset {st.get(\"theme\", {}).get(\"preset\", \"?\")}')
print(f'Telemetry:  {st.get(\"telemetry_enabled\", \"?\")}')
print(f'Wizard:     {\"completed\" if st.get(\"wizard_completed\") else \"not completed\"}')

# Display settings
disp = st.get('display', {})
if disp:
    print(f'Display:    dim={disp.get(\"dim_sec\",\"?\")}s sleep={disp.get(\"sleep_sec\",\"?\")}s rotate={disp.get(\"rotate\",\"?\")}°')
    gcode_mode = disp.get(\"gcode_render_mode\", 0)
    mode_names = {0: \"Auto\", 1: \"3D View\", 2: \"2D Layers\", 3: \"Thumbnail Only\"}
    print(f'G-code:     {mode_names.get(gcode_mode, \"Unknown\")} (mode={gcode_mode})')

# Touch calibration
cal = st.get('input', {}).get('calibration', {})
if cal:
    print(f'Touch cal:  {\"valid\" if cal.get(\"valid\") else \"NOT calibrated\"}')

# Fans
fans = st.get('printer', {}).get('fans', {})
if fans:
    fan_list = [f'{k}={v}' for k, v in fans.items() if v]
    print(f'Fans:       {\"  \".join(fan_list)}')

# Filament sensors
fs = st.get('printer', {}).get('filament_sensors', {})
sensors = fs.get('sensors', [])
if sensors:
    sensor_names = [f'{s.get(\"klipper_name\",\"?\")} ({s.get(\"type\",\"?\")}/{s.get(\"role\",\"?\")})' for s in sensors]
    print(f'Fil sens:   {\", \".join(sensor_names)}')

# Hardware snapshot
hw = st.get('printer', {}).get('hardware', {}).get('last_snapshot', {})
if hw:
    print()
    print('--- Hardware ---')
    for cat in ['heaters', 'fans', 'sensors', 'leds', 'filament_sensors']:
        items = hw.get(cat, [])
        if items:
            print(f'{cat:16s} {\"  \".join(items)}')

# Logs
if d.get('log_tail'):
    print()
    print('--- Log Tail ---')
    log = d['log_tail']
    if isinstance(log, list):
        for line in log[-20:]:
            print(f'  {line}')
    else:
        for line in str(log).strip().split('\\n')[-20:]:
            print(f'  {line}')

if d.get('crash_txt'):
    print()
    print('--- Crash Info ---')
    print(d['crash_txt'])
"
        ;;
esac
