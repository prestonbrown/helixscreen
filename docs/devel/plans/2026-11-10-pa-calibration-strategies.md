# Pressure-advance calibration strategies (Creator 5 Pro stock sweep)

Status: plan, not started. Preston's answers of 2026-11-10 are folded in (§ Decisions).

## Why

`helix::pacal` (`include/pa_calibration.h`) models one kind of calibration: send ONE firmware
command, read its console output with regexes. That fits the Snapmaker U1 (`FLOW_CALIBRATE`) and
the Klipper4FlashForge firmware for the Creator 5 (`FF_PA_CALIBRATE` from its `[ff_pa]` extra,
github.com/Klipper4FlashForge/firmware).

It does not fit a stock or Z-Mod Creator 5 Pro, which is what our C5 (192.168.30.130) runs. There
is no `ff_pa` there. The sweep lives in the stock touchscreen app (`firmwareExe`), which drives it
as a stream of short gcodes over two stock Klipper commands, and scores it on the host side. With
HelixScreen replacing that app, nothing runs the sweep. So today the PA screen never appears on a
stock or Z-Mod C5.

Other printers will need other shapes again (the K2 is the next likely one, shape unknown). So
the change is a strategy abstraction that the panel talks to, with one strategy per kind of
procedure and one table row per firmware.

## What the C5 stock firmware does

Recovered from `firmwareExe` by Klipper4FlashForge (`docs/notes/51-pa-calibration-recovered.md`
in their repo), and confirmed against the strings in our C5's binary
(`/usr/prog/PROGRAM/software/firmwareExe`, 2.0.0) on 2026-11-09.

- **Klipper side** (`klippy/extras/pa_adjust.py`, stock): `PA_ACTION ACTION=<a> PC=<pc>` and
  `PA_GET`, both forwarded to `mcu eboard`. `ACTION=11` arms a capture of a 16-bit sample stream
  the eBoard receives on USART3. Any other value stops it, and the eBoard scores the trace.
  `PA_GET` replies `Result is value=<n>`, and **only 9 passes**. Both are immediate MCU commands,
  not queued behind motion. Neither has `desc=`, so neither is in `/printer/gcode/help`, and
  `pa_adjust` has no status, so it is not in `objects/list` either. The `[pa_adjust]` config
  section is the only evidence it exists.
- **Prologue:** `G92 E0`, `M83`, `SET_VELOCITY_LIMIT ACCEL=5000`,
  `SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=9`, `SET_PIN PIN=enable_pin_tmc_x VALUE=1.00`, the
  same for `_y`, then `M400`. The two pins are a second driver latch on PH2/PH3 that Klipper does
  not know about: **1 disables the X/Y drivers.** Every `G1 X..` in the sweep is planned and
  stepped against dead drivers, so the head stays parked over the purge chute and about 1 g of
  filament drops into it.
- **Park** (done by the app's caller before the sweep): purge chute X275 Y254 plus a per-tool
  offset dx {0, -2, -2, -2}, dy {0, 0, 1.5, -8}, Z = probe Z + 2.8.
- **One line per candidate**, candidates in the fixed order 0.0100 0.0200 0.0150 0.0350 0.0250
  0.0300 0.0400, Y = 50 + 5·i:
  `PA_ACTION ACTION=11 PC=666`, `SET_PRESSURE_ADVANCE ADVANCE=<k>`, `G1 X40 Y<y> F30000`, then
  slow/fast/slow/slow/fast/slow segments (`F1080`/`F10980`, E 0.0568 mm per mm throughout),
  `PA_ACTION ACTION=0 PC=666` **with no M400 before it**, `M400`, `PA_GET`.
- **Selection:** at most 5 sweeps of 7 lines. A sweep's winner is its smallest passing K. Stop
  after 3 winners; the result is their mean. Fewer than 3 winners after 5 sweeps is a failure.
- **Restore** (`resetTmcPa`): both pins back to 0, `M400`, `SET_KINEMATIC_POSITION` to the parked
  coordinates (the carriage never left them), `M400`.
- **Apply:** the app installs per-tool K for one print with the stock
  `SET_PA_ADVANCE T0=.. ENABLE=1` (`virtual_sdcard.py`) and clears it at print end.

Read live on our C5: `output_pin enable_pin_tmc_x` / `_y` are status objects, both 0 at rest.

## Decisions (Preston, 2026-11-10)

1. **Apply for session.** On success the C5 strategy sends
   `SET_PRESSURE_ADVANCE EXTRUDER=<extruder> ADVANCE=<k>` and the panel shows the value plus the
   `[extruderN] pressure_advance:` line to make it permanent. Nothing writes printer.cfg.
   `Outcome::applied = true`.
2. **Stuck latch: release and unhome on connect, without asking.** If either pin reads non-zero
   at connect and no run of ours is live, send `SET_PIN .. VALUE=0` for both, then `M84`, and show
   a notice. Nothing moves; the next move needs a home.
3. **K2: leave a slot.** Build the two kinds we know. The `Run` interface must take a third
   without the panel changing.
4. **Refuse a run where every line got the same verdict** (all 9 or none 9), with the message
   that the sensor is not telling candidates apart. The stock app would report 0.0100.
5. (Earlier, 2026-11-09) A PA run's end turns off a nozzle preheat the user set. Stays as is.

## Design

### Generic layer: `include/pa_calibration.h` + new `include/pa_calibration_run.h`

`pa_calibration.h` keeps its capability questions (`is_supported`, `provider_name`, `is_per_tool`,
`sane_range`, `is_plausible`) and gains one trait the panel needs:

```cpp
/// Whether the firmware sets heater targets itself during the run. True for one-command
/// firmwares (the panel calls CalibrationOwnedHeat::let_firmware_set), false when HelixScreen
/// drives the run and stays the only one heating.
bool firmware_sets_heat(const PrinterDiscovery& hw);
```

`Procedure` and `procedure_for()` become internal to the one-command strategy. Generic code stops
naming them; `estimate_minutes()`/`applies_result` reach the panel through traits.

`pa_calibration_run.h`, the interface every strategy hides behind (checked with clang
`-std=c++17 -fsyntax-only`, 2026-11-10):

```cpp
namespace helix::pacal {
enum class Verdict : uint8_t { Unknown, Pass, Fail };
struct Attempt { int index; int expected; float k_tried; Verdict verdict; };
struct Outcome { float k; bool applied; };
struct RunRequest { int tool_index; int temp_c; };
struct RunCallbacks {                       // fire off the LVGL thread
    std::function<void(const Attempt&)> on_attempt;
    std::function<void(const Outcome&)> on_result;
    std::function<void(const std::string&)> on_error;
};
class Run {
  public:
    virtual ~Run() = default;
    virtual void stop() = 0;               // end at the next safe point, send owed cleanup;
                                           // no callback after it returns
    virtual void before_disconnect() = 0;  // send owed cleanup now, fire-and-forget
};
std::shared_ptr<Run> start_run(IMoonrakerAPI&, IMoonrakerClient&, const PrinterDiscovery&,
                               const RunRequest&, RunCallbacks);   // nullptr = unsupported
std::vector<std::string> recovery_status_objects(const PrinterDiscovery&);
std::string recovery_gcode(const PrinterDiscovery&, const nlohmann::json& status); // "" = clean
}
```

### The provider table gains a kind

`src/printer/pa_calibration.cpp`'s `Provider` gets `enum class Kind { FirmwareCommand, HostSweep }`
plus a detection for config sections. Rows, matched in this order:

| Row | Detect | Kind |
|---|---|---|
| Snapmaker U1 flow calibrator | object `filament_parameters` | FirmwareCommand |
| Klipper4FlashForge `ff_pa` | object `ff_pa` | FirmwareCommand |
| FlashForge stock eBoard sweep | config section `pa_adjust` **and** objects `output_pin enable_pin_tmc_x`, `output_pin enable_pin_tmc_y` | HostSweep |

`ff_pa` stays ahead of the stock row: a K4FF machine carries `[pa_adjust]` too, and its own
command is the better path there. `start_run()` switches on the kind. That switch is the one place
a third kind (K2) gets added.

Detection by config section needs `PrinterDiscovery` to record `[pa_adjust]`. Find how
`parse_config_keys` classifies sections (`sections_of`, `hw::ObjectKind`), and add the
section there rather than a vendor-named probe.

### Strategy 1: one firmware command (`src/printer/pa_run_firmware_command.cpp`)

A `Run` around the existing `IAdvancedAPI::start_pa_calibrate` and `PACalibrateCollector`,
unchanged in behaviour. `stop()` = the existing cancel (stop listening; the firmware finishes the
command it is in). `before_disconnect()` = nothing owed. Fix the wrong comment in
`pa_calibration.cpp`: `ff_pa.py` is Klipper4FlashForge's, not "our firmware repo".

### Strategy 2: host-driven sweep (`src/calibration/flashforge_pa_sweep.{h,cpp}`)

A vendor-named file in `src/calibration/`, the same shape as `snapmaker_screws_tilt.cpp` (a
self-pinned `shared_ptr` collector that steps commands). Split in two:

- **`pa_sweep::Sweep`, pure** (checked with clang, 2026-11-10). No I/O, no timers.
  `next()` returns the next `Step {Prologue|Line|Restore|Done, script, candidate}`, `on_verdict(int)`
  feeds a `PA_GET`, `stop()`/`fail()`, `result()`, `failure()`. Plus free functions
  `parse_pa_get(line)`, `select(verdicts_by_sweep, cfg)`, and
  `latch_recovery_gcode(pin_x, pin_y)`. `Config` holds the stock constants above as defaults.
- **The driver**, a `Run`: sends each `Step::script` as ONE `gcode/script` call (the whole line
  block, so Klipper sees the same stream the app sends, with no round trip between moves and no
  added M400), listens on `notify_gcode_response` for `Result is value=`, and feeds the verdict
  back. The prologue homes if `toolhead.homed_axes` lacks xyz, then
  `SAVE_GCODE_STATE NAME=helix_pa`, then parks over the chute at the tool's offset, and sets Z to
  `probe Z + 2.8`. What that probe Z is on Z-Mod needs a check on the C5, and only then are the
  pins latched. Restore: both pins to 0, `M400`,
  `SET_KINEMATIC_POSITION` to the parked X/Y/Z, `M400`, the extruder's prior `pressure_advance`,
  the prior `max_accel`/`square_corner_velocity`, `RESTORE_GCODE_STATE NAME=helix_pa`. Then on
  success `SET_PRESSURE_ADVANCE EXTRUDER=.. ADVANCE=<k>` (decision 1).
- `stop()`: send nothing new. When the line in flight returns, send Restore, then end with
  `Failure::Stopped`. A heater-off the panel sends after `stop()` queues behind at most one line
  (about 5 s plus M400), because every step is short.
- `before_disconnect()`: send Restore immediately, fire-and-forget. Klipper runs it after the
  line in flight.
- Verdicts: `Attempt.verdict` is Pass/Fail per line, `expected` = 21 (3 winning sweeps × 7).
- Refusals before anything latches: no filament or a cold nozzle (the panel already heats),
  tool index outside the 4 offsets.

### Panel (`ui_panel_calibration_pa.cpp`)

`begin_measure()` calls `pacal::start_run()` and keeps the `shared_ptr<Run>` where `pa_cancel_`
is now. `stop_run()`, `before_disconnect()` and `on_deactivating` call `Run::stop()` /
`before_disconnect()` before `heat_.turn_off()`. `heat_.let_firmware_set()` happens only when
`pacal::firmware_sets_heat(hw)`. The per-candidate chips can show pass/fail when
`Attempt.verdict != Unknown`. The panel never names a firmware.

### Shutdown and interrupted runs

- **Graceful quit / SIGTERM:** a host sweep with the latch set must make SIGTERM take the graceful
  path, as a held heater does now. Today that is `HeaterHold::any_held_signal_safe()`. Rename
  `HeaterHold` to a name that covers any printer-side cleanup owed (proposal: `CleanupHold`) in
  the same change, and have the sweep hold one from latch to restore. The graceful path then runs
  `before_disconnect()`.
- **Hard kill, crash or lost link:** connect-time recovery (decision 2). Add
  `pacal::recovery_status_objects(hw)` to the subscription builder
  (`src/api/moonraker_discovery_sequence.cpp`, beside `zoffset::required_status_objects`), and
  send `pacal::recovery_gcode(hw, status)` from the connect steps
  (`src/application/discovery_steps_core.cpp`, beside `zoffset::persistence_enable_gcode`) when it
  is non-empty and no print is active. Plus a toast: "A pressure advance run was interrupted. The
  X/Y drivers were released; home before moving."

### Mock

The mock Moonraker (`src/mock/`) gets a `[pa_adjust]` config section, the two `output_pin`
objects (with `SET_PIN` updating them) and `PA_GET` replies from a verdict script in
`HELIX_MOCK_PA_VERDICTS` (comma list, cycled; default `0,9,9,0,0,0,0`, so the smallest pass is
0.0200). Document it in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`.

## Tests

New `tests/unit/test_flashforge_pa_sweep.cpp`, tag `[pa_sweep]`, on the pure `Sweep`:

- The prologue latches both pins only after the park move, and Restore releases both before
  `SET_KINEMATIC_POSITION`.
- Line scripts carry the candidates in the stock order with Y = 50 + 5·i, and have no `M400`
  between the last `G1` and `PA_ACTION ACTION=0`.
- `select`: smallest pass per sweep; mean of the first 3 winners; stops after 3 winners; fewer than
  3 winners in 5 sweeps fails; a sweep with no pass contributes nothing.
- All-same verdicts across the run fail with `NoDiscrimination` (all 9, and none 9).
- `stop()` mid-sweep yields Restore next, then Done, with `Failure::Stopped`.
- `parse_pa_get`: `Result is value=9` → 9; the `SET_PRESSURE_ADVANCE` echo, empty and garbage →
  nullopt.
- `latch_recovery_gcode`: empty when both are 0, release + `M84` when either is non-zero.
- Tool offsets applied to the park for T0..T3.

Extend `tests/unit/test_pa_calibration.cpp` (`[pa_calibration]`):

- The stock row matches on `[pa_adjust]` + both pins, and loses to `ff_pa` when both are present.
- `firmware_sets_heat` is true for U1 and K4FF, false for the stock sweep.
- `recovery_status_objects` is empty on a U1.

Driver against the mock client (`[pa_sweep]`, fixture like `PACalibrateTestFixture`): a full run
with scripted verdicts reports the expected mean and sends `SET_PRESSURE_ADVANCE EXTRUDER=`;
`stop()` during a line sends Restore after that line's reply and fires no callback;
`before_disconnect()` sends Restore at once.

Panel (`test_pa_calibration_panel.cpp`): on a host sweep no `let_firmware_set` is called and the
heater-off follows Restore. On a firmware command, behaviour is unchanged.

Each new case gets one hand break (red), restored. `make mutate-diff` once at the end of the
series.

## Constraints

- Vendor rule: "FlashForge", `PA_ACTION`, the pin names and the chute numbers appear only in
  `flashforge_pa_sweep.*` and the table row. Generic code asks `pacal::`.
- Threading: every `Run` callback is off the LVGL thread; the panel's existing `tok.defer`
  wrapping stays. The driver pins itself (`shared_from_this`) across its gcode callbacks.
- Firmware builds are gnu++17: no C++20 in the new headers.
- No exceptions: `parse_pa_get` uses `text_io::parse_leading`, not `std::stoi`.
- `helix::Regex`, never `std::regex`, if the driver matches lines.

## Hardware bring-up (C5, needs Preston present: filament loaded, chute clear, about 1 g per run)

Preston is away from the printers until 2026-11-15, so this waits. Every write is asked for first.

1. By hand from the console, no HelixScreen involved: `PA_ACTION ACTION=11 PC=666`, a few moves,
   `PA_ACTION ACTION=0 PC=666`, `M400`, `PA_GET`. A value that arrives (even 0) proves the path.
2. One sweep through HelixScreen against a hand-tuned PA tower on the same filament.
3. Kill HelixScreen with SIGKILL mid-line. On restart, check the pins are released, the axes
   unhomed and the toast shown.
4. Check afterwards that `pressure_advance`, both pins, `max_accel` and `square_corner_velocity`
   are back where they started (except the applied K).

## When it ships

The "What the C5 stock firmware does" section moves into
`docs/devel/printer-research/FLASHFORGE_CREATOR5_FIRMWARE.md`, which has nothing on PA today. The
strategy table and how to add a kind go into `docs/devel/printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md`
and the `pa_calibration_run.h` header comment. Then this file is deleted.

## Out of scope

- K2 or any third kind: the slot only.
- Persisting K to printer.cfg.
- Running the sweep at print start as the stock app can.
