# CFS (Creality Filament System) Filament Backend

The Creality Filament System is a 4-bay hub (K2 series built-in; K1/K1C/K1 Max official
upgrade) whose `box` Klipper object is shared by several firmwares - macro dialect and
status schema are two independent detection axes. Topology is `PathTopology::HUB`; the
Fork dialect additionally carries the external spool as an `external: true` entry.

## CFS (Creality Filament System)

The `box` Klipper object is shared by several firmwares that agree on almost nothing. CFS support therefore has **two independent axes** — do not infer one from the other:

**Axis 1 — macro dialect** (`CfsMacroVariant`), latched at backend construction:

| Printer family | Stock firmware path | Macro dialect | Detection signal |
|----------------|--------------------|---------------|-----------------|
| K2, K2 Pro, K2 Plus (built-in CFS) | Creality K2 firmware | `CR_BOX_*` primitives + `BOX_SAVE_FAN`/`BOX_MODE_WAIT` envelope | `PrinterDetector::is_creality_k1() == false` |
| K1, K1C, K1 Max (official CFS upgrade ≥ v2.3.5.33) | Creality K1 CFS upgrade firmware | Plain `BOX_*` primitives, no mode-wait (fan-save **does** exist — see below) | `PrinterDetector::is_creality_k1() == true` |
| K2 Plus on a community Kalico port | [`Jacob10383/kalico`](https://github.com/Jacob10383/kalico) + a reimplemented box.py | High-level bare `T<n>` / `BOX_UNLOAD` | `api_version == 1` in the box payload |

**Axis 2 — box schema** (`CfsSchema`), detected per-payload by `AmsBackendCfs::detect_schema()`:

| Schema | Shape | Parser |
|--------|-------|--------|
| `Stock` | `T1`–`T4` nested units, four parallel arrays each, material **codes** | `parse_stock_box_status()` |
| `Flat` | One `slots[]` array of self-describing objects, plain material names, `#RRGGBB` colors | `parse_flat_box_status()` |

Both axes are decided **from the payload, never from `PrinterDetector`** — a community port reports as stock K2 Plus hardware by every model signal, so model detection cannot see the firmware swap. `Stock` is the default for anything ambiguous.

The command dialect is selected by the explicit `api_version == 1` field rather than inferred from the `Flat` status layout, so another firmware can use the same layout without inheriting this one's commands. It also cannot be detected with `has_macro("BOX_LOAD")`: the Fork commands are registered in Python, so they are not gcode_macros and never appear in `printer.objects.list`.

A `Flat` box whose module we cannot identify still has its control paths refused by `reject_if_flat_schema()`. Full field mapping, command signatures and remaining gaps: `printers/CREALITY_K2_SUPPORT.md` § "Community Kalico port".

[`Jacob10383/kalico`](https://github.com/Jacob10383/kalico) is the Kalico (Danger-Klipper) fork the port builds on — it is the firmware *base*, and it does **not** contain the CFS modules. box.py and its siblings are dropped in by the port's installer and are not committed to any public repo, so the repo link is context rather than a source for the command surface. To read the modules themselves, fetch them from the port's content-addressed firmware store: `printers/CREALITY_K2_SUPPORT.md` § "Getting the module source".

### Firmware requirements

**Minimum versions**

| Family | Minimum firmware | Confidence |
|--------|------------------|------------|
| K1, K1C, K1 Max | **v2.3.5.33** (official CFS upgrade image) | Lower bound only — see below |
| K2, K2 Pro, K2 Plus | **Not established** | No data point |

- **K1 series:** Requires the **official Creality K1/K1C/K1 Max CFS upgrade firmware**. `v2.3.5.33` is the
  oldest version anyone has reported CFS working on (the reporter for #968), and `v2.3.5.34` has been
  read directly from Creality's CDN image and verified to carry the full `BOX_*` command set. Neither
  establishes that `.33` is the *floor* — no earlier `2.3.5.x` has been tested, so treat it as a known-good
  lower bound rather than a proven minimum.

  The version line matters more than the number: **`2.3.5.x` is the CFS line, `1.3.3.x` is not.** A K1C on
  `1.3.3.46` is stock non-CFS firmware, does not expose the `box` object, and the backend stays disabled —
  that is expected, not a bug. Community open-source K1 firmwares (Guilouz, etc.) do not currently bundle
  the CFS macros; install Creality's signed CFS-aware image to use the upgrade.

- **K2 series:** Stock firmware; detection is automatic when the CFS unit is paired (RS-485, exposes the
  `box` Klipper object). **We have no minimum version for the K2 line.** No K2 firmware image has been
  unpacked, no reporter version has been recorded against a working or failing CFS setup, and there is no
  version gating anywhere in the code. The `version` field in the `box` payload (e.g. `"1.1.3"`) is the
  **CFS module's** firmware, not the printer's, so it cannot stand in for one.

  To establish it, capture `printer.info` / the OTA version from a K2 with working CFS and record it here.
  Do not infer a K2 minimum from the K1 numbers — the two families do not share a version line, an
  architecture (K1 is MIPS, K2 is ARM Cortex-A7), or a macro dialect.

> **"K2 SE" is a K1-family board.** It is served by the K1 MIPS OTA image and speaks the plain `BOX_*`
> dialect, not `CR_BOX_*`. Do not classify it from the "K2" in its name — see the dialect note below.

> **Correction — `BOX_SAVE_FAN`/`BOX_RESTORE_FAN` DO exist on K1.** This doc previously recorded them as
> "verified absent in the public K1-Max `box.cfg` dump." That evidence was invalid: they are C-extension
> commands registered from `box_wrapper.cpython-38-mipsel-linux-gnu.so` (handlers `cmd_save_fan` /
> `cmd_restore_fan`), never `[gcode_macro]`s, so a config dump could never have listed them. Verified by
> symbol grep of the extension in `CR4CU220812S11_ota_img_V2.3.5.34`. `BOX_MODE_WAIT` genuinely is absent.
>
> Two evidence traps to avoid repeating: **neither `box.cfg` nor `printer.gcode.help` can prove a `BOX_*`
> command absent.** gcode.py records a description only when one is supplied, and the extension carries
> 5 help strings against 69 handlers — roughly 64 commands are executable and invisible to help. Only a
> symbol grep of the `.so` settles presence.
>
> This has been **fixed**: the K1 envelope now emits `BOX_SAVE_FAN`/`BOX_RESTORE_FAN`, and the
> `dispatch_action_script` error unwind (gated to K2 for the same wrong reason) runs on K1 too.
> Re-confirmed against the extension a second time on 2026-08-16.

### Macro dialect comparison

| Operation | K2 emission | K1 emission |
|-----------|-------------|-------------|
| Envelope open | `SAVE_GCODE_STATE` → `BOX_SAVE_FAN` → `BOX_GO_TO_EXTRUDE_POS` → `BOX_MODE_WAIT` | `SAVE_GCODE_STATE` → `BOX_SAVE_FAN` → `BOX_ERROR_CLEAR` → `BOX_CHECK_MATERIAL` |
| Load slot N | `CR_BOX_PRE_OPT` → `CR_BOX_EXTRUDE TNN=…` → `CR_BOX_WASTE` → `CR_BOX_FLUSH TNN=…` → `CR_BOX_END_OPT` | `BOX_EXTRUDE_MATERIAL TNN=…` → `BOX_EXTRUDER_EXTRUDE TNN=…` → `BOX_MATERIAL_FLUSH` |
| Unload current | `CR_BOX_PRE_OPT` → `CR_BOX_CUT` → `BOX_MODE_WAIT` → `CR_BOX_RETRUDE` → `CR_BOX_END_OPT` | `BOX_CUT_MATERIAL` → `BOX_RETRUDE_MATERIAL` |
| Envelope close (with wipe) | `BOX_NOZZLE_CLEAN` → `BOX_RESTORE_FAN` → `BOX_MOVE_TO_SAFE_POS` → `RESTORE_GCODE_STATE` | `BOX_NOZZLE_CLEAN` → `BOX_RESTORE_FAN` → `BOX_MOVE_TO_SAFE_POS` → `RESTORE_GCODE_STATE` |
| Tool remap | `BOX_MODIFY_TN T<src>=T<dst>` | (same syntax — but **inert as we use it**, see below) |
| Color sync | `BOX_MODIFY_TN_DATA ADDR=… NUM=… PART=color_value DATA=0RRGGBB` + `BOX_UPDATE_SAME_MATERIAL_LIST` | (same — `PART` names match the tn_data.json fields; the same-material refresh follows on both families because group membership requires exact color equality) |

The K1 envelope is shorter only because `BOX_MODE_WAIT` does not exist on K1 — verified by
symbol grep, which finds it nowhere in the extension. `BOX_SAVE_FAN`/`BOX_RESTORE_FAN` are
present on both families and are now emitted on both.

> **`BOX_MODIFY_TN` is not broken on K1 — our use of it is.** The command writes the remap
> table and persists all 16 keys to tn_data.json exactly as documented; it just prints
> nothing, which is why the #968 reporter read it as a no-op. The real problem is that
> **only the firmware's own `T0`–`T15` / `T1A`–`T4D` entrypoints read `Tnn_map`.** We emit
> `BOX_EXTRUDE_MATERIAL TNN=<physical>` directly, bypassing the mapping layer, so a remap we
> write can never take effect. Whether K2's module behaves the same way is unverified — our
> K2 remap may be inert for the same reason. Mechanism and options:
> [CREALITY_CFS_INTERNALS.md § Tool remap](CREALITY_CFS_INTERNALS.md#tool-remap-box_modify_tn).

### Implementation

| File | Role |
|------|------|
| `include/ams_backend_cfs.h` | `CfsMacroVariant` enum, `AmsBackendCfs` class, static `load_gcode/unload_gcode/swap_gcode(idx, variant)` helpers |
| `src/printer/ams_backend_cfs.cpp` | `wrap_with_park_k1` / `wrap_with_park_k2` envelopes, K1-vs-K2 body emission |
| `include/printer_discovery.h` | `box` object handler — enables CFS for both K1 and K2 (#968 gate flipped) |

`AmsBackendCfs::macro_variant_` is latched in the constructor by querying `PrinterDetector::is_creality_k1()`. All member operations (`load_filament`, `unload_filament`, `change_tool`) thread `macro_variant_` into the gcode helpers. Static call sites without an explicit variant default to `K2` to preserve existing test behavior.

### Endless spool (auto-refill)

CFS reports `Available` + `ReadOnly` + `FirmwareManaged`, with `enabled` from
`box.auto_refill` (stock) or `box.runout_swap_enabled` (flat fork) via
`AmsSystemInfo::endless_spool_enabled`. On and off are therefore distinguishable, which the
old two-bool struct could not express - it hardcoded `supported = true` and buried the real
state in an untranslated `description`.

`AmsBackendCfs` deliberately does **not** override `get_endless_spool_config()`. The box picks
the refill spool itself from its own `same_material` groups and exposes no per-slot mapping to
read, so the base's empty relation is the truthful answer, and it is what keeps the context
menu from drawing a backup dropdown that could only ever read "None". `box.same_material` is
parsed for one purpose only - a material-code-to-name lookup used when resolving a slot's
material name - and is not wired to endless spool.

The user-facing on/off control is the `toggle_auto_refill` device action, which emits
`BOX_ENABLE_AUTO_REFILL ENABLE=1|0` — a setter, not a toggle: it inverts the last
box-reported `endless_spool_enabled` and sends the explicit argument, mirroring
Creality's own master-server (string tables in both OTA images; a bare call leaves the
handler's `gcmd.get_int` without its argument, whose behavior is unverified). It is not
an endless-spool *edit* in the
`set_endless_spool_backup()` sense, which is why editability stays `ReadOnly`.

### Bypass / external spool

Supported, with a different mechanism per dialect. `supports_bypass` starts `false`
in the constructor and converges on the first full box frame in `handle_status_update`:

- **Flat schema:** true only when the Fork dialect is identified (`api_version == 1`)
  AND the payload carries an `external: true` entry (`find_external_slot_index()`,
  latched into `external_slot_index_`). An unidentified Flat module keeps bypass off —
  same rule as `reject_if_flat_schema`, no verified command means no button.
- **Stock schema:** true unconditionally on the first full box frame. Every stock CFS
  machine pairs the external holder with the `filament_switch_sensor filament_sensor`
  we already subscribe to, which is all the sensor-derived rule below needs.

`enable_bypass()` / `disable_bypass()` consult `bypass_available_for()` (the
`force_bypass_controls` override folds in like every other backend):

- **Fork:** `enable` dispatches `T<external_slot_index_>` — the port's own box.py
  registers that command for the holder and owns the whole attended flow (heat →
  wastebin → wait up to `EXTERNAL_WAIT = 30 s` for insertion → feed → flush). `disable`
  while engaged dispatches `BOX_UNLOAD`, whose external branch ejects and then waits
  for the user to pull the filament clear. Engaged state is firmware-reported:
  `loaded_slot` naming the external entry maps to the `-2` sentinel in
  `parse_flat_box_status()`.
- **Stock:** no Klipper-side command loads from the holder (Creality's own screen drives
  the box over RS-485; its Klipper-lane traffic for this is only
  `BOX_ENABLE_CFS_PRINT ENABLE=1/0` via `gcode/script` — verified from the master-server
  string tables in both OTA images). `enable` sends `BOX_ENABLE_CFS_PRINT ENABLE=0` (the
  box must stand down, or a print-file tool change / its runout refill can drive bay
  filament into the tube the external spool occupies), latches `bypass_declared_`, and
  `derive_stock_bypass_locked()` maps toolhead-sensor filament with no active lane to the
  `-2` sentinel. `disable` re-arms with `ENABLE=1` and drops the declaration.

Two deliberate subtleties in the stock derivation:

- **It is armed by the declaration only.** "Filament at the toolhead, no active lane" is
  also the #1199 state where the box drops its lane report while bay filament stays
  threaded — that state must stay `-1`, and only the user's toggle says the next
  filament the sensor sees belongs to the holder.
- **`is_bypass_active()` is `current_slot == -2 || bypass_declared_`.** After the user
  pulls the external filament back out, the sentinel clears but the declaration stands
  until the toggle is turned off — matching what the sidebar toggle shows.

The declaration survives restarts: `enable_bypass`/`disable_bypass` persist it through
`SettingsManager::get/set_bypass_declared()` (per-printer `ams/bypass_declared`),
`on_started()` restores it — pairing with the `ENABLE=0` the box's own tn_data.json
kept — and a full box frame reporting an explicit `enable == 1` drops it (someone
re-armed the CFS through Creality's own screen; in-memory clear on the bg thread, the
persisted flag re-clears idempotently on the next boot).

### Known limitations on K1

Full mechanism for each of these, with sources and evidence tiers:
**[CREALITY_CFS_INTERNALS.md](CREALITY_CFS_INTERNALS.md)**.

The full `BOX_*` command surface has since been read directly out of the shipped extension in
`CR4CU220812S11_ota_img_V2.3.5.34`, so the items below rest on the artifact rather than on
inference.

**Fixed:**

- ~~Part-cooling ran through every K1 operation.~~ `BOX_SAVE_FAN`/`BOX_RESTORE_FAN` do exist on
  K1 and are now in the K1 envelope, along with the matching error-unwind (#1278).
- ~~Two runout give-up wordings were unmatched.~~ There are **four** literals, not two;
  `no tray with ingredients found` and `no auto refill` both fell through every tier, so those
  paths paused the print with no modal at all. Both now matched.
- ~~`BOX_MODIFY_TN` no-ops on K1.~~ It does not. It persists silently and applies to the
  slicer's `T0`-`T15`, which is the entrypoint that resolves `Tnn_map`. Comments corrected in
  three places; behaviour was already right.
- ~~No phase verification.~~ The primitives record and queue failures instead of raising, so a
  whole sequence could return success while nothing moved. `finish_action()` now checks the
  toolhead filament switch against the operation's latched intent and raises a fault through
  `current_error()` when they disagree. Applies to every dialect — it reads physical state, not
  macros. See [CREALITY_CFS_INTERNALS.md](CREALITY_CFS_INTERNALS.md#failures-are-deferred-not-raised--fixed-host-side-verification).

**Still open:**

- **`BOX_ERROR_CLEAR` opening every sequence discards queued retry work** — reviewed and
  kept: a user-initiated load wants a clean slate, and `BOX_TNN_RETRY_PROCESS` is deliberately
  not wired to a button because it can resume a paused print as a side effect.
- **Resume swallows the entire K1 body** — seven commands no-op during resume handling. No
  longer silent (phase verification reports it), but it cannot be blocked up front: neither
  Klipper nor the box publishes a "resuming" state, and gating on "paused" would break
  loading filament during a runout pause.
- **Swap flush is a deliberate choice, not a bug.** `swap_gcode()` emits a bare
  `BOX_MATERIAL_FLUSH`, matching the firmware's own `BOX_LOAD_MATERIAL_WITH_MATERIAL` macro.
  The colour-aware `BOX_MATERIAL_CHANGE_FLUSH LAST_TNN=<old> TNN=<new>` exists and is what the
  internal `T*` path uses, but adopting it changes purge length on unowned hardware and adds a
  blockage failure mode — gated on #1278.
- `BOX_MODIFY_TN_DATA` (color sync) syntax is confirmed correct — `PART` values are the
  tn_data.json field names. Material-type writeback is unblocked *except* for the
  `material_type` value domain, which is still unknown.
- `BOX_LOAD_MATERIAL_WITH_MATERIAL` and `BOX_QUIT_MATERIAL` (K1 high-level orchestrators) are not used; HelixScreen drives the primitives directly to keep behavior parallel between the two backends.
- Bed-area shrink for the rear-mounted K1 CFS upgrade (~5 mm Y) is not yet applied via the printer database.
- Hardware validation for K1/K1C is pending — track via [#968](https://github.com/prestonbrown/helixscreen/issues/968).

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
