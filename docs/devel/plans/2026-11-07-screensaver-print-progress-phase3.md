# Screensaver Print Progress, Phase 3 (the other savers' scenes) Implementation Plan

> **For agentic workers:** after Task 1 lands on `feature/screensaver-progress-p3`, Tasks 2-5 run in parallel, one agent each in its own worktree branched from that commit. Verification Cadence (root `CLAUDE.md`) wins: tags while building, ONE review and the hand-off at the finish, done by the orchestrator.

**Goal:** Each saver that today shows only the progress card also tells the print's story in its own scene: pipes fill one screen per print, fireworks build to a finale, the printer fills with colour, toast browns, a planet approaches, a light grows at the end of the tunnel, and fireflies spell the percent.

**Architecture:** `SaverBase` keeps one `ClockState` for every saver, stepped before `on_frame`; the card reads it, and `SimSaver` passes its reading to any sim whose `init`/`step` take a `const ClockReading&`. Every scene effect is a pure function of that reading inside the sim (or the sprite saver), eased from `dt_ms`. The progress card stays on all eight savers (registry rows stay `CARD`): the scenes carry the feel, the card the exact time.

**Spec:** `docs/devel/plans/2026-11-07-screensaver-print-progress-design.md` (Phase 3 table, Decided, Constraints). Builds on Phase 2 (`feature/screensaver-progress-p2` @ d91a56337f): its `StrokeGlyphSet` / `stroke_glyph_distance` are used by Task 4.

## Global Constraints

- CLOCK mode (no print) draws exactly as today: every existing fingerprint line in `tests/fixtures/screensaver/fingerprints.txt` stays, and no rng draw is added, skipped or reordered in CLOCK paths. New `<saver>_print` fingerprints (COUNTDOWN 40% via `clock_input_at(elapsed, true)`) are added, one zeus value each, recorded with the mirror-only line swap described in the Phase 2 ledger.
- Sims stay pure; nothing allocates in `step` (size new buffers in `init`; count them in `heap_bytes()`); heap tests stay exact.
- Every effect works on the cheapest rung (or degrades to something visible there) and at 16 and 32 bpp.
- State colours from `include/screensaver_print_colors.h`.
- Test budget and file-size ratchets: run `python3 scripts/check_test_budget.py --base origin/main` and `python3 scripts/check_growth_ratchets.py --base origin/main` before each commit; add `Test-budget: +N <reason>` / `File-size: <path> <reason>` lines as needed.
- Tags run on zeus: `scripts/test-host-run.sh test '[tags]'` (check `ssh zeus.local uptime` < ~60). Never `make remote-test` (it targets thelio) and never a local test build while thelio's pool is saturated.

## Review Focus

1. A saver started after a print ended (latched DONE): each scene must open directly in its end state, not animate from scratch (e.g. pipes: a full screen plus teapot, not an empty grid).
2. A new print starting while a scene shows an end state: the scene returns to its print-start state cleanly (pipes dissolve and restart; fireworks stop the finale).
3. PAUSED for hours: nothing accumulates (fireworks launches, confetti bursts, pipe steps).
4. The cheapest rung of each saver still shows progress (toasters keep only 2 toasts; fireflies draw 48 flies; hyperspace has no tunnel).
5. The progress card and the scene effect never fight for the same pixels in a way that hides the card.

---

### Task 1: One print reading for every saver, and print-aware sims

**Files:** `include/screensaver_base.h`, `src/ui/screensaver/screensaver_base.cpp`, `include/screensaver_sim_saver.h`, `tests/unit/test_screensaver_parts.cpp`, `tests/unit/test_screensaver_sim_contract.cpp`, `tests/unit/test_screensaver_fingerprint.cpp`.

**Interfaces:**
- `SaverBase` gains `protected: const ClockReading& print_reading() const;` backed by a `ClockState print_` stepped from `read_input()` in `run_frame` **before** `on_frame` (and once in `start()` before `on_start`). `PrintCard` drops its own `ClockState` and reads `print_`.
- `SimSaver<Sim>`: when `sim_inits_with_v<Sim, const ClockReading&>` / `sim_steps_with_v<Sim, const ClockReading&>`, `on_start`/`on_frame` pass `print_reading()` (same position as Aqua: `init(frame, rng, level, reading)`, `step(dt, frame, rng, reading, dirty)`). Savers with their own `on_frame` (fireworks, fireflies) pass it explicitly.
- Contract test: a `print_reading_for(ClockMode, pct)` helper and, for each sim that takes a reading, a print variant of the three contract cases (COUNTDOWN 40%).
- Fingerprint test: a `clock_input_end(elapsed, phase)` helper for DONE/FAILED runs.

- [ ] Failing test: `"a scene saver reads the print a frame before the card would"` (a probe sim taking a reading sees PROGRESS on the same frame its source turns PRINTING); `"a sim without a reading still starts and steps"` (existing probe unchanged).
- [ ] Implement; run `[screensaver_parts],[screensaver_print_card],[screensaver_sim_contract]` on zeus; commit `feat(screensaver): every saver reads the print before it draws`.

### Task 2 (agent A): Pipes and Fireworks

**Pipes** (`screensaver_pipes*.{h,cpp}`, tests `[pipes_sim]`):
- `PipesSim::init/step` take the reading. While a print runs (PROGRESS, COUNTDOWN, PAUSED): one screen per print: `max_pipes_` is lifted and the dissolve is suppressed; growth runs only while `taken < target_taken`, `target_taken = progress * nodes` (smoothed from `countdown_s` when known so growth is not bursty), still at most `kPipesMaxStepsPerTick` per tick; PAUSED grows nothing.
- DONE: the screen fills to the target, then one teapot is placed at an empty node and drawn (`joint_piece(TEAPOT, ...)`, exposed as needed). FAILED/CANCELLED: growth stops and one desaturate pass greys the canvas (one whole dirty box).
- A new print after an end, or CLOCK after an end: dissolve, then normal behaviour.
- Tests: `"a print fills one screen as it goes"` (taken/nodes tracks progress within 5% at 25/50/75%); `"a finished print sets a teapot on the full screen"`; `"a failed print greys the pipes and stops"`; `"a new print clears the screen and starts again"`; `pipes_print` fingerprint.

**Fireworks** (`screensaver_fireworks*.{h,cpp}`, `[fireworks_sim]`):
- `FireworksSim::step` takes the reading. Running print: launch gap bounds shrink with progress (`max_gap` from the pacing value to 900 ms at 100%), burst size grows from 50% to 100% of the level's sparks (pool size unchanged). PAUSED: no launches. DONE: the finale re-arms whenever it ends, until the reading leaves DONE. FAILED/CANCELLED: one red (`kPrintFailed`) small burst, then no launches.
- Tests: `"launches come faster and bigger as the print goes"`; `"a finished print loops the finale"`; `"a failed print fizzles once in red and goes quiet"`; `"a paused print launches nothing for an hour"` (Review Focus 3); `fireworks_print` fingerprint.

### Task 3 (agent B): Starfield and Hyperspace

**Starfield** (`screensaver_starfield*`, tests in `test_screensaver_sim.cpp`, `test_screensaver_dirty_budget.cpp`):
- A planet at the centre: radius `lerp(0.02, 0.22) * min(w, h)` by progress (eased), shaded disc with a lit limb in a fixed planet colour; drawn only when its integer radius changes (dirty: its box); stars whose square meets the disc are neither drawn nor erased there. PAUSED: stars slow to 15% speed. DONE: the planet grows to fill the view over 4 s and holds. FAILED/CANCELLED: the planet shrinks and drifts off over 6 s.
- Tests: `"the planet grows with the print"`; `"stars never punch holes in the planet"`; `"a finished print arrives at the planet"`; dirty-budget tests unchanged in CLOCK; `starfield_print` fingerprint.

**Hyperspace** (`screensaver_hyperspace*`, `[hyperspace_sim]`):
- A light at the vanishing point: a radial additive glow, radius `lerp(4, 0.18 * min(w,h))` px by progress, drawn every frame (the frame repaints anyway) at the rung's block size, capped on block 0. PAUSED: camera speed 25%. DONE: drop out: camera speed eases to 0, tunnel fades out, no starbursts, then frames stop redrawing (empty dirty) once still. FAILED/CANCELLED: the light dims out and the tunnel closes.
- The existing "every frame dirties the whole canvas" and "the camera always flies forward" cases stay CLOCK-only by construction.
- Tests: `"the light at the end of the tunnel grows with the print"`; `"a finished print drops out into still stars"`; `hyperspace_print` fingerprint.

### Task 4 (agent C): Fireflies spell the percent

**Files:** `screensaver_fireflies*`, `[fireflies_sim]`; uses `StrokeGlyphSet` from Phase 2.
- Per-fly targets: `std::vector<SaverVec3> fly_target_` sized in `init` (heap constant); a GATHER state driven by the reading and `elapsed_ms_` (never by `schedule_next_mode`, so CLOCK rng is unchanged): every 60 s during a running print, the swarm gathers for 6 s into the percent text ("63%") rasterised from `StrokeGlyphSet` at a size fitting 70% of the screen width, points sampled from covered pixels (deterministic stride, no rng), then releases to their baits. DONE: gathers into "DONE" and holds, glowing (`kGlowGain`). FAILED/CANCELLED: flies turn `kPrintFailed` and scatter (bait waypoints re-drawn once).
- The cheapest rung draws 48 flies: the glyph sampling picks the targets for the drawn flies first so the text reads there.
- Tests: `"the swarm gathers into the percent every minute of a print"` (at the hold, ≥70% of drawn flies within 3 px of a glyph pixel); `"a finished print holds DONE"`; `"a failed print turns the swarm red and scatters it"`; `"no gather happens without a print"` (rng sequence identical to a CLOCK run); `fireflies_print` fingerprint.

### Task 5 (agent D): Bouncing printer, bouncing logo, flying toasters

**Bounce** (`screensaver_bounce.{h,cpp}`, tests in `test_screensaver.cpp`):
- An owned copy of the decoded sprite; rows above the fill line (`src_h * (100 - pct) / 100`) rewritten to greyscale (luma `r*77+g*150+b*29>>8`, alpha kept) only when the line moves; `lv_obj_invalidate(img_)`. Wall tints pause while printing. DONE: full colour; confetti on every bounce at most one burst alive (logo too: split the confetti gate from `decorated()`); cheapest rung: a green `kPrintDone` corner flash instead. FAILED/CANCELLED: steady `kPrintFailed` recolor at the existing `RECOLOR_OPA`.
- Tests: `"the sprite fills with colour from the bottom as the print goes"` (pixel rows of the owned buffer: grey above, colour below the line at 30%); `"a finished print throws confetti on every bounce, one burst at a time"`; `"a failed print tints the sprite red"`; the logo variants.

**Toasters** (`ui_screensaver.{h,cpp}`):
- Six pre-tinted toast buffers built at start (light to dark brown) plus one burnt black; toast objects switch source by progress band; FAILED/CANCELLED: burnt. DONE: every 3 s a toast pops from a visible toaster (one reused `lv_image`, rising and falling over 1.2 s).
- Tests: `"toast browns as the print goes"`; `"a failed print burns the toast"`; `"a finished print pops toast"`.

### Task 6: Docs, review, hand-off (orchestrator)

- `docs/devel/SCREENSAVERS.md` § Print progress: each saver's scene effect and the card decision; user guide screensaver table.
- Merge Tasks 2-5 into `feature/screensaver-progress-p3`; `[screensaver]` on zeus; one fresh review; screenshots of each saver at ~40% and DONE; budget and growth checks; delete this plan and the design doc (the last phase ships); hand the branch to the main lander.
