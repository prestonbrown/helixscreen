#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# AFC live smoke test — queries all AFC objects and verifies expected fields exist.
# Usage: ./scripts/afc-test.sh [host:port]  (default: 192.168.1.100:7125)

set -euo pipefail

HOST="${1:-192.168.1.100:7125}"
BASE="http://${HOST}"
PASS=0
FAIL=0
WARN=0

green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }

query() {
    local obj="$1"
    local encoded
    encoded=$(python3 -c "import urllib.parse; print(urllib.parse.quote('$obj'))")
    curl -sf "${BASE}/printer/objects/query?${encoded}" | python3 -c "
import sys, json
data = json.load(sys.stdin)
# Extract the object's status dict
status = data.get('result', {}).get('status', {})
obj_data = status.get('$obj', {})
json.dump(obj_data, sys.stdout, indent=2)
"
}

query_objects_list() {
    curl -sf "${BASE}/printer/objects/list" | python3 -c "
import sys, json
data = json.load(sys.stdin)
objects = data.get('result', {}).get('objects', [])
for obj in objects:
    print(obj)
"
}

# check_fields <label> <json> <field>...
#
# A field prefixed with '?' is OPTIONAL: its absence is reported as informational
# rather than a failure. Two distinct reasons a key is legitimately absent:
#   - version-gated  — added by a newer AFC than the box under test
#   - state-gated    — AFC only emits it in certain states (`selector` when a
#                      selector exists, `filament_error_pos`/`current_pos` only
#                      while buffer fault tracking is armed, `endstops` only when
#                      the lane has them)
# Without this split every older or idle printer produced red FAILs and the
# signal was unusable.
check_fields() {
    local label="$1"
    local json_data="$2"
    shift 2
    local spec=("$@")

    local required=()
    local optional=()
    for f in "${spec[@]}"; do
        if [[ "$f" == \?* ]]; then
            optional+=("${f#\?}")
        else
            required+=("$f")
        fi
    done

    local present=()
    local missing=()
    local absent_optional=()
    local extra=()

    # Get actual keys from JSON
    local actual_keys
    actual_keys=$(echo "$json_data" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for k in sorted(data.keys()):
    print(k)
" 2>/dev/null || true)

    if [[ -z "$actual_keys" ]]; then
        red "  ✗ ${label}: object returned no data (not present on this printer?)"
        FAIL=$((FAIL + 1))
        return
    fi

    for field in "${required[@]}"; do
        if echo "$actual_keys" | grep -qx "$field"; then
            present+=("$field")
        else
            missing+=("$field")
        fi
    done

    for field in "${optional[@]}"; do
        if echo "$actual_keys" | grep -qx "$field"; then
            present+=("$field")
        else
            absent_optional+=("$field")
        fi
    done

    # Find unexpected fields (potential drift) — checked against BOTH lists.
    while IFS= read -r key; do
        [[ -z "$key" ]] && continue
        local found=false
        for field in "${spec[@]}"; do
            if [[ "$key" == "${field#\?}" ]]; then
                found=true
                break
            fi
        done
        if ! $found; then
            extra+=("$key")
        fi
    done <<< "$actual_keys"

    # Report
    if [[ ${#missing[@]} -eq 0 ]]; then
        green "  ✓ ${label}: all ${#required[@]} required fields present (${#present[@]} total)"
        PASS=$((PASS + 1))
    else
        red "  ✗ ${label}: missing REQUIRED fields: ${missing[*]}"
        FAIL=$((FAIL + 1))
    fi
    if [[ ${#absent_optional[@]} -gt 0 ]]; then
        printf '    · %s: optional/newer fields absent: %s\n' "$label" "${absent_optional[*]}"
    fi
    if [[ ${#extra[@]} -gt 0 ]]; then
        yellow "  ⚠ ${label}: unknown fields (drift?): ${extra[*]}"
        WARN=$((WARN + 1))
    fi
}

# AFC unit type → Klipper object prefix. Mirrors the mechanical rule in
# AmsBackendAfc::parse_afc_state: "AFC_" + type with underscores stripped, with
# ViViD as the sole exception (its Klipper filename is lowercase).
unit_klipper_key() {
    local type="$1" name="$2"
    if [[ "$type" == "ViViD" ]]; then
        echo "AFC_vivid ${name}"
    else
        echo "AFC_${type//_/} ${name}"
    fi
}

echo "=== AFC Live Smoke Test ==="
echo "Host: ${HOST}"
echo ""

# 1. Discover AFC objects
echo "--- Discovering AFC objects ---"
ALL_OBJECTS=$(query_objects_list)
AFC_OBJECTS=$(echo "$ALL_OBJECTS" | grep -i "^AFC" || true)
echo "Found AFC objects:"
echo "$AFC_OBJECTS" | sed 's/^/  /'
echo ""

# 2. Check AFC global state
echo "--- AFC Global State ---"
AFC_STATE=$(query "AFC")
check_fields "AFC" "$AFC_STATE" \
    current_load current_lane next_lane current_state \
    current_toolchange number_of_toolchanges spoolman \
    error_state bypass_state quiet_mode position_saved \
    units lanes extruders hubs buffers message led_state \
    '?td1_present' '?lane_data_enabled' \
    '?maps'

# Report the version AFC advertises alongside what the payload actually proves.
# The afc-install DB value is written by the installer and goes stale when AFC is
# updated another way, so feature-detect rather than trusting it.
echo ""
echo "--- Version cross-check ---"
DB_VERSION=$(curl -sf "${BASE}/server/database/item?namespace=afc-install" 2>/dev/null | python3 -c "
import sys, json
try:
    print(json.load(sys.stdin).get('result', {}).get('value', {}).get('version', '<unset>'))
except Exception:
    print('<unreadable>')
" 2>/dev/null || echo "<unreadable>")
echo "  afc-install DB version: ${DB_VERSION}"
echo "$AFC_STATE" | python3 -c "
import sys, json
d = json.load(sys.stdin)
# Each marker is a field whose presence implies at least that AFC version.
markers = [('lane_data_enabled', '1.0.32+'), ('td1_present', '1.0.32+'),
           ('next_lane', '1.0.32+'), ('position_saved', '1.0.32+'),
           ('maps', '1.2.0+')]
seen = [(k, v) for k, v in markers if k in d]
missing = [(k, v) for k, v in markers if k not in d]
for k, v in seen:
    print(f'  payload has {k:20} → implies {v}')
for k, v in missing:
    print(f'  payload LACKS {k:18} → older than {v}')
"

# 3. Check AFC steppers (lanes)
LANES=$(echo "$AFC_STATE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for lane in data.get('lanes', []):
    print(lane)
" 2>/dev/null || true)

echo ""
echo "--- AFC Lanes ---"
# A lane is published as EITHER `AFC_stepper <name>` (BoxTurtle/HTLF/EMU/ViViD)
# OR `AFC_lane <name>` (OpenAMS, and the synthesized standalone-toolhead lanes on
# Snapmaker U1). Same schema either way, so probe stepper first and fall back —
# querying only AFC_stepper failed every lane check on an OpenAMS box.
while IFS= read -r lane; do
    [[ -z "$lane" ]] && continue
    LANE_OBJ="AFC_stepper ${lane}"
    LANE_DATA=$(query "$LANE_OBJ")
    if [[ -z "$(echo "$LANE_DATA" | python3 -c 'import sys,json; print(",".join(json.load(sys.stdin).keys()))' 2>/dev/null)" ]]; then
        LANE_OBJ="AFC_lane ${lane}"
        LANE_DATA=$(query "$LANE_OBJ")
    fi
    check_fields "$LANE_OBJ" "$LANE_DATA" \
        name unit hub extruder buffer buffer_status \
        lane map load prep tool_loaded loaded_to_hub \
        material spool_id color weight extruder_temp \
        runout_lane filament_status filament_status_led \
        status dist_hub \
        '?bed_temp' '?remember_spool' '?filament_name' '?multi_color_hexes' \
        '?initial_weight' '?td1_td' '?td1_color' '?td1_scan_time' \
        '?selector' '?endstops'
done <<< "$LANES"

# --- Unit-level objects ---
# `units` is a list of flat "Type Name" strings; the Klipper object name is
# derived mechanically. Never checked before, so a whole object class went
# unvalidated even though the UI reads topology from it.
echo ""
echo "--- AFC Units ---"
UNITS=$(echo "$AFC_STATE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for u in data.get('units', []):
    print(u)
" 2>/dev/null || true)
while IFS= read -r unit; do
    [[ -z "$unit" ]] && continue
    if [[ "$unit" != *" "* ]]; then
        yellow "  ⚠ unit string '${unit}' has no space — cannot derive Klipper object"
        WARN=$((WARN + 1))
        continue
    fi
    U_TYPE="${unit%% *}"
    U_NAME="${unit#* }"
    U_KEY=$(unit_klipper_key "$U_TYPE" "$U_NAME")
    UNIT_DATA=$(query "$U_KEY")
    check_fields "$U_KEY" "$UNIT_DATA" lanes extruders hubs buffers
done <<< "$UNITS"

# 4. Check AFC hubs
HUBS=$(echo "$AFC_STATE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for hub in data.get('hubs', []):
    print(hub)
" 2>/dev/null || true)

echo ""
echo "--- AFC Hubs ---"
while IFS= read -r hub; do
    [[ -z "$hub" ]] && continue
    HUB_DATA=$(query "AFC_hub ${hub}")
    check_fields "AFC_hub ${hub}" "$HUB_DATA" \
        state cut cut_cmd cut_dist cut_clear cut_min_length \
        cut_servo_pass_angle cut_servo_clip_angle cut_servo_prep_angle \
        lanes afc_bowden_length
done <<< "$HUBS"

# 5. Check AFC extruders
EXTRUDERS=$(echo "$AFC_STATE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for ext in data.get('extruders', []):
    print(ext)
" 2>/dev/null || true)

echo ""
echo "--- AFC Extruders ---"
while IFS= read -r ext; do
    [[ -z "$ext" ]] && continue
    EXT_DATA=$(query "AFC_extruder ${ext}")
    check_fields "AFC_extruder ${ext}" "$EXT_DATA" \
        tool_stn tool_stn_unload tool_sensor_after_extruder \
        tool_unload_speed tool_load_speed buffer lane_loaded \
        tool_start tool_start_status tool_end tool_end_status lanes \
        '?on_shuttle' '?is_standalone' '?next_pickup' '?status'
done <<< "$EXTRUDERS"

# 6. Check AFC buffers
BUFFERS=$(echo "$AFC_STATE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
for buf in data.get('buffers', []):
    print(buf)
" 2>/dev/null || true)

echo ""
echo "--- AFC Buffers ---"
while IFS= read -r buf; do
    [[ -z "$buf" ]] && continue
    BUF_DATA=$(query "AFC_buffer ${buf}")
    # HelixScreen's buffer-health UI requires fault_detection_enabled /
    # error_sensitivity / distance_to_fault, so those are REQUIRED here even
    # though the committed afc_buffer.json fixture predates them.
    # filament_error_pos / current_pos appear only while fault tracking is armed;
    # fps_value / smoothed_fps / set_point only on type: FPS_PSF buffers.
    check_fields "AFC_buffer ${buf}" "$BUF_DATA" \
        state lanes enabled \
        fault_detection_enabled error_sensitivity distance_to_fault \
        '?rotation_distance' '?fault_timer' \
        '?active_lane' '?multiplier' '?multiplier_high' '?multiplier_low' \
        '?filament_error_pos' '?current_pos' \
        '?fps_value' '?smoothed_fps' '?set_point'
done <<< "$BUFFERS"

# Summary
echo ""
echo "=== Summary ==="
green "Passed: ${PASS}"
[[ $FAIL -gt 0 ]] && red "Failed: ${FAIL}" || echo "Failed: 0"
[[ $WARN -gt 0 ]] && yellow "Warnings: ${WARN}" || echo "Warnings: 0"
echo ""

[[ $FAIL -eq 0 ]] && green "All checks passed!" || { red "Some checks failed!"; exit 1; }
