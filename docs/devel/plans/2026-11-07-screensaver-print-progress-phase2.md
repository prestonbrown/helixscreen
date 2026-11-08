# Screensaver Print Progress, Phase 2 (the three savers that already show it) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (Preston chose native execution for Phase 1; same here unless he says otherwise). Steps use checkbox (`- [ ]`) syntax. The project's Verification Cadence (root `CLAUDE.md`) overrides those skills: tags while building, ONE review and the hand-off to the main lander at the finish.

**Goal:** The flip clock spells how a print stands in large split-flap letters and shows its details on a second row of small flip cards; Aqua's tank fills with the print; High Fly counts down in its big numerals under a sky that warms toward golden hour, drops into a sunset at the end, and holds a slow circle while paused.

**Architecture:** Every change is a pure function of the `ClockReading` the savers already receive (Phase 1), eased inside each sim from `dt_ms`. Nothing new allocates per frame; nothing new runs in CLOCK mode, so the idle look and the idle fingerprints stay as they are.

**Tech Stack:** C++17, LVGL 9.5 canvas savers, Catch2, `make`.

**Spec:** `docs/devel/plans/2026-11-07-screensaver-print-progress-design.md` (Phase 2, Decided, Constraints). Phase 1 shipped on `origin/main` `76b83226c5`.

## Global Constraints

- CLOCK mode (no print) draws exactly what it draws today: the `flipclock`, `aqua` and `highfly` fingerprints in `tests/fixtures/screensaver/fingerprints.txt` do not change. Only the `*_print` fingerprints are re-recorded, each in the task that changes that saver.
- Sims stay pure (no LVGL, no clock, no allocation in `step`). New state is sized in `init`/`build_scene` and counted in `heap_bytes()`.
- Heap caps hold: aqua `<= 1536 KiB`, highfly `<= 4608 KiB` (their heap tests). No new full-frame buffer in either.
- No rng draw is added or skipped in CLOCK paths.
- Works at every rung, the still rung included, and at 16 and 32 bpp.
- State colours come from `include/screensaver_print_colors.h` (`print_state_color`).
- Card letters are glyphs and stay English (Decided in the spec); any text drawn with `lv_draw_label` goes through `lv_tr()`.
- Test budget: each commit that grows test lines carries `Test-budget: +N <reason>`; no new `TestAccess` references (reuse existing helpers).
- Tag runs: `make remote-test TAG='[...]'` on zeus when `ssh zeus.local uptime` < ~60, else `make t` here.

## Review Focus

1. **A print whose end lands while the flip clock is mid-flip:** the shape changes from 7 digit cards to 4 letter cards; the relayout must repaint cleanly with no leftover card pixels (dirty-box test covers it in Task 3).
2. **12-hour time on the flip clock's small row:** the finish time has no AM/PM card; the main row's meridiem marker must not appear over letter cards. Pinned in Task 2.
3. **Aqua at the still rung across a whole print:** the level must step (a handful of full repaints over the print), never every frame. Pinned in Task 4.
4. **High Fly countdown over 99 hours and under one minute:** hours clamp to 99; the big group never shows a leading zero hour. Pinned in Task 6.
5. **Portrait screens (480x800):** flip clock letter row plus small row fit at the widest drift; Aqua's raised water stays below High Fly-style clipping. Pinned in Tasks 3 and 4.

---

### Task 1: Letters and percent as stroke glyphs

**Files:**
- Modify: `include/screensaver_stroke_digits.h`, `src/ui/screensaver/screensaver_stroke_digits.cpp`
- Test: `tests/unit/test_screensaver_clock_reading.cpp` (the stroke digit cases live there)

**Interfaces:**
- Produces:
  ```cpp
  /// Every character a stroke glyph exists for.
  inline constexpr char kStrokeGlyphChars[] = "0123456789%ACDEFILNOPSTU";
  /// Distance, in glyph units, from (px, py) to `c`'s centre line; digits as stroke_digit_distance.
  float stroke_glyph_distance(char c, float px, float py);
  /// Coverage for any of kStrokeGlyphChars at one size, built per character on first use.
  struct StrokeGlyphSet {
      int32_t w = 0, h = 0;
      void init(int32_t unit, float stroke);     // sets w/h, frees nothing yet built
      void ensure(char c);                        // rasterises `c` once
      uint8_t at(char c, int32_t gx, int32_t gy) const;  // 0 for a glyph not ensured
      size_t heap_bytes() const;
      void clear();
  };
  ```
  Letter shapes share the digits' box (1 x `kStrokeDigitPathH`) and stroke model; capitals, geometric, no serifs.

- [ ] **Step 1: Failing tests:** `"every stroke glyph's centre line is fully covered"` (each of `kStrokeGlyphChars`, like the digit case); `"letters that share strokes still draw differently"` (pairwise coverage differs for O/D, E/F, P/D, I/L, S/5, O/0); `"a glyph set holds only the glyphs it was asked for"` (`heap_bytes()` grows on `ensure('D')`, not again on a second `ensure('D')`, `at` of an un-ensured glyph is 0).
- [ ] **Step 2:** run `[clock_reading]`; expect a compile failure.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** run `[clock_reading]`; PASS.
- [ ] **Step 5:** commit `feat(screensaver): stroke glyphs for the letters and percent sign`.

### Task 2: Flip clock faces for every print state

**Files:**
- Modify: `include/screensaver_flip_clock_sim.h`, `src/ui/screensaver/screensaver_flip_clock_sim.cpp` (`flip_face`, `countdown_face`), `src/ui/screensaver/screensaver_flip_clock.cpp` (`caption_text` goes)
- Test: `tests/unit/test_screensaver_flip_clock.cpp`

**Interfaces:**
- Consumes: Task 1 characters on `FlipCard::digit` (now any of `kStrokeGlyphChars` or NUL).
- Produces:
  - `FlipFace` loses `caption`, `eta_minute_of_day`, `progress_pct` and gains `FlipShape sub_shape; std::array<FlipCard, kFlipMaxSubCards> sub{}; ClockMode mode;` with `kFlipMaxSubCards = 8`. `FlipShape::has_caption()` becomes `FlipFace::has_sub()` (`sub_shape.group_count > 0`). `enum class FlipCaption` is deleted.
  - Mapping (`flip_face`), main row / small row:

    | Mode | Main cards | Small cards |
    |---|---|---|
    | CLOCK | HH:MM as today | none |
    | PROGRESS | `63%` (one group) | time of day `9:41` |
    | COUNTDOWN | H:MM:SS as today | finish time `9:41`, then `63%` |
    | PAUSED | `PAUSE` | time left `1:12` (H:MM, when `has_countdown`), then `63%` |
    | DONE | `DONE` | `100%` |
    | FAILED | `FAIL` | the progress reached, `41%` |
    | CANCELLED | `STOP` | the progress reached, `72%` |

    Small-row groups are separated like main-row groups (a colon between hour and minute groups, a gap before the percent group). `meridiem` is set only in CLOCK mode.

- [ ] **Step 1: Failing tests:** replace the caption cases with `"the cards spell how the print stands, with its details on the small row"` (the whole table above via `cards_text` extended to print `main|sub`, e.g. `"PAUSE|1:12 63%"`), `"letter faces carry no AM/PM marker"` (12-hour PAUSED/DONE faces have `meridiem == NONE`; Review Focus 2), and keep the countdown and clock cases.
- [ ] **Step 2:** run `[screensaver_flip_clock]`; expect compile failures.
- [ ] **Step 3:** implement the faces; `FlipClockScreensaver` stops drawing the caption label (the meridiem label stays).
- [ ] **Step 4:** PASS (layout and drawing tests may fail until Task 3; run `[screensaver_flip_clock]` after Task 3 if so, and say so in the ledger).
- [ ] **Step 5:** commit with Task 3.

### Task 3: The small card row and coloured letters

**Files:**
- Modify: `include/screensaver_flip_clock_sim.h` (`FlipLayout`, `FlipClockSim`), `src/ui/screensaver/screensaver_flip_clock_sim.cpp` (`arrange`, `layout_flip_cards`, `draw_card`, `step`, `build_glyphs`)
- Test: `tests/unit/test_screensaver_flip_clock.cpp`, `tests/fixtures/screensaver/fingerprints.txt` (`flipclock_print` only)

**Interfaces:**
- Produces:
  - `FlipLayout` gains `std::array<DirtyRect, kFlipMaxSubCards> sub_cards{}; uint8_t sub_card_count; int32_t sub_unit;` and the small row's colons; `caption` is deleted. The small row sits where the caption sat, `kSubCardScale = 0.32` of the main unit, centred under the main row; `arrange` counts its height instead of `kCaptionH`.
  - Small cards fold with the same animation and duration as the main cards (their own `CardAnim`s).
  - `FlipClockSim` keeps two `StrokeGlyphSet`s (main and small) instead of `StrokeDigitSet`; `ensure()`s each card's character when a face shows it.
  - Digit ink is `print_state_color(face.mode, kDigit)`: white while running, amber PAUSE, green DONE, red FAIL and STOP; the small row uses the same ink.
- [ ] **Step 1: Failing tests:** extend `"one card per digit fits every screen shape..."` to letter faces and the small row (480x272, 272x480, 480x800, 800x480, 1024x600 at the widest drift; Review Focus 5); `"the letters draw in the state colour"` (DONE card has `kPrintDone` pixels, FAIL `kPrintFailed`, PAUSE `kPrintPaused`, no state colour on a running face); `"small cards flip only where their character changes"` (COUNTDOWN 63% to 64% redraws one small card box); `"a print ending mid-flip repaints the whole layout"` (Review Focus 1: every changed pixel lies in a dirty box across a COUNTDOWN-to-DONE switch during a fold); the existing `"every pixel a flip clock frame changes lies in one of its dirty boxes"` runs a print face too.
- [ ] **Step 2:** run `[screensaver_flip_clock],[screensaver_fingerprint]`; expect failures.
- [ ] **Step 3:** implement; re-record `flipclock_print` only (the `flipclock` line must not change; state that in the commit).
- [ ] **Step 4:** PASS, then one screenshot set at 800x480 (COUNTDOWN, PAUSED, DONE, FAILED) for the finish.
- [ ] **Step 5:** commit `feat(screensaver): the flip clock spells the print's state and shows its details on small cards`.

### Task 4: Aqua's water level is the progress

**Files:**
- Modify: `include/screensaver_aqua_sim.h`, `src/ui/screensaver/screensaver_aqua_sim.cpp` (`apply_view`, `Scene::sample_slice`, `draw_spray`, `step`, `evaluate`, `emit_crests` call site, `draw_bubbles`)
- Test: `tests/unit/test_screensaver_aqua.cpp`, fingerprints (`aqua_print` only)

**Interfaces:**
- Produces:
  ```cpp
  /// How far above today's rest level the water stands for `r`, in water units: 0 with no
  /// print, rising linearly to kAquaBrimRise at 100%; DONE at the brim; FAILED and CANCELLED 0.
  float aqua_level_target(const ClockReading& r);
  inline constexpr float kAquaBrimRise = 0.5f;      // well under kCamH (0.8)
  inline constexpr float kAquaLevelRate = 0.05f;    // water units per second, both ways
  inline constexpr float kAquaStillLevelStep = 0.05f; // still rung repaints per this much
  /// Wave and mode amplitude scale: 1, easing to kAquaCalm while PAUSED.
  inline constexpr float kAquaCalm = 0.35f;
  ```
  - The level moves the camera, not the water: `AquaSim` keeps `level_` (eased toward the target at `kAquaLevelRate`) and uses `kCamH - level_` everywhere `kCamH` is read today (`apply_view`'s `v.cam_h`, `sample_slice`'s `vy0`, `draw_spray`). Crest scoring and splashing are unchanged.
  - Calm: `AquaWater` gains `set_amplitude(float)` applied to the mode amplitudes and to `gerstner_at_phase` / `exact_height` alike; splashes are skipped while PAUSED.
  - Done: on the step into DONE, up to 120 of the existing bubbles respawn at the bottom at their top speed (no new bubbles; rungs with no bubbles skip it).
  - Still rung: `painted_ = false` whenever `level_` crosses a `kAquaStillLevelStep` boundary (Review Focus 3).
  - The clock text (`AquaScreensaver::draw_clock_text`) draws in the state colour for PAUSED and ended prints.
- [ ] **Step 1: Failing tests:** `"the water stands higher as the print goes"` (pure table for `aqua_level_target`: CLOCK 0, 0% 0, 50% 0.25, 100% 0.5, DONE 0.5, FAILED 0, CANCELLED 0); `"the water line rises with progress"` (rim row at the same frame and seed is higher on screen at 100% than at 0%, after easing time); `"paused water calms"` (frame-to-frame grey change in PAUSED is under half of COUNTDOWN's, same seed); `"the still rung repaints the water only per level step"` (a whole 0-to-100% print at the still rung produces at most `kAquaBrimRise / kAquaStillLevelStep + 2` full repaints); the heap cases still pass unchanged; `aqua` fingerprint unchanged.
- [ ] **Step 2:** run `[screensaver_aqua],[screensaver_fingerprint],[screensaver_sim_contract]`; expect failures.
- [ ] **Step 3:** implement; re-record `aqua_print` only.
- [ ] **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(screensaver): Aqua's tank fills with the print and calms while it is paused`.

### Task 5: High Fly counts down in its big numerals

**Files:**
- Modify: `include/screensaver_highfly_sim.h`, `src/ui/screensaver/screensaver_highfly_sim.cpp` (`draw_numerals`, `draw_numeral_group`), `src/ui/screensaver/screensaver_highfly.cpp` (`draw_clock_text`), `src/ui/screensaver/screensaver_clock.cpp` only if a shared line helper is cleaner
- Test: `tests/unit/test_screensaver_highfly.cpp`

**Interfaces:**
- Produces:
  ```cpp
  /// The six numeral slots for `r`: {H, H, M, M} big and {S, S} small. The time of day for CLOCK,
  /// PROGRESS and ended prints; for COUNTDOWN, and PAUSED with an estimate, the time left with
  /// hours clamped to 99 and a leading zero hour blank (-1).
  std::array<int, 6> highfly_numerals(const ClockReading& r);
  ```
  Both `draw_numerals` (the bake key) and `draw_numeral_group` read it. While the numerals show a countdown the left text block reads the finish time and the time of day (`"ETA 9:41 PM"`, `"now 8:12 PM"` through `lv_tr`), the AM/PM marker is blank, and PAUSED/ended lines draw in the state colour.
- [ ] **Step 1: Failing tests:** `"the big numerals count down during a print"` (table: CLOCK 14:25:07 → {1,4,2,5,0,7}; COUNTDOWN 3725 s → {-1,1,0,2,0,5}; 999 h → {9,9,5,9,5,9}; 59 s → {-1,0,0,0,5,9}; DONE → the time; Review Focus 4); `"the countdown numerals carry no AM/PM"` (LVGL saver test: meridiem label text empty in COUNTDOWN, 12-hour).
- [ ] **Step 2:** run `[screensaver_highfly]`; expect compile failure.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** PASS.
- [ ] **Step 5:** commit with Task 6.

### Task 6: High Fly's sky follows the print

**Files:**
- Modify: `include/screensaver_highfly_sim.h`, `src/ui/screensaver/screensaver_highfly_sim.cpp` (`build_sky`, `apply_view`'s `slice_lut_` bake, `draw_billboard`, `compose` haze, `Scene::sample_slice` x offset, `step`)
- Test: `tests/unit/test_screensaver_highfly.cpp`, fingerprints (`highfly_print` only)

**Interfaces:**
- Produces:
  ```cpp
  /// The sky's colours for one moment of a print.
  struct HighflyPalette { Rgb top, bottom, glow, lit, fog; };
  /// Today's constants for CLOCK, PROGRESS-free idle; blended toward golden hour with progress
  /// (kHighflyGolden at 100%), kHighflySunset for DONE, kHighflyOvercast for FAILED and CANCELLED.
  HighflyPalette highfly_palette(const ClockReading& r);
  inline constexpr int kHighflyPaletteSteps = 10;  // the sky rebakes at most this often per print
  ```
  - `build_sky` and the `slice_lut_` bake take the palette; the sky rebakes into the existing `sky_` buffer only when the quantised step (progress / 10%, or the end state) changes. Billboards and the bottom haze read `lit` and `fog` per frame. Cloud textures are not recoloured.
  - Done: the step into DONE starts a fly-through at once (`fly_start_s_ = t_`); the palette switches to sunset while the whiteout covers the frame.
  - Paused: forward speed eases to 25% and the view sways sideways (`kFieldX0`, band, layer and tower x offsets plus `sin(t * 0.15) * 24` texels) for a slow holding pattern.
  - Still rung: `painted_ = false` on a palette step change.
- [ ] **Step 1: Failing tests:** `"the palette is today's sky without a print and warms with progress"` (CLOCK returns the current constants exactly; red/blue ratio of `top` and `glow` rises monotonically over 0, 50, 100%; DONE differs from 100%; FAILED is greyer: lower saturation than CLOCK); `"the sky repaints per palette step, not per frame"` (a 0-to-100% print at the still rung gives at most `kHighflyPaletteSteps + 3` full repaints); `"the end of a print flies through a cloud into the sunset"` (fly-through starts on the DONE step; after it clears, sampled sky pixels match `kHighflySunset`'s glow hue); heap cap holds; `highfly` fingerprint unchanged.
- [ ] **Step 2:** run `[screensaver_highfly],[screensaver_fingerprint],[screensaver_sim_contract]`; expect failures.
- [ ] **Step 3:** implement; palette values are the builder's within those roles (golden: warm amber top and glow; sunset: deep orange to violet; overcast: blue-grey); re-record `highfly_print` only.
- [ ] **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(screensaver): High Fly counts down under a sky that warms with the print`.

### Task 7: Docs and the finish

**Files:** `docs/devel/SCREENSAVERS.md` (flip clock, Aqua Surface, High Fly sections, Print progress), `docs/user/guide/settings/display.md` (the three rows of the screensaver table), the design doc (mark Phase 2 shipped).

- [ ] **Step 1:** write the docs.
- [ ] **Step 2: Finish, once:** one fresh review of the branch (Review Focus above); screenshots at 800x480, headless, of each saver in COUNTDOWN, PAUSED, DONE and FAILED (`helix-screen ctl screensaver <name>` during `HELIX_MOCK_AUTO_PRINT=1`); fix Critical/Important with a red-first test; hand the branch and SHA to whoever lands the next main batch.
- [ ] **Step 3:** mutation testing after landing belongs to the lander's batch run; fix any survivor it reports.
