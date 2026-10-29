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
| K2, K2 Pro, K2 Plus (built-in CFS) | Creality K2 firmware | `CR_BOX_*` primitives + `BOX_SAVE_FAN`/`BOX_MODE_WAIT` envelope | neither `is_creality_k1()` nor `is_creality_hi()` |
| K1, K1C, K1 Max (official CFS upgrade ≥ v2.3.5.33) | Creality K1 CFS upgrade firmware | Plain `BOX_*` primitives, no mode-wait (fan-save **does** exist — see below) | `PrinterDetector::is_creality_k1()` or `is_creality_hi()` |
| K2 Plus on a community Kalico port | [`Jacob10383/kalico`](https://github.com/Jacob10383/kalico) + a reimplemented box.py | High-level `BOX_SELECT_SLOT` (or bare `T<n>`) / `BOX_UNLOAD` | `api_version == 1` in the box payload |

**Axis 2 — box schema** (`CfsSchema`), detected per-payload by `AmsBackendCfs::detect_schema()`:

| Schema | Shape | Parser |
|--------|-------|--------|
| `Stock` | `T1`–`T4` nested units, four parallel arrays each, material **codes** | `parse_stock_box_status()` |
| `Flat` | One `slots[]` array of self-describing objects, plain material names, `#RRGGBB` colors | `parse_flat_box_status()` |

Both schemas arrive as deltas, so each parser reads a merged copy of the box rather than the frame: `stock_box_state_` merges stock frames recursively (a `T<n>` frame may carry only the unit fields that changed), and `flat_box_state_` replaces each top-level field a flat frame carries, because Klipper sends a changed field of box.py's object whole. A flat frame that edits a slot carries `slots` alone, and its `temp_c`, `humidity_pct` and `load_path` come from the merge. A frame with neither `slots` nor a `T<n>` unit is a delta of the schema the box spoke last: flat after a `slots` frame, stock after a `T<n>` frame. A `T<n>` frame after flat ones returns the schema to stock and the Fork dialect to the model's own. Presence gates (`runout`, `runout_swap_enabled`, `filament_detected`) and the per-bay RFID fingerprints still read the frame itself, since a field it omits is one it said nothing about. Both merges reset at `on_started()` and when the schema switches. A bay's swap fingerprint is `material_type|color_value` on stock (`101001|0FF5500`) and `MATERIAL|RRGGBB` on flat, so across a schema switch only the colour still compares: a baseline whose shape names the other schema, live or loaded from the record's persisted fingerprint, is a swap when its colour differs from the reading's (stock's leading `0` aside), and otherwise is dropped so the reading becomes a first observation that the record then persists (`AmsBackendCfs::fingerprint_schema`, `src/printer/ams_backend_cfs.cpp#check_hardware_event_clear`). A shape both schemas can spell (`PLA|`) is compared as usual.

Both axes are decided **from the payload, never from `PrinterDetector`** — a community port reports as stock K2 Plus hardware by every model signal, so model detection cannot see the firmware swap. `Stock` is the default for anything ambiguous.

The command dialect is selected by the explicit `api_version == 1` field rather than inferred from the `Flat` status layout, so another firmware can use the same layout without inheriting this one's commands. It also cannot be detected with `has_macro("BOX_LOAD")`: the Fork commands are registered in Python, so they are not gcode_macros and never appear in `printer.objects.list`.

### Fork API revisions

Within `api_version == 1` the payload's `api_revision` (absent means 1) says what the
firmware adds. The backend re-reads it from the merged flat state on every frame
(`AmsBackendCfs::fork_api_revision`) and keeps two answers, both dropped when the box
reports stock again:

| Capability | Present when | What changes |
|------------|--------------|--------------|
| Print mapping (`fork_has_print_mapping`) | `api_revision >= 2`, or `print_mapping_version == 1` | Manual load, swap and external-spool load send `BOX_SELECT_SLOT SLOT=<n>` instead of `T<n>` (`fork_slot_gcode`). During a print started with `BOX_PRINT_START MAP=`, `T<n>` is the file's tool `n` and loads its mapped slot; `BOX_SELECT_SLOT` always names the slot |
| Per-box readings | `api_revision >= 2` and a `boxes[]` array | Each unit's environment comes from its own `boxes[]` entry |
| `BOX_PRINT_MAP` (`fork_has_print_map`) | `api_revision >= 4` (`kForkPrintMapRevision`) | Tool remap is `RemapStrategy::PrePrintSend`. Revision 3 has the command without its `MAP=` clear and gets no remap |

`SLOT=` and the slot side of `MAP=` take the firmware's global slot number,
`(address - 1) * 4 + local`, which is already the bay's global index here, so a bay is
sent as its own index; the external spool is `external_slot_index_`. Firmware without
print mapping registers no `BOX_SELECT_SLOT`, and there `T<n>` is slot `n`.

**Environment.** From revision 2 each `boxes[]` entry carries `temp_c` and
`humidity_pct` for its box, joined on `address` (never array position) into that unit's
`AmsUnit::environment`, the per-unit path every backend's environment zones read. An
entry with `online: false` or null readings, or a box `boxes[]` does not list, has no
reading. An entry with `online: false` also sets that unit's `AmsUnit::connected` to false, so its unit card shows the Disconnected chip until the box answers again. `boxes[]` is a top-level field, so the flat merge keeps the last one across
frames that omit it. Revision 1 payloads, and revision 2 ones with no `boxes[]`, carry
one top-level pair, which goes to the box `load_path.box_addr` names, else the lowest
box. The per-box `fault` object is not read: no per-unit status or error surface exists
to show its `title` and `message`.

**Tool remap.** `BOX_PRINT_MAP MAP=<tool>:<slot>,...` sets the map for the next print
only, so a revision 4 box declares `RemapStrategy::PrePrintSend` and
`requires_preprint_send()`, and `build_preprint_gcode()` emits the line before the print
starts (`AmsBackendCfs::print_map_gcode`). The firmware keeps the map for the next print
file loaded within 10 minutes and uses it once, so a remapped start whose print never
loads leaves a map that a later start would pick up. Every start therefore sends one
line. When the file's tools are known it is the full map, remapped or not: it names
every tool the file uses, an unmapped one at its own slot (the firmware's default
`T<n>` = slot `n`), and nothing else, since the firmware refuses a start whose map lists
a tool the file does not use. A full map replaces a pending one, so no clear goes in
front of it. A start with nothing to map (the file's tools unknown and no remap moving a
tool) sends the clearing form `BOX_PRINT_MAP MAP=` (`kClearPrintMapGcode`) instead. A remap whose target slot a manual load would refuse (`firmware_slot_locked()`), or that sits in a unit reading disconnected (a `boxes[]` entry with `online: false`, or the chain-wide `driver_ready: false`), is refused by `check_preprint_remap()`: nothing is sent and the print does not start, with the reason in the same "Print setup failed" dialog a printer-side refusal gets; `build_preprint_gcode()` called on a refused remap returns the clear. `can_set_tool_mapping()` refuses on this route: `BOX_MODIFY_TN` is a
stock command. The revision is the only evidence the command exists: it is registered
in Python, and the app has no registered-command lookup. Below revision 4 Fork has no
remap route (`RemapStrategy::None`) and sends no print map at all. A reprint has no
recorded routing to replay (`last_print_tool_mapping()` is empty), but Fork answers
`default_print_routing_is_identity()`, so the controller sends the identity map for the
file's tools: the same line a start with no remap sends, overwriting any map a failed
start left pending, or the clear when the file's tools are unknown. A restart from the
beginning (`restart_print_from_beginning()`) carries no routing and sends the clear
after its cancel.

A `Flat` box whose module we cannot identify still has its control paths refused by `reject_if_flat_schema()`. Full field mapping, command signatures and remaining gaps: `printer-research/CREALITY_CFS_K2_INTERNALS.md` § "Community Kalico port".

[`Jacob10383/kalico`](https://github.com/Jacob10383/kalico) is the Kalico (Danger-Klipper) fork the port builds on — it is the firmware *base*, and it does **not** contain the CFS modules. box.py and its siblings are dropped in by the port's installer and are not committed to any public repo, so the repo link is context rather than a source for the command surface. To read the modules themselves, fetch them from the port's content-addressed firmware store: `printer-research/CREALITY_CFS_K2_INTERNALS.md` § "Getting the module source".

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
| Unload current | `CR_BOX_PRE_OPT` → `BOX_CUT_MATERIAL` → `BOX_MODE_WAIT` → `CR_BOX_RETRUDE` → `CR_BOX_END_OPT` | `BOX_CUT_MATERIAL` → `BOX_RETRUDE_MATERIAL` |
| Envelope close (with wipe) | `BOX_NOZZLE_CLEAN` → `BOX_RESTORE_FAN` → `BOX_MOVE_TO_SAFE_POS` → `RESTORE_GCODE_STATE` | `BOX_NOZZLE_CLEAN` → `BOX_RESTORE_FAN` → `BOX_MOVE_TO_SAFE_POS` → `RESTORE_GCODE_STATE` |
| Tool remap | `BOX_MODIFY_TN T<src>=T<dst>` | (same syntax; persists silently and no `map` echo follows, see below) |
| Color sync | `BOX_MODIFY_TN_DATA ADDR=… NUM=… PART=color_value DATA=0RRGGBB` + `BOX_UPDATE_SAME_MATERIAL_LIST` | (same — `PART` names match the tn_data.json fields; the same-material refresh follows on both families because group membership requires exact color equality) |

Both dialects cut with `BOX_CUT_MATERIAL`, the firmware's positioned cut. The K2's
`CR_BOX_CUT` (`M8200 C`) is a bare cutter primitive: it strikes in X from wherever the
toolhead stands, and Creality's own tool change moves to `[box]` `pre_cut_pos` before it.
The envelope leaves the head at the purge chute (`extrude_pos`), and on a K2 Pro a strike
from there runs into the frame, so no HelixScreen script emits `CR_BOX_CUT`.

The K1 envelope is shorter only because `BOX_MODE_WAIT` does not exist on K1 — verified by
symbol grep, which finds it nowhere in the extension. `BOX_SAVE_FAN`/`BOX_RESTORE_FAN` are
present on both families and are now emitted on both.

> **`BOX_MODIFY_TN` works on K1.** The command writes the remap table and persists all 16
> keys to tn_data.json; it just prints nothing, which is why the #968 reporter read it as a
> no-op. The remap applies where it is meant to: the slicer's in-print `T0`-`T15` changes
> resolve through `Tnn_map`. Our manual load and swap emit `BOX_EXTRUDE_MATERIAL
> TNN=<physical>` and bypass the table by design, since they already name the physical slot.
> What is K1-specific is the missing echo: the K1 box status has no `map` field, so
> `reports_firmware_tool_mapping()` is false there and the optimistic local update is the only
> confirmation (`src/printer/ams_backend_cfs.cpp#set_tool_mapping_impl`).
> On K2 the box echoes a remap back in `map`. Mechanism:
> [CREALITY_CFS_K1_INTERNALS.md § Tool remap](printer-research/CREALITY_CFS_K1_INTERNALS.md#tool-remap-box_modify_tn---corrected-no-defect).

### Implementation

| File | Role |
|------|------|
| `include/ams_backend_cfs.h` | `CfsMacroVariant` enum, `AmsBackendCfs` class, static `load_gcode/unload_gcode/swap_gcode(idx, variant)` helpers (load and swap take the Fork `BOX_SELECT_SLOT` choice) |
| `src/printer/ams_backend_cfs.cpp` | `wrap_with_park_k1` / `wrap_with_park_k2` envelopes, K1-vs-K2 body emission, both schema parsers, the Fork revision gates and `BOX_PRINT_MAP` |
| `include/cfs_status_parse.h` / `src/printer/cfs_status_parse.cpp` | Pure parse of the small status objects around a `box` frame (frame shape, filament sensor) into `std::optional` structs |
| `include/printer_discovery.h` | `box` object handler — enables CFS for both K1 and K2 (#968 gate flipped) |

`AmsBackendCfs::macro_variant_` starts in the constructor from the model, `K1` when `PrinterDetector::is_creality_k1()` or `is_creality_hi()` matches and `K2` otherwise, and that model answer is kept in `model_macro_variant_`. The first flat frame carrying `api_version == 1` switches it to `Fork`; a later stock (`T<n>`) frame hands back the model's variant (`src/printer/ams_backend_cfs.cpp#handle_box_frame`). All member operations (`load_filament`, `unload_filament`, `change_tool`) thread `macro_variant_` into the gcode helpers. Static call sites without an explicit variant default to `K2` to preserve existing test behavior.

**A bay's global index is the firmware's slot number on both schemas,** `(box address - 1) * 4
+ local`, so every per-bay store (lane_data overrides, lane records, RFID baselines, insert
bookkeeping) stays on the physical bay whichever boxes are on the bus. An address below the
highest reporting box gets an absent placeholder unit (`AmsUnit::absent`): its bays read
EMPTY, validation refuses them, no tool maps to them, and `AmsSystemInfo::present_slot_count()`
/ `slot_exists()` leave them out of anything counted or offered to the user. `total_slots` is
the index span (#1464).

**`total_slots == 0` means the box size is unknown, not "no bays" (#1623).** The count is
committed only by the first unit-bearing box frame, and the print-start slot-mapping
restore fires on klippy READY, unordered against that frame - its recovery record is
deleted when a send is refused, so a pre-frame refusal would lose the mapping for good.
Every slot-index bound therefore goes through `slot_index_ceiling()`
(`src/printer/ams_backend_cfs.cpp`): the attached unit count once known, the TNN alphabet
(4 units x 4 bays = 16) until then, which keeps the pre-frame window permissive while a
4-slot CFS still refuses index 7. A remap's **tool number** is a routing-table key, not a
bay, so its bound stays the TNN alphabet even after the unit count is known - a slicer or
Creality's own UI can hold a high key while fewer units are attached.

**Tag reading.** The stock `material_type`/`color_value` arrays are latched RFID data,
so the bay's box record is its tag reading: material, brand and the catalog's nozzle
range for the code, colour; on the flat schema `material`, `brand`, `name` (the product)
and `color`, with the per-material temperature. A user edit is written back into that
same record, so the reading is folded frame by frame (`helix::ams::fold_tag_frame`): a
field the own-write echo guard withholds keeps the tag's value. After a restart the box
holds the user's values and they become the reading, and a value set on the printer's
screen cannot be told from a tag read
(`src/printer/ams_backend_cfs.cpp#file_box_readings`, `src/printer/ams_tag_reading.cpp#fold_tag_frame_into`).

What a stock tag reading can carry is whatever `box_wrapper` publishes per bay in
`T<n>`: `material_type` (catalog code, resolved through `assets/filaments.json`'s `cfs`
scheme to material, brand and nozzle range), `color_value`, `vender` and `remain_len`
(shown as remaining length). The CFS `TagReading` states no weight and no bed temperature.
The per-unit `uuid` array is not read at all. A tagged bay's `vender` entry is the tag's
whole 40-character record (field layout in `printer-research/CREALITY_CFS_K2_INTERNALS.md`
§ "RFID tags"), and HelixScreen reads three things from it: occupancy (a sentinel means an
empty bay), a brand fallback for a material code the catalog does not know
(`AmsBackendCfs::brand_from_vender`: supplier code `0276` and `"unknown"` read as Creality,
another supplier's record as no brand, a short vendor name as itself), and a Spoolman spool
id from the serial field (below). The printer has no known command that writes a tag.

**Swap detection.** `check_hardware_event_clear()`
(`src/printer/ams_backend_cfs.cpp#check_hardware_event_clear`) feeds each bay's
fingerprint to `rfid_tracker_` (a `helix::ams::SlotFingerprintTracker`). A `Changed`
event on a bay with an override runs `clear_override_locked()`
(`src/printer/ams_subscription_backend.cpp#clear_override_locked`): the override, the
lane's records and the per-spool fields (brand, Spoolman link, weights) go, so they do not
follow the old spool. The fingerprint is `material_type|color_value`, so two blind spots
follow from it: a different spool with the same Creality code and colour reads
`Unchanged` and keeps the previous link and fields, and an untagged bay (sentinel code)
yields an empty fingerprint, `NoSignal`, which never clears. In both cases the user must
re-pick the spool, unless the new spool's tag names a Spoolman spool: that relinks the lane
on its own. The serial is deliberately not part of the fingerprint: every persisted
`material|color` baseline would read `Changed` against a three-part reading on the first
frame after an upgrade, and a stock serial (always `000001`) adds no swap signal. Separately, a bay that reads empty after a confirmed runout drops only
its Spoolman handle (`strip_spoolman_link_on_runout_locked`).

**Spoolman link.** A tag written by CFTag ([koen01/cftag](https://github.com/koen01/cftag),
`GetSpoolmanSerialNum()` in MainActivity.java) carries a Spoolman spool id in the record's
serial field, chars 29-34, as `String.format("%06d", id)` for ids 1 to 999999.
`AmsBackendCfs::tag_record_spoolman_id` decodes it into the bay's
`TagReading::spoolman_id` (`src/printer/ams_backend_cfs.cpp#file_box_readings`). Creality
writes its factory serial `000001` on every stock tag (CFTag's own default serial is the
same, with reserve `000000`), so 1 names no spool, and neither do 0, a serial that is not
six digits, or anything but a whole record. A real Spoolman spool #1 therefore cannot be
linked from a tag. The generic tag-link policy (`FILAMENT_SLOT_METADATA.md` § "Tag-declared
Spoolman links") turns that id into a link; `tags_name_spoolman_spools()` answers true on
the stock schema. Otherwise a stock lane's `spoolman_id` comes from a user pick (editor,
picker, QR scan). The flat (Kalico port) schema publishes no tag record, so it never
decodes an id; it stores a per-slot `spoolman_id` that HelixScreen writes with
`_BOX_SLOT_SET` and reads back as the box's own record, which is a stored user pick, not
tag data.

**Active spool.** `manages_active_spool()` is false, so `AmsState` sets Moonraker's active
spool from the loaded lane (`architecture/07-filament-ams.md` § "Spoolman without AMS").
`filament_loaded` is the toolhead `filament_switch_sensor` (`apply_filament_sensor`),
which trips at the end of the extrude step, before waste and flush. A load HelixScreen
starts moves `current_slot` to the target lane at dispatch, so the sync fires when the
sensor rises; until then the loaded-lane card and the Active Spool widget show nothing
loaded. On a swap the sensor can stay high from the outgoing filament while
`current_slot` already names the incoming lane, which can set the new spool active before
the old filament is out.

### Endless spool (auto-refill)

CFS reports `Available` + `ReadOnly` + `FirmwareManaged`, with `enabled` derived from
`box.auto_refill` (stock) or `box.runout_swap_enabled` (flat fork) via
`AmsSystemInfo::endless_spool_enabled`. On and off are therefore distinguishable, which the
old two-bool struct could not express - it hardcoded `supported = true` and buried the real
state in an untranslated `description`.

The bit is applied from any frame that carries it, whether or not a unit is up yet. The box
sends it once, in its first full frame (often while every unit still reads `state=None`), and
resends it only when it changes, so a bare `{"auto_refill": n}` frame counts as a full update.
Until a frame has carried it, `enabled` is `Unknown` with restriction `NotReady`.

When auto-refill is on AND the frame carried `same_material` AND no group pairs two or more
lanes, `enabled` is `OnWithoutBackup` instead of `On` (#1391): the firmware swaps between
identical spools, and with every group a singleton a runout stops the print despite the
setting being on. A frame without `same_material` (the flat dialect never sends it; a stock
delta may omit it) keeps the plain `On` answer - no data is not a negative. A stock delta
that omits the field retains the last known grouping instead of clearing it
(presence-gated like `filament_runout`); only a schema switch to flat clears it.

`AmsBackendCfs` deliberately does **not** override `get_endless_spool_config()`. The box picks
the refill spool itself from its own `same_material` groups and exposes no per-slot mapping to
read, so the base's empty relation is the truthful answer, and it is what keeps the context
menu from drawing a backup dropdown that could only ever read "None". `box.same_material` is
parsed for two purposes: a material-code-to-name lookup used when resolving a slot's material
name, and the per-lane group ordinals (`AmsSystemInfo::endless_spool_group_ids`) behind the
`OnWithoutBackup` derivation above. Nothing is pushed back to the firmware on its basis.

The user-facing on/off control is the generic Endless Spool switch in the device operations
(`AmsBackend::set_endless_spool_enabled()`; the capabilities report `switchable`), which reaches
`AmsBackendCfs::apply_endless_spool_enabled()` and emits
`BOX_ENABLE_AUTO_REFILL ENABLE=1|0` — a setter, not a toggle: it sends the switch's value as
the explicit argument, mirroring Creality's own master-server (string tables in both OTA images; a bare
call leaves the handler's `gcmd.get_int` without its argument, whose behavior is
unverified). The switch renders the cached state, and has no value until a frame has
reported one. A send becomes the cached state once Klipper reports the command complete,
because the box never confirms a value it already held; a send that errors (the flat fork
registers no `BOX_ENABLE_AUTO_REFILL`, Klipper not ready, a box rejection) leaves the cache
alone. On the identified fork dialect the same setter sends `_BOX_SET_RUNOUT_SWAP ENABLE=1|0`,
the module's own setter for `runout_swap_enabled`; an unidentified flat module is not
`switchable`, and is refused like every other control path. `on_started()` forgets the bit, so a restart reads `Unknown` until the box reports it
again. It is not an endless-spool *edit* in the
`set_endless_spool_backup()` sense, which is why editability stays `ReadOnly`.

### Bypass / external spool

Supported, with a different mechanism per dialect. `supports_bypass` starts `false`
in the constructor and converges on the first full box frame in `handle_status`:

- **Flat schema:** true only when the Fork dialect is identified (`api_version == 1`)
  AND the payload carries an `external: true` entry (`find_external_slot_index()`,
  latched into `external_slot_index_`). An unidentified Flat module keeps bypass off —
  same rule as `reject_if_flat_schema`, no verified command means no button.
- **Stock schema:** true on the first full box frame that names a connected unit. Every
  stock CFS machine pairs the external holder with the `filament_switch_sensor
  filament_sensor` we already subscribe to, which is all the sensor-derived rule below
  needs. A frame with no unit leaves it false: see § No CFS on the bus.

`enable_bypass()` / `disable_bypass()` consult `bypass_available_for()` (the
`force_bypass_controls` override folds in like every other backend):

- **Fork:** `enable` selects the external slot, `BOX_SELECT_SLOT SLOT=<external_slot_index_>`
  with print mapping and `T<external_slot_index_>` without (§ Fork API revisions). The
  port's own box.py owns the whole attended flow (heat →
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

### No CFS on the bus

Stock K2 firmware registers `box` whether or not a CFS is attached. With none, every
address `T1`..`T4` reads `state: "None"` and the box's own `state` reads `"disconnect"`, so
the parse finds no unit (#1904). The backend
still runs, because it owns the external-spool load and unload, and it reports the
machine through `AmsSystemInfo::external_spool_only`:

- `apply_box_units_locked()` sets it when a stock frame finds no unit, none has been
  seen this session, and the box's `state` (from the merged frames) is `"disconnect"`.
  Every address reading `"None"` is also a K2 with a CFS at boot, before its unit answers,
  so that alone decides nothing (#1933); a bare `state` delta counts as a full frame so the
  verdict follows it. A unit seen once keeps its place in the list when a later frame
  drops it, so the flag cannot return on a unit that went briefly quiet. A unit coming
  online clears it, and clears a `-2` the declaration does not hold.
- `supports_bypass` stays false (the toggle would have no other side), and
  `is_bypass_active()` is true. Load therefore falls through to `LOAD_MATERIAL` and
  unload stays on the backend's bypass script, `QUIT_MATERIAL` plus the long retract
  (§ CFS bypass in `FILAMENT_MANAGEMENT.md`).
- `derive_stock_bypass_locked()` is armed without a declaration: with no bay to claim
  the filament, the toolhead sensor alone says whether the spool is loaded.

`AmsState` presents no filament system for such a backend
(`AmsSystemInfo::presents_filament_system()`): `ams_type` reads `NONE`, `ams_slot_count`,
`ams_supports_bypass` and `ams_has_device_ops` read 0, and `is_available()` is false. The
filament panel then shows its external-spool row and the home grid drops the slot and
bypass widgets, while `ams_action` and `ams_filament_loaded` stay live so the panel's
Load and Unload track their progress.

The toolhead switch, `filament_switch_sensor filament_sensor`, is claimed at discovery as the
CFS's own (`AmsBackendCfs::owns_filament_sensor`). Once the backend reports the empty bus,
`AmsState::filament_hardware_absent()` releases that claim: the sensor appears in
Settings > Sensors and the filament sensor wizard, and can be given the runout role. Until
the first frame answers, the claim holds, and hardware validation keeps it throughout, so
no "sensor available" toast offers it.

`box.enable == 0` is not this state. It is what the stock bypass declaration sends, and
a box stood down that way still has its unit. `HELIX_MOCK_CFS_BOXES=none` reproduces the
empty bus under `--test`.

### Known limitations on K1

Full mechanism for each of these, with sources and evidence tiers:
**[CREALITY_CFS_K1_INTERNALS.md](printer-research/CREALITY_CFS_K1_INTERNALS.md)**.

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
  macros. See [CREALITY_CFS_K1_INTERNALS.md](printer-research/CREALITY_CFS_K1_INTERNALS.md#failures-are-deferred-not-raised--fixed-host-side-verification).

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
  blockage failure mode, so it needs a K1 + CFS to settle.
- `BOX_MODIFY_TN_DATA` (color sync) syntax is confirmed correct — `PART` values are the
  tn_data.json field names. Material-type writeback is unblocked *except* for the
  `material_type` value domain, which is still unknown.
- `BOX_LOAD_MATERIAL_WITH_MATERIAL` and `BOX_QUIT_MATERIAL` (K1 high-level orchestrators) are not used; HelixScreen drives the primitives directly to keep behavior parallel between the two backends.
- Bed-area shrink for the rear-mounted K1 CFS upgrade (~5 mm Y) is not yet applied via the printer database.
- Hardware validation for K1/K1C is pending — track via [#968](https://github.com/prestonbrown/helixscreen/issues/968).

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
