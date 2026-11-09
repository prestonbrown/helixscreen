# Extended maintenance reminders: design

Status: draft 2026-11-08, waiting on Preston's answers to the open questions at the end.
prestonbrown/helixscreen#1901. Ships on main (1.2). Deleted in the change that ships the work
(`docs/CLAUDE.md`); what lasts moves into a `docs/devel/MAINTENANCE.md` written in that change.

## Why

The home tile tracks three hour meters: HEPA filter, nozzle, linear rails. FlashForge's stock
Creator 5 firmware (1.9.6 onward, `docs/devel/printer-research/FLASHFORGE_CREATOR5_FIRMWARE.md`)
tracks seven, each with a prompt after a number of print hours and a page explaining the job.
Every item on that list applies to most FDM printers, so the work belongs in the generic
maintenance model and not in any printer backend.

## The signal

What a meter measures, and where each input comes from:

| Input | Source | Who publishes it | Who has none |
|---|---|---|---|
| Print time | `print_stats.print_duration`, which counts printing and leaves out pauses | Klipper, every printer | none |
| Filter run time | A firmware filter counter read by the chamber air provider | Snapmaker U1 only today (`src/printer/chamber_air_modes.cpp#match`, the `u1.filter_reset` row) | everyone else counts print time instead |
| Wall-clock time | The system clock, trusted only once it is past `ams::k_unknown_stamp_before` | every board once NTP or an RTC has set it | a board booted offline with no RTC, until the clock is set |
| Filament length | `print_stats.filament_used`, per job, and per extruder for multi-tool printers (`include/printer_print_state.h#PrinterPrintState/get_extruder_filament_used_subject`) | Klipper, every printer | none, but see "Intervals" for why v1 does not use it |
| Enclosure | `printer_is_enclosed`: the database flag, the user's enclosure override, or a configured chamber heater (`include/bed_drying.h#is_enclosed`, `src/printer/printer_state.cpp#refresh_bed_drying_capability`) | resolved for every printer | an open printer reads 0 |
| Chamber filter fan | `printer_has_chamber_filter_fan` (`src/printer/printer_capabilities_state.cpp`) | printers whose Klipper config names one | most open printers |
| Mainboard fan | A Klipper `controller_fan` object (`src/printer/hardware_object_table.cpp`, `ObjectKind::ControllerFan`) | most Klipper configs that cool a board | some stock vendor configs that drive the board fan outside Klipper |

## What exists (audit, 2026-11-08)

- **The pure rules**: `include/maintenance_meters.h`. `MeterId { Filter, Nozzle, Rails }`,
  `kMeterKeys = {"filter", "nozzle", "rails"}`, `kDefaultIntervals = {200, 500, 300}`,
  `kIntervalChoices` (Off, 25 h to 2000 h), `accrue`, `reset_meter`, `set_interval`, `most_due`,
  `to_json` / `from_json`, `from_plugin` (the example plugin's store), `DurationTracker`.
  The enum's ints index `State::meters` and name the subjects; persistence goes by key, so the
  enum order is free to change.
- **Storage**: `settings.json`, per printer, at `<printer prefix>maintenance`
  (`src/system/settings_manager.cpp#get_maintenance_record`,
  `src/system/settings_manager.cpp#set_maintenance_record`). Not the Moonraker
  database. Shape: `{ "total_h", "meters": { "<key>": { "interval_h", "used_h", "reset_at_h",
  "notified" } } }`. Shipped since v1.1.0-beta.10 (`git tag --contains 1b8639345d`), so records
  exist in the field and their keys must keep meaning what they mean.
- **The live model**: `src/ui/panel_widgets/maintenance_model.cpp`. Loads on the widget's
  subject hook, accrues from `print_duration` whether or not the tile is placed, saves every
  6 minutes of printing, on a due crossing, and at print end (`on_print_active`).
  `load` takes over the example plugin's store the first time a printer has no record.
- **Reset**: `confirm_reset` (a `modal_confirm`), then `reset_meter`, which clears a firmware
  filter counter on the printer first and resets locally only once the printer accepted it
  (`src/ui/panel_widgets/maintenance_model.cpp#reset_meter`).
- **The prompt**: `notify_due` shows one WARNING toast the moment a meter crosses its interval,
  mid-print included, latched by `Meter::notified`. Nothing goes to notification history and the
  toast has no action.
- **The overlay**: `ui_xml/maintenance_overlay.xml`, a `<repeat count="3">` card per meter
  (ring, name, "37 h of 300 h", "% left · reset at", a Reset button) and a `setting_group` of
  three interval dropdowns (`src/ui/controls/ui_maintenance_overlay.cpp`, one templated
  callback per meter). Reached only from the home tile
  (`src/ui/panel_widgets/maintenance_widget.cpp#maintenance_clicked_cb`).
- **The tile**: `ui_xml/components/panel_widget_maintenance.xml`, six rungs. Wide and Full draw
  a row per meter from a `<repeat count="3">`, and `measure` in
  `src/ui/panel_widgets/maintenance_widget.cpp` budgets `kMeterCount` rows.
- **Actionable notices**: `ui_notification_warning_with_action` plus
  `helix::ui::set_notification_action_handler("id", fn(arg))`; an action string `id:arg` reaches
  the handler with `arg` (`src/ui/notify/ui_panel_notification_history.cpp#"notification_action_handler(const std::string& action)"`).
  `ToastManager::show_with_action` gives the toast a button. Exemplar:
  `src/application/hardware_setup_prompter.cpp`.
- **The printer database**: `assets/config/printer_database.json`, 108 printers. Per-model
  optional fields are read through static `PrinterDetector` accessors (`is_enclosed`,
  `get_belt_tensioner`) and tested in `tests/unit/test_printer_detector.cpp`.
- **Cold pull (#2011, unlanded, `feat/cold-pull`)**: `ColdPullActions::reset_meter` is set on
  Done only, and `ColdPullRunner` logs "no cold pull meter to reset" in its place
  (`src/ui/filament/cold_pull_runner.cpp` on that branch). Its design names
  `maintenance::reset_meter()` as the hook. The runner is pinned to one head.
- **No nozzle-change flow exists** anywhere in `src/`; the hook below is ready for one.
- **Clock trust**: `plugin_prompt_clock_trusted` in `src/system/helix_plugin_installer.cpp` is
  the only "is the wall clock real" rule, file-local. A second caller is coming, so it moves to
  one shared function (see Interfaces).

## FlashForge's list against ours

| FlashForge item | HelixScreen meter | Key | Notes |
|---|---|---|---|
| XY axis cleaning and lubrication | XY axis (today "Linear rails") | `rails` (kept) | The existing meter, renamed on screen |
| Z axis cleaning and lubrication | Z axis | `z_axis` (new) | Seeded from `rails` on first load, see Migration |
| Nozzle and hotend replacement | Nozzle | `nozzle` (kept) | Servicing it also resets Cold pull |
| Nozzle cold pull | Cold pull | `cold_pull` (new) | #2011's Done resets it |
| Extruder cleaning | Extruder | `extruder` (new) | Filament dust in the drive gears |
| Air filter replacement | Air filter (today "HEPA filter") | `filter` (kept) | Many printers have carbon, not HEPA, so the name widens |
| Mainboard and power supply fan cleaning | Electronics fans | `electronics_fans` (new) | |

Printers in our database this list maps onto: `flashforge_creator_5` and
`flashforge_creator_5_pro` (enclosed, four-head tool changers), `flashforge_ad5m_pro` and its
ForgeX twin (enclosed), `flashforge_adventurer_5m` and its ForgeX twin, `flashforge_ad5x`
(open frame, so the filter starts Off). FlashForge's own interval numbers are not in our research
notes; reading them off the Creator 5 Pro's stock firmware is a hw-verify task before the
database rows get numbers (open question 6).

## Goals

- Seven meters, each with a default interval, Off, a one-line description on its card, and a
  task page that explains the job.
- A meter that does not apply to the printer starts Off, decided from what the printer reports.
- Stored hours carry over for the three existing meters with no loss.
- A guided flow that finishes a service (cold pull now, a nozzle change later) resets the right
  meters itself.
- The overlay at 800x480 shows every enabled meter without scrolling.

## Non-goals

- Per-head meters on multi-tool printers (open question 3).
- Vendor-specific explainer text or pictures. The text says what to do in general and points at
  the printer's manual for model specifics.
- A maintenance log or history of past services beyond the last reset.
- Syncing meters across screens through the Moonraker database.

## Item list and defaults

Display order is the enum order. Persistence goes by key, so renumbering costs nothing on disk;
only the subjects' `m<i>` names and the tests move.

| # | `MeterId` | Key | Name / short | One-line description (card) | Generic default | Starts Off when |
|---|---|---|---|---|---|---|
| 0 | `XyAxis` | `rails` | XY axis / XY | Clean and lubricate the X and Y rails or rods | 300 h | never |
| 1 | `ZAxis` | `z_axis` | Z axis / Z | Clean and lubricate the Z screws and guides | 500 h | never |
| 2 | `Nozzle` | `nozzle` | Nozzle / Nozzle | Replace a worn nozzle or hotend | 500 h | never |
| 3 | `ColdPull` | `cold_pull` | Cold pull / Pull | Pull debris out of the nozzle with a cold pull | 200 h | never |
| 4 | `Extruder` | `extruder` | Extruder / Extr. | Brush filament dust out of the extruder gears | 300 h | never |
| 5 | `Filter` | `filter` | Air filter / Filter | Replace the chamber air filter | 200 h | not enclosed, no chamber filter fan and no firmware filter counter |
| 6 | `ElectronicsFans` | `electronics_fans` | Electronics fans / Fans | Clean dust from the mainboard and power supply fans | 500 h | no `controller_fan` object |

The 300 h, 500 h and 200 h values for the kept meters are today's `kDefaultIntervals`. The new
ones are first guesses for Preston to confirm (open question 5).

**Per-printer defaults** come from a new optional database field, read through one accessor:

```json
"maintenance": { "cold_pull": { "hours": 150 }, "filter": { "hours": 300, "days": 180 },
                 "electronics_fans": { "hours": 0 } }
```

A key present overrides the generic default and the applicability rule for that meter (`hours`
0 means Off on this model); a key absent falls back to the table above. Unknown keys and bad
values are ignored field by field, as `from_json` treats the settings file.

## Intervals

- **Print hours** stay the primary measure for every meter, as today.
- **Calendar**, as a second limit: an optional "or every N days, whichever first", choices
  `{0 (none), 30, 60, 90, 180, 365}`. It catches the cases print hours miss: grease that dries
  on a printer used twice a month, a carbon filter that saturates sitting in a dusty room. It
  defaults to none on every meter unless the printer's database row sets one. A meter's due
  share is the larger of its hour share and its day share; the ring, the status and the tile's
  "most due" all use that share. The card says "12 d left" when days are the closer limit.
  A reset stamps `reset_at_s` only when the clock is trusted; an untrusted stamp leaves the day
  limit idle until the next reset with a trusted clock, and an untrusted "now" never makes a
  meter due.
- **Filament length**: not in v1. Print hours already track filament through the extruder
  closely enough for every item on the list, and the one item where length would beat hours
  (abrasive filament wearing a nozzle) depends on the material, which a length counter cannot
  see. Adding a third accrual source also adds a per-tool baseline for multi-tool printers.
  Open question 2 asks whether Preston wants it anyway.

## Storage and migration

Same file, same path, same record. Per meter the record gains two optional fields:

```json
"meters": { "z_axis": { "interval_h": 500, "used_h": 12.5, "reset_at_h": 340.0,
                        "notified": false, "interval_d": 0, "reset_at_s": 1793000000 } }
```

- **Kept keys**: `rails`, `nozzle`, `filter` keep their records untouched. `rails` becomes the XY
  meter by name only; it already meant the motion rails, and every existing user's hours there
  were counted against the XY motion they mostly service.
- **Unseeded meters**: a meter whose record has no `interval_h` is unseeded. `from_json` marks
  it so, `to_json` writes its hours but leaves `interval_h` out, so it stays unseeded until the
  printer's defaults are known. Once the printer is identified and its capabilities have
  arrived, `seed_defaults` gives every unseeded meter its interval (model row, else generic,
  else Off when it does not apply) and the model saves. A seeded meter is never re-seeded: the
  applicability rule picks a starting point, the user's choice wins after that.
- **The Z seed**: `z_axis` is seeded from `rails` on first load (its `used_h` and `reset_at_h`),
  because the last rails service is the best guess at the last Z service. Every other new meter
  starts at 0 used hours: an upgrade must not raise four overdue notices at once.
- **Existing users' filter on an open printer** keeps its interval: it is seeded, so the new
  applicability rule does not turn it Off. A new install on the same printer starts it Off.
- **The example plugin**: `from_plugin` reads only the three keys it ever wrote; unchanged.
- No compat shim: nothing here reads an old shape differently. Missing fields default, as now.

## Prompts

- A crossing during a print is latched, not shown. At print end (`on_print_active` going to 0),
  and at load when the printer is idle, every meter that is due and not yet notified is
  announced once:
  - one meter: a WARNING toast "Cold pull is due" with a "Details" action that opens its task
    page, plus a notification-history entry with action `maintenance:<key>`;
  - several: one toast "3 maintenance tasks are due" whose action opens the overlay, plus one
    history entry per meter.
- `notified` latches as today, so the notice comes once per interval; Reset or a raised interval
  re-arms it.
- The tile keeps showing Danger for as long as a meter is due, so a dismissed notice is not lost.

## Reset flows

1. **By hand, from the task page**: "Mark done" opens the existing confirm
   (`confirm_reset`), whose text names every meter the service covers ("Reset Nozzle and Cold
   pull?"). The firmware filter counter keeps its printer-first reset.
2. **From a guided flow**: `record_service(MeterId)` resets without asking, because finishing the
   flow is the confirmation, saves, and toasts "Cold pull done: meter reset". The cold pull
   runner's `a.reset_meter` branch calls `record_service(MeterId::ColdPull)`; a future nozzle
   change flow calls `record_service(MeterId::Nozzle)`. Cancel or abandon never calls it.
3. **What a service covers** is one pure table: `Nozzle` covers `{Nozzle, ColdPull}` (a new
   nozzle is a clean one); every other meter covers itself. Both paths above read it.

## UI

### Measured (main tree's build, `--test`, headless, tile placed 2x2)

| Surface | 800x480 (MEDIUM) | 480x272 (MICRO) |
|---|---|---|
| Overlay viewport (`overlay_content`) | 708 x 414 | 430 x 224 |
| Card (`mt_card_0`), pitch | 676 x 90, 97 | 414 x 59, 63 |
| Reset button on a card | 110 x 40 | 75 x 24 |
| Interval row (`row_interval_0`) | 692 x 107 | 420 x 51 |
| Three cards + total, then three interval rows | 313 + 22, then 321: scrolls 258 px | scrolls about 1.5 screens |
| Tile 2x2, Full rung (`mt_full`) | 219 x 225: title 22, rows 30 high | 134 x 129: title 18, rows 18 high |

Seven meters in today's layout would be seven 97 px cards plus seven 107 px rows at 800x480,
about 1,450 px, three and a half screens. So the layout changes:

### Overlay

- **Cards only, interval rows gone from it.** Each card: ring with the time left, name, the
  one-line description, "37 h of 300 h". The Reset button and the "% left · reset at" note move
  to the task page. The whole card is the tap target that opens the task page.
- **Two columns at MEDIUM and up** (`ui_breakpoint >= 3`): (676 - `space_sm`) / 2, about
  334 px per card. With the button gone the text column gets about 334 - 16 padding - 58 ring -
  12 gap = 248 px, enough for the longest English name in `font_body`. Four rows of about 81 px
  plus the total line is about 350 px, inside 414: no scroll at 800x480. The plan's first
  implementation step re-measures this with `ctl geom` before committing to it, and a German
  pass checks the longest translated name.
- **One column below MEDIUM**, scrolling: at 480x272 seven 63 px cards are about 440 px, two
  screens. Acceptable on that panel.
- **Off meters** have no card. A last row, "Reminder intervals", opens a sub-overlay with one
  `setting_dropdown_row` per meter (hours) and, for each, the days choice: the only place an Off
  meter is turned back on, so it lists all seven.

### Task page (one overlay, filled per meter)

`maintenance_task_overlay.xml`, opened from a card, a toast action or a history entry with the
meter as its argument. Destroyed on close, so it costs nothing while unused. Top to bottom:

- name, ring, "37 h of 300 h", the day limit when set, "Last done at 340 h" (and the date when
  the stamp is trusted);
- **What and why**: two or three sentences;
- **Steps**: three to five numbered lines, ending with "Your printer's manual has the parts and
  lubricant for your model";
- **Actions**: "Mark done" (primary), and "Start cold pull" on the Cold pull page when a guided
  flow for it is registered (`find_guided_device_flow`-style lookup by id, so the page never
  names the cold pull overlay), later "Start nozzle change" the same way.

The text is DATA: a C++ table of `TR_NOOP` strings per meter, published to
`maintenance_task_*` string subjects the XML binds. Draft copy, for Preston's read:

| Meter | What and why | Steps |
|---|---|---|
| XY axis | Dust and old grease on the X and Y rails or rods cause noise, ringing and wear. | Power off. Wipe each rail or rod clean with a lint-free cloth. Apply a thin film of the lubricant your manual names. Move the toolhead end to end by hand to spread it. |
| Z axis | Dirty or dry Z screws bind, which shows up as uneven layer lines. | Power off. Wipe the Z screws and guides clean. Apply the lubricant your manual names, sparingly. Raise and lower the bed or gantry through its travel. |
| Nozzle | Nozzles wear, abrasive filament fastest; a worn one under-extrudes and strings. | Heat the hotend. Follow your manual to swap the nozzle or hotend. Re-check the Z offset after the swap. |
| Cold pull | A cold pull draws out burnt filament and debris before they clog the nozzle. | Run the guided cold pull, or heat, push filament through, cool to the pull temperature and pull firmly. Repeat until the tip comes out clean. |
| Extruder | Filament dust packs into the drive gears and costs grip. | Unload filament and power off. Open the extruder as your manual shows. Brush the gear teeth clean. Reassemble and load filament. |
| Air filter | A saturated filter stops trapping fumes and particles. | Power off. Remove the filter cover. Replace the filter element. |
| Electronics fans | Dusty board and power supply fans run hot and fail early. | Power off and unplug the printer. Blow the dust out of the fans and vents. Check each fan spins freely. |

### Home tile

The tile cannot list seven meters: the 2x2 Full rung at 480x272 has 129 px for a title and
18 px rows. It shows the **most-due enabled meters, as many as fit**, ranked:

- new ranked slot subjects `maintenance_r<k>_{short,left,status,pct,on}` for k < `kTileRows`
  (4), filled from `due_order(state)`, which the XML binds instead of `m<i>`;
- `measure` budgets three rows for Wide and Full (today's count), and the widget hides slots
  past what its measured height holds; the rung therefore does not move when meters are
  enabled or disabled;
- Glance, Strip, Column and Cell already show the single most-due meter and do not change.

## Interfaces

`include/maintenance_meters.h` (pure, no LVGL):

```cpp
enum class MeterId : int { XyAxis = 0, ZAxis, Nozzle, ColdPull, Extruder, Filter, ElectronicsFans };
inline constexpr int kMeterCount = 7;
inline constexpr std::array<const char*, kMeterCount> kMeterKeys = {
    "rails", "z_axis", "nozzle", "cold_pull", "extruder", "filter", "electronics_fans"};
inline constexpr std::array<int, 6> kDayChoices = {0, 30, 60, 90, 180, 365};

struct Meter {            // existing fields, plus:
    bool seeded = true;                  ///< false: interval not yet chosen for this printer
    int interval_d = 0;                  ///< 0 = no calendar limit
    std::optional<int64_t> reset_at_s;   ///< trusted wall clock at the last reset
};

/// What the printer reports, for deciding which meters apply.
struct Applicability { bool enclosed, chamber_filter_fan, firmware_filter, controller_fan; };

/// One meter's database override; nullopt fields fall back to the generic default.
struct MeterDefault { std::optional<int> hours, days; };
using ModelDefaults = std::array<MeterDefault, kMeterCount>;

[[nodiscard]] bool applies(MeterId m, const Applicability& a);
[[nodiscard]] MeterDefault default_for(MeterId m, const Applicability& a, const ModelDefaults& model);
/// Gives every unseeded meter its default; returns whether anything changed.
bool seed_defaults(State& s, const Applicability& a, const ModelDefaults& model);
/// The meters servicing @p m also resets, @p m first.
[[nodiscard]] std::vector<int> service_covers(MeterId m);
/// Due share, the larger of hours and days; @p now_s nullopt when the clock is untrusted.
[[nodiscard]] double used_fraction(const Meter& m, std::optional<int64_t> now_s);
/// Enabled meters, most due first, ties in display order.
[[nodiscard]] std::vector<int> due_order(const State& s, std::optional<int64_t> now_s);
/// Meters due and not yet notified; latches them.
std::vector<int> take_pending_notices(State& s, std::optional<int64_t> now_s);
void reset_meter(State& s, int meter, std::optional<int64_t> now_s);   // gains the clock
[[nodiscard]] ModelDefaults model_defaults_from_json(const nlohmann::json& maintenance_field);
```

`accrue` stops returning crossings to show at once; the model calls `take_pending_notices` at
print end and at idle load instead. `meter_status`, `hours_left` and `most_due` gain the
`now_s` argument the same way.

Elsewhere:

- `PrinterDetector::get_maintenance_defaults(const std::string& printer_name)` returning
  `maintenance::ModelDefaults`, beside `is_enclosed`.
- `helix::maintenance_model::record_service(maintenance::MeterId)` and
  `open_task_page(maintenance::MeterId)`; the notification action `maintenance:<key>` is
  registered in `init_subjects`.
- `wall_clock_trusted(int64_t s)` in one shared header, called by the plugin prompt snooze and
  the meters. `scripts/syntax_check.py` on every changed header before the plan's first commit.
- `scripts/check_layering.py` on the cold pull hook: `src/ui/filament` calling into
  `src/ui/panel_widgets` is inside one layer today, but the check confirms it.

## ESP32 cost

The model, tile and overlay are in the firmware build
(`firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`).

- Per-meter subjects: nine `lv_subject_t` (about 40 bytes each on a 32-bit build) plus 264 bytes
  of string buffers, about 620 bytes in `HELIX_PSRAM_BSS`. Four new meters: about 2.5 KB. The
  ranked tile slots add about 0.8 KB. Task page subjects: about 1 KB of buffers.
- The overlay keeps its tree (`destroy_on_close` false): four more cards are roughly 50 LVGL
  objects, about 6 to 8 KB of heap while it exists. Losing the three interval rows wins some of
  that back. The task page and the intervals sub-overlay are destroyed on close.
- Flash: the explainer table is about 2 KB of English strings, plus the translation catalogs.
- Measure with the firmware size report in the implementing change, as
  `docs/devel/PERFORMANCE.md` describes; nothing here is a candidate to cut.

## Tests (`[maintenance]`, red first)

Pure (`tests/unit/test_maintenance_meters.cpp`):

- defaults table: seven keys, the kept three keep their keys and defaults;
- a record with only the three old keys loads them unchanged and the four new meters unseeded;
- unseeded meters round-trip without `interval_h`; seeded ones keep it;
- `seed_defaults`: open printer with no filter fan starts Filter Off; enclosed starts it at
  200 h; a firmware filter counter alone turns it on; no `controller_fan` starts Electronics
  fans Off; a model row overrides both ways (`hours: 0` on an applicable meter, hours on an
  inapplicable one); a seeded meter is never changed; returns false when nothing changed;
- the Z seed copies `rails` used and reset hours once, and not when `z_axis` already exists;
- calendar: a day limit makes a meter due before its hours; an untrusted now never makes it
  due; an untrusted reset stamp leaves the day limit idle; status bands use the larger share;
- `service_covers(Nozzle)` is `{Nozzle, ColdPull}`, every other meter only itself;
- `due_order`: Off meters absent, ties in display order;
- `take_pending_notices` returns each due meter once, re-armed by a reset or a raised interval;
- `model_defaults_from_json` ignores unknown keys, wrong types and negative numbers per field.

Model (`tests/unit/test_maintenance_model.cpp`):

- a crossing mid-print shows nothing; print end shows one toast and one history entry; three
  crossings show one toast and three entries;
- the history action `maintenance:cold_pull` opens the Cold pull task page;
- `record_service(Nozzle)` resets Nozzle and Cold pull with no modal, saves, and the next
  session loads it;
- the firmware filter reset still goes printer first through Mark done;
- seeding waits for the printer's capabilities, then saves once.

UI (`tests/unit/test_widget_size_maintenance.cpp` and an overlay test):

- the tile's ranked rows show the most-due meters, hide the extra slots at 480x272 2x2, and the
  rung does not change when a fourth meter is enabled;
- the overlay at 800x480 with seven meters enabled has no vertical scroll; at 480x272 it scrolls;
- the intervals sub-overlay lists all seven, Off included.

Printer database (`tests/unit/test_printer_detector.cpp`): `get_maintenance_defaults` for a
model with the field, without it, and for an unknown printer.

Cold pull (on #2011's branch or after it lands): Done calls `record_service(ColdPull)`; Cancel
and Abandon do not.

Screenshots at the finish: overlay at 800x480 and 480x272, one task page, the tile at 2x2 on
both.

## Decisions (Preston, 2026-11-08)

These override anything above that disagrees.

- Z is a new meter seeded from the existing `rails` hours; `rails` stays as XY, renamed on screen.
- No filament length limit.
- One meter per printer, including tool changers.
- A toast plus a history entry at print end, never mid-print, with no snooze.
- Cold pull is emergency service, not suggested maintenance: the Cold pull meter is dropped, so
  there are seven meters minus that one plus the belt check below.
- The remaining generic intervals get checked against Bambu's published recommendations before
  they ship.
- A session may read the Creator 5 Pro's stock firmware for the FlashForge intervals.
- The belt tension meter appears on Voron-style printers only.
