#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch and display debug bundles from crash.helixscreen.org
#
# Usage:
#   ./scripts/debug-bundle.sh <share_code>          # Pretty-print bundle
#   ./scripts/debug-bundle.sh <share_code> --raw     # Raw JSON output
#   ./scripts/debug-bundle.sh <share_code> --save     # Save to file
#   ./scripts/debug-bundle.sh <share_code> --summary  # One-line summary

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
    echo ""
    echo "Options:"
    echo "  --raw       Output raw JSON (for piping to jq, etc.)"
    echo "  --save      Save JSON to debug-bundle-<code>.json"
    echo "  --summary   Print one-line summary"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") ZYZCAT4L"
    echo "  $(basename "$0") ZYZCAT4L --summary"
    echo "  $(basename "$0") ZYZCAT4L --raw | jq '.printer'"
    exit 1
}

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
print(f'[{code}] {ts} | v{v} | {model} | {plat} {ram}MB | klippy={state} | lang={lang} | uptime={uptime_h}h')
"
        ;;
    --pretty|*)
        echo "$BODY" | python3 -c "
import json, sys

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
    print(f'3D viewer:  {\"enabled\" if disp.get(\"gcode_3d_enabled\") else \"disabled\"}')

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
