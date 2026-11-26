# Nozzle size: one capability layer, settable per head

Goal (1.2): the user sets each toolhead's nozzle size from HelixScreen, and every
consumer reads one answer for "what nozzle is on this head". Asked for on Discord;
Preston, 2026-11-26: "we need a hardware/vendor abstraction layer that understands what
the printer/backend supports, single / multiple extruder support, etc."

## The signal

`nozzle_diameter` is the orifice size Klipper reads from `[extruderN]`. Mainline Klipper
derives `max_extrude_cross_section` (4 × d²) and the extrude-only limits from it, reads it
once at startup, has no G-code to change it, and does not report it in
`extruder.get_status()`.

| Printer / firmware | Where the size lives | Live status field | How it changes | Heads |
|---|---|---|---|---|
| Mainline Klipper, Kalico, Qidi (Q2 checked 11-26: field null), K1 family | `[extruderN] nozzle_diameter` in printer.cfg | none | edit printer.cfg + `FIRMWARE_RESTART` | one per `[extruderN]` |
| Creality K2 / Plus / Pro (K2 Plus checked 11-26) | printer.cfg `user_nozzle_diameter` overrides `nozzle_diameter` | `extruder.nozzle_diameter` | `SET_NOZZLE_DIAMETER VALUE=<mm>`: live at once, saved by `CXSAVE_CONFIG`, no restart; the extrusion cap moves at the next restart | `extruder` only |
| Snapmaker U1 | firmware's own record, outside configfile | `extruderN.nozzle_diameter` + `nozzle_volume_type` | `printer.control.nozzle_properties` webhook; 0.2/0.4/0.6/0.8 only; refused while printing or paused | 4 |
| Firmware-supervised configs (`KlipperConfigEditor` refuses them) | as above | as above | none: read-only | |

## Today

- The fit check (`src/printer/fit_check.cpp#fit_machine_from_settings`, `#fit_machine_with_live_nozzles`) takes config per `[extruderN]` and lets a live field win. That's correct, and it already follows a K2 change.
- `PrinterDiscovery::nozzle_diameter_mm()` reads `[extruder]` only, default 0.4. PA tuning (`ui_overlay_pa_tuning.cpp`, line width = d × 1.125) and the nonlinear PA tower use it: wrong for any head but T0, and stale on a K2 or U1 whose live size differs from config.
- U1 only: the Toolhead device-action rows in `src/printer/ams_backend_snapmaker.cpp#get_device_actions` set diameter and flow type.
- Nothing else lets a user set the size.

## Design

`include/nozzle_size.h` + `src/printer/nozzle_size.cpp`, `helix::nozzle_size`: the only
module that knows these firmwares. It's shaped like `z_offset_persistence` (one row per
mechanism, capability questions as free functions), and generic code never names a vendor.

```cpp
enum class Mechanism { None, ConfigEdit, KlipperCommand, VendorRpc };

struct HeadCapability {
    int extruder_index;             // 0 = "extruder", 1 = "extruder1", ...
    std::string extruder;           // "extruder", "extruder1"
    std::optional<double> current;  // live field if reported, else config
    bool settable;
    Mechanism mechanism;
    bool restart_required;          // ConfigEdit: FIRMWARE_RESTART
    bool idle_only;                 // refused while printing/paused
    std::vector<double> choices;    // U1: firmware's list; others: kCommonSizes
    std::vector<std::string> flow_variants;  // every head: Standard / High Flow
};
struct Capability { std::vector<HeadCapability> heads; };

Capability capability(const PrinterInputs&);          // pure
std::optional<double> head_nozzle(const PrinterInputs&, int extruder_index);  // the one resolver
ChangePlan plan_change(const PrinterInputs&, int extruder_index, double mm);  // classification
```

- **PrinterInputs** is what the rows decide from: configfile `[extruderN]` sections, live extruder status, whether `SET_NOZZLE_DIAMETER` is registered (one `printer.gcode.help` query, which discovery doesn't make today), the backend's nozzle record (U1), and whether the config is firmware-supervised.
- **Detection is by capability, never by board:**
  - the K2 row matches when the command is registered;
  - the U1 row matches when the Snapmaker backend reports nozzle records;
  - otherwise ConfigEdit, or None when the config is supervised.
  A Kalico or mod that adds `SET_NOZZLE_DIAMETER` gets the K2 path for free. A head the command doesn't cover (it registers on `extruder` only) falls back to ConfigEdit.
- **`plan_change` returns the decision, and the caller executes it**:
  - `KlipperCommand{gcode}`;
  - `VendorRpc{method, params}` (U1, reusing `snapmaker_nozzle_properties.h`);
  - `ConfigEdit{section, key="nozzle_diameter", value}` through `KlipperConfigEditor::safe_edit_value`, which already backs up, restarts and reverts on a failed restart;
  - `Refused{reason}` (printing, supervised, a size not in the firmware's list, an unknown head).
  K2 config writes go through its command, never printer.cfg, because `user_nozzle_diameter` overrides any `nozzle_diameter` edit.
- **One resolver:** the fit check, PA tuning, the PA tower and anything else needing "this head's nozzle" call `head_nozzle()`. `PrinterDiscovery::nozzle_diameter_mm()` either goes or delegates to it (caller grep decides).
- **Flow type on every head.** Every row offers `flow_variants`. On the U1 the type is firmware-written through its webhook (High Flow only at 0.4 mm). Everywhere else, including a supervised config, it is the user's statement about the nozzle, kept per printer in `SettingsManager` (a bitmask by extruder index, `nozzle/high_flow_heads`, default Standard) and applied at once with no confirmation, since Klipper has no such setting. `head_volume_type()` is the one rule for it: a type the extruder reports wins, else the stored one, else standard. The fit check's `nozzle_volume_types` takes that value, so a file sliced for High Flow on a Standard head (or the reverse) raises `NozzleFlowMismatch` on any printer. The plan is `ChangePlan::Kind::StoredFlow`.
- **Choices:** `kCommonSizes` = 0.2, 0.25, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0, 1.2 for ConfigEdit and the K2; the firmware's own list for the U1.

## UI (decided 2026-11-26)

**One Toolhead settings overlay, reachable from two places.** The U1's "Toolhead"
section is a filament-system device-action section that only the U1 has, so it can't be
the universal home. `toolhead_settings_overlay` is where per-head settings live,
opened from:

- **Settings › Printing › MACHINE › "Toolhead"** on every printer. It sits beside Machine Limits, Motion and Retraction, which are the machine's physical configuration, some of which already writes Klipper config. The row's description shows the current answer ("0.4 mm nozzle"; on several heads each one's size, labeled the same way).
- **The filament system settings**, where one is present: a "Toolhead settings" row that opens the same overlay. On the U1 it replaces the Toolhead device-action section.

Not the Printer Manager overlay: it holds identity and detected capabilities (name,
image, versions, capability chips), not settings a user changes.

**A head is a physical extruder, never a T-number.** Where tools are lanes (AD5X IFS,
CFS, ACE: T0-T3 feed one nozzle) there is one head. Tool numbers map to heads through
ToolState's existing `tool_extruder` mapping.

Inside: one card per head (one on a single-extruder printer, one per extruder otherwise). Each card is labeled by `helix::head_identity()` spelled through the `head_*_label()` family (`include/display_numbering.h`, `docs/devel/TOOL_ABSTRACTION.md` § "What a head is called"). That gives `E<n>` where tools are lanes, the representative tool where every tool is a head, and the user's numbering style either way. Never a hand-built label.
Each head shows what its capability row offers:

- **Nozzle size:** a dropdown of `choices`, a **fixed list** (no free entry). It's read-only with the reason when `settable` is false, and disabled while printing (`idle_only`).
- **Flow type** on every head (Standard / High Flow): firmware-written on the U1, where High Flow exists at 0.4 mm only; kept by HelixScreen elsewhere and applied at once. It's modeled as `HeadCapability::flow_variants`, so the overlay never names a vendor. The U1's two diameter/flow device-action rows go; the backend's webhook plumbing stays and is driven through the module.

Confirmations, in the confirm dialog:
- a ConfigEdit change says Klipper restarts to apply it;
- a K2 change says it applies now and that Klipper's extrusion limit follows at the next restart;
- the U1's 0.2 mm confirm stays.

Measure first with `ctl geom` at 800x480 and the smallest breakpoint. Reuse the existing setting rows (`setting_dropdown_row` or its equivalent, `setting_group`); the overlay is a new screen, not a new widget.

## Phases

1. Module + resolver + consumers (fit check, PA tuning, PA tower, discovery): pure, test-first, no UI.
2. `plan_change` + executors + the settings rows (mock: ConfigEdit path, K2 command path, U1 path).
3. Hardware checks (`hw-verify`):
   - K2 Plus .30.196: the command path, read back live and after a restart.
   - Q2 or the V0: the ConfigEdit path, with restart and revert.
   - U1: unchanged behavior.
   - Every device step is confirmed with Preston: a restart is a printer action.

## Open

- None for phase 2. Phase 3 hardware checks are confirmed with Preston step by step.
