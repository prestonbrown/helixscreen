# Probe preparation as runtime config, and three screws-tilt corrections - design

**Status:** in flight on `feat/probe-preparation`.

| Item | State |
|------|-------|
| 1 - `probe_preparation` runtime rules | **done** - evaluator + 11 tests (fail-closed mutation-verified), DB rule, ESP32 classification, all four call sites wired with additive timeouts |
| 2 - bind the error label | **done** - plus an `lv_tr()` fallback so an empty message never renders blank |
| 3 - AD5X `screws_tilt_direction` | **done** - one line, plus 4 regression tests over the full config->detector->parser chain. Direction confirmed wrong on hardware by Preston before the flip; it is a binary, so the flip settles it |
| 4 - re-centering hint line | **done** - `suggest_screw_recentering()` + 4 tests + panel/XML; turn vocabulary extracted so it cannot drift from the per-screw rows |
| 5 - `ScrewsTilt` macro slot | **done** - slot + 6 tests, resolved name threaded through all five naming sites |
| 6 - reference screw spinning arrow | **done** - `screw_is_settled()` seam, both call sites routed through it |

**Verification:** full suite 96/96 shards, 11955 cases, 0 failures, exit 0 (shard 62's single
skip is `prerendered_images`, environmental - a fresh worktree has not run
`make gen-all-images`). `test-hidden` exit 0, 3949 assertions - no static-destruction crash
from the new database accessor. Translation pipeline run: 1 new key, `en.yml` populated, 8
locales carry empty placeholders, all 9 `ui_xml/translations/*.xml` regenerated.

Nothing committed yet.

**Verified on:** AD5X at 192.168.1.66, ZMOD 1.7.2-37, Klipper 12, stock MCU blob. All ZMOD
citations below are read from that rig's live config, not from the ZMOD repo.

## The problem

Four separate defects, all reachable from the screws-tilt panel, all fixable in runtime
config rather than the binary.

### 1. We probe without preparing the probe

The AD5X probes with a load cell (`printer.base.cfg` `[probe] pin: !PB3`, a digital trigger
off the load-cell board). Its zero drifts whenever the mechanical preload changes, which is
exactly what turning a bed screw does. ZMOD exposes `LOAD_CELL_TARE` to re-zero it, and
calls it on every probing path it considers important - except the ones our UI uses.

ZMOD's tare call sites (`/usr/data/config/mod/base.cfg`):

| Line | Macro | Reachable from our UI? |
|------|-------|------------------------|
| 211 | `BED_LEVEL_SCREWS_TUNE` | No - we send `SCREWS_TILT_CALCULATE` directly |
| 243 | `_FIX_BED_MESH_CALIBRATE` | **No - zero callers anywhere in the live config** |
| 1070 | `_PROBE_POINT` | No |
| 1183 | `_MESH_PROBE` | No - only called from `_MESH_TEST` (base.cfg:1141), a diagnostic |
| 1307 | `_PREPARE_PRINT` | No - print path only |
| 1412 | `_START_PRINT` | No - print path only, and conditional on `weightValue != 0` |

The paths we *do* use:

- `SCREWS_TILT_CALCULATE` - ZMOD overrides it (`base.cfg:215`, `rename_existing:
  _SCREWS_TILT_CALCULATE`) but the override only calls the stock command and parks. No tare.
- `BED_MESH_CALIBRATE` - ZMOD overrides it (`base.cfg:68`). With `use_kamp = 0`
  (`mod_data/variables.cfg:40`, this rig) it goes `_TEST_MIN_MAX` then stock
  `_BED_MESH_CALIBRATE`. No tare. The KAMP branch does not tare either.
- `AUTO_FULL_BED_LEVEL` / `_FULL_BED_LEVEL` - goes through `_ORIG_CLEAR_NOZZLE`
  (`mod/ad5x.cfg:462`), which homes and heats but never tares.

So there is **no tare-carrying bed mesh entry point on ZMOD at all**. Picking a different
macro cannot fix meshing. Screws-tilt is the one operation where a better macro exists
(`BED_LEVEL_SCREWS_TUNE`), and it costs a 130/80 heat-and-wait.

Observed failure on the rig, from `/usr/data/config/mod_data/log/console.log`. 36 matching
`Probe triggered prior to movement` entries in `/usr/data/logs/printer.log`:

```
01:47:02  >> SCREWS_TILT_CALCULATE   !! Probe triggered prior to movement    (x8 over 17 min)
02:03:09  !! Probe samples exceed samples_tolerance    (z swinging -4.11 / -2.28 / -0.42 / +1.40)
02:05:52  >> LOAD_CELL_TARE
02:06:08  >> SCREWS_TILT_CALCULATE   -> clean, all four corners
02:07:19  >> LOAD_CELL_TARE          // N 1. Weight: 80.0  ->  N 2. Weight: 0.0
02:07:31  >> SCREWS_TILT_CALCULATE   -> clean
```

Our call sites, all of which send the bare command:

```
src/api/moonraker_advanced_api.cpp:1816   SCREWS_TILT_CALCULATE
src/api/moonraker_advanced_api.cpp:1780   BED_MESH_CALIBRATE
src/ui/ui_probe_overlay.cpp:606           PROBE_ACCURACY SAMPLES=N
src/ui/ui_panel_calibration_zoffset.cpp:587,602   Z_ENDSTOP_CALIBRATE / PROBE_CALIBRATE
```

`ScrewsTiltPanel::start_probing()` increments `probe_count_`
(`src/ui/ui_panel_screws_tilt.cpp:386`) and re-runs after each screw adjustment, so every
iteration compounds the drift.

### 2. The error message is computed and then thrown away

The collector already captures Klipper's `!! ` lines and calls `complete_error(line)`
(`src/api/moonraker_advanced_api.cpp:787`). `on_screws_tilt_error()` sanitizes it and writes
it to a subject registered as `"error_message_text"` (`ui_panel_screws_tilt.cpp:176`, `:486`).

But the label never binds to it:

```xml
<!-- ui_xml/screws_tilt_panel.xml:234 -->
<text_muted name="error_message"
            width="80%" text="An error occurred during probing."
            translation_tag="An error occurred during probing." .../>
```

Hardcoded `text=`, no `bind_text`. Every failure shows the same generic string, so
`Probe triggered prior to movement` never reaches the user.

### 3. The AD5X is missing `screws_tilt_direction`

Preston hit this on the rig: the panel said tighten when the correct action was loosen. The
direction is binary, so that observation settles which value is right - there is nothing
further to verify on hardware. Confirmed in the shipped database, where the AD5X is the only
FlashForge entry without the override:

```
flashforge_adventurer_5m           screws_tilt_direction=ccw
flashforge_adventurer_5m_forgex    screws_tilt_direction=ccw
flashforge_ad5m_pro                screws_tilt_direction=ccw
flashforge_ad5m_pro_forgex         screws_tilt_direction=ccw
flashforge_ad5x                    ** MISSING **
```

The comment on the flip in `src/api/screws_tilt_parser.cpp:90` even names the cause
("FlashForge Adventurer 5M family"). ZMOD declares `screw_thread: CW-M3`
(`mod/ad5x.cfg:121`), same as its AD5M siblings, so the same inversion applies. The AD5X was
missed when the key was added.

### 4. The reference screw can be the only screw that should move

Klipper's `screws_tilt_adjust` reports every screw relative to `screw1`, which it treats as
a fixed base. When all three non-reference screws need the same large correction, following
that literally means three multi-turn adjustments where one would do.

Real output from the rig at 01:45:59, all three the same direction:

```
Left Near (base) : x=12.5, y=12.5, z=-3.89750
Right Near       : x=202.0, y=12.5, z=-4.59750 : adjust CW 01:24
Right Far        : x=202.0, y=202.0, z=-5.81000 : adjust CW 03:49
Left Far         : x=12.5, y=202.0, z=-5.34750 : adjust CW 02:54
```

The adjustment vector is only determined up to a uniform translation: adding a constant to
every screw moves the bed vertically without changing its tilt. Klipper arbitrarily picks
"reference = 0". Nothing makes that the cheapest choice.

## Design

### Item 1: `probe_preparation` rules in the printer database

The mechanism must be runtime config, not a compiled table. That already works:

- `helix::find_readable()` (`src/application/data_root_resolver.cpp:108-116`) checks the
  writable user path **before** the bundled asset, so `~/helixscreen/config/printer_database.json`
  overrides the shipped copy with no rebuild.
- `printer_database.d/*.json` drop-ins merge at higher priority than the bundle
  (`src/printer/printer_detector.cpp:99`, `merge_user_extensions()`).
- `macro_match` is already an implemented heuristic type
  (`src/printer/printer_detector.cpp:642`), alongside `object_exists`, `sensor_match`,
  `hostname_match` and friends.

Proposed top-level key:

```json
"probe_preparation": [
  {
    "id": "zmod_tare",
    "when": [
      { "type": "macro_match", "field": "macros", "pattern": "LOAD_CELL_TARE" }
    ],
    "operations": ["screws_tilt", "bed_mesh", "probe_accuracy", "z_offset_calibrate"],
    "gcode": ["LOAD_CELL_TARE"],
    "label": "Zeroing load cell",
    "timeout_s": 60,
    "reason": "ZMOD's load-cell zero drifts with bed screws and thermal preload; ZMOD only tares on the print path"
  }
]
```

- `when` is a list evaluated as **AND**. Every command in `gcode` must be guarded by a
  predicate; the block is not self-validating the way a single macro name was.
- `gcode` accepts a string or an array of strings. Arrays are joined with `\n`.
- Rules are evaluated in order, **first match wins** per operation.
- `label` feeds the panel's progress text. `timeout_s` is **added to** the operation's
  existing budget, never replaces it.

### Why predicate-keyed and not a key on the printer entry

`screws_tilt_direction_override()` (`printer_detector.cpp:1958`) keys on the configured
printer *name* matched against `printers[].name`. That shape is wrong here for three
reasons:

1. **Renames.** If ZMOD renames the macro, a model-keyed rule still matches the printer and
   still sends the dead name. Prepended into one script, `Unknown command` aborts the probe
   too, which is worse than the current bug. A predicate simply stops matching and degrades
   to today's behavior. Better still, both names ship as two rules and the predicate acts as
   the version detector:

   ```json
   { "id": "zmod_tare_v2", "when": [{"type":"macro_match","field":"macros","pattern":"LOAD_CELL_ZERO"}], "gcode": ["LOAD_CELL_ZERO"], ... },
   { "id": "zmod_tare_v1", "when": [{"type":"macro_match","field":"macros","pattern":"LOAD_CELL_TARE"}], "gcode": ["LOAD_CELL_TARE"], ... }
   ```

   Model-keyed data cannot express that, because both ZMOD versions report the same printer.

2. **Verified coverage is AD5X-on-ZMOD ONLY.** Checked on the rig 2026-08-25: `[zmod_tenz]`,
   which registers `_LOAD_CELL_TARE`, is instantiated *only* in `mod/ad5x.cfg`. `ff5.cfg` (the
   AD5M/FF5M shared config), `mod.cfg`, `base_mod.cfg` and `klipper13.cfg` reference it zero
   times. `base.cfg` is shared and *calls* it six times but never defines it, so whether an
   AD5M-on-ZMOD install registers the macro at all cannot be answered from an AD5X rig.

   An earlier draft of this doc claimed the predicate "covers AD5M+ZMOD, AD5X+ZMOD and
   Forge-X". That was inference and is unsupported - do not repeat it. The predicate-keyed
   design means an over-broad claim is harmless in behaviour (the rule simply does not fire
   where the macro is absent), but it must not be stated as verified coverage.

3. **One entry, two firmwares.** `flashforge_adventurer_5m` matches AD5M running klipper-mod
   *or* ZMOD. Its heuristics exclude ForgeX (`SUPPORT_FORGE_X`), AD5X stock
   (`SET_EXTRUDER_SLOT`) and AD5X ZMOD (`_IFS_`), but nothing separates klipper-mod from ZMOD
   on the AD5M itself. `zmod_tenz.py` is a ZMOD-supplied klippy extra, so only one of those
   two has the macro. (This does *not* apply to the AD5X: ZMOD is the only AD5X mod -
   klipper-mod refused it in issue #296, Forge-X says "pretty unlikely ever" - and factory
   firmware runs no Moonraker on 7125, so `flashforge_ad5x` already means AD5X-on-ZMOD.)

4. **Vendor rule.** Keeping the vendor name in data means adding the next firmware touches
   one JSON file and zero C++, which is the test `CLAUDE.md` asks for. The evaluator names no
   vendor at all.

### C++ surface

Reference shape is `include/z_offset_persistence.h` - a table plus capability questions as
free functions.

```cpp
// include/probe_preparation.h
namespace helix::probe_prep {

enum class Operation { ScrewsTilt, BedMesh, ProbeAccuracy, ZOffsetCalibrate };

struct Preparation {
    std::string gcode;      ///< Joined block, empty when nothing is needed
    std::string label;      ///< Progress text; empty to leave the panel's default
    uint32_t extra_timeout_ms = 0;
};

/// Resolve the preparation for one operation. Empty gcode = send nothing.
Preparation resolve(const PrinterDiscovery& hw, Operation op);

} // namespace helix::probe_prep
```

Call sites prepend the block into the **same** `execute_gcode` script, so a failing
preparation aborts the probe rather than letting it run on a bad zero. This matches the
existing pattern at `src/ui/ui_probe_overlay.cpp:585` (`gcode = "G28\n"`). Continue-on-error
is deliberately not supported; it would require a separate `execute_gcode` and a swallowed
error, which is a different code path, not a flag.

### Loader changes required

1. **Drop-in merging needs one more top-level key.** `merge_extension_file()`
   (`printer_detector.cpp:208`) already merges a second top-level structure besides
   `printers` - `console_filter_sets`, an object merged by key, last writer wins. So there is
   precedent and the addition follows it rather than inventing a mechanism.

   `probe_preparation` mirrors **printers**, not filter sets: an array merged by `id`, where a
   matching id **replaces in place** (preserving evaluation order), `"enabled": false`
   disables a bundled rule, and a new id appends. In-place replacement is what makes
   first-match-wins safe across drop-ins, so no explicit `priority` field is needed - a
   drop-in overriding a bundled rule keeps that rule's position.

   One related fix: the validator rejects a file with no `printers` array
   (`printer_detector.cpp:242`), guarded by a `merged_sets` escape so a filter-sets-only file
   is legal. A `probe_preparation`-only drop-in - the expected delivery shape - needs the same
   escape, or every such file logs an error.
2. **`compact()` is less of a constraint than it first looks.** It runs at
   `printer_detector.cpp:1754` and erases `heuristics` *inside each printer entry*, then
   `malloc_trim`s (`:120-152`). A **top-level** `probe_preparation` array is untouched by it,
   so it survives compaction for free. The only hard rule is therefore: do not put these rules
   inside a printer's `heuristics` array.

   Latching the resolved `Preparation` onto `PrinterDiscovery` (the way `has_screws_tilt_` is
   set in `parse_objects()`, `printer_discovery.h:269`) is still worth doing so the rules are
   evaluated once rather than per probe, but it is an optimisation, not a correctness
   requirement. Memory cost of keeping the array is a handful of rules, nothing like the
   98-printer heuristics table compaction exists to reclaim.

### Timeouts

`CALIBRATION_TIMEOUT_MS` is 300000 (`include/moonraker_advanced_api.h:53`), sized for a bare
mesh or screws-tilt at roughly 90s each. A preparation that heats from cold can eat several
minutes of that. Hence per-rule `timeout_s` added to the operation budget, rather than
raising the global constant - which would also make a genuinely hung probe sit for ten
minutes.

### Boundary

Literal g-code only. No templating or interpolation. If material-dependent temperatures are
wanted later that is a real feature with real questions about where values come from, and
half-building it now via string substitution will hurt.

Shipped rules stay minimal and always carry `reason`, since they now execute arbitrary
motion and heating. A user editing their own drop-in gains no privilege they lack at the
g-code console.

### Item 2: bind the error label

Add `bind_text="error_message_text"` to `ui_xml/screws_tilt_panel.xml:234` and drop the
hardcoded `text=`. XML-only, so it takes effect on relaunch with no rebuild. Once bound, the
preparation's own failure (`Cell Tare: Error. Weight: 80.0->80.0 https://wiki.zmod.link/FAQ/`,
raised by `gcmd.error` in `zmod_tenz.py`) surfaces through the same path for free.

### Item 3: AD5X screw direction

Add `"screws_tilt_direction": "ccw"` to the `flashforge_ad5x` entry in
`assets/config/printer_database.json`. One key, pure runtime config.

### Item 4: reference-screw re-centering, shown as a hint line (approved)

The principled version of "loosen the reference instead of tightening the other three":
re-center the adjustment vector on an offset chosen to minimize work, rather than on
whatever screw Klipper called the base.

`evaluate_screw_level()` (`include/calibration_types.h:292`) already computes
`signed_adjustment_minutes()` for every screw and produces a `ScrewLevelReport`. Re-centering
is a pure transform over that vector: subtract a chosen offset from every signed value, then
recompute `in_spec` and `worst_index`.

**Corrected 2026-08-25 after checking the math against real data.** The original
"re-centre on the mode, tie-break on the median" rule does not survive contact with an actual
bed. Worked against the rig's own 01:45:59 output (0 / 84 / 229 / 174 minutes):

```
Klipper's advice        moves 3 screws
re-centred on median    -> [-174, -90, +55, 0]   still moves 3 screws
```

Re-centring is only a saving when the non-reference screws are already level **with each
other**. When they disagree among themselves it just changes *which* screws move, at the cost
of contradicting every other UI. The real firing condition is therefore:

1. The non-reference screws are within `tolerance_minutes` of each other, and
2. their common offset from the reference exceeds one full turn (60 clock-minutes - the
   notation is turns:minutes, so this is pitch-independent).

Then the hint is: move the reference by the negation of that common offset, and leave the
rest alone. `[0, +150, +150, +150]` becomes "loosen the base 2 1/2 turns" instead of
"tighten three screws 2 1/2 turns each", which is the case that prompted this.

Implemented as `suggest_screw_recentering()` in `include/calibration_types.h`, returning a
`ScrewRecenterHint` with `available=false` whenever it would not genuinely save work. The
negative case (the rig's own numbers) is a test, not just a comment.

Only apply the transform when it actually helps - a threshold such as "every non-reference
screw is more than one full turn in the same direction" keeps normal cases showing exactly
what Klipper said.

Risks, which is why this needs a decision rather than an implementation:

- **Divergence from every other UI.** Fluidd, Mainsail and the console show Klipper's raw
  numbers. A user cross-checking will see different values from ours.
- **The reference screw may be chosen deliberately** (nearest the Z reference), and we do not
  know that from the config.
- **Screw travel limits are unknown, but they are symmetric.** A screw can bottom out at the
  tight end as easily as it runs out at the loose end, so this is not a hazard that
  re-centering introduces - Klipper's default advice ("tighten these three 2.5 turns") can
  exhaust travel just as easily. If anything, minimizing total turning also minimizes total
  travel consumed. What re-centering does change is *which* screw spends its travel, and we
  cannot know which one has room. That makes this a property to surface in the UI, not an
  argument against the transform.

Mitigation option: keep Klipper's numbers as the primary display and add the re-centering as
a hint line ("Instead of tightening 3 screws 2.5 turns, loosen Left Near 2.5 turns"), or put
it behind a setting. That preserves cross-checkability.

This also gives the travel-limit escape hatch for free, because both forms describe the same
plane. If a screw hits a stop partway, the user can achieve the identical result by moving
the others the opposite way - which is exactly the other form of the same adjustment.
Presenting both is strictly more useful than presenting either alone, and it is one
transform, not two features:

```
Right Near : tighten 1 1/2 turns
Right Far  : tighten 3 3/4 turns      or, instead of all three:
Left Far   : tighten 2 3/4 turns      Left Near (base): loosen 2 3/4 turns
```

### Item 5: a `ScrewsTilt` macro slot

`StandardMacroSlot` (`include/standard_macros.h`) is already the seam for "semantic
operation -> this printer's macro", with pattern detection, HELIX_* fallbacks and user
override in Settings (`MacroSource::CONFIGURED`). Screws-tilt is the one operation that
bypasses it: `MoonrakerAdvancedAPI::calculate_screws_tilt()` hardcodes the command string.

Additions:

- `StandardMacroSlot::ScrewsTilt` in the enum.
- `DETECTION_PATTERNS`: `{"SCREWS_TILT_CALCULATE", "BED_LEVEL_SCREWS_TUNE"}`.
- `FALLBACK_MACROS`: `""` (no HELIX_ fallback - there is nothing generic to synthesize).
- `SLOT_METADATA`: `{"screws_tilt", "Bed Screw Adjustment"}`.

**Detection order is deliberate.** `SCREWS_TILT_CALCULATE` ranks first even though
`BED_LEVEL_SCREWS_TUNE` is the richer macro, because the latter heats to 130/80 and blocks on
`TEMPERATURE_WAIT` (`base.cfg:198-206`). Auto-selecting it would silently turn a ~90s
operation into a multi-minute one for every existing ZMOD user. With item 1 supplying the
tare, the bare command plus preparation already gets most of the benefit at none of the cost.
`BED_LEVEL_SCREWS_TUNE` becomes an opt-in the user picks in Settings, which is exactly what
the slot mechanism exists for.

**Parameterization: FOUR functional sites, not two.** All live in
`moonraker_advanced_api.cpp` and all take the resolved name. Line numbers as of this branch:

| Line | Site | What breaks if missed |
|------|------|-----------------------|
| `:1839` | the command actually sent (first arg to `with_probe_preparation`) | **The slot silently does nothing.** User picks `BED_LEVEL_SCREWS_TUNE`, we keep firing `SCREWS_TILT_CALCULATE`. Fix this one first |
| `:778` | `line.find("SCREWS_TILT_CALCULATE")`, the unknown-command detector | Diagnostic stops firing |
| `:779` | `"... requires [screws_tilt_adjust] in printer.cfg"` | Wrong macro named in a user-facing error |
| `:829` | `MoonrakerError::json_rpc_error("SCREWS_TILT_CALCULATE", ...)` | Stale error context reaching the user AND telemetry. Easy to miss - it reads as a label, not a comparison |

Credit to helixscreen-ad for the last two; my own first pass listed only `:778`/`:779` and
missed the one that matters most. This is the same defect class as #1354 (a name copied where
it should have been passed), so it belongs in the slot change rather than after it.

**Not `macro_patterns.h`.** Checked and agreed with helixscreen-ad: discovery keys on the
config *section* `screws_tilt_adjust` (`printer_discovery.h:271`, `:688`), telemetry uses the
capability `hw.has_screws_tilt()` (`:1566`, `:1891`), and the executable literal lives in one
file. There is no second copy to drift from, so moving the names into `macro_patterns.h`
would be indirection with nothing deduplicated. That header exists because three nozzle-clean
copies had measurably diverged; a single-site name has no such problem.

**Parsing survives the swap.** `BED_LEVEL_SCREWS_TUNE` calls `SCREWS_TILT_CALCULATE`
internally, so the screw lines are byte-identical. Its extra chatter (the `RESPOND` about
cleaning the nozzle, heating messages) is rejected by `parse_screws_tilt_line`, which requires
a `" :"` separator or a `" (base)"` marker (`src/api/screws_tilt_parser.cpp:33-44`). That skip
path is already exercised in `--test` via the legend line the mock emits
(`moonraker_api_mock.cpp:1173`).

**Timeout.** `BED_LEVEL_SCREWS_TUNE` heating from cold can exceed `CALIBRATION_TIMEOUT_MS`.
It needs the same additive budget mechanism as item 1's `timeout_s`.

**Interaction with item 1.** If the slot resolves to `BED_LEVEL_SCREWS_TUNE`, that macro
already tares at `base.cfg:211`, so a `probe_preparation` rule firing as well would tare
twice. For a bare `LOAD_CELL_TARE` that is idempotent and costs about a second, but it stops
being harmless the moment a rule grows a heat step. Add an optional field so a rule can stand
down when the operation already resolves to a macro that prepares itself:

```json
"skip_if_macro_in": ["BED_LEVEL_SCREWS_TUNE"]
```

Evaluated against the slot-resolved macro name, so it stays correct when the user changes the
slot in Settings rather than depending on what we happened to ship.

### Item 6: the reference screw renders a spinning arrow

Two functions decide how a screw is drawn and they disagree about the reference screw.

`get_adjustment_color()` guards correctly (`src/ui/ui_panel_screws_tilt.cpp:704`):

```cpp
if (screw.is_reference || in_spec) {
    return get_theme_color("success");
}
```

`create_screw_indicator()` guards on `in_spec` alone, with a comment asserting something that
is not true (`:591`):

```cpp
if (in_spec) {
    // Inside the level window (reference screws always are): show a static
    // checkmark, no rotation animation
```

`evaluate_screw_level()` does **not** guarantee that. `in_spec` is measured against the
midpoint of the spread, not against zero (`include/calibration_types.h:331`):

```
in_spec[i] = |2*signed_minutes[i] - (highest + lowest)| <= tolerance
```

The reference screw is 0 by construction, so it is in spec only when 0 happens to sit near
the midpoint. Worked against the rig's own 01:45:59 output (base 0, CW 01:24, CW 03:49,
CW 02:54 -> 0, 84, 229, 174 minutes):

```
highest=229 lowest=0
  Left Near (base)   |2*0   - 229| = 229     <- tolerance is single-digit minutes
  Right Near         |2*84  - 229| =  61
  Right Far          |2*229 - 229| = 229
  Left Far           |2*174 - 229| = 119
```

The base ties the worst screw. So it falls to the else branch, and because a reference screw
carries an empty `adjustment` string, `is_clockwise` evaluates false and it draws
**rotate-left (loosen), animated**. The user sees a green circle with a spinning loosen arrow
in it: the colour from one function, the icon from the other.

Fix: both call sites want the same question answered, so hoist it rather than repeating the
predicate and letting them drift again.

```cpp
/// A screw the user should not turn: the datum, or one already inside the window.
[[nodiscard]] inline bool screw_is_settled(const ScrewTiltResult& s, bool in_spec) {
    return s.is_reference || in_spec;
}
```

Used by `get_adjustment_color()` and `create_screw_indicator()` both, and the false comment
goes away with it.

One open nuance, not a blocker: a checkmark reads as "this corner is level", but the
reference is the *datum*, not necessarily level - the bed can be badly out while its base
screw shows green. The text layer already draws that distinction, since
`friendly_adjustment()` returns `"Reference"` for the base and `"Level"` for an in-spec screw
(`calibration_types.h:216-221`). Collapsing both to one checkmark loses it. A neutral datum
marker (pin or anchor) would keep it. Going with the checkmark as requested; noting the
alternative.

This interacts with item 4 only in presentation. The per-screw icons keep mirroring Klipper,
so the base stays settled there. The hint line is where the alternative lives, including the
case where the cheapest move is to turn the base itself.

## Scope

| # | Change | Files | Runtime-only? |
|---|--------|-------|---------------|
| 1 | `probe_preparation` schema + ZMOD rule | `assets/config/printer_database.json` | yes |
| 1 | Evaluator + `PrinterDiscovery` latch | `include/probe_preparation.h`, `src/printer/probe_preparation.cpp`, `include/printer_discovery.h` | no |
| 1 | Top-level drop-in merge | `src/printer/printer_detector.cpp` | no |
| 1 | Prepend at 4 call sites, thread timeout/label | `moonraker_advanced_api.cpp`, `ui_probe_overlay.cpp`, `ui_panel_calibration_zoffset.cpp`, `ui_panel_screws_tilt.cpp` | no |
| 2 | Bind error label | `ui_xml/screws_tilt_panel.xml` | yes |
| 3 | AD5X `screws_tilt_direction` | `assets/config/printer_database.json` | yes |
| 4 | Re-centering | `include/calibration_types.h`, panel | no - **pending decision** |
| 6 | `screw_is_settled()` seam; reference draws a checkmark | `include/calibration_types.h`, `src/ui/ui_panel_screws_tilt.cpp` | no |
| 5 | `ScrewsTilt` slot + parameterized command | `include/standard_macros.h`, `src/printer/standard_macros.cpp`, `moonraker_advanced_api.cpp`, `ui_panel_screws_tilt.cpp` | no |

MAJOR work: 4+ files, touches the probe path. Worktree, test-first.

## Test plan

Tests go before implementation.

- **Evaluator** (new `tests/unit/test_probe_preparation.cpp`, tag `[calibration][probe_prep]`):
  rule matches when the macro is present; does not match when absent; AND semantics across
  multiple `when` entries; first-match-wins across two rules; string and array `gcode` forms
  produce the same joined block; unknown operation names are ignored, not fatal; a malformed
  rule is skipped without taking the database down (cf. the `safe_string` note at
  `printer_detector.cpp:88` - a throw inside the loader costs the entire printer database).
- **Drop-in merge** (extend the detector tests): a `printer_database.d/*.json` file
  contributing a top-level `probe_preparation` entry is merged and outranks the bundle.
- **Survives `compact()`**: resolve, compact, then assert the latched `Preparation` is still
  readable.
- **Call sites**: assert the sent script is `"<preparation>\n<command>"` and that the
  timeout passed is base + `extra_timeout_ms`. Assert on the synchronous entry point, not
  through the deferred path, or the assertion is vacuous.
- **Per-iteration**: drive two consecutive `start_probing()` calls and assert the
  preparation is sent both times.
- **Direction** (extend `tests/unit/test_screws_tilt_result.cpp`): a mock resolved to
  `flashforge_ad5x` flips CW to CCW.
- **Slot** (extend the standard-macros tests): `SCREWS_TILT_CALCULATE` is detected by default
  even when `BED_LEVEL_SCREWS_TUNE` is also present; a user-configured value outranks
  detection; the resolved name reaches both the `execute_gcode` call and the unknown-command
  message; `skip_if_macro_in` suppresses the preparation when the slot resolves to a
  self-preparing macro.
- **Re-centering** (approved, hint line): pure-function tests over the vectors above, plus
  a test that the per-screw values are left untouched.
- **Reference icon** (extend `test_screws_tilt_result.cpp`): `screw_is_settled()` is true for
  the reference even when `evaluate_screw_level()` puts it out of spec. Use the 0/84/229/174
  vector above, which reproduces it - a level bed will not, since every screw is in spec then
  and the bug hides.

Mutation-verify at least one new assertion per area (one spot-check, not exhaustive).
Remember `make -j` builds only `helix-screen`; run `make test` before `./build/bin/helix-tests`,
from the repo root. Swap is currently at 19Mi free, which is the condition that kills the
`helix-tests` link, so keep parallelism modest and check `free -h` first.

## Open questions

1. *(decided)* Item 4 ships as a **hint line**: Klipper's per-screw numbers stay the primary
   display so we remain cross-checkable against Fluidd and the console, with the re-centered
   alternative offered underneath.
2. **Thermal preload.** ZMOD heats to 130/80 before taring in `BED_LEVEL_SCREWS_TUNE`
   (`base.cfg:198-206`), which implies the load cell's zero is temperature-sensitive. Our
   screws-tilt panel does not heat at all. A cold tare followed by a cold probe is at least
   self-consistent, so it may not matter. Because `gcode` is a list, this is testable on the
   rig by editing JSON:

   ```json
   "gcode": ["M104 S130", "M140 S80", "TEMPERATURE_WAIT SENSOR=heater_bed MINIMUM=78", "LOAD_CELL_TARE"]
   ```

   Measure before deciding what ships.
3. *(decided - now item 5 below)* A `ScrewsTilt` macro slot is in scope.
4. **Upstream, as a question not a bug report.** `_FIX_BED_MESH_CALIBRATE` (`base.cfg:234`)
   has zero callers - verified at full depth across the whole config tree, 13 hits and all 13
   are the definition line, one per language copy of `base.cfg`. That is unusual for a
   private `_`-prefixed macro, but "unusual" is not "broken": it could be intended for manual
   invocation, staged for a future release, or left over from a refactor. Ask ghzserg what it
   is for and whether the normal mesh path is meant to tare, rather than telling him it is a
   bug. We are the ones with a partial view of his design. Do this after we have measured the
   thermal question, so the question arrives with data attached.
