# Screensaver Print Progress, Phase 1 (shared groundwork) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. The project's Verification Cadence (root `CLAUDE.md`) overrides those skills: tags while building, ONE review and ONE `make land-gate` at the finish.

**Goal:** Every screensaver receives the print and its latched end state; the end of a print plays in the running saver instead of waking the screen; a too-heavy saver during a print falls back to the flip clock; savers with no in-scene display get a drifting progress card.

**Architecture:** The pure print model in `screensaver_clock_reading.h` grows the failed, cancelled and no-estimate states. A pure `PrintEndLatch`, stepped by the manager on every idle-check tick, decides how long an ended print stays on screen. `SaverBase` becomes the input sink for every saver and hosts the progress card, a small LVGL canvas of its own above the saver's scene, so no saver's dirty tracking changes.

**Tech Stack:** C++17 (firmware-compatible headers), LVGL 9.5, Catch2, `make`.

**Spec:** `docs/devel/plans/2026-11-07-screensaver-print-progress-design.md` (Phase 1, Decided, Constraints).

## Global Constraints

- Works at the cheapest level of every saver and at 16 and 32 bpp (`PixelFormat::RGB565` / `XRGB8888`).
- Sims stay pure: no LVGL, no clock in `*_sim` files. The print reaches them as input.
- The gate's printing budget is unchanged (`saver_budget_share`).
- The user's sleep timeout wins: `decide_idle` is not changed.
- An end state holds until input, a new print, a printer switch or disconnect, or an app restart; display sleep does not acknowledge it; never persisted.
- With no saver running, print completion behaves exactly as today.
- Overlay setting "Show print progress", on by default, hides only the running-print card; paused and ended states show regardless.
- Constants `kCamelCase`; no exceptions, no RTTI, no `std::regex`; spdlog only; `#include "hv/json.hpp"`.
- Everything new under `#if HELIX_ENABLE_SCREENSAVER` except the completion-alert decision, which builds everywhere.
- Comments describe the code as it is; no development archaeology.
- Tests: each new or changed test is broken by hand once and seen red. Tag runs go to zeus (`make remote-test TAG='[tag]'`, check `ssh zeus.local uptime` < ~60 first).

## Review Focus

1. **A print ends while no saver runs, the user never touches, the saver starts later:** it must show the end state (latch set by the idle tick, not by a saver). Pinned in Task 2.
2. **App starts with the printer already Complete / Cancelled / Error:** must read as no print, never a stale DONE. Pinned in Task 2.
3. **Printer reconnects mid-print, or the user switches printers while DONE is latched:** latch clears; a running print on the new printer shows normally. Pinned in Task 2.
4. **ERROR with a saver running:** heaters still turn off, the modal is still created, the display does not wake. Pinned in Task 3.
5. **Portrait and tiny screens (480x272, 272x480) and the card's drift range:** the card stays fully on screen at every drift offset. Pinned in Task 6.

---

### Task 1: Failed, cancelled and no-estimate states in the print model

**Files:**
- Modify: `include/screensaver_clock_reading.h`, `src/ui/screensaver/screensaver_clock_reading.cpp`, `src/ui/screensaver/screensaver_clock.cpp` (`clock_print_phase`, `clock_print_text`/`clock_lines`)
- Modify (mode switches only): `src/ui/screensaver/screensaver_flip_clock*.cpp`, `screensaver_aqua*.cpp`, `screensaver_highfly*.cpp`
- Test: `tests/unit/test_screensaver_clock_reading.cpp`, `tests/unit/test_screensaver_flip_clock.cpp`

**Interfaces:**
- Produces:
  - `enum class ClockPrintPhase : uint8_t { NONE, PREPARING, PRINTING, PAUSED, COMPLETE, FAILED, CANCELLED };`
  - `enum class ClockMode : uint8_t { CLOCK, PROGRESS, COUNTDOWN, PAUSED, DONE, FAILED, CANCELLED };` (PROGRESS = running print, progress known, no estimate yet)
  - `ClockReading::progress_pct` filled in PROGRESS, COUNTDOWN, PAUSED; 100 in DONE; last known in FAILED/CANCELLED.
  - `bool clock_mode_is_end(ClockMode m);` true for DONE, FAILED, CANCELLED.
  - `kClockDoneHoldMs` is deleted: the end state lasts as long as the input says so (Task 2 decides that).

- [ ] **Step 1: Write the failing tests** in `test_screensaver_clock_reading.cpp`:
  - `"a running print with no estimate reads its progress"`: PRINTING, `remaining_s=-1`, `progress_pct=37` → `mode()==PROGRESS`, `reading().progress_pct==37`; then `remaining_s=600` → COUNTDOWN.
  - `"an ended print reads its end for as long as the input says so"`: run COUNTDOWN, then COMPLETE for 10 min of steps → DONE throughout, progress 100; FAILED → `FAILED` with the last progress kept; CANCELLED → `CANCELLED`; then NONE → CLOCK.
  - `"an end seen without a running print still reads as that end"`: first input COMPLETE → DONE (the latch, not ClockState, filters stale ends).
  - Replace `"paused freezes, done holds then returns to the clock, cancelled clears"` and the flip clock's `"a finished print holds 0:00:00 and DONE for a minute, then shows the clock"` / `"a cancelled print goes straight back to the clock"` with the behaviour above. Update `"the flip clock reads the print lifecycle, with ended prints as no print"` to: Cancelled→CANCELLED, Error→FAILED, Complete→COMPLETE, Idle→NONE.
- [ ] **Step 2: Run** `make remote-test TAG='[screensaver_clock_reading],[screensaver_flip_clock]'`. Expected: the new cases FAIL (no PROGRESS/FAILED/CANCELLED).
- [ ] **Step 3: Implement** the enums, `clock_mode_is_end`, `ClockState::step` (drop `done_ms_` and the hold; ended phases map straight to their mode), `clock_print_phase`. `clock_lines` for FAILED and CANCELLED returns `lv_tr("FAILED")` / `lv_tr("CANCELLED")` with the time-left line empty; PROGRESS returns `"{pct}%"` and an empty second line. The three clock savers treat PROGRESS like COUNTDOWN's caption path and FAILED/CANCELLED like DONE until Phase 2 restyles them; `clock_face_same` compares mode and progress for PROGRESS too. Add the two strings to the translation source (`docs/devel/TRANSLATION_SYSTEM.md`).
- [ ] **Step 4: Run** the same command. Expected: PASS; also `[screensaver_aqua],[screensaver_highfly],[screensaver_parts]`. Re-record the fingerprints only if `[screensaver_fingerprint]` fails because of the removed hold (state why in the commit).
- [ ] **Step 5: Commit** `feat(screensaver): the print model reads failed, cancelled and no-estimate prints`

### Task 2: The end-state latch, and every saver receives the print

**Files:**
- Create: `include/screensaver_print_latch.h`, `src/ui/screensaver/screensaver_print_latch.cpp`
- Modify: `include/screensaver.h` (`SaverHost`, `Screensaver::clock_input_sink`), `include/screensaver_base.h` + `src/ui/screensaver/screensaver_base.cpp`, `include/screensaver_clock_saver.h`, `src/ui/screensaver/screensaver_manager.cpp` (`clock_input`, constructor, `on_idle_check_tick`), `src/application/display_manager.cpp#DisplayManager::screensaver_host`
- Test: `tests/unit/test_screensaver_print_latch.cpp` (new, tag `[screensaver_print_latch]`), `tests/unit/test_screensaver_parts.cpp`, `tests/unit/test_screensaver_flip_clock.cpp`

**Interfaces:**
- Consumes: Task 1's `ClockPrintPhase`.
- Produces:
  ```cpp
  struct PrintLatchInput {
      ClockPrintPhase host_phase;   // what the printer reports now
      uint64_t printer_epoch;       // changes on reconnect or printer switch
      uint32_t inactive_ms;         // ms since the last user input (lv_display_get_inactive_time)
  };
  class PrintEndLatch {
    public:
      /// Advances by dt_ms. Returns the phase savers should see.
      ClockPrintPhase step(uint32_t dt_ms, const PrintLatchInput& in);
      ClockPrintPhase latched() const;   // COMPLETE, FAILED, CANCELLED or NONE
      void reset();
  };
  ```
  Rules: a running phase (PREPARING, PRINTING, PAUSED) passes through and clears any latch; a transition from a running phase to an end phase latches it; an end phase never seen after a running one reads NONE; input after the latch (`inactive_ms` smaller than the time since latching) clears it; an epoch change clears it; NONE from the host clears it.
  - `SaverHost` gains `std::function<uint64_t()> printer_epoch;` and `std::function<uint32_t()> inactive_ms;` (unset = 0 / never changes).
  - `SaverBase` derives from `ClockInputSink`; `Screensaver::clock_input_sink()` is overridden in `SaverBase` to return `this` for every saver. `ClockSaver` stops deriving from it separately. `SaverBase` gains `protected: ClockInput read_input() const` (inherited).
  - `ScreensaverManager` owns `helix::ui::PrintEndLatch print_latch_`, steps it in `on_idle_check_tick()` (which runs with or without a saver), and `clock_input()` substitutes the latch's phase for the host's.

- [ ] **Step 1: Write the failing tests** in `test_screensaver_print_latch.cpp`:
  - `"a print that ends while running latches its end"` (PRINTING→COMPLETE → COMPLETE for 2 h of steps with `inactive_ms` growing).
  - `"an end the latch never saw running reads as no print"` (first input CANCELLED, ERROR→FAILED, COMPLETE → NONE). (Review Focus 2)
  - `"input after the end acknowledges it, input before does not"`.
  - `"a new print, a reconnect and a printer going idle clear the latch"` (Review Focus 3).
  - `"display sleep does not clear the latch"`: no input, 8 h of steps → still latched.
  - In `test_screensaver_parts.cpp`, replace `"a saver with no clock has no clock input"` with `"every saver reads the print from its source"` (Starfield and Toasters return a non-null sink whose input carries the source's print).
  - In `test_screensaver_flip_clock.cpp` (manager section): `"a print that ends while no saver runs shows its end when a saver starts"` (Review Focus 1) driving `ScreensaverManagerTestAccess` ticks with a host whose phase goes PRINTING→COMPLETE before `start(FLIP_CLOCK)`; the flip clock's `clock().mode()` is DONE.
- [ ] **Step 2: Run** `make remote-test TAG='[screensaver_print_latch],[screensaver_parts],[screensaver_flip_clock]'`. Expected: FAIL (no latch, null sinks).
- [ ] **Step 3: Implement** the latch, `SaverBase` as sink, the manager wiring and the two host functions: `inactive_ms` = `lv_display_get_inactive_time(nullptr)`; `printer_epoch` = the active client's `connection_generation()` combined with the active printer's index or id, whichever `MoonrakerManager` exposes (confirm a printer switch changes it; add a test-access hook if not).
- [ ] **Step 4: Run** the same command plus `[screensaver]`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(screensaver): an ended print stays on every saver until someone touches the screen`

### Task 3: The running saver is the completion alert

**Files:**
- Create: `include/print_completion_alert.h`, `src/print/print_completion_alert.cpp` (pure, built everywhere)
- Modify: `src/print/print_completion.cpp` (the ending switch that calls `wake_display()`)
- Test: `tests/unit/test_print_completion_alert.cpp` (new, tag `[print_completion_alert]`)

**Interfaces:**
- Produces:
  ```cpp
  enum class PrintEnding : uint8_t { COMPLETE, CANCELLED, ERROR };
  struct CompletionAlertPlan { bool wake = false; bool modal = false; bool toast = false; };
  CompletionAlertPlan plan_completion_alert(PrintEnding ending, CompletionAlertMode mode,
                                            bool on_print_status, bool saver_running);
  ```
  Today's behaviour when `saver_running` is false (ERROR: wake + modal always; on print status: nothing; OFF: nothing; NOTIFICATION: wake + toast; ALERT: wake + modal). When `saver_running` is true: `wake` is false in every case, `modal` as today, `toast` false. Sounds and `TURN_OFF_HEATERS` stay outside the plan, unchanged.

- [ ] **Step 1: Write the failing test** `"a running saver takes the place of the completion wake"`: a table over all 3 endings × 3 modes × on_print_status × saver_running with the exact expected `{wake, modal, toast}` per row (Review Focus 4: ERROR + saver → `{false, true, false}`).
- [ ] **Step 2: Run** `make remote-test TAG='[print_completion_alert]'`. Expected: FAIL to link.
- [ ] **Step 3: Implement** the function and route `print_completion.cpp` through it; `saver_running` is `ScreensaverManager::instance().is_active()` under `#if HELIX_ENABLE_SCREENSAVER`, else `false`.
- [ ] **Step 4: Run** the same tag. Expected: PASS.
- [ ] **Step 5: Commit** `feat(print): a running screensaver shows the print's end instead of waking the screen`

### Task 4: A saver too heavy during a print falls back to the flip clock

**Files:**
- Modify: `src/ui/screensaver/screensaver_manager.cpp#ScreensaverManager::on_idle_check_tick` (the `if (printing) run_black_screen(type);` branch)
- Test: `tests/unit/test_screensaver_gate_manager.cpp`

**Interfaces:** none new. Rule: while printing, a saver measured too heavy at its bottom level launches `kDefaultScreensaverType` when that is a different type and not `too_heavy_for_identity`; otherwise the black screen. Not marked, as today.

- [ ] **Step 1: Write the failing tests** `"a saver too heavy during a print gives way to the flip clock"` (Hyperspace forced over budget with `is_printing` true → `running_type()==FLIP_CLOCK`, stored entry for hyperspace unmarked) and `"the flip clock too heavy during a print shows the black screen"`.
- [ ] **Step 2: Run** `make remote-test TAG='[screensaver_gate_manager]'`. Expected: the first FAILS (black screen).
- [ ] **Step 3: Implement** the branch.
- [ ] **Step 4: Run** the same tag. Expected: PASS.
- [ ] **Step 5: Commit** `feat(screensaver): a saver too heavy during a print gives way to the flip clock`

### Task 5: State colours

**Files:**
- Create: `include/screensaver_print_colors.h`
- Test: `tests/unit/test_screensaver_print_card.cpp` (new, tag `[screensaver_print_card]`; Task 6 adds to it)

**Interfaces:**
- Produces: `inline constexpr Rgb kPrintPaused{255, 176, 32}; kPrintDone{72, 200, 104}; kPrintFailed{232, 72, 64};` and `constexpr Rgb print_state_color(ClockMode m, Rgb accent)` (PAUSED→paused, DONE→done, FAILED and CANCELLED→failed, else accent).

- [ ] **Step 1: Failing test** `"each print state has its own colour, distinct at 16-bit"`: the three colours and a typical accent stay pairwise different after an RGB565 round trip through `PixelWriter`, and `print_state_color` maps every `ClockMode`.
- [ ] **Step 2–4:** red, implement, green with `make remote-test TAG='[screensaver_print_card]'`.
- [ ] **Step 5: Commit** with Task 6 (`feat(screensaver): ...progress card`).

### Task 6: The progress card (pure drawing)

**Files:**
- Create: `include/screensaver_print_card.h`, `src/ui/screensaver/screensaver_print_card.cpp`
- Test: `tests/unit/test_screensaver_print_card.cpp`

**Interfaces:**
- Consumes: `ClockReading` (Task 1), `print_state_color` (Task 5), `StrokeDigitSet` / `build_stroke_digits` (`include/screensaver_stroke_digits.h`), `PixelWriter`, `FrameTarget`, `ClockDrift`.
- Produces:
  ```cpp
  struct PrintCardLayout { int32_t w = 0, h = 0; int32_t ring_d = 0; int32_t unit = 0; };
  PrintCardLayout layout_print_card(int32_t screen_w, int32_t screen_h);
  /// What the card shows; equal faces draw identical pixels.
  struct PrintCardFace { ClockMode mode; int32_t progress_pct; int32_t countdown_s; };
  PrintCardFace print_card_face(const ClockReading& r);
  class PrintCardPainter {
    public:
      void init(const PrintCardLayout& layout);          // builds the digit glyphs
      void draw(FrameTarget& card, const PrintCardFace& face) const;
      void clear();
  };
  ```
  Values: `ring_d = lround(0.22 * min(w, h))`; card `h = ring_d + 2 * pad`, `pad = lround(0.12 * ring_d)`; digits height ≈ `0.55 * ring_d`; card width fits the ring plus five glyph cells (`H:MM` / `M:SS` / `100%`); corner radius `pad`; background `{16, 16, 18}`; ring track `{48, 48, 52}`, ring stroke `lround(0.12 * ring_d)`, ring arc = progress in the state colour (accent = `{120, 180, 255}`). Text: COUNTDOWN and PAUSED `H:MM` from one hour up, `M:SS` below; PROGRESS `NN%`; ended states draw a geometric mark in the ring centre instead of text (DONE check, FAILED cross, CANCELLED square) with the ring full in the state colour; PAUSED adds two vertical bars in the ring centre. `%` and `:` are drawn from strokes like the digits. No `lv_draw_label`: the card is format-agnostic.

- [ ] **Step 1: Failing tests:**
  - `"the card fits every screen shape at every drift offset"`: for 800x480, 1024x600, 480x272, 272x480, 1920x1080, `layout_print_card` plus `clock_drift_range` keep the card inside the screen at ±max drift (Review Focus 5).
  - `"the ring fills with progress in the state colour"`: draw at 0, 50, 100 % → lit-arc pixel count increases monotonically; PAUSED arc pixels are `kPrintPaused`.
  - `"each end state draws its own mark"`: DONE / FAILED / CANCELLED faces draw pairwise different card bytes and contain their state colour.
  - `"the card shows time left, then minutes and seconds under an hour, then percent without an estimate"`: faces for 3725 s, 59 s and PROGRESS 63 % draw different bytes; equal faces draw identical bytes. Run in both pixel formats (`GENERATE`).
- [ ] **Step 2: Run** `make remote-test TAG='[screensaver_print_card]'`. Expected: FAIL to link.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same tag. Expected: PASS.
- [ ] **Step 5: Commit** `feat(screensaver): a progress card any saver can show`

### Task 7: Savers show the card, behind a setting

**Files:**
- Modify: `include/screensaver_registry.h` (`ScreensaverInfo` gains `PrintDisplay print_display`), `include/screensaver_base.h` + `src/ui/screensaver/screensaver_base.cpp` (`run_frame` and `start`/`stop`), `include/display_settings_manager.h` + `src/system/display_settings_manager.cpp` (key), `ui_xml/settings_display_overlay.xml`, the display settings overlay's callback registration (`src/ui/settings/` file that registers `on_sleep_while_printing_changed`), translation source
- Test: `tests/unit/test_screensaver_print_card.cpp`, `tests/unit/test_screensaver_parts.cpp`, the display settings test that covers `SleepWhilePrinting`

**Interfaces:**
- Consumes: Tasks 2, 5, 6.
- Produces:
  - `enum class PrintDisplay : uint8_t { SCENE, CARD };` FLIP_CLOCK, AQUA, HIGHFLY are `SCENE`; every other row `CARD`.
  - `bool print_card_visible(PrintDisplay d, bool setting_on, ClockMode m);` → `d == CARD && (m is PAUSED or an end || (setting_on && m is PROGRESS or COUNTDOWN))`.
  - `DisplaySettingsManager::get_screensaver_print_progress() / set_screensaver_print_progress(bool)`, subject `settings_screensaver_print_progress`, key `ScreensaverPrintProgress`, default `true`.
  - `SaverBase` owns the card: an `lv_canvas` child of the saver overlay in `kSaverBuildCanvasFormat`, created at start for `CARD` savers, positioned by its own `ClockDrift`, repainted only when `print_card_face()` changes, hidden/shown by `print_card_visible` (`// DECLARATIVE_OK:` procedural canvas on a C++-built overlay). `SaverBase` keeps its own `ClockState` for the card, stepped in `run_frame` from `read_input()`.
- XML: a `setting_toggle_row name="row_screensaver_print_progress"` under the Test button, label "Show Print Progress", description "Show time left during a print on screensavers without their own display", hidden with `settings_has_screensaver = 0` and when `settings_screensaver_type` is 0.

- [ ] **Step 1: Failing tests:**
  - `"the card shows for paused and ended prints whatever the setting, and for running prints only with it"`: full truth table of `print_card_visible`.
  - `"only savers without their own print display show the card"`: every `kScreensavers` row's `print_display` matches the list above.
  - `"a card saver repaints its card only when the face changes"` (LVGL fixture, Starfield with a scripted source: same second → card canvas buffer unchanged; next second → changed; DONE → visible with the setting off).
  - Settings: `"the print progress setting defaults on and persists"`.
- [ ] **Step 2: Run** `make remote-test TAG='[screensaver_print_card],[screensaver_parts],[display_settings]'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same tags plus `[screensaver]`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(screensaver): savers without their own print display show the progress card`

### Task 8: Docs and the finish

**Files:**
- Modify: `docs/devel/SCREENSAVERS.md` (the shared clock face, the gate, a new "Print progress" section: latch rules, completion alert, card, setting), `docs/devel/plans/2026-11-07-screensaver-print-progress-design.md` (mark Phase 1 shipped; the file is deleted when Phase 3 ships), user guide's display settings page for the new toggle, `docs/devel/CHANGELOG_1_1_DRAFT.md` is not touched (this is 1.2).

- [ ] **Step 1:** Write the docs (path + `#function` citations, no line numbers).
- [ ] **Step 2: Finish, once:** one review of the branch; screenshots at 800x480, headless, of Starfield with the card at ~30 % countdown, paused, DONE and FAILED, and of the flip clock reading PROGRESS and FAILED (`HELIX_MOCK_AUTO_PRINT=1`, `HELIX_SCREENSAVER_NOW`); hand the branch and SHA to whoever lands the next main batch for its single `make land-gate`.
- [ ] **Step 3:** After landing: `make mutate-diff MUTATE_ARGS="--base <origin/main it landed on>"` on zeus, background, capped at 30 min.
