# AD5X infinite-spool capability matrix — design

**Status:** approved. Source-verified against zmod 1.7.1 (local) + bambufy master + lessWaste V1.2.40 (webfetch), and independently corroborated by two on-device sources (raza616 + ninjamida).
**Related:** #981 (AD5X color-revert tracking), #1247 (original "stock zMod has no switchover" claim — now refuted), #1250 (runout surface).

## The problem

HelixScreen tells AD5X users the wrong thing about automatic slot-to-slot switchover. We claim:

1. **Stock zMod has no backup-spool switching at all** (`src/printer/ams_backend_ad5x_ifs.cpp:5401-5406`, `docs/devel/FILAMENT_MANAGEMENT.md:2522`, `docs/devel/printers/FLASHFORGE_AD5X_SUPPORT.md`). **False.**
2. **bambufy has no backup/failover** (`docs/devel/FILAMENT_MANAGEMENT.md:2539`). **False.**
3. `variable_backup_filament_spent` exists in lessWaste (`docs/devel/printers/FLASHFORGE_AD5X_SUPPORT.md:212`, `docs/devel/printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md:290`). **False.**

The user-facing runout string `"No auto-switchover plugin (lessWaste or bambufy) is installed, so this printer will not change to a backup spool on its own."` (translated into 9 languages) is shown to every stock-zMod user on a runout, telling them their printer will not swap spools when in fact it just did, or is about to.

## Source evidence

Read directly, not via reports.

### Stock zMod — `ANALOG_PRUTOK`

zmod 1.7.1 local tree: `~/Code/Printing/zmod/_extracted/ad5x-1.7.1/mod/_mod/.shell/zmod_ifs.py:629-679`.

`head_switch_sensor`'s runout_gcode is wired unconditionally to `ANALOG_PRUTOK` in `translate/en/ad5x_display_off.cfg:39-44` (and every other language variant):

```cfg
[zmod_ifs_switch_sensor head_switch_sensor]
pause_on_runout: True
runout_gcode:
    SET_GCODE_VARIABLE MACRO=_A_CHANGE_FILAMENT VARIABLE=purge VALUE=0
    _ENABLE_SENSOR
    ANALOG_PRUTOK
```

`cmd_ANALOG_PRUTOK` (`zmod_ifs.py:629-679`) scans slots `1..color_limit` and switches if all three hold:

```python
if (
    i != prutok and
    ffm_info[f"ffmType{i}"] == filament_type and   # same material
    ffm_info[f"ffmColor{i}"] == filament_color and  # same color (exact hex)
    self.get_port(i)                                 # AND filament physically present
):
    # rewrite file.json mapping, _A_CHANGE_FILAMENT, RESUME
```

No enable flag, no print-mode gate. The feature is **always on** for stock zMod in display-off mode (HelixScreen's operating mode). zmod's own wiki names this **"Infinite Spool Mode"** — confirmed verbatim by raza616 (on-device source).

In display-on mode (`translate/en/ad5x.cfg:8-9`) `head_switch_sensor` is inert (`pause_on_runout: False`, no runout_gcode) — but display-on is mutually exclusive with HelixScreen, so irrelevant to us.

### bambufy — `variable_backup`, default ON

bambufy master, `bambufy.cfg` `_IFS_VARS` macro dict: `variable_backup: 1`. Reset macro `_IFS_VARS_RESET` restores `backup=1`. Switchover logic in `_RUNOUT_HEAD` (cfg lines 107-162): same type+color+present match as `ANALOG_PRUTOK`, then `_IFS_COLORS_ASSIGN`, then `PAUSE reason="backup"` which auto-resumes via `RESUME FORCE=1`.

bambufy **overrides** the stock `head_switch_sensor` runout_gcode with its own `_RUNOUT_HEAD` (the only sensible design — running both paths in parallel would double-handle every runout).

### lessWaste — `variable_backup`, default OFF

lessWaste V1.2.40, `lesswaste_src.cfg:969`: `variable_backup: 0`. Same `_RUNOUT_HEAD` shape as bambufy (lessWaste is a fork of bambufy V1.2.10). Prefix is `less_waste_*` on save_variables (not `bambufy_*`).

### Refuted claims

- `variable_backup_filament_spent` **does not exist** in lessWaste. Grepped zero matches across the 1995-line cfg. "Consumed" slots are inferred from `filament_detected == false` on the port sensor, not tracked in a variable.
- bambufy's PAUSE-reason inventory includes `nobackup` (cfg:149), missing from our doc's list of six.
- No plugin (nor stock zMod) disables switchover in multicolor. The community report's "bambufy doesn't support multicolor" describes the *de facto* outcome of multicolor prints typically loading one spool per color (so no same-color backup exists), not a code restriction.

## The corrected matrix

| Mode | `availability` | `enabled` | `editability` | `restriction` | `provider` | Trigger / rule |
|------|----------------|-----------|---------------|---------------|------------|----------------|
| **Stock zMod** (`!has_ifs_vars_`) | `Available` | `On` | `ReadOnly` | `FirmwareManaged` | `"zmod"` | `ANALOG_PRUTOK` on head runout; unconditional |
| **bambufy** (`var_prefix_ == "bambufy"`) | `Available` | `variable_backup` (**default On**) | `ReadOnly` | `PluginReadOnly` | `"bambufy"` | `_RUNOUT_HEAD` on head runout |
| **lessWaste** (`var_prefix_ == "less_waste"`) | `Available` | `variable_backup` (**default Off**) | `ReadOnly` | `PluginReadOnly` | `"lessWaste"` | `_RUNOUT_HEAD` on head runout |

All three modes apply the same eligibility rule for the backup slot: same material type AND same color AND port presence sensor detects filament. The match is exact-hex on color and exact-string on type.

The `has_ifs_vars_` discriminator stays — it still cleanly separates "stock zMod switchover" from "plugin switchover". Only the *meaning* of `!has_ifs_vars_` flips: from "no switchover" to "stock zMod switchover".

## Design

Five change surfaces, each independently reviewable.

### 1. Capability matrix — `get_endless_spool_capabilities()`

`src/printer/ams_backend_ad5x_ifs.cpp:5360-5394`. The `!has_ifs_vars_` branch changes from:

```cpp
caps.availability = EndlessSpoolAvailability::RequiresPlugin;
caps.enabled = EndlessSpoolEnabled::Off;
caps.restriction = EndlessSpoolRestriction::PluginMissing;
return caps;
```

to:

```cpp
caps.availability = EndlessSpoolAvailability::Available;
caps.provider = "zmod";
caps.enabled = EndlessSpoolEnabled::On;  // unconditional — no toggle exists
caps.editability = EndlessSpoolEditability::ReadOnly;
caps.restriction = EndlessSpoolRestriction::FirmwareManaged;
return caps;
```

The `has_ifs_vars_` branch is unchanged in shape — `Available`, provider set from prefix, `enabled` mirrors `ifs_backup_variable_` (with `Unknown` when never read), `PluginReadOnly`.

### 2. Runout detail copy — `build_runout_detail_locked()`

`src/printer/ams_backend_ad5x_ifs.cpp:5396-5437` has four branches today. Branch 1 is rewritten; branches 2-4 stay structurally the same:

- **Branch 1 — stock zMod (`!has_ifs_vars_`)**: was the misleading "No auto-switchover plugin" string. New copy describes Infinite Spool Mode, sharing the rule phrasing with branch 4 (since `ANALOG_PRUTOK` and `_RUNOUT_HEAD` apply the same type+color+present match):

  > "Infinite Spool Mode will switch to a slot whose filament type and colour both match the active spool and whose own port sensor reads filament present."

  Plus the existing per-slot match / no-match suffix (`find_backup_slot_locked(runout_slot_)`).

- **Branch 2 — plugin installed, `ifs_backup_variable_` unread**: unchanged ("is installed, but its backup-spool setting could not be read").

- **Branch 3 — plugin installed, backup off**: unchanged.

- **Branch 4 — plugin installed, backup on**: unchanged copy shape; the `plugin_name` local (`"bambufy"` / `"lessWaste"`) still interpolates so the user knows which system is driving the swap.

The "No auto-switchover plugin" string is **retired**. It is removed from `translations/*.yml` via `make translation-sync`, and the per-language entries in `ui_xml/translations/*.xml` drop out via `make translations`.

### 3. Internal naming — keep "endless spool" in code, "infinite spool" in AD5X user copy

`EndlessSpoolAvailability` / `EndlessSpoolCapabilities` / `ams_endless_state` / `ams_endless_text` / `is_endless_spool_backup_eligible()` — **unchanged**. These are cross-backend abstractions (AFC, Happy Hare, CFS, AD5X all funnel through them); renaming would be a flag day across six backends for zero behavioral gain.

User-facing copy on **AD5X only** uses "Infinite Spool Mode" / "infinite spool" wording, matching zmod's own wiki and what AD5X users see in Mainsail/Fluidd. The copy is in AD5X-specific call sites (`build_runout_detail_locked()`, the AMS panel status line, the slot context-menu wording) — not in the cross-backend restriction text in `ams_endless_spool.cpp`.

### 4. Header / type comments

`include/ams_backend_ad5x_ifs.h:60-103,309`: the comments justify the old `RequiresPlugin` choice. Rewrite to describe the corrected matrix and point at `ANALOG_PRUTOK`.

`include/ams_types.h:1582-1594`: the `EndlessSpoolAvailability` doc comment cites `#1247` and "AD5X on stock zMod" as the canonical `RequiresPlugin` example. Update to: AD5X stock zMod is now `Available/FirmwareManaged`; `RequiresPlugin` is retained for future use by a backend whose package genuinely can be missing.

`EndlessSpoolRestriction::FirmwareManaged` (`ams_types.h:1639-1641`) already references "the AD5X plugin's type+colour match" — extend to mention stock zMod's `ANALOG_PRUTOK` as the other case.

### 5. Tests

Three test surfaces assert the old matrix:

- `tests/unit/test_ams_backend_ad5x_ifs.cpp:10200` (`SECTION("stock zMod: no plugin, and backup is a definite OFF")`) and `:10325-10383` (`SECTION("stock zMod: RequiresPlugin, not Unsupported")`) — flip to assert `Available` / `FirmwareManaged` / `enabled == On` / `provider == "zmod"`.
- `tests/unit/test_ams_context_menu.cpp:90-93` uses AD5X stock as its `RequiresPlugin` example. Since no real backend now uses `RequiresPlugin`, swap to a synthetic `EndlessSpoolCapabilities` fixture (the test only exercises the cross-backend rendering path, not AD5X behavior).
- `tests/unit/test_ams_endless_spool.cpp:67-99, 870-922` — generic restriction-text tests. `RequiresPlugin` and `PluginMissing` text paths still need coverage (the enum value is retained), so these stay; add parallel coverage for `Available/FirmwareManaged/provider="zmod"` rendering.

## Documentation changes

### Devel docs

| File | Sections / lines | Change |
|------|------------------|--------|
| `docs/devel/FILAMENT_MANAGEMENT.md` | § "Auto-switchover plugin visibility" (~2520-2545); § "The status line" (~1240); capability table at :1332 and :2656; provider/restriction prose at :1140-1240; :2390-2431 (Moonraker visibility table — note `ANALOG_PRUTOK` works stock) | Rewrite against source. Correct bambufy default. Remove "bambufy has no backup/failover". Add `nobackup` to PAUSE reason list. Replace the `RequiresPlugin` row in the AD5X matrix with `Available`/`FirmwareManaged`/`provider="zmod"`. |
| `docs/devel/printers/FLASHFORGE_AD5X_SUPPORT.md` | :151, :163 (firmware history), :187-229 (bambufy vs lessWaste table) | Add zMod "Infinite Spool Mode" row to the comparison table. Delete the `variable_backup_filament_spent` row (:212). Fix the bambufy backup column ("No" → "Yes, default on"). |
| `docs/devel/printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` | :249, :287, :290 | Remove `variable_backup_filament_spent`. Add a section on `ANALOG_PRUTOK` citing `zmod_ifs.py:629-679` and `ad5x_display_off.cfg:39-44`. |

### User-facing docs

Any user-facing doc that mentions AD5X switchover should describe "Infinite Spool Mode" rather than implying a plugin is required. A search for `auto-switchover` / `backup spool` / `endless spool` across `docs/user/` will find the affected surfaces; the README and TROUBLESHOOTING are the most likely.

### Translations

The retired string `"No auto-switchover plugin (lessWaste or bambufy) is installed, so this printer will not change to a backup spool on its own."` is removed from `translations/en.yml` and the 8 non-English YAMLs. New strings (the "Infinite Spool Mode will switch …" copy) are added as `lv_tr("…")` literals in `build_runout_detail_locked()`, then `make translation-sync && make translations` propagates empty placeholders into all 9 languages. Per `[L064]`, the generated `ui_xml/translations/*.xml` artifacts are committed.

## Decisions

- **No special multicolor modeling.** None of the three modes disable switchover in multicolor; the same-color match rule already covers the case. Adding print-color-count awareness would cross subsystems (gcode parsing, print-status) for no behavioral gain. If we ever want a UI hint when a multicolor print has no viable backups, that is a separate, larger piece of work tracked as #1140-adjacent debt.
- **`RequiresPlugin` stays in the enum.** It loses its only caller, but removing a public enum value is a wider blast radius than keeping it. Its doc comment is updated to drop the AD5X reference and frame it as available-for-future-use.
- **Internal "endless spool" naming stays; user copy says "infinite spool" on AD5X.** Renaming the cross-backend types would be a flag day across AFC/Happy Hare/CFS/QIDI/Snapmaker/AD5X for no behavioral gain. User-facing copy is AD5X-local and can match zmod's own terminology cheaply.
- **bambufy default.** `variable_backup: 1` in bambufy's `_IFS_VARS`. Our `parse_ifs_vars_macro_locked()` reads the actual current value, so no code change is needed for this — only the doc.

## Out of scope

- Driving `variable_backup` from HelixScreen (writing to `_IFS_VARS backup=0/1`). Both plugins expose the toggle in their own Mainsail/Fluidd dialog; HelixScreen reporting it as `ReadOnly` is correct.
- Detecting "multicolor print with no viable backup" to surface a pre-print warning.
- The AD5X color-revert bug (`project_ad5x_color_revert`, #981) — separate issue, related but not blocking.
- Updating `MEMORY.md` / `LESSONS.md` for the corrected model. The claude-recall `.claude-recall/LESSONS.md:258` entry references the old "stock lacks `_IFS_VARS`" framing; it stays as written (it correctly warned that plans ≠ code, and the doc self-contradiction it flagged is one of the things this spec fixes).

## Verification

- `make -j` builds clean.
- `make test-run` — flipped AD5X tests pass; `test_ams_endless_spool` restriction-text tests pass; `test_ams_context_menu` passes with the synthetic fixture.
- Translation pipeline: `make translation-sync && make translations` produces the expected adds/drops in `translations/*.yml` and regenerates `ui_xml/translations/*.xml`.
- Manual: under `--test`, navigate to the AD5X AMS panel on a stock-zMod mock; confirm the status line reports Infinite Spool Mode as available and on. (`scripts/screenshot.sh` for the artifact.)
- Doc lint: `scripts/check_imperative_ui.py --list` count unchanged or lower.

## Open follow-ups (not blocking)

- Read the `_RUNOUT_HEAD` source in bambufy/lessWaste locally if checked out later, to confirm the sensor-override claim first-hand (currently inferred from the documented behavior — both plugins can't be running their runout handler alongside stock `ANALOG_PRUTOK`).
- Once an AD5X is available in the fleet, capture the actual on-wire text of `PAUSE REASON=nobackup` for the parser.
- Consider parsing `_PRINT_HEAD INFO=0` (zmod's "no backup match" respond_raw marker) as a direct signal — analogous to what the doc suggests for lessWaste's `PAUSE REASON=`.
