# Screensaver print progress: design

Status: for Preston's approval (builder check done, answers folded in). Ships on main (1.2). Deleted in the change that ships the
last phase (`docs/CLAUDE.md`).

## Why

The flip clock's countdown earned a lot of user praise: from across the room you can tell how
a print is going without touching anything. Preston reads that as a sign the direction is right,
not as a request for one specific number. The goal is that **every screensaver tells you, at a
glance, how the print is going**, in a way that belongs to that saver's scene wherever the scene
allows it.

## Goals

- At a glance, from across a room: is a print running, roughly how far along, is it paused, is
  it done, did it fail.
- Improve the three savers that already show the print (flip clock, Aqua, High Fly) first.
- Give every other saver a print display: woven into the scene where it fits, a shared
  readout drawn over the scene where it does not.
- A finished, failed or cancelled print stays on screen until someone touches it.
- No black screen during a print because a saver is too heavy.

## Non-goals

- Filename, layer, temperatures or any detail beyond progress, time and state.
- Interaction on the saver: any touch still wakes the screen.
- A per-saver settings page.

## Anti-goals (would make this a failure even if it works)

- Progress that is only readable up close: small grey text is what we are moving away from.
- A saver that loses its character to a progress bar bolted across it.
- Any saver that makes a print slower: the gate's printing budget still rules.
- Burn-in: anything drawn in the same place for hours drifts, as the clocks do today.
- A saver that stops showing progress on its cheapest level or at 16-bit depth.

## What exists (audit, 2026-11-07)

- `ClockReading` / `ClockState` (`include/screensaver_clock_reading.h`) decide what a readout
  says: CLOCK, COUNTDOWN, PAUSED or DONE, with countdown seconds, the end time and percent.
  The manager feeds them to the three `ClockSaver`s through `SaverHost::print_status`
  (`src/application/display_manager.cpp#DisplayManager::screensaver_host`).
- The other nine savers receive no print state (`SaverBase::clock_input_sink` returns null).
- `clock_print_phase` maps Cancelled and Error to NONE, so they look like no print.
- DONE holds for `kClockDoneHoldMs` (60 s), and only when the saver saw the print running.
- Progress shows only once a remaining-time estimate exists.
- A saver too heavy at its bottom level **while printing** shows the black screen for the rest
  of the run (`src/ui/screensaver/screensaver_manager.cpp`, `run_black_screen`).
- Screensavers are not built for ESP32 (`HELIX_ENABLE_SCREENSAVER=0`), so firmware size is not a
  constraint here.

## Decided (Preston, 2026-11-07)

- Every brainstormed idea below is in.
- End states (done, failed, cancelled) hold **until touched**.
- A saver too heavy during a print **falls back to the flip clock**, not black.
- Ships on **main / 1.2**.
- Order: shared groundwork, then the **existing three**, then the rest.
- Savers with no natural fit get a shared **overlay readout**, behind a setting that is **on by
  default**.
- **The saver is the completion alert.** A print that ends while a saver runs no longer wakes
  the display (`src/print/print_completion.cpp` calls `wake_display()` for every ending under
  the default ALERT mode, and always for ERROR). The saver shows DONE, FAIL or STOP instead.
  The completion sound still plays, and the completion modal is still created, so it waits
  behind the saver and is there when someone touches the screen. With no saver running,
  completion behaves as today.
- **The user's sleep timeout still wins.** A latched end state holds until a touch or until the
  display sleep stage, whichever comes first. Neither the latch nor a running print holds off
  sleep; `decide_idle` is unchanged.
- **Paused and failed always reach the screen.** On a saver with no in-scene paused or failed
  look, the overlay shows them even when the overlay setting is off; the setting hides only the
  running-print readout. Scenes add paused and failed looks where they fit naturally, and the
  Phase 3 table is not a promise of one for every saver.

## Phase 1: shared groundwork

### One print reading for every saver

Extend the shared model so every saver can ask the same questions. Shape left to the builder,
but it must provide:

- Phase: NONE, PREPARING, PRINTING, PAUSED, DONE, FAILED, CANCELLED. Cancelled and Error stop
  mapping to NONE.
- Progress 0 to 1, available as soon as the printer reports a percentage, with or without a
  time estimate.
- Countdown seconds and the end time, when an estimate exists (today's `ClockState` behaviour,
  wall-clock ticking and the 10 s resync, unchanged).
- A latched end state: DONE, FAILED or CANCELLED stays until a touch acknowledges it, a new
  print starts, the active printer changes or disconnects, or the app restarts (the latch is
  never persisted). Display sleep ends the run but does not acknowledge: a touch that wakes a
  slept display does. It lives in the manager, not in a saver, so it survives the saver stopping and
  starting, a Random draw, or a fallback. A saver that starts after the print ended still
  shows it, as long as nobody has touched the screen since. The 60 s `kClockDoneHoldMs` goes.
- Every saver receives it: the `clock_input_sink` path generalises from the three `ClockSaver`s
  to `SaverBase`, so a saver opts in by reading it.

### State colours

One small palette shared by all savers, so a glance reads the same everywhere:

| State | Colour role |
|---|---|
| Printing | the saver's own accent (no change) |
| Paused | amber |
| Done | green |
| Failed / cancelled | red |

Values go in one header beside `screensaver_pixel_writer.h`, at contrast that survives RGB565.
They are not LVGL theme tokens, because the canvas savers write raw pixels.

### The completion wake

`print_completion.cpp` asks the screensaver manager whether a saver is running before it wakes
the display. When one is, it skips `wake_display()` and lets the saver show the end state; the
sound and the modal go ahead as today. Tests: an ending under each `CompletionAlertMode`, with
and without a running saver, asserts whether the display woke and that the modal exists.

### Heavy saver fallback

The printing branch of the gate's too-heavy path launches the flip clock (at whatever level the
gate allows) instead of `run_black_screen`. Black stays the last resort only when the flip
clock is itself too heavy. The stored saver choice is not rewritten, as today.

### The overlay readout

For savers marked as having no in-scene display:

- A compact card: a progress ring or bar, time left in large digits, and the state colour. It is
  sized to read from across a room, not a caption, and its size comes from the screen, as the
  clocks' layouts do.
- It drifts on the same `ClockDrift` schedule as the clocks.
- Shown only while a print is running or an end state is latched; nothing when idle.
- Drawn by the saver host into the saver's frame (canvas savers) or as an LVGL object over the
  sprite savers (toasters, bouncing printer/logo). The builder picks one mechanism or both.
- Setting: **Display → Screensaver → "Show print progress"**, on by default. Off turns off only
  the overlay; in-scene progress stays, because it is part of the scene.

## Phase 2: the existing three

### Flip clock

- End states spell out on the cards in large split-flap letters: **DONE**, **FAIL**, **STOP**
  (cancelled) and **PAUSE**, in place of frozen digits. The stroke glyph set grows the letters
  it needs.
- The dim caption ("done at 9:41 · 63%") becomes a second row of small flip cards, with the
  same folding animation, so it reads at a distance.
- Before an estimate arrives, the cards show the percentage.
- State colours tint the digits or card edges (the builder chooses which reads better).

### Aqua

- **The water level is the progress**: the tank fills from low to the brim over the print.
- Paused: the water calms (low wave amplitude, no splashes).
- Done: brim-full with a burst of bubbles, then calm water held until touched.
- Failed or cancelled: the water drains to low, with the red state colour on the text.
- The ring clock stays; the small top progress ring may go now that the water carries it.
- Must work on the "still" rung, which paints water once: the level change repaints it at
  progress steps (for example each whole percent), not every frame.

### High Fly

- While printing, the big numerals become the **countdown**; the time of day moves to the
  small text.
- The sky runs from morning to golden hour over the length of the print.
- Done: drop out of the cloud deck into a sunset, held until touched.
- Paused: the camera flies a slow holding circle.
- Failed or cancelled: a darker, overcast sky with the red state colour on the text.

## Phase 3: the rest

| Saver | In-scene progress | End states |
|---|---|---|
| Pipes | One print fills one screen: growth paced so the grid fills as the print completes | A teapot appears on done; failed stops growth and greys the pipes |
| Fireworks | Launch rate and burst size build with progress | Grand finale on done, looping until touched; failed = one red fizzle, then quiet |
| Bouncing printer | The picture starts grey and fills with colour bottom-up, layer by layer | Done: corner-hit confetti on every bounce; failed: red tint |
| Bouncing logo | Same bottom-up fill on the logo | Same |
| Flying toasters | Toast browns with progress | Done: a toaster pops its toast; failed: burnt black toast |
| Starfield | A planet at the centre grows as you approach | Done: arrival, the planet fills the view; failed: it drifts away |
| Hyperspace | A light at the end of the tunnel grows | Done: drop out of hyperspace into a still starfield |
| Fireflies | Every minute or so the swarm gathers into the percentage, then scatters | Done: the swarm holds "DONE"; failed: red flies scatter |
| Maze | None: no natural fit | Overlay readout |

Savers whose in-scene display only shows rough progress (planet size, toast colour) also show
the overlay readout when the setting is on, so the time left is always available. The builder
decides per saver whether the scene alone is enough and records the decision in
`docs/devel/SCREENSAVERS.md`.

## Constraints

- Every display works on the saver's cheapest level and at 16 and 32 bpp.
- The gate's printing budget is unchanged; new drawing counts against it.
- Sims stay pure (no LVGL, no clock): the print reading is an input like the time, so runs
  replay exactly and the fingerprint test keeps covering them.
- Pixel fingerprints (`tests/unit/test_screensaver_fingerprint.cpp`) change on purpose; each
  phase re-records the ones it touches.
- The text goes through `lv_tr()`; the card letters (DONE, FAIL, STOP, PAUSE) are glyphs, which
  stay English. That is acceptable for a split-flap face, and is flagged here for Preston.

## Tests (per phase)

- Shared model: phase mapping including FAILED and CANCELLED; percentage before an estimate;
  latch held across saver stop and start; cleared by a touch and by a new print.
- Gate: too heavy while printing launches the flip clock; black only when the flip clock is too
  heavy.
- Completion wake: skipped while a saver runs, unchanged otherwise; the modal is still created.
- Latch cleared by a touch, a new print, a printer switch and a disconnect; not by sleep.
- Each saver: a test that its display changes with progress and with each end state, plus the
  re-recorded fingerprint. Each test is broken by hand once to see it fail.
- One screenshot per saver per state (printing at ~30%, paused, done, failed) at 800x480,
  headless, in the PR.

## Left to the builder

- The exact shape of the extended print reading and how savers subscribe to it.
- Overlay geometry, ring vs bar, and rendering mechanism.
- Colour values within the palette roles.
- Per-saver tuning (sky gradients, pacing curves, glyph shapes for the letters).
