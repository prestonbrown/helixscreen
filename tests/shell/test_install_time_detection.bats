#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install-time printer detection: Tier-1 unchanged; Tier-2 B/C/no-op.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export SETTINGS_FILE="$INSTALL_DIR/config/settings.json"
    export HELIX_PRESET_DIR="$WORKTREE_ROOT/assets/config/presets"
    unset _HELIX_PRINTER_SEED_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/printer_seed.sh"
    # Stub binary: echoes $FAKE_VERDICT, or exits non-zero if FAKE_UNREACHABLE=1.
    export FAKEBIN="$BATS_TEST_TMPDIR/helix-screen"
    cat > "$FAKEBIN" <<'SH'
#!/bin/sh
[ "${FAKE_UNREACHABLE:-0}" = "1" ] && exit 1
printf '%s\n' "$FAKE_VERDICT"
SH
    chmod +x "$FAKEBIN"
    export HELIX_SCREEN_BIN="$FAKEBIN"
}

@test "tier2 B: confident unambiguous verdict seeds full preset + marker" {
    rm -f "$SETTINGS_FILE"
    export FAKE_VERDICT='{"model":"Qidi Q2","preset":"qidi_q2","confidence":92,"runner_up_preset":"qidi_q1_pro","runner_up_confidence":70}'
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d.get('preset')=='qidi_q2',d;p=d['printers']['default'];assert p['fans']['part']=='fan_generic cooling_fan',p;assert p['wizard_completed'] is False"
}

@test "tier2 B: Max 4 verdict seeds Max 4 preset" {
    rm -f "$SETTINGS_FILE"
    export FAKE_VERDICT='{"model":"Qidi Max 4","preset":"qidi_max4","confidence":96,"runner_up_preset":"qidi_q2","runner_up_confidence":78}'
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d.get('preset')=='qidi_max4',d;p=d['printers']['default'];assert p['fans']['exhaust']=='fan_generic chamber_circulation_fan',p;assert p['heaters']['chamber']=='heater_generic chamber',p"
}

@test "tier2 C: ambiguous near-tie pre-fills host only, no preset marker" {
    rm -f "$SETTINGS_FILE"
    export FAKE_VERDICT='{"model":"Qidi Q2","preset":"qidi_q2","confidence":86,"runner_up_preset":"qidi_q1_pro","runner_up_confidence":84}'
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert 'preset' not in d,d;assert d['printers']['default']['moonraker_host']=='127.0.0.1'"
}

@test "tier2 no-op: Moonraker unreachable leaves settings absent" {
    rm -f "$SETTINGS_FILE"
    export FAKE_UNREACHABLE=1
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    [ ! -f "$SETTINGS_FILE" ]
}

@test "tier2 no-op: detected model with no preset does nothing" {
    rm -f "$SETTINGS_FILE"
    export FAKE_VERDICT='{"model":"Some Printer","preset":null,"confidence":95,"runner_up_preset":null,"runner_up_confidence":0}'
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    [ ! -f "$SETTINGS_FILE" ]
}

# --- Re-run safety -----------------------------------------------------------
# Seeding also runs on `install.sh --update`, after the user's settings.json has
# been restored. Clearing wizard_completed there sends a configured user back
# through first-boot setup; on a multi-printer config the app's stale-entry
# recovery then archives the active printer out of the list entirely.

# Write a settings.json that looks like a user who finished the wizard.
_seed_configured_settings() {
    cat > "$SETTINGS_FILE" <<'JSON'
{
  "active_printer_id": "default",
  "preset": "voron_v24",
  "printers": {
    "default": {
      "wizard_completed": true,
      "moonraker_host": "192.168.1.50",
      "heaters": {"bed": "heater_bed", "hotend": "extruder"}
    }
  }
}
JSON
}

@test "tier2 B re-run: preserves wizard_completed on an already-configured install" {
    _seed_configured_settings
    export FAKE_VERDICT='{"model":"Qidi Q2","preset":"qidi_q2","confidence":92,"runner_up_preset":"qidi_q1_pro","runner_up_confidence":70}'
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    python3 -c "
import json
d = json.load(open('$SETTINGS_FILE'))
p = d['printers']['default']
assert p['wizard_completed'] is True, 'wizard_completed was clobbered: %r' % p.get('wizard_completed')
assert d['preset'] == 'voron_v24', 'preset was clobbered: %r' % d.get('preset')
assert p['moonraker_host'] == '192.168.1.50', p
assert p['heaters']['bed'] == 'heater_bed', p
"
}

@test "tier2 B fresh: still marks a brand-new install as needing the wizard" {
    rm -f "$SETTINGS_FILE"
    export FAKE_VERDICT='{"model":"Qidi Q2","preset":"qidi_q2","confidence":92,"runner_up_preset":"qidi_q1_pro","runner_up_confidence":70}'
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    python3 -c "
import json
d = json.load(open('$SETTINGS_FILE'))
assert d['printers']['default']['wizard_completed'] is False, d
assert d['preset'] == 'qidi_q2', d
"
}

@test "tier2 B re-run: an existing incomplete wizard stays incomplete" {
    cat > "$SETTINGS_FILE" <<'JSON'
{"active_printer_id": "default", "printers": {"default": {"wizard_completed": false}}}
JSON
    export FAKE_VERDICT='{"model":"Qidi Q2","preset":"qidi_q2","confidence":92,"runner_up_preset":"qidi_q1_pro","runner_up_confidence":70}'
    run seed_from_moonraker_detection
    [ "$status" -eq 0 ]
    python3 -c "
import json
d = json.load(open('$SETTINGS_FILE'))
assert d['printers']['default']['wizard_completed'] is False, d
assert d['preset'] == 'qidi_q2', d
"
}
