#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: printer_seed
# Generic, data-driven per-printer settings.json seeding (GitHub #986).
#
# Some printers need known-good device-level defaults written into the install's
# settings.json BEFORE the app starts for the first time:
#   - the touch-calibration matrix (top-level "input" block) — the first-run
#     touch-calibration wizard reads settings.json /input/calibration/valid at
#     startup, before Moonraker is even connected, so it MUST be pre-baked.
#   - display orientation/panel config (top-level "display" block, e.g. rotate)
#     — needed for the very first boot frame to render right-side-up.
#
# Single source of truth (#986): the source of seed data is the runtime preset
# at assets/config/presets/<printer_id>.json. The installer reads ONLY the two
# install-critical, pre-Moonraker device-level blocks from it: top-level "input"
# and top-level "display". It deliberately does NOT seed:
#   - the preset's "printer" block (heaters/fans/leds/filament_sensors), nor
#   - the top-level "preset" key.
# Both are applied by the app's runtime preset loader instead. The app's
# PrinterDetector::auto_detect_and_save() (src/printer/printer_detector.cpp)
# applies the FULL preset (printer.* + set_preset → has_preset() → wizard
# preset-mode) once Moonraker connects. If the installer pre-set "preset" or a
# printer_type, that path would hit the "already set" branch and SKIP that full
# apply. So the installer's job is strictly the two device-level blocks; the
# rest is the app's responsibility.
#
# The seeded blocks are deep-merged WITHOUT clobbering any keys the user has
# already set: existing settings always win (the preset is the lower-priority
# side).
#
# Seeded ids are recorded to ${INSTALL_DIR}/config/.seeded_settings so uninstall
# can be aware of what the installer wrote.
#
# Reads: INSTALL_DIR, SUDO (and HELIX_PRESET_DIR override for tests)
# Calls: file_sudo() from common.sh

# Source guard
[ -n "${_HELIX_PRINTER_SEED_SOURCED:-}" ] && return 0
_HELIX_PRINTER_SEED_SOURCED=1

# Resolve the directory holding the runtime presets (single source of truth).
# Override with HELIX_PRESET_DIR (tests). Otherwise prefer the installed copy
# under ${INSTALL_DIR}/assets/config/presets, then fall back to the in-repo
# source tree (dev installer running from a checkout).
_helix_preset_dir() {
    if [ -n "${HELIX_PRESET_DIR:-}" ]; then
        echo "$HELIX_PRESET_DIR"
        return 0
    fi
    if [ -n "${INSTALL_DIR:-}" ] && [ -d "${INSTALL_DIR}/assets/config/presets" ]; then
        echo "${INSTALL_DIR}/assets/config/presets"
        return 0
    fi
    # Dev checkout: <repo>/assets/config/presets, relative to this module.
    local _self_dir
    _self_dir="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || _self_dir=""
    if [ -n "$_self_dir" ] && [ -d "${_self_dir}/../../../assets/config/presets" ]; then
        (cd "${_self_dir}/../../../assets/config/presets" && pwd)
        return 0
    fi
    echo "${INSTALL_DIR:-/opt/helixscreen}/assets/config/presets"
}

# Record a seeded printer-id to the state file (for uninstall awareness).
# Args: $1 = printer_id
_record_seeded_settings() {
    local printer_id="$1"
    local state_file="${INSTALL_DIR}/config/.seeded_settings"

    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    # Don't duplicate.
    if [ -f "$state_file" ] && grep -qF "settings:${printer_id}" "$state_file" 2>/dev/null; then
        return 0
    fi
    echo "settings:${printer_id}" | $(file_sudo "${INSTALL_DIR}/config") tee -a "$state_file" >/dev/null
}

# Pre-bake the install-critical device-level subset of a printer's runtime
# preset into the install's settings.json. Only the top-level "input" and
# "display" blocks (see SEED_BLOCKS in the embedded Python) are extracted and
# deep-merged; the preset's "printer" block and top-level "preset" key are
# deliberately omitted — the app's runtime PrinterDetector applies the full
# preset once Moonraker connects. Existing user keys win over the seeded subset
# (the preset is the lower-priority side). Creates settings.json from the subset
# if it is absent or empty.
#
# Graceful by design: a missing preset is a no-op success, and an absent
# python3 logs a warning and skips WITHOUT failing the install.
#
# Args: $1 = printer_id
seed_settings_for_printer() {
    local printer_id="$1"
    [ -n "$printer_id" ] || return 0

    local preset_dir
    preset_dir="$(_helix_preset_dir)"
    local fragment="${preset_dir}/${printer_id}.json"

    # Missing preset → nothing to seed for this printer. Success.
    if [ ! -f "$fragment" ]; then
        return 0
    fi

    # JSON deep-merge requires python3. jq is NOT guaranteed on minimal targets
    # (established #969 fallback pattern), and a deep recursive merge in pure
    # shell is unsafe. If python3 is unavailable, warn and skip — never fail.
    if ! command -v python3 >/dev/null 2>&1; then
        log_warn "python3 not available; skipping settings seed for ${printer_id}"
        return 0
    fi

    # Extract the install-critical device-level blocks from the preset and
    # strip any "_"-prefixed provenance keys; merge_settings_defaults then fills
    # only keys the existing settings do not already define.
    local subset
    subset=$(FRAGMENT_PATH="$fragment" python3 - <<'PY'
import json, os

# The ONLY preset blocks baked into the user's settings.json BEFORE first launch.
# These are the install-critical, pre-Moonraker device-level blocks:
#   - "input":   touch calibration. The first-run touch-calibration wizard reads
#                settings.json /input/calibration/valid at startup, before the
#                app connects to Moonraker, so it MUST be pre-baked.
#   - "display": panel orientation/config (e.g. rotate) needed for the first
#                boot frame to render right-side-up.
# Everything else (the preset's "printer" block and the top-level "preset" key)
# is deliberately NOT seeded — the app's PrinterDetector applies the FULL preset
# once Moonraker connects. Pre-setting "preset" here would make that path skip
# the full apply (the "already set" branch). Add a block here ONLY if the app
# reads it before its runtime preset loader runs.
SEED_BLOCKS = ["input", "display"]

def strip_underscore(obj):
    """Recursively drop any "_"-prefixed keys (provenance/metadata) so the
    preset's nested documentation (e.g. input.calibration._comment) never leaks
    into the user's settings.json."""
    if isinstance(obj, dict):
        return {k: strip_underscore(v) for k, v in obj.items()
                if not k.startswith("_")}
    if isinstance(obj, list):
        return [strip_underscore(v) for v in obj]
    return obj

try:
    with open(os.environ["FRAGMENT_PATH"]) as f:
        preset = json.load(f)
except (ValueError, OSError):
    preset = {}
out = {}
for key in SEED_BLOCKS:
    if isinstance(preset, dict) and isinstance(preset.get(key), dict):
        out[key] = strip_underscore(preset[key])
print(json.dumps(out))
PY
    ) || subset=""
    if [ -z "$subset" ]; then
        log_warn "Failed to merge settings seed for ${printer_id}; leaving settings.json unchanged"
        return 0
    fi

    log_info "Seeding settings defaults for ${printer_id}..."
    if merge_settings_defaults "$subset"; then
        _record_seeded_settings "$printer_id"
        log_success "Seeded settings defaults for ${printer_id}"
    else
        log_warn "Could not write seeded settings.json for ${printer_id}"
    fi

    return 0
}

# Deep-merge a JSON object into the install's settings.json, filling only keys
# it lacks: a value already there always wins. Creates settings.json when it is
# absent or empty. An unparseable one is left alone for Config::init, which
# keeps it as .corrupt and recovers from the rolling backup.
#
# The write renames a synced temp file into place, as ConfigStorageFile::store
# does, so power loss leaves the old file or the new one, never a truncated
# one. Through a settings.json symlink into printer_data, the temp file and the
# rename target are both the link's resolved file: the rename stays inside one
# directory and the link survives.
# Args: $1 = JSON object text
# Returns 1 when python3 is missing, settings.json is unparseable, or the merge
# or write failed.
merge_settings_defaults() {
    command -v python3 >/dev/null 2>&1 || return 1

    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    local settings="${INSTALL_DIR}/config/settings.json"
    if [ -L "$settings" ]; then
        settings=$(readlink -f "$settings" 2>/dev/null) || settings=""
        [ -n "$settings" ] || return 1
    fi
    local target_dir
    target_dir=$(dirname "$settings")
    local tmp_out="${target_dir}/.settings.json.seed.$$"

    if ! SETTINGS_PATH="$settings" FRAGMENT_JSON="$1" TMP_OUT="$tmp_out" python3 - <<'PY'
import json, os, sys

def load(path):
    if not os.path.exists(path):
        return {}
    try:
        with open(path) as f:
            data = f.read().strip()
        if not data:
            return {}
        obj = json.loads(data)
    except (ValueError, OSError):
        sys.exit(3)
    if not isinstance(obj, dict):
        sys.exit(3)
    return obj

def deep_merge(base, frag, top_level=False):
    """Return base with frag's keys filled in where base lacks them.
    Existing base values always win (fragment is lower priority)."""
    for k, v in frag.items():
        # Never copy a top-level "_"-prefixed provenance key into the user's
        # settings.json.
        if top_level and k.startswith("_"):
            continue
        if k in base and isinstance(base[k], dict) and isinstance(v, dict):
            deep_merge(base[k], v)
        elif k not in base:
            base[k] = v
        # else: base already has a non-dict value → keep it (user wins)
    return base

merged = deep_merge(load(os.environ["SETTINGS_PATH"]),
                    json.loads(os.environ["FRAGMENT_JSON"]), top_level=True)
with open(os.environ["TMP_OUT"], "w") as f:
    json.dump(merged, f, indent=2)
    f.write("\n")
    f.flush()
    os.fsync(f.fileno())
PY
    then
        rm -f "$tmp_out" 2>/dev/null || true
        return 1
    fi

    if ! $(file_sudo "$target_dir") mv "$tmp_out" "$settings" 2>/dev/null; then
        rm -f "$tmp_out" 2>/dev/null || true
        return 1
    fi
    return 0
}

# Persist a channel match_channel_to_version derived from a prerelease version
# as the app's /update/channel, or its updater offers stable while Moonraker's
# update_manager follows beta. A channel already in settings.json is the user's
# and stays. Runs after setup_config_symlink so the write reaches printer_data.
seed_update_channel() {
    [ "${_R2_CHANNEL_FROM_VERSION:-}" = "yes" ] || return 0
    if merge_settings_defaults '{"update": {"channel": 1}}'; then
        log_info "App update channel: beta, unless settings.json already named one"
    else
        log_warn "Could not record the beta update channel in settings.json;"
        log_warn "choose Beta in the app's update settings to keep receiving beta builds."
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Tier-2: Moonraker-based printer detection with confidence gate
# ---------------------------------------------------------------------------

# Call the installed helix-screen binary in detect-printer mode.
# Prints the JSON verdict line on success, empty string on failure.
# Never fails — Moonraker unreachable is a graceful no-op.
detect_printer_via_moonraker() {
    local bin="${HELIX_SCREEN_BIN:-${INSTALL_DIR:-/opt/helixscreen}/helix-screen}"
    [ -x "$bin" ] || { echo ""; return 0; }
    local out
    if out=$("$bin" --detect-printer --host 127.0.0.1 --port 7125 2>/dev/null); then
        echo "$out"
    else
        echo ""
    fi
    return 0
}

# Extract a single string field from a JSON object stored in an env var.
# Prints the value, or empty string if absent / null.
# Requires python3; if absent, prints empty and returns.
_json_field() {
    command -v python3 >/dev/null 2>&1 || { echo ""; return 0; }
    JSON_STR="$1" JSON_KEY="$2" python3 - <<'PY'
import json, os
try:
    v = json.loads(os.environ["JSON_STR"]).get(os.environ["JSON_KEY"])
    print("" if v is None else v)
except Exception:
    print("")
PY
}

# Seed the FULL preset (B-path) into settings.json.
# Writes preset["printer"] under printers.<active_id>, seeds top-level display,
# sets the top-level "preset" marker, and sets wizard_completed=false.
# This mirrors the runtime apply_preset_file so that first-boot enters
# preset-mode immediately without waiting for Moonraker detection.
#
# Args: $1 = printer_id (e.g. "qidi_q2")
seed_full_preset_for_printer() {
    local printer_id="$1"
    [ -n "$printer_id" ] || return 0
    local preset_dir
    preset_dir="$(_helix_preset_dir)"
    local fragment="${preset_dir}/${printer_id}.json"
    [ -f "$fragment" ] || return 0
    if ! command -v python3 >/dev/null 2>&1; then
        log_warn "python3 not available; skipping full-preset seed for ${printer_id}"
        return 0
    fi
    [ -d "${INSTALL_DIR}/config" ] || $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    local settings="${INSTALL_DIR}/config/settings.json"
    local tmp_out="${settings}.seed.$$"
    log_info "Seeding FULL preset for ${printer_id} (preset-mode)..."
    if SETTINGS_PATH="$settings" FRAGMENT_PATH="$fragment" PRESET_ID="$printer_id" TMP_OUT="$tmp_out" python3 - <<'PY'
import json, os

settings_path = os.environ["SETTINGS_PATH"]
fragment_path = os.environ["FRAGMENT_PATH"]
preset_id     = os.environ["PRESET_ID"]
tmp_out       = os.environ["TMP_OUT"]

def load(p):
    try:
        with open(p) as f:
            d = f.read().strip()
        o = json.loads(d) if d else {}
        return o if isinstance(o, dict) else {}
    except (ValueError, OSError):
        return {}

def strip_us(o):
    """Recursively drop _-prefixed provenance keys."""
    if isinstance(o, dict):
        return {k: strip_us(v) for k, v in o.items() if not k.startswith("_")}
    if isinstance(o, list):
        return [strip_us(v) for v in o]
    return o

def merge(base, frag):
    """Fill base with frag keys not already in base (base wins)."""
    for k, v in frag.items():
        if k in base and isinstance(base[k], dict) and isinstance(v, dict):
            merge(base[k], v)
        elif k not in base:
            base[k] = v
    return base

base   = load(settings_path)
preset = strip_us(load(fragment_path))

# Seed top-level display block (device-level, needed at first boot).
if isinstance(preset.get("display"), dict):
    base["display"] = merge(base.get("display", {}), preset["display"])

# Place preset["printer"] under printers.<active_id>.
active   = base.get("active_printer_id", "default")
printers = base.setdefault("printers", {})
if not isinstance(printers.get(active), dict):
    printers[active] = {}
pnode = printers[active]
if isinstance(preset.get("printer"), dict):
    merge(pnode, preset["printer"])

# Top-level structural markers for multi-printer settings shape.
# setdefault, never assign: this seeding runs on --update too, after the user's
# settings.json has been restored. Overwriting wizard_completed would send a
# configured user back through first-boot setup, and on a multi-printer config
# the app's stale-entry recovery would then drop the printer node entirely.
base["active_printer_id"] = active
pnode.setdefault("wizard_completed", False)
base.setdefault("preset", preset.get("preset", preset_id))

with open(tmp_out, "w") as f:
    json.dump(base, f, indent=2)
    f.write("\n")
PY
    then
        if [ -f "$tmp_out" ] && $(file_sudo "${INSTALL_DIR}/config") mv "$tmp_out" "$settings" 2>/dev/null; then
            log_success "Seeded FULL preset for ${printer_id}"
        else
            log_warn "Could not write full-preset settings.json for ${printer_id}"
            rm -f "$tmp_out" 2>/dev/null || true
        fi
    else
        log_warn "Failed to merge full preset for ${printer_id}; leaving settings.json unchanged"
        rm -f "$tmp_out" 2>/dev/null || true
    fi
    return 0
}

# C-path: pre-fill moonraker_host=127.0.0.1 in the default printer node
# so the app finds Moonraker without any wizard interaction.
# Idempotent — does NOT overwrite an existing moonraker_host value.
_seed_moonraker_host_localhost() {
    command -v python3 >/dev/null 2>&1 || return 0
    local settings="${INSTALL_DIR}/config/settings.json"
    local tmp_out="${settings}.host.$$"
    SETTINGS_PATH="$settings" TMP_OUT="$tmp_out" python3 - <<'PY' || return 0
import json, os

p = os.environ["SETTINGS_PATH"]
t = os.environ["TMP_OUT"]
try:
    with open(p) as f:
        d = json.load(f)
    if not isinstance(d, dict):
        d = {}
except Exception:
    d = {}

a  = d.get("active_printer_id", "default")
d["active_printer_id"] = a
pr = d.setdefault("printers", {})
if not isinstance(pr.get(a), dict):
    pr[a] = {}
pr[a].setdefault("moonraker_host", "127.0.0.1")

with open(t, "w") as f:
    json.dump(d, f, indent=2)
    f.write("\n")
PY
    [ -f "$tmp_out" ] && $(file_sudo "${INSTALL_DIR}/config") mv "$tmp_out" "$settings" 2>/dev/null || rm -f "$tmp_out" 2>/dev/null
}

# Confidence thresholds for the B/C gate.
# B (full preset-mode seed): confidence >= MIN_CONFIDENCE AND margin >= MIN_MARGIN
# C (device-level + host pre-fill): preset present but ambiguous
# Override at call-site via environment for testing.
HELIX_DETECT_MIN_CONFIDENCE="${HELIX_DETECT_MIN_CONFIDENCE:-85}"
HELIX_DETECT_MIN_MARGIN="${HELIX_DETECT_MIN_MARGIN:-10}"

# Tier-2 orchestrator: run helix-screen --detect-printer and apply the
# appropriate seeding path based on the confidence gate.
# No-op (success) if Moonraker is unreachable or the verdict has no preset.
seed_from_moonraker_detection() {
    local verdict
    verdict="$(detect_printer_via_moonraker)"
    [ -n "$verdict" ] || return 0

    local preset conf rconf
    preset="$(_json_field "$verdict" preset)"
    conf="$(_json_field "$verdict" confidence)"
    rconf="$(_json_field "$verdict" runner_up_confidence)"

    # No preset in the verdict → nothing to seed.
    [ -n "$preset" ] || return 0

    [ -n "$conf" ]  || conf=0
    [ -n "$rconf" ] || rconf=0
    local margin=$(( conf - rconf ))

    if [ "$conf" -ge "$HELIX_DETECT_MIN_CONFIDENCE" ] && [ "$margin" -ge "$HELIX_DETECT_MIN_MARGIN" ]; then
        log_info "Detection B (conf=${conf}, margin=${margin}) -> full preset ${preset}"
        seed_full_preset_for_printer "$preset"
    else
        log_info "Detection C (conf=${conf}, margin=${margin}) -> device-level seed ${preset}"
        seed_settings_for_printer "$preset"
        _seed_moonraker_host_localhost
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Detect a known printer-model that needs install-time seeding/config.
# Prints a printer-id to stdout (empty if none matched).
#
# Returns "sovol_sv06_ace" when the stock Sovol touchscreen UI binary
# `mksclient` is present at its confirmed install path
# /home/sovol/printer_data/build/mksclient (also globbed as
# /home/*/printer_data/build/mksclient for robustness if the user account
# differs). This is the confirmed fingerprint: published Sovol/R8CEH configs
# show mksclient is a plain binary at that path, run as user `sovol`. Empty
# otherwise.
#
# TODO(#986): the binary path + `sovol` user is now the CONFIRMED fingerprint
# (from published Sovol/R8CEH firmware configs). The only remaining uncertainty
# is the exact /etc/hostname and /proc/device-tree/model strings on the panel,
# which we chose not to fetch — the binary path is a strong, unambiguous signal
# on its own, so hostname matching was dropped to avoid false negatives on
# renamed hosts.
detect_printer_model() {
    # Confirmed signal: the stock Sovol UI binary at its known build path.
    # HELIX_SOVOL_MKSCLIENT lets tests redirect the path under a temp HOME.
    local _bin
    for _bin in \
        "${HELIX_SOVOL_MKSCLIENT:-}" \
        /home/sovol/printer_data/build/mksclient \
        /home/*/printer_data/build/mksclient; do
        [ -n "$_bin" ] || continue
        if [ -f "$_bin" ]; then
            echo "sovol_sv06_ace"
            return 0
        fi
    done

    echo ""
    return 0
}
