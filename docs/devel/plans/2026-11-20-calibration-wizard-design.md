# Calibration: guided wizard, per-step screens, one template

Status: design, awaiting approval. Milestone 1.2. Supersedes the narrower framing of
prestonbrown/helixscreen#2150. Visual design (clickable, all breakpoints):
[HelixScreen Calibration canvas](https://claude.ai/artifact/MkbUt2u6mXHEeJmbHSjbjx).

## Intent

A printer owner should be able to calibrate everything their printer wants, needs or can
calibrate from the screen, without a console, a forum thread or a guess. Two entry points
share one set of step screens:

- **Guided calibration**: pick steps up front, answer the hands-on parts first, then walk
  away while the rest runs. Results are reviewed and saved together at the end. It is the
  optional last page of first-run setup and can be started any time from the Calibration
  screen.
- **Individual access**: every step reachable on its own from one Calibration screen that
  shows what each step last measured and when.

Prior art Preston likes: Bambu's and current Creality calibration flows, which are mostly
automatic but keep the user informed.

## Goals

1. One catalog decides, per printer, which calibration steps exist, their order, which are
   required, recommended or optional, and what each needs (prerequisites, time, heat,
   noise, hands).
2. A guided run: pick, prerequisites once, hands-on steps first, unattended rest, results
   with old next to new, one save.
3. Every step screen built on one template (intro, running, hands-on prompt, result), the
   same screen standalone and inside the wizard.
4. Works at 800x480, micro (480x272), portrait (480x800, 600x1024) and the tiny classes,
   with no scrolling on the step screens at 800x480.
5. Each step records its result and date; the Calibration screen shows them.
6. Firmware-provided routines (motor calibration, extruder encoder, cutter position, belt
   tension, probe scan calibration) get the same UI without per-routine screens.
7. Heater power for MPC comes from the printer database where known, never a bare guess.

## Non-goals

- New calibration algorithms. Every step drives what the firmware or an existing
  HelixScreen calibrator already does.
- Probe Management (accuracy test, probe controls) stays its own screen; it is a tool,
  not a calibration step.
- Touch-screen calibration stays where it is (setup's first step, Settings > Touch).
- Recalibration reminders ("nozzle changed, check Z again") beyond recording results.
  The history store makes them possible later.

## Anti-goals (a failure even if it works)

- A wizard that runs a step the printer does not have, or runs one in an order that
  wastes it (a mesh before Z offset, PID before an MPC migration).
- Moving the machine, heating or running a firmware routine without the user having
  confirmed prerequisites for that step.
- Losing results: a crash or restart partway through must not throw away steps already
  done without telling the user.
- A second copy of a rule: one Z diagram widget, one temperature display, one catalog.
- Vendor names in generic code (`.claude/rules/vendor-abstraction.md`).

## Constraints

- Declarative UI (`.claude/rules/declarative-ui.md`), threading rules
  (`.claude/rules/threading.md`), no exceptions in firmware builds, design tokens only.
- Headers parsed by the ESP32 build stay free of libhv types (as `live_sweep_preview.h`).
- Every target send goes through `TemperatureController`.
- Klipper `SAVE_CONFIG` restarts Klipper on mainline; Kalico accepts `SAVE_CONFIG
  RESTART=0`. Pending autosave changes are all-or-nothing (see Open question 1).
- `LiveSweepPreview` reads Klipper's Unix socket (`BeltStreamClient`), so the live
  resonance curve exists only when HelixScreen runs on the Klipper host.

## What exists today (main, 2026-11-20)

| Piece | Where | Use in this design |
|---|---|---|
| Advanced panel calibration section, 14 capability-gated rows | `ui_xml/advanced_panel.xml` | Replaced by one "Calibration" row opening the Calibration screen |
| Setup wizard steps, `StepId::InputShaper` | `include/wizard_step.h`, `src/ui/wizard/ui_wizard_input_shaper.cpp` | Replaced by a `Calibration` step |
| Belt tension + cutter provider table | `include/belt_cutter_calibration.h` | Folds into the catalog as two firmware-routine rows |
| PA provider table, measured PA | `include/pa_calibration.h`, `include/pa_calibration_run.h` | Wrapped as the PA step |
| Z offset strategies (`PROBE_CALIBRATE`, `FIRMWARE_MANAGED`, `ENDSTOP`) | `include/printer_profile_state.h` | Decide whether Z offset is hands-on and needs paper |
| Z offset indicator, shared by 3 screens | `src/ui/calibration/ui_z_offset_indicator.cpp` | Upgraded in place (tape mode) |
| PID/MPC panel incl. migration edits | `src/ui/calibration/ui_panel_calibration_pid.cpp` | Rebuilt on the template; migration logic kept |
| Input shaper calibrator, live sweep preview | `include/input_shaper_calibrator.h`, `include/live_sweep_preview.h` | Wrapped as the shaper step |
| Moonraker-DB result store pattern | `include/motor_ripple_store.h` | Pattern for the calibration history store |
| `temp_display`, `heater_icon`, temp graph | `ui_xml/components/`, `temp_graph_overlay.xml` | The running view of heater steps |
| 3D bed mesh renderer | bed mesh panel | The running and result view of the mesh step |

## Approaches

**A. Everything as data.** Each step is a descriptor (gcode lines, waits, prompts, power
cycles, save) run by one generic runner. Cheap to add firmware routines; but Z offset,
heaters, mesh and shaper need rich live views and logic that a script cannot express, so
they would sit outside the model anyway.

**B. Everything as classes.** A `CalibrationStep` interface implemented once per step.
Rich steps fit naturally; but every firmware routine becomes a class, which is the
per-routine duplication #2150 exists to remove.

**C. Classes for rich steps, data for firmware routines (recommended).** One
`CalibrationStep` interface. The built-in steps (Z offset, heaters, bed mesh, input
shaper, pressure advance, belt compare) implement it by wrapping the calibrators that
already exist. Firmware routines are rows in one provider table, all run by a single
`ScriptedStep` that renders the template from the row. The wizard, the Calibration
screen and the setup step only ever see `CalibrationStep`.

## Design (approach C)

### Catalog

`include/calibration_catalog.h`, namespace `helix::calibration`. Owns which steps a
printer has and every fact the screens show about them. No LVGL.

- `enum class StepKind`: `ZOffset, Heaters, BedMesh, InputShaper, PressureAdvance,
  BeltCompare, BedScrews, ToolOffsets, Firmware` (the last covers every provider-table
  row).
- `struct StepInfo`: id (stable string, also the history key), name, `Tier` (Required,
  Recommended, Optional, OnRequest), estimated minutes, flags (`heats`, `loud`, `moves`,
  `needs_hands`, `power_cycle`, `saves_config`), prerequisite set, order rank.
- `std::vector<StepInfo> applicable_steps(const PrinterDiscovery&, const ProfileState&)`.
- Tiers come from the printer database (`calibration` block per printer; the field
  exists) with defaults by capability when a printer has no entry.
- Order is a fixed rank table, so dependencies are explicit: motor calibration, belt
  tension, cutter, MPC migration, Z offset, heaters, bed mesh, input shaper, pressure
  advance. The guided run then moves hands-on steps first where the rank allows
  (Z offset needs nothing hot to start, so it can lead).

### Prerequisites

`enum class Prereq`: `BedClear, ToolheadUnloaded, FilamentLoaded, BuildPlate, Paper,
Accelerometer`. Each step declares its set; the prerequisites screen shows the union for
the picked steps, each line naming the steps it is for. A prerequisite that can be
measured is measured and pre-ticked (`ToolheadUnloaded` from the toolhead sensor or the
filament backend, `Accelerometer` from discovery). `Paper` only appears when the Z offset
strategy is the paper test (`PROBE_CALIBRATE` or `ENDSTOP`); nozzle-contact probes
(strain gauge, `load_cell_probe`, Cartographer or Beacon touch) never ask for paper and
their Z offset step is automatic. The per-routine "Is the bed clear?" modal is not shown
inside a guided run.

### Step interface

`include/calibration_step.h`:

```cpp
class CalibrationStep {
  public:
    virtual ~CalibrationStep() = default;
    virtual const StepInfo& info() const = 0;
    virtual const char* intro_component() const = 0;   // XML component on the template
    virtual void start(StepContext&) = 0;               // begins; reports through subjects
    virtual void abort() = 0;                           // safe at any point; ABORTs a manual probe
    virtual StepResult result() const = 0;              // before/after text, pending config
};
```

`StepResult` carries display strings for the results screen and the values to persist.
Steps report progress through subjects (`calibration_step_progress`,
`calibration_step_phase`), so the running view binds instead of polling.

### Firmware routines

`include/calibration_routines.h`: provider table keyed on a detection predicate, as
`z_offset_persistence`. A row lists its steps as data: `Gcode`, `WaitIdle`,
`Prompt{art, text, button}`, `PowerCycle{resume_hint}`, `SaveConfig`. `ScriptedStep` runs
any row. `belt_cutter_calibration.*` becomes two rows and its header is deleted.
Power-cycle steps persist the run's position in the history store and resume on
reconnect.

### Guided run

1. **Pick**: tiles from `applicable_steps()`; required steps locked on; total time and
   hands-on time update live. Micro: a scrolling list. Portrait: one column.
2. **Before you start**: the prerequisite union, measured ones pre-ticked. The chime line
   appears only when sounds are enabled and the printer has a speaker.
3. **Hands-on steps**, then **unattended steps** with a step list and the current step's
   live view. "Stop calibration" disappears once finished.
4. **Results**: before next to after per step, keep or discard (see Open question 1),
   one save.

### Z offset step and the shared indicator

- During `PROBE_CALIBRATE` / `Z_ENDSTOP_CALIBRATE` the readout is **Z position**, never
  "height above bed": Z=0 can be a millimeter or two off the surface.
- The indicator becomes a tape: the nozzle is fixed and the scale scrolls; its zoom
  follows the selected step (±2 mm at 1 mm steps, ±0.1 mm at 0.01).
- **Estimate from history**: the paper should drag near `probe trigger Z − saved
  z_offset` (endstop: `position_endstop`). The estimated bed is drawn dashed, rises as Z
  falls, and the nozzle rests on it once past; the outline turns amber ("past last
  time's position"). No history: the bed is drawn below a break, "distance unknown".
- Controls: Raise / `toolhead_view` (a bed icon where the bed moves in Z, with the
  motion panel's expand arrows and "Raise bed" / "Lower bed") / Lower, and four step
  sizes. No floor at Z=0; the 1 mm step shows a warning.
- One widget: `z_offset_indicator` gains the tape mode and serves the Z offset step, the
  Print Tune overlay and the home Z offset widget.

### Heaters step

- Pick Nozzle or Bed, temperature chips per heater, one "also measure part-fan cooling"
  switch. Method is automatic: MPC where the firmware has it (Kalico today), PID
  otherwise; changing it is under Advanced.
- PID to MPC migration is its own confirm page: what MPC is, the `printer.cfg` change,
  the restart, PID values kept. The existing migration edits
  (`build_control_migration_edits`) do the work.
- Heater power comes from a new printer-database field
  `heater_power: {"extruder": W, "heater_bed": W, "source": "..."}`; unknown asks with
  guidance (cartridge label, 40–70 W typical). Research in progress (sourced table per
  printer, to be reviewed before any value lands).
- Running view: `heater_icon` + `temp_display` (current / target) and the standard red
  temperature graph.

### Bed mesh and input shaper running views

- Bed mesh: the 3D renderer fills in as points arrive, parsed from Klipper's
  `probe at X,Y is z=Z` console lines; unprobed area is a flat gray plane. Scanning
  probes (no per-point lines) show progress, then the finished mesh. Measure render cost
  per point on the slowest supported board before committing.
- Input shaper: `LiveSweepPreview` when HelixScreen runs on the Klipper host; otherwise
  sweep progress from Klipper's per-Hz console lines, then the chart from the CSV.

### History store

`include/calibration_history_store.h`, the `motor_ripple_store` pattern: one Moonraker
database entry per printer, keyed by step id, holding date, before/after display strings
and the values a later estimate needs (Z offset drag point, PA K per filament, MPC
constants). The Calibration screen and the Z offset estimate read it.

### Setup wizard

`StepId::InputShaper` is replaced by `StepId::Calibration`, placed last. Setup is marked
complete before the calibration step begins, so a failure or abort during calibration
never re-runs setup. "Skip for now" leaves everything as it was.

### Calibration screen

One row in Advanced opens it. A "Guided calibration" banner (steps due, minutes), then a
tile per applicable step with its status: done (date and result), check again, never
run, or on request.

### Layout rules

- Footer: Back on the left; Skip and the primary action together on the right; the
  primary action is always bottom right.
- Micro: instruction text moves into the header; three tight columns; 44 px minimum
  touch targets. Portrait: diagram on top, controls under it, footer at the bottom.
- Each step's intro: left panel (what is being calibrated, current value), right panel
  (the only settings that matter, badges), footer.

## Phasing

1. Catalog, prerequisites, history store, Calibration screen (rows open today's panels).
2. Guided run (pick, prerequisites, runner, results), setup wizard step.
3. Z offset step + `z_offset_indicator` tape mode (Print Tune and home widget follow).
4. Heaters step, MPC confirm page, `heater_power` database field with reviewed values.
5. Bed mesh progressive 3D, input shaper live view.
6. Remaining intros on the template: PA, input shaper, bed mesh, belt compare, bed
   screws, tool offsets, vertical artifact compensation.
7. Firmware routines through `ScriptedStep`: motor calibration, extruder two-stage with
   power cycle, belt-sensor recalibration, probe scan calibration.

## Testing

- Unit: `applicable_steps()` per printer persona (tiers, order, Z offset needs paper or
  not); prerequisite union; time totals; expected-drag-Z math including negative Z;
  console parsing of probe points; `ScriptedStep` sequencing including power-cycle
  resume; history store round trip.
- UI: one `ctl` screenshot per changed screen at 800x480, 480x272 and 480x800.
- Hardware: K2 Plus on Jacobean (strain gauge, MPC migration, cutter, belts), a
  paper-test printer (Voron or CB1 rig), a bed-moves-in-Z printer.

## Open questions

1. **Saving.** Klipper keeps pending `SAVE_CONFIG` changes all-or-nothing, and mainline
   `SAVE_CONFIG` restarts Klipper. Per-step keep/discard therefore needs one of:
   (a) all-or-nothing on mainline, per-step only on Kalico (`SAVE_CONFIG RESTART=0`
   after each step); (b) HelixScreen writes kept values itself through its config-edit
   path and never calls `SAVE_CONFIG` for discarded steps; (c) drop per-step discard
   and keep "Save all / Discard all". Also: any step that restarts Klipper (MPC
   migration) loses pending changes, so it runs first or saves first. Recommendation:
   (b), since the config-edit path exists and it behaves the same on every firmware.
2. **Tiers per printer.** Database `calibration` block per printer with capability
   defaults (recommended), or capability defaults only?
3. **Setup placement.** Calibration as the very last setup page, after Telemetry
   (recommended, setup is already committed), or where Input Shaper sits now?
