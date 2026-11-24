# Analog Clock screensaver: design

Target: 1.2. Agreed with Preston on 2026-11-24 from rendered mockups at 800x480, 480x272 and
480x800.

## Intent

A good-looking analog clock among the screensavers, with several faces that each show a
running print in their own way. It is the fifth clock saver, beside the flip clock, nixie,
Aqua and High Fly, and like them it shows the print in its own scene (`PrintDisplay::SCENE`),
never on the progress card.

## Goals, non-goals, anti-goals

- **Goals:** five faces in one saver, picked by a new Clock Face setting; each face reads as
  a real clock at a glance, from across a room; each face shows a running print, a pause and
  how a print ended in a way that fits that face; runs on the same boards the other clock
  savers run on, stepping down through a level ladder.
- **Non-goals:** faces that follow the app theme (each face has a fixed palette, designed
  as a whole); user-designed faces; a vintage or cream-dial face (shown, rejected).
- **Anti-goals:** a station clock that copies a real railway clock brand's design or name;
  any face that is hard to read the time from at a glance (Orrery is the edge of this, and
  stays only because its hands are big planets on distinct orbits).

## The saver

- `ScreensaverType::ANALOG_CLOCK = 14`, stable name `"analog"`, label `"Analog Clock"`,
  `SAVER_DEPTH_16 | SAVER_DEPTH_32`, `PrintDisplay::SCENE`. The default saver for a fresh
  install stays the flip clock.
- `AnalogClockSim` / `AnalogClockState` in `include/screensaver_analog_clock_sim.h`, no LVGL,
  driven by `ClockState` / `ClockReading` (`screensaver_clock_reading.h`). A thin
  `AnalogClockScreensaver : ClockSaver<AnalogClockSim, AnalogClockState>` draws the
  translated text (subdial words, the date, Bauhaus's column) with `SaverLabel`, as
  `FlipClockScreensaver` draws its AM/PM.
- **A face is data plus a small draw hook, not five savers.** The hands' angles, the print
  arc/wedge geometry, motion (sweep, tick, sprung tick) and state colors are shared; each face
  supplies its palette, dial art, hand shapes and which print display it uses.
- **Dial art is cached** once per face per size, as nixie caches its tubes. Each frame
  restores the dial under the old hands' boxes and draws the new hands. Minute and hour
  hands move rarely; the seconds hand is the per-frame cost.
- Print state colors come from `print_state_color()` (`screensaver_print_colors.h`): paused
  amber, done green, failed or cancelled red. Paused/done/failed words come from
  `clock_state_word()`, so `spell_states = false` shows the symbol instead.
- Burn-in: the whole face drifts by `clock_drift_at()` like the other clocks.

## The Clock Face setting

- A new display setting `screensaver_clock_face`, a stable name: `chronograph` (default),
  `station`, `neon`, `bauhaus`, `orrery`, or `shuffle`, stored as 0 to 5 like the other display
  dropdown settings.
- A **Clock Face** row in Settings > Display directly under the Screensaver row, shown only
  while Analog Clock is selected (`bind_flag_if_not_eq` on `settings_screensaver_type`). It
  lists the five faces and Shuffle. Settings > Display only; the setup wizard keeps just the
  Screensaver row. "Station Clock" and "Shuffle" are the labels, because the existing
  "Station" and "Rotate" keys already translate to other meanings.
- **Shuffle** picks a different face than last time on each run. When the Random saver picks
  Analog Clock, it picks a random face too.
- No per-face level store entries: the gate measures the saver as a whole. The costliest face
  (Neon, for its glow and trail) sets the levels; see the ladder below.

## The faces

All faces show the current time; the print rows apply only while a print is running or its
end is latched (`PrintEndLatch`). Proportions below are for a dial of radius R (225 px at
800x480).

### Chronograph (default)

- **Look:** steel bezel, deep navy radial dial, applied steel batons filled with pale green
  lume and a soft glow. Double baton at 12; wide batons at 3, 6, 9; narrow at the others. Fine
  minute track inside the bezel. Steel hands with lume fill. A thin center sweep seconds hand
  in the accent orange (`#e8902a`). Date window under 12 ("TUE 24"). **"HelixScreen"** in
  small letter-spaced type above the 6 baton (the maker's mark; this face
  only).
- **No print:** the bezel is a plain 60-minute scale with a lume pip at 12. Left subdial is a
  24-hour hand; right subdial is the weekday (seven letters around it). No bottom subdial.
- **Printing:** the bezel is a 0-100 scale that fills in orange with progress. Subdials:
  left PRINT (%, hand at the fraction), right LEFT (time left, hand on a 12-hour scale),
  bottom ENDS (finish time, hand pointing at the finish hour). The bottom subdial is smaller
  (36 px at R 225) and sits higher than the side ones so the maker's mark fits beneath it.
- **Paused:** the center seconds hand **stops** where it is, as a real chronograph's does,
  the subdials freeze, and the bezel, subdial hands and the bottom word take the paused amber.
  The stopped hand is the main cue; amber alone is too close to the printing orange.
- **Done:** bezel full in green, PRINT 100%, right subdial TOOK (the print's duration),
  bottom word DONE.
- **Failed / cancelled:** bezel at the progress reached, in red; right subdial blank; bottom
  word FAIL (or STOP for cancelled, per `clock_state_word`).
- **Subdial hands are short** enough that the number below the pivot is never covered.
- **Smallest breakpoints (MICRO, TINY):** subdial captions (PRINT, LEFT, ENDS, 24H) are
  dropped; the numbers stay.

### Station

- **Look:** railway-style white dial (`#f4f4f0`) in a dark rim, bold black batons for the
  hours, thin minute ticks, tapered black hands with a soft drop shadow, a thin red seconds
  hand with a round red disc near its tip and a red center. Generic railway style; no brand.
- **Motion: sprung tick.** The seconds hand steps once a second and settles with a damped
  overshoot (about 0.3 s; the mockup used `1 - e^(-18t) cos(45t)`). The minute hand holds
  for the whole minute and jumps at :00 with the same spring, as a slave clock does; the hour
  hand moves with each minute jump. Between springs nothing moves, so the saver can sleep
  until the next tick.
- **Printing:** a blue (`#2f8cff`) arc just inside the rim from the hour hand's angle to the
  finish hour, ending in a blue dot with a white ring at the finish time.
- **Paused / done / failed:** the arc takes the state color; the dot stays at the finish
  time.
- Brightest face by far. Not the default.

### Neon

- **Look:** near-black (`#03040a`), every line a glowing stroke: cyan (`#38e8ff`) ring, hour
  ticks at 12 places, longer at the quarters, cyan hour and minute hands with round caps, a
  white seconds hand with a **magenta afterglow trail** fading behind it. Hollow white center
  ring.
- **Motion:** smooth sweep. The trail is drawn analytically: a ring band whose alpha falls
  with the angle behind the hand, so it needs no history buffer.
- **Printing:** a magenta (`#ff3fb4`) progress ring outside the dial with a faint full-ring
  track; "2:31 LEFT" in magenta below center, drawn **under** the hands.
- **Paused / done / failed:** the ring takes the state color. (A breathing or flashing ring
  would redraw the dial every frame; left out.)

### Bauhaus

- **Look:** flat charcoal dial (`#1b1c1f`) on near-black, twelve cream dots (larger at the
  quarters), no numerals, cream round-capped hour and minute hands, a thin orange
  (`#ff6a1a`) seconds hand and orange center.
- **Motion:** ticks once per second, no spring.
- **Layout:** the clock sits left, a text column right: weekday (bold), date, a hairline,
  then while printing the file name, a progress bar and "62% · done at 12:40". In portrait
  the column stacks under the clock. Fonts scale from the cramped axis.
- **Printing:** an orange wedge at 18% opacity from the hour hand's angle to the finish hour,
  plus the column's print rows.
- **Paused / done / failed:** the column says so in words, and the wedge and bar take the
  state color.

### Orrery

- **Look:** deep blue-black space with sparse stars outside the orbits; a glowing sun at the
  center; three orbits with a planet each: hour (amber, largest, inner), minute (light blue),
  seconds (pale, smallest, outer), each with a short fading comet trail. Twelve small dots
  around the outside mark the hours.
- **Motion:** smooth orbits.
- **Printing:** a dotted green orbit outside everything fills with progress.
- **Paused / done / failed:** the print orbit takes the state color.

## Layout at each size

- Round faces center on the screen with R = min(w, h) / 2 minus a margin, and scale every
  stroke, font and inset with R. Checked at 800x480 and 480x272 (Chronograph reads at 480x272
  with captions dropped).
- Bauhaus: clock left and column right on landscape, column under the clock on portrait
  (checked at 480x272 and 480x800).

## Level ladder (`kAnalogClockLevels`)

| Level | Period | What changes |
|---|---|---|
| 0 | 16 ms (`kSaverFastPeriod`) | Each face's own motion, trails and glows |
| 1 | 33 ms | Same |
| 2 | 250 ms | Every face ticks once a second, with no springs, trails or hand glows |

A ticking or settled face writes nothing between ticks, at any level.

## Left to the builder

- Exact hand shapes and antialiasing through `PixelWriter`; whether the dial cache is one
  buffer per face or rebuilt on a face change.
- Which strings need translation keys (subdial captions, weekday letters, the date window,
  Bauhaus's column) and their short forms.
- The mockup generator is throwaway and not checked in; the face descriptions above are the
  spec.
