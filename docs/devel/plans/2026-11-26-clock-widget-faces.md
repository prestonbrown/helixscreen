# Clock widget faces (#2198 follow-up)

The home clock widget gets a per-instance **Face**: Text (today's widget, the default), the five
analog faces (Chronograph, Station, Neon, Bauhaus, Orrery), Flip and Nixie. A face is a plain
clock: no print data, no finish-time arc. Scaffolding: deleted when the work ships.

## Shape

- **One copy of each face.** The sims (`AnalogClockSim`, `FlipClockSim`, `NixieClockSim`) already
  paint into any `FrameTarget`, so a tile reuses them on a canvas sized to its content box. What
  lived only in the saver classes moves to shared free functions both call:
  - `analog_text_for(const AnalogText&, const ClockReading&)` and
    `analog_write_dial_texts(lv_obj_t* text_canvas, lv_draw_buf_t& buf, const AnalogClockSim&, const ClockReading&)`
    (`include/screensaver_analog_clock.h`), out of `AnalogClockScreensaver::text_for` / `write_texts`.
  - `CardClockMarkers` (`include/screensaver_card_markers.h`): the AM/PM `SaverLabel` pair the
    flip and nixie savers keep over their cards, with
    `template <class Sim> void draw(SaverCanvas&, Sim&, const FlipFace&, const DirtyRect& box, Rgb, lv_text_align_t, RedrawFn, under, dirty)`.
- **Host options on the sims, saver defaults unchanged.**
  - `struct AnalogClockOptions { bool drift = true; bool seconds = true; bool side_column = true; };`
    `void AnalogClockSim::set_options(const AnalogClockOptions&)`, kept across `clear()`.
    `seconds = false` draws no seconds hand and holds the minute hand for the whole minute, so the
    face changes once a minute. `side_column = false` lays Bauhaus out like the others.
    `AnalogGeom analog_layout(AnalogStyle, int32_t w, int32_t h, bool side_column = true)`.
    `analog_draw_hands(..., bool smooth, bool seconds = true)`.
  - `void FlipClockSim::set_drift(bool)`, `void NixieClockSim::set_drift(bool)`.
- **The widget's rules** (`src/ui/panel_widgets/clock_face.h`, no LVGL):
  ```cpp
  enum class ClockFace : uint8_t { TEXT, CHRONOGRAPH, STATION, NEON, BAUHAUS, ORRERY, FLIP, NIXIE };
  inline constexpr size_t kClockFaceCount = 8;
  const char* clock_face_id(ClockFace);          // stored at the instance config's "face"
  ClockFace parse_clock_face(std::string_view);  // TEXT for empty or unknown
  const char* clock_face_label(ClockFace);       // English, through lv_tr at the picker
  // HELIX_ENABLE_SCREENSAVER only:
  int32_t clock_face_measure_px(ClockFace, int32_t w, int32_t h);
  int32_t clock_face_min_px(ClockFace);
  ClockFace clock_face_shown(ClockFace chosen, int32_t w, int32_t h);
  ScreensaverType clock_face_saver(ClockFace);
  struct ClockFaceTempo { size_t level; bool seconds; uint32_t period_ms; };
  ClockFaceTempo clock_face_tempo(ClockFace, const std::optional<ui::SaverLevelEntry>& standing,
                                  int cpu_cores);
  uint32_t clock_ms_to_next_minute(int32_t seconds_of_day, uint32_t ms_into_second);
  ```
- **`ClockFaceView`** (`src/ui/panel_widgets/clock_face_view.h`): a `SaverCanvas` in the tile,
  the face's sim and state, the hidden text canvas for analog text, an `LvglTimerGuard`.
  `bool show(lv_obj_t* parent, ClockFace, int32_t w, int32_t h, const ClockFaceTempo&)`,
  `void step(uint32_t dt_ms, const ui::ClockInput&)`, `void hide()`, `size_t heap_bytes() const`.
  `hide()` deletes the timer, frees the sim and the canvas buffer and deletes the canvas objects.
- **`ScreensaverManager::standing_here(ScreensaverType)`**: the stored entry for this board and
  app version (`HELIX_SCREENSAVER_LEVEL` as a forced entry), nullopt when never measured.
- **Per instance.** The clock becomes `multi_instance` (`clock:N`); the size mode and "a face
  shows" are per-instance subjects named through `xml_attrs()`; the shared time/date/uptime text
  subjects stay shared. A saved bare `clock` keeps working.
- **Picking.** `has_edit_configure()`; the gear opens `ClockFacePicker` (a `ContextMenu`, the
  `LightTargetPicker` pattern) listing the eight faces with a small preview of each, the current
  one highlighted. A pick saves `{"face": id}` and re-applies in place.

## Size thresholds (pixels of the canvas, the tile's content box)

| Face | Measure | Minimum |
|---|---|---|
| Chronograph | dial diameter (`2 * analog_layout(...).r`) | 110 px (subdials) |
| Station, Neon, Bauhaus, Orrery | dial diameter | 80 px |
| Flip | a card's width with HH:MM in one row (`flip_row_card_px`) | 18 px |
| Nixie | a tube's width with HH:MM in one row | 16 px |

Below the minimum the tile shows Text. 1x1 tiles (2x2 tracks): about 104 px content at 800x480,
about 70 px at 480x320, so 1x1 shows analog at 800x480 and 1024x600 and Text at 480x320.

## Seconds rule

The face reuses the measurement of the saver it shares its sim with (analog_clock, flip_clock,
nixie_clock), as `standing_here()` reads it:

- **Too heavy** on this board and version: no seconds; the face repaints once a minute (analog
  draws no seconds hand; flip/nixie swap without animating).
- **Measured, not too heavy**: the settled level, never above rung 1 (33 ms; a tile never runs at
  16 ms). Analog shows seconds, moving the way that rung moves; flip/nixie animate where the rung
  animates.
- **Never measured**: the ladder's bottom rung. Analog ticks its seconds hand once a second on
  2 or more cores and shows no seconds on 1 core; flip/nixie swap without animating.

The timer: analog with seconds runs at the rung's period; everything else sleeps to the next
whole minute and, where the rung animates, runs at the rung's period only until the flip or fade
finishes.

## Tests

- `[clock_face]` rules: ids round-trip and unknown reads Text; the fallback by pixels per face at
  just below and at the minimum; 1x1 at 800x480 analog, at 480x320 Text; the tempo rule for too
  heavy, measured (and clamped to rung 1), unmeasured multi-core and single-core;
  `clock_ms_to_next_minute`.
- `[clock_face]` sims: an analog face with `seconds = false` writes nothing across a minute's
  seconds and redraws at the minute; drift off holds the face still past `kClockDriftStepMs`
  (analog, flip, nixie); Bauhaus without its column centres the dial.
- `[clock_widget]`: the face shows on a canvas sized to the tile and falls back to Text below the
  minimum; detach frees the canvas and stops the timer; a recycled widget re-applies on attach;
  two instances keep their own faces and size modes; the picker's pick saves `face`.
- Saver unchanged: `[screensaver_analog_clock]`, `[screensaver_flip_clock]`,
  `[screensaver_nixie_clock]`, `[screensaver]` pass as they are.
