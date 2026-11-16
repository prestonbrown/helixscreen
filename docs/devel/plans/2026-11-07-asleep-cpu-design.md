# Status work while the display sleeps (#2037)

With the display asleep, HelixScreen still costs about 3% of one core on the Creator 5 Pro
(`docs/devel/PERFORMANCE.md` § "Open costs"). The main loop wakes 41 times a second, and
the remaining cost comes from printer status that keeps flowing into subjects and widgets
nobody can see. This plan sets out where that work goes, which design holds it back, and
how to measure the result on the K1C and the AD5M. It was written from code reading: the
build host's load was above 40 at the time, so every number below is either already in
PERFORMANCE.md or is a measurement still to be taken.

## What runs for each status frame today

1. **WebSocket thread.** `src/api/moonraker_client.cpp#on_ws_message` parses
   the frame and copies every `notify_callbacks_` entry. `notify_status_update` with a
   `bed_mesh` key is parsed in place (`parse_bed_mesh`). Callbacks then run on this thread.
   One of them is `src/application/moonraker_manager.cpp#register_callbacks`,
   which pushes the whole JSON onto `m_notification_queue`, stamped with the klippy epoch.
   AMS backends, `GcodeErrorRouter`, `MoonrakerPerformanceSource` and the plugin backend
   register their own callbacks and never use that queue.
2. **Main thread, every loop pass.** `src/application/application.cpp#main_loop`
   calls `process_notifications()` before `check_display_sleep()` and `lv_timer_handler()`.
   `src/application/moonraker_manager.cpp#process_notifications` drains the
   queue under its mutex. Each status frame goes through `parse_status_notification` into
   `src/printer/status_dispatch.cpp#dispatch_status_frame`, which calls
   `PrinterState::update_from_status`, `ToolState`, `LedController` and every sensor manager.
3. **PrinterState.** `src/printer/printer_state.cpp#update_from_status` walks
   the delta through 14 components (temperature, motion, print domain, fans, excluded
   objects, calibration and others). Each one calls `lv_subject_set_*` directly.
   The status path never hops through `ui_queue_update()`: `set_*_internal()` is for the
   off-thread one-offs (versions, klippy state), and status already arrives on the main
   thread.
4. **Subjects.** `lib/lvgl/src/core/lv_observer.c#lv_subject_set_int` notifies only when
   the value changed (`lv_subject_notify_if_changed`). A steady value costs a compare. A
   temperature in deci-degrees changes on almost every report.
5. **Observers.** Three kinds fire.
   - XML bindings (`bind_text`, `bind_flag_if_*`, `bind_style_if_*`) run inside the
     notification and write the widget at once.
   - `observe<V>()` with the default `Dispatch::Deferred`
     (`src/core/observer_factory.cpp#int_deferred_cb`) allocates one closure per
     notification into the UpdateQueue. Nothing coalesces them, so five changes queued
     before a drain run the handler five times.
   - Raw `ObserverGuard` and immediate observers, for example the PRINT_START collector in
     `src/application/moonraker_manager.cpp#init_print_start_collector`.
6. **Widgets.** A label whose text changes calls `lv_obj_mark_layout_as_dirty`
   (`lib/lvgl/src/core/lv_obj_pos.c#lv_obj_mark_layout_as_dirty`). That sends
   `LV_EVENT_REFR_REQUEST`, which resumes the refresh timer. The refresh timer then runs
   `layout_update_core` over every object on the screen. That function recurses into hidden
   children too, so hiding the screen does not stop the walk. Invalidation and drawing are
   skipped only when the screen is hidden.

## What sleep already changes

| Mechanism | What it does now | Cite |
|---|---|---|
| Update queue drain | 200 ms while asleep, set on every loop pass | `include/refresh_timing.h#update_queue_period_ms`, `src/application/application.cpp#main_loop` |
| Main loop sleep cap | 200 ms while asleep. The input read timer (33 ms) still wakes it about 30 times a second | `include/refresh_timing.h#main_loop_sleep_ms` |
| Hidden spinners | Park their animations | PERFORMANCE.md § "Where idle CPU goes" |
| Debug-touch timer | 500 ms while the setting is off | `include/refresh_timing.h#debug_touch_period_ms` |
| Software overlay, panel power-off | Take `active_screen_hide_hold()`, so the hidden screen does not draw. Power-off also turns invalidation off | `src/application/display_sleep_controller.cpp#create_sleep_overlay`, `#suppress_flush_for_sleep` |
| Hardware blank (Allwinner AD5M, `BacklightBackendBrightnessCli`), Android host sleep | Leave the screen visible. Every status-driven label change is drawn and flushed to a blanked panel, and so is the fbdev self-heal's full-screen invalidate every 10 s | `src/application/display_sleep_controller.cpp#enter_sleep`, `supports_hardware_blank()` on `BacklightBackendAllwinner` in `src/platform/display/backlight_backend.cpp` |

Status is applied at full rate in every mode. Klipper's API server sends subscription
updates at most every 250 ms, so a printer with a heater reporting sends about 4 frames a
second when idle, and the same while it prints, with positions, velocities and progress
added.

## Inventory of the subjects updated at Moonraker's rate

Static counts in today's tree. XML is `bind_*="<subject>"` occurrences under `ui_xml/`.
C++ is files outside `src/printer/`, `src/mock/` and `src/api/` that look the subject up.
How many of these are instantiated at runtime depends on which panels and home widgets
exist, so the census under "Measurement" counts live observers.

| Subject | XML bindings | C++ consumer files | Non-UI consumers that need every value |
|---|---:|---:|---|
| `bed_temp` | 10 | 20 | `TemperatureHistoryManager` (1 Hz samples), `print_start_collector` |
| `bed_target` | 10 | 26 refs | none hot: user action |
| `extruder_temp` | 6 | 4 (plus per-extruder subjects) | `TemperatureHistoryManager` |
| `chamber_temp` | 10 | 12 | `TemperatureHistoryManager`, `print_start_collector` |
| `chamber_effective_target`, `chamber_mode` | 9 each | | none hot |
| `live_velocity`, `live_extruder_velocity` | 2 tune displays | 6 | none |
| `position_x/y/z`, `gcode_position_*` | 2-3 `toolhead_*_text` | 4 | `print_start_collector` |
| `fan_speed` and per-fan subjects | | 4 | fan "stuck" detection in `printer_fan_state` |
| `print_progress`, `print_progress_text`, `print_progress_display` | 3 each | 5 | `print_start_collector` reads progress on a state edge |
| `print_duration`, `print_elapsed`, `print_time_left` | | 3-10 | `print_start_collector` reads duration on a state edge |
| `print_layer_current` | | 9 | `print_photos_controller`, `ui_print_layer_pause` (each layer matters) |

The cost per frame is about which observers write labels, multiplied by one full layout
walk of the screen per refresh that a dirty label requests. The measurement below splits
the two.

## Options

**(a) Defer observer-driven widget updates while asleep, replay on wake.** XML bindings are
object-bound observers and could be recognised in a `patches/` change to `lv_observer.c`.
The roughly 590 C++ `observe<>`/`ObserverGuard` sites cannot be: whether a handler writes
a widget or records data is not visible to the subject. Holding all of them starves data
observers (the temperature history, the PRINT_START collector, the photo and layer-pause
controllers). Holding only the XML bindings leaves the C++ widget writers running. A replay
also has to call only the held observers, because re-notifying a data observer with a value
it already saw re-runs its edge logic. The PrinterState walk still runs on every frame.
Wide reach, partial saving, and a misclassified observer fails silently.

**(b) Apply status less often while asleep.** One choke point,
`MoonrakerManager::process_notifications`, upstream of PrinterState. It skips the
component walk, the subject notifications, the observers and the layout walk together.
In its plain form ("apply every N seconds") it delays wake triggers and erases
transitions: a `print_stats.state` that goes printing, complete, standby inside one window
is never seen as complete.

**(c) Coalesce at the PrinterState-to-subject boundary**, latest value wins per subject,
with an always-deliver list for wake triggers. Subjects stay fresh for readers, and only
notification is held. It needs either a held-notify hook patched into LVGL, or a holding
setter at every hot `lv_subject_set_*` call in 14 components. Both touch PrinterState
directly. `update_from_status` still walks every frame. The data observers starve exactly
as in (b). An always-deliver list fails unsafe: a new trigger subject that nobody adds to
the list is held by default.

**(d) Unsubscribe the hot objects while asleep.** This would also save the WebSocket
thread's JSON parse. The wake would then need a fresh query before its first frame, which
costs one Moonraker round trip, 50-500 ms on these boards, and that either delays the wake
or paints stale values. Rejected.

**Recommendation: (b) with (c)'s per-key rule, moved one step upstream.** Coalesce whole
status deltas at the Moonraker-to-PrinterState boundary, latest value wins per object
attribute. Instead of an always-deliver list, use a closed **telemetry list**: a frame is
held only when every attribute in it is on that list, and any other attribute releases the
held delta and is applied at once. A forgotten attribute is then delivered rather than
held, so the list fails safe. Pair it with Step 0 below, which is independent of the rest
and is the larger lever on the AD5M.

## Design

### Step 0: hide the active screen under every sleep mechanism

`enter_sleep()` takes `active_screen_hide_hold()` for `HardwareBlank` and `HostSleep`, as
the overlay and power-off paths already do. `restore_display_output()` releases it before
the post-wake `lv_refr_now()`, keeping the #303 order: output restored first, then the
synchronous render. This stops drawing and flushing to a blanked AD5M panel. It also covers
the "spinners under hardware-blank keep animating" open cost, and the self-heal invalidate,
since invalidations from a hidden screen are dropped. It does nothing for the layout walk.

### Step 1: hold telemetry frames while asleep

New pure header, `include/status_coalescer.h` (no LVGL, gnu++17, no exceptions: firmware
builds `src/printer/`):

```cpp
namespace helix {

/// Telemetry held while the display sleeps is applied at least this often. Matches the
/// temperature history's 1 Hz sample rate, so the graph keeps its resolution across sleep.
inline constexpr uint32_t kAsleepStatusIntervalMs = 1000;

/// True when every object attribute in a notify_status_update delta is on the telemetry
/// list. An empty delta, an unknown object or any other attribute is false.
bool status_is_telemetry_only(const json& status);

/// The telemetry list, as "object-prefix.attribute" pairs. Exposed for the golden test.
const std::vector<std::string_view>& telemetry_attributes();

class StatusCoalescer {
  public:
    struct Held {
        json status;          ///< Merged delta: the latest value of each attribute held
        double eventtime;     ///< Newest frame's eventtime
        uint64_t klippy_epoch;
    };
    /// Folds a telemetry-only delta in. Latest value wins per object attribute.
    void hold(const json& status, double eventtime, uint64_t klippy_epoch, uint32_t now_ms);
    bool empty() const;
    uint64_t klippy_epoch() const;   ///< Meaningful only while !empty()
    /// True once the oldest held frame is at least interval_ms old.
    bool due(uint32_t now_ms, uint32_t interval_ms) const;
    std::optional<Held> take();      ///< Empties the coalescer
    void clear();
};

} // namespace helix
```

The telemetry list starts here and grows only by measurement. Prefixes match Klipper
object names (`extruder` matches `extruder1`; `temperature_sensor` matches
`temperature_sensor chamber`):

| Object prefix | Attributes |
|---|---|
| `extruder`, `heater_bed`, `heater_generic` | `temperature`, `power` |
| `temperature_sensor`, `temperature_fan` | `temperature`, and `speed` for a temperature fan |
| `fan`, `heater_fan`, `controller_fan`, `fan_generic` | `speed`, `rpm` |
| `motion_report` | `live_position`, `live_velocity`, `live_extruder_velocity` |
| `toolhead` | `position`, `print_time`, `estimated_print_time` |
| `gcode_move` | `gcode_position`, `position`, `speed` |
| `print_stats` | `print_duration`, `total_duration`, `filament_used` |
| `virtual_sdcard` | `progress`, `file_position` |
| `display_status` | `progress` |
| `idle_timeout` | `printing_time` |
| `system_stats` | `sysload`, `cputime`, `memavail` |

Everything else is applied at once, `print_stats.info.current_layer` included: the
photo and layer-pause controllers act on each layer.

`MoonrakerManager` gains two methods, main thread only:

```cpp
/// While on, telemetry-only status frames are held and applied every
/// kAsleepStatusIntervalMs. Turning it off applies whatever is held.
void set_status_hold(bool on);
/// Applies the held delta now, through dispatch_status_frame(). Never inside an
/// UpdateQueue batch.
void apply_held_status();
```

Rules inside `process_notifications()`, in queue order:

- A status frame is held when hold is on, `status_is_telemetry_only()` is true, it carries
  neither the cached-snapshot nor the whole-objects marker, and its klippy epoch matches the
  held delta's (or nothing is held).
- Any other notification (a non-telemetry frame, a marked frame, an epoch change,
  `_connection_state`) first calls `apply_held_status()`, then is processed as today.
- After the drain, if `due(now, kAsleepStatusIntervalMs)`, apply the held delta. `now` is
  `DisplayManager::get_ticks()`, the loop's own tick. Tests drive it with `lv_tick_inc`
  through `LVGLTestFixture::advance_ms`.
- `shutdown()` and the printer-retarget path (`src/application/printer_retarget.cpp`)
  **drop** the held delta with `clear()`. It belongs to the old session or printer.

The invariant this buys: every state PrinterState passes through is a state the printer
reported, since the merge of consecutive deltas is the cumulative state at the last one,
and no frame is ever applied before one that arrived earlier.

Wiring: the existing `DisplayManager::register_sleep_callback` turns hold on at
`cb(true)`. Turning it off cannot wait for `cb(false)`, which runs after the wake's first
frame. See below. Dim and the screensaver do not hold: the screen is lit.

### Wake triggers stay immediate

By construction. Every field that drives a wake today, or should, is off the telemetry
list. That covers `print_stats.state` and `.message` (print completion, cancel and error
through `src/print/print_completion.cpp`), `webhooks.state`, `pause_resume.is_paused`,
`idle_timeout.state`, filament sensor fields, and every `target`. A frame carrying any of
them releases the held delta and applies in the same `process_notifications()` call as
today, so the coalescer adds no latency. The delay that remains today (a deferred observer
waits for the 200 ms drain, and the loop can sleep up to 200 ms) is unchanged and gets
measured. Releasing the held delta first means a trigger's observers read up-to-date
telemetry: the PRINT_START collector reads `print_duration` and `print_progress` on a state
edge.

G-code responses (action prompts) and AMS backend callbacks never pass through this queue,
so the coalescer cannot touch them.

### Every screen correct on its first frame after wake

`wake()` paints synchronously (`lv_refr_now`) straight after `restore_display_output()`.
Two things can be stale at that moment: the held delta, and observer work still waiting in
the UpdateQueue. That second one is already a latent problem today, with up to 200 ms of
queued updates painted late. `DisplaySleepHost` gains one hook:

```cpp
/// Brings every subject and queued observer up to date before the first frame after a
/// sleep: applies held status, ends the hold, drains the UpdateQueue.
std::function<void()> settle_for_wake_frame;
```

Application wires it to `apply_held_status(); set_status_hold(false);
UpdateQueue::instance().drain_until_empty();`. `wake()` calls it when `was_sleeping`,
before `restore_display_output()`.

A wake requested **inside** an UpdateQueue batch (`print_completion`'s deferred observer
calls `wake_display()`, and so do the remote-control commands) cannot drain or apply
status there. The rest of the outer batch is in a local queue a nested drain cannot reach,
and status has never been applied inside a batch, so its observers have never run in one.
In that case `wake()` sets the sleep state and records the first frame as owed. The settle,
`restore_display_output()` and `lv_refr_now()` then run from
`DisplaySleepController::finish_owed_wake_frame()`, which the main loop calls right after
`process_notifications()`. The added delay is one awake loop pass, 33 ms or less, and the
panel stays dark until that frame is ready.

## Tests

Tag `[status_hold]` for the pure layer and the manager, `[display][wake_frame]` for the
wake path, beside `tests/unit/test_status_frame_dispatch.cpp` and
`tests/unit/application/test_display_sleep_wake_resume.cpp`. Each new test is seen red
once by breaking the line it pins.

| # | Case | Pins |
|---|---|---|
| 1 | `extruder.temperature` + `heater_bed.power` frame is telemetry-only | classifier accepts the list |
| 2 | `print_stats` with `print_duration` and `state` is not | one non-telemetry attribute releases |
| 3 | an unknown object (`foo_sensor bar.value`) is not; an empty delta is not | fails safe |
| 4 | table: `print_stats.state/.message/.info`, `webhooks.state`, `pause_resume.is_paused`, `idle_timeout.state`, `filament_switch_sensor x.filament_detected`, `filament_motion_sensor x.enabled`, `extruder.target`, `heater_bed.target`, `display_status.message`, `exclude_object.current_object` are each not telemetry | wake triggers never held |
| 5 | golden list equals `telemetry_attributes()` | growing the list is a reviewed diff |
| 6 | property: 200 random telemetry-frame sequences; merged delta applied once equals sequential application | latest wins, union of keys |
| 7 | `due()` false before the interval from the FIRST hold, true at it; `take()` empties | interval clock |
| 8 | hold off: every frame applies, observer count equals frame count | awake unchanged |
| 9 | hold on: 4 telemetry frames in 900 ms leave `bed_temp` at its pre-hold value; at 1000 ms it holds the 4th value and its observer fired once | coalescing |
| 10 | hold on: held telemetry, then a `print_stats.state` = complete frame, in one drain: an immediate observer on `print_state_enum` reads the newest held `print_duration` | release-before-apply ordering |
| 11 | a cached-snapshot frame, a whole-objects frame, an epoch change and a `_connection_state` each release the held delta first, in queue order | markers and sessions |
| 12 | retarget and `shutdown()` drop the held delta: the new printer's subjects never see it | no cross-printer apply |
| 13 | a trigger frame queued while asleep is applied in the same `process_notifications()` call | no added trigger latency |
| 14 | **first frame after wake**: asleep, `bed_temp` change held, a deferred `observe<int>` label handler on it; wake by touch through `tick()`; an `LV_EVENT_REFR_START` display-event probe captures the label text and `UpdateQueue::pending_count()` on the first render | first frame correct, queue drained |
| 15 | wake from inside a queued callback that also queues a label update after the wake call: no render inside the batch; `finish_owed_wake_frame()` renders once with both the held value and the later label update | owed-frame path |
| 16 | each `SleepMechanism`: the hide hold is taken at sleep and released before the first wake frame (Step 0) | hardware blank hidden |
| 17 | sleep turns hold on; wake turns it off; dim and the screensaver leave it off | wiring |
| 18 | 60 s asleep with telemetry at 4 Hz: `TemperatureHistoryManager` keeps at least 50 samples | history resolution |

## Measurement protocol

Method, units and caveats as in PERFORMANCE.md § "Units and method": percent of one core
from `/proc/<pid>/stat`, wakes from `voluntary_ctxt_switches`, per-thread from
`/proc/<pid>/task/*/stat`. Asleep runs use the config copy with `"sleep_sec": 60` and
`"dim_sec": 0`, the board's own Moonraker (not `--test`), a wait of about 110 s, and a
check that the log says `Display sleeping (<mechanism>)`. Record the mechanism: the AD5M
should read hardware blank, and the K1C's is whatever its log says. Delivery and the
detached runner follow the K1C/AD5X runs in
`docs/devel/plans/2026-11-05-careful-testing-plan.md` phase 3. The AD5M is password-only
dropbear (PERFORMANCE.md, the `sshpass` shim) and has 107 MB of RAM, so stop the service
and run one instance. Claim `device:<name>` for each board. Nothing here moves a machine.
An asleep-while-printing run on real hardware needs the plate confirmed clear and every
step confirmed. Without that, use a mock print as a floor.

A temporary instrument block, never committed, in the style of PERFORMANCE.md § "Finding
what wakes the main loop", logs every 10 s with a timestamp:
- status frames received, applied and held
- microseconds in `process_notifications()` and in `UpdateQueue::process_pending()`
- refresh passes and their microseconds, from `LV_EVENT_REFR_START`/`REFR_READY` display
  events
- flushed pixels
- subject notifications and observer callbacks, from a counter in `subject_notify`
- once, a census of observers per hot subject (`subs_ll` length)

It also takes `HELIX_PROBE_DROP_STATUS_ASLEEP=1`, which discards status frames while
asleep, so the run that uses it is the ceiling for any saving.

| Run | Build | Board state | Windows |
|---|---|---|---|
| A | main tip, instrumented | asleep, idle, real Moonraker | 3 x 60 s |
| B | same | as A, `HELIX_PROBE_DROP_STATUS_ASLEEP=1` | 3 x 60 s |
| C | Step 0 only | as A | 3 x 60 s |
| D | Step 0 + Step 1, `kAsleepStatusIntervalMs` 1000 | as A | 3 x 60 s |
| E | as D with 5000 | as A | 3 x 60 s |
| F | baseline and D | asleep, mock print `--sim-speed 1` (floor) | 3 x 60 s |
| G | baseline and D | awake idle (`scripts/perf-farm-check.sh` idle phase) | its own |
| H | baseline and D | wake latency: `print_stats.state` change (mock completion) to `Display woken`, and touch to first `REFR_READY`, from log timestamps | 10 each |

Report medians per board. **Gate after A and B**: if B is not at least 1 point below A on
either board, the status path is not where the cost is. Stop and report the split before
building Step 1.

## Expected saving and its basis

- On the Creator 5 Pro, the 41 wakes a second while asleep are mostly the 33 ms input read,
  which must stay. At 0.25-0.4 ms per wake (PERFORMANCE.md § "Where idle CPU goes"), the
  wake floor is about 1.0-1.6% of a core. That leaves about 1.4-2.0 points of the 3.0% for
  status work and everything else.
- Step 1 at a 1 s interval applies 1 frame a second instead of about 4, so it removes
  about three quarters of the main-thread status cost. The WebSocket thread's parse stays.
- The estimate is 3.0% down to about 1.8-2.2% asleep idle on boards that already hide the
  screen. On the AD5M, Step 0 removes drawing and flushing as well, so its relative saving
  should be larger. Its baseline has not been measured yet.
- GrumpyScreen's quoted 0.7% is below the wake floor. Matching it needs the input read
  itself to change, which is out of scope.
- Run B replaces this estimate with a measured ceiling.

## Risks

- **Starved per-frame consumers.** Anything that needs every telemetry value sees one a
  second while asleep. The 1 Hz temperature history is the known one (test 18), and the
  interval is chosen for it. The list is closed and fails safe.
- **Stale PrinterState for logic.** Telemetry is up to 1 s old while asleep (Moonraker is
  already 250 ms behind). The non-UI readers are the history and the PRINT_START collector,
  and the ordering rule (test 10) covers the collector's reads on a state edge. A future
  safety rule that reads live telemetry must not run off a held subject. The review should
  check that none does today.
- **Owed wake frame.** New state in `DisplaySleepController`, a critical path. A missed
  `finish_owed_wake_frame()` leaves the panel dark while the app believes it is awake. The
  main loop calls it unconditionally, and test 15 pins it.
- **Hide hold under hardware blank.** Widgets that wait for their own draw (the G-code
  viewer's stall watchdog) now read hidden on the AD5M too. That is already true on every
  other mechanism.
- **Merge cost.** One JSON merge per held frame on the main thread, bounded by the list's
  size. The instrument's `process_notifications()` time checks it.
- **Test isolation.** Hold state lives in `MoonrakerManager`. Fixtures that build one must
  see it off and empty at both ends.

## Review and verification

PrinterState's input path, DisplayManager and threading are critical paths, so this is
MAJOR work: a worktree, test-first, one review at the finish against
`docs/devel/REVIEW_RUBRIC.md`. That review should cover three things. First, that nothing
runs off the main thread: the coalescer lives in `process_notifications()`. Second, the
release-before-apply ordering and the owed-frame path. Third, the telemetry list, entry
by entry, for anything a wake or a safety decision reads. Then one `make land-gate`,
`make mutate-diff` after landing, and the device runs above as the evidence. Step 0 can
land on its own before Step 1, but only if run C on the AD5M shows it pays.

## Open questions

1. Action prompts (`src/application/gcode_response_routing.cpp#attach`,
   `set_on_show`) and the klippy-shutdown recovery modal show while asleep but never call
   `wake_display()`. The issue assumes "prompts" wake the screen. Should this work make them
   wake, or should that be its own issue?
2. The interval: 1 s keeps the history at full resolution, while 5 s saves more and leaves
   a stepped graph after a long sleep. Run E against D decides, unless Preston prefers one.
3. Should asleep-while-printing get a real-print run on the K1C? `sleep_while_printing`
   defaults on, so it is the common case, but only a real print carries Klipper's load.
4. Per-observer coalescing of `observe<V>` deferred closures (one queued closure per
   observer, latest value wins) would also help while awake. It changes semantics for
   handlers that act on every value, so it belongs in a separate issue if run A's census
   shows deferred handlers dominate.

## Decisions (Preston, 2026-11-07)

- Action prompts and the Klipper-shutdown recovery modal wake the display, as part of this work rather than a separate issue.

## Status (2026-11-07): implemented, measurement pending

Steps 0 and 1, the prompt and recovery-dialog wake, and the first-frame-correct wake are
implemented on `docs/plan-sleep-cpu`. The device runs in "Measurement protocol" (A to H)
have not been taken: they need the K1C and the AD5M, and this plan stays until they are.

What differs from the design above:

- The hold is wired in `src/application/asleep_status_wiring.cpp#wire_asleep_status`, not
  inline in Application. Sleep turns the hold on; waking leaves it to the settle hook,
  since a wake inside an UpdateQueue batch notifies its callbacks at once.
- The owed wake frame is finished from `DisplayManager::check_display_sleep()`, which the
  main loop calls right after `process_notifications()`.
- The coalescer's clock is `lv_tick_get()`, which the firmware also has.
- Action prompts and the shutdown and error recovery dialog wake the display through
  `include/display_wake_request.h#request_display_wake`, a handler the active
  DisplayManager installs. A disconnected-printer dialog does not wake.
- Test 17 (sleep and wake wiring) is covered by the two wake-frame cases, which run
  through `wire_asleep_status`. Test 18 (history samples across a 60 s sleep) is not a
  unit test: the history throttles on the wall clock, so it is left to run D.
- The headless mock cannot show the saving. Its simulation sends whole objects every
  tick (`target`, `state`, `homed_axes` beside the temperatures), so no frame is
  telemetry-only and the hold releases on each one. On thelio (load 24 to 50), asleep
  with a mock print at `--sim-speed 1`, three 60 s windows read 1.18/1.38/1.28% of one
  core before and 1.18/1.40/1.33% after, run side by side: no change, as expected. Only
  a real Moonraker, which sends changed fields only, measures Step 1.
