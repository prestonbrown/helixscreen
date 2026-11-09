# Maintenance Reminders

Print-hour meters for consumable parts: how they count, how they start, when they announce, and
where to change them. The rules are pure C++ in `include/maintenance_meters.h`; the live state,
subjects and notices are `src/ui/panel_widgets/maintenance_model.cpp`.

## The meters

| Display | Key | Default | Applies when |
|---|---|---|---|
| XY axis | `rails` | 300 h | always |
| Z axis | `z_axis` | 500 h | always |
| Nozzle | `nozzle` | 500 h | always |
| Extruder | `extruder` | 300 h | always |
| Air filter | `filter` | 200 h | enclosed, a chamber filter fan, or a firmware filter counter |
| Electronics fans | `electronics_fans` | 500 h | a `controller_fan` object exists |
| Belt tension | `belt_tension` | none | only with a printer database row (Voron-style printers) |

There is one meter per printer, tool changers included. Cold pull is emergency service, not
scheduled maintenance, so it has no meter. Display order is the `MeterId` enum; persistence goes by
key.

## What a meter counts

Print hours come from `print_stats.print_duration`, which leaves out pauses. The Air filter follows
a firmware filter counter on printers that keep one (`src/printer/chamber_air_modes.cpp#match`). A
meter can also have a calendar limit, chosen from `kDayChoices`; its due share is the larger of its
hour share and its day share. A limit stays idle until the wall clock is trustworthy
(`wall_clock_trusted`, `include/wall_clock_trust.h`), so a board with no RTC never reports a meter
due from a bad clock.

## Starting values

A meter whose record has no `interval_h` is unseeded. `try_seed` in the model waits for hardware
discovery (`Capability::HardwareGeneration`) and, while the wizard is identifying the printer, for a
printer type. `seed_defaults` then applies the database row, the generic default, or Off. A seeded
meter never changes on its own: the user's choice wins.

A per-model override lives in `assets/config/printer_database.json`:

```json
"maintenance": { "belt_tension": { "hours": 300 }, "filter": { "hours": 300, "days": 180 } }
```

`hours: 0` turns a meter off on that model. The field is read by
`PrinterDetector::get_maintenance_defaults`. Unknown keys and bad values are ignored field by field.

Records live in `settings.json` at `<printer prefix>maintenance`. Upgrading from three meters keeps
their records; a record with no `z_axis` entry starts Z from the rails hours and every other new
meter at zero.

## Notices

A meter that falls due is latched by `take_pending_notices`, so it announces once per interval. The
model announces at print end, and a few seconds after the printer becomes known when nothing is
printing. One due meter gets a toast with **Details**; several share one toast that opens the
overview. Each due meter also gets a history entry whose action is `maintenance:<key>`. Mid-print
crossings wait for the print to end. There is no snooze. Reset or a raised interval re-arms a meter.

## Screens

| Surface | XML | Notes |
|---|---|---|
| Home tile | `ui_xml/components/panel_widget_maintenance.xml` | Ranked rows from `due_order` (`maintenance_r<k>_*`); the rung budgets three rows, `maintenance_tile_rows` hides what does not fit |
| Overview | `ui_xml/maintenance_overlay.xml` | A card per enabled meter, two columns from MEDIUM up |
| Task page | `ui_xml/maintenance_task_overlay.xml` | What and why, steps, Mark done. Text is `kMeterText` in the model |
| Intervals | `ui_xml/maintenance_intervals_overlay.xml` | Hours and day limit per meter; the only place an Off meter comes back on |

A firmware filter counter is cleared on the printer before the meter resets
(`src/ui/panel_widgets/maintenance_model.cpp#reset_meter`).

## Adding a meter

Add the `MeterId`, its key, its `kDefaultIntervals` entry and its `kMeterText` row, then raise
`kMeterCount` and the `count` of the `<repeat>` in the overview and intervals XML. New strings go
through `make translation-sync`.
