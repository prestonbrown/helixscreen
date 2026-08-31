# Design brief — Pressure Advance calibration screen

A brief for designing one **new** screen in **HelixScreen**, the touchscreen UI for
Klipper-based 3D printers. It sits beside the existing Z Calibration and Tool
Offset screens. Everything below is real behaviour of the machine and of the
surrounding app; the goal is a screen design, not a feature invention.

---

## 1. The physical thing being done

**Pressure advance** (PA) is a single number in the printer's firmware — Klipper
calls it `pressure_advance`, slicers often call it *K*. It compensates for the
lag between the extruder motor pushing filament and plastic actually leaving the
nozzle. Too low and corners bulge outward; too high and gaps appear on
acceleration. It is the single most impactful print-quality tuning number after
temperature.

**It is a property of the filament, not the machine.** Every spool — every
*material*, really — wants its own value. PLA and PETG from the same brand will
differ; so will the same PLA printed at 200 °C and at 230 °C. That is why
temperature is part of this screen and not an afterthought.

**Calibrating it means printing.** The printer heats the nozzle, extrudes a test
pattern while sweeping the PA value across a range, then determines which value
produced the cleanest extrusion, and reports that number. On the machines this
screen targets the determination is **automatic** — the firmware measures and
returns a value; the user is *not* asked to squint at a printed part and pick a
line. The whole run takes roughly **two to five minutes**, most of it heating,
and the machine moves and extrudes on its own throughout.

**Before starting, the user must physically:** have the build plate **on** the
bed (unlike tool offset calibration, this one prints), have filament loaded in
the tool being calibrated, and have the plate clean. The result is a small blob
or line pattern that they will peel off afterwards.

**The number is the deliverable, and it has to leave the machine.** This is the
defining constraint of the screen. The user's slicer runs on a PC or a phone,
across the room. The touchscreen has **no keyboard, no clipboard, and no
copy-paste** into anything the user owns. So a value like `0.0412` has to be
transcribed by a human, by eye, correctly, first time — or handed over by some
other route (the app already has a QR-code modal component used elsewhere, and
the printer can also apply and persist the value itself).

---

## 2. Who is looking at this

Someone standing at the machine who has just loaded a new spool — a new brand, a
new colour, a new material — and wants their prints to be as good as this
filament allows. They are more engaged than the average user (they know what a
slicer is), but they are **not** experts: many have heard "you should tune
pressure advance" and have never done it, because the classic way involves
printing a tuning tower and judging it by eye.

They are doing this **occasionally** — per new spool at most, realistically a few
times a season. So the screen cannot assume remembered knowledge. It must say
what the number is, what it is for, and what to do with it.

They will be holding a phone or walking to a PC when the number appears.

---

## 3. Hardware and platform constraints (hard limits)

| Constraint | Value |
|---|---|
| Physical screen | **800 × 480 px** capacitive touchscreen, landscape |
| Space this screen gets | **708 × 480 px** (a left nav rail takes 92 px and is always visible) |
| Input | **Finger only.** No mouse, no hover, no right-click, no keyboard, no clipboard |
| Minimum comfortable touch target | ~44 px |
| Theme | Dark by default; a light theme exists — never hard-code colours |
| Rendering | LVGL 9.5 (an embedded C UI toolkit) on a low-power ARM/MIPS CPU |
| Smaller variants | The same UI must degrade to **480 × 272** and **480 × 320** panels |

**What LVGL can and cannot do — please respect this:**

- **Yes:** flex row/column layout, scroll containers, rounded rects, borders,
  solid fills, opacity, icon fonts, spinners, progress bars, sliders, images,
  QR codes, simple animations.
- **No:** CSS gradients on arbitrary shapes, drop shadows with blur (expensive),
  SVG, backdrop blur, arbitrary transforms, web-style typography control.
- Fonts are **pre-baked bitmap sizes**. Roughly: body ~16 px, small ~13 px,
  heading ~20 px, and a small number of larger display sizes exist for readouts.
  Arbitrary sizes are not free — each one costs flash.
- Colours and spacing come from **design tokens**, not raw hex. Existing tokens
  include `card_bg`, `elevated_bg`, `section_bg_color`, `text`, `text_muted`,
  `primary`, `success`, `warning`, `border`, and a `space_xs…space_lg` scale.

Anything requiring per-frame redraw of large areas will be visibly slow.

---

## 4. The data model

**One target at a time.** The user picks:

1. **Which tool.** On a tool changer (four heads, `T0`–`T3`) this is a real
   choice, and each tool has its own extruder and therefore its own PA value.
   **On a single-extruder printer — the majority — there is exactly one, and the
   choice should not be presented at all.** The design must work in both cases;
   a picker that looks empty or stranded on a single-tool machine is a failure.
2. **A nozzle temperature.** Range roughly **170–300 °C**, the value that matters
   is whatever they will actually print this filament at. The app already offers
   four **material presets** (e.g. "Generic PLA", "Generic PETG", "Generic ABS",
   "Generic TPU") that fill in a sensible temperature, plus a −/+ stepper.
   Preset names come from user data, so they can be long — "Generic TPU" already
   overflows a quarter-width button on the small panels.

**What comes back is one number**, the pressure advance / K value:

- Range **0.0 – 1.0**, shown to **4 decimal places** (`0.0412`). All four digits
  are meaningful.
- **Direct-drive** extruders (most of these machines) land around **0.02 – 0.08**.
  **Bowden** extruders land around **0.3 – 0.9**. A value far outside the
  expected band for that machine means the run went wrong even if nothing errored.
- The printer also knows the **currently active** value (what it is using right
  now, from its config). Showing new-vs-current is meaningful: "0.0412, you were
  using 0.0300" is a much better result than a bare number.

**Context worth carrying:** which tool and what temperature produced the number.
A PA value with no material or temperature attached to it is nearly useless a
week later, and this is exactly the mistake the user will make when they write it
on a sticky note.

---

## 5. What the user can do

1. **Choose a tool** (multi-tool machines only) and a **temperature**.
2. **Start** the calibration. One confirmation first, covering the physical
   prerequisites (plate on, filament loaded, plate clean).
3. **Stop / abort** a run in progress. Aborting cancels the print and cools down;
   nothing is kept.
4. When it finishes: **read the number**, and do one or more of —
   - **transcribe it** into their slicer's filament profile (the primary
     intent, and the reason this screen exists);
   - **apply it now** to the running printer, so the next print uses it without
     touching the slicer;
   - **save it** to the printer's config permanently (this restarts the
     printer's firmware and briefly disconnects the UI).
5. **Leave** the screen (a back arrow in the header).

Applying and saving are genuinely optional — a user who tunes per-filament in
their slicer wants *only* the number, and never wants it baked into the machine.
Both audiences are real; neither should feel like the afterthought.

---

## 6. The states the screen moves through

**A — Idle, never calibrated on this machine.** No previous result. The screen
has to explain what pressure advance is and why they'd want this, in very few
words, without becoming a wall of text.

**B — Idle, with a previous result.** Common return case. The last value, and
ideally the tool/temperature it came from, is the most useful thing on screen.

**C — Confirming.** One dialog covering the physical prerequisites before any
run. The plate must be **on** — the opposite of the tool-offset screen's
requirement, which is a real source of confusion for anyone who used that one.

**D — Heating.** Up to a couple of minutes of nothing but a rising temperature.
Currently the least interesting part of the wait and the longest.

**E — Printing / measuring.** The machine is extruding a test pattern by itself.
The user can only watch or stop. Together D and E are **two to five minutes** —
this is the state most in need of design attention: the user needs a sense of
overall progress, of which phase they're in, and roughly how long is left.

**F — Complete.** The number exists. This is the screen's whole reason to be, and
it must be **readable from a step back**, unambiguous about decimal places, and
obvious about what to do next. Do not let a `0.0412` sit in body text next to
three buttons of equal weight.

**G — Failed or refused.** Real failure modes: no filament detected in the
selected tool, the printer refused because a print job is already running, the
heater failed to reach target, the calibration macro isn't installed on this
printer, or the measurement produced an out-of-range value. Messages can be
**long — 100–200 characters** — and are the most valuable text on screen when
they appear. A failed run changes nothing; any previous value survives.

**H — Unsupported.** Some printers simply cannot do this automatically. The
entry point is hidden in that case, so the screen never renders — but worth
knowing the capability is conditional.

---

## 7. What exists today

**Nothing — this screen does not exist yet.** That is the point of the exercise.
For continuity, two neighbouring screens set the current visual language and are
worth treating as the family this must join:

- **PID / heater tuning** — closest structural sibling: pick a heater, pick a
  temperature (material presets + a `[−] 200°C [+]` stepper), press Start in the
  header, watch a progress state, get a result you can save. Its layout is a
  header bar with the primary action as a header button, then a column of
  `section_bg_color` cards inside the content area.
- **Tool offset calibration** — a per-row list with per-row results, a
  confirmation before each run, and a separate explicit Save that restarts the
  firmware.

The known weaknesses in those screens are the traps to avoid here: a running
state that is just a spinner and a line of text; long error messages dumped into
a small muted label; and result numbers presented with no units, no context, and
no sense of whether they are sane.

---

## 8. What a good design would achieve

In rough priority order:

1. **The number gets out of the machine intact.** Legible at arm's length,
   unambiguous, and with at least one route out that doesn't rely on the user
   copying four decimal digits by eye without a typo.
2. **The wait is worth looking at** for several minutes — phase (heating vs
   printing), overall progress, some sense of time remaining.
3. **Setup is two decisions, not a form.** Tool and temperature, with the
   single-tool case collapsing cleanly to one decision.
4. **The result is contextualised** — what it replaces, whether it looks sane
   for this machine's extruder type, and what material/temperature produced it.
5. **The three things you can do with the result** (write it down, apply now,
   save permanently) have an honest hierarchy instead of three identical buttons.
6. **Errors are first-class content**, not a status line.
7. **A newcomer understands what pressure advance is** from the idle state,
   in one or two sentences, without the screen becoming documentation.

---

## 9. Fixed vs open

**Fixed — cannot be designed away:**

- The calibration is a real print: it takes minutes, the machine moves, the
  plate must be on, filament must be loaded.
- Temperature is part of the input, because the answer depends on it.
- The result is a single number, 4 decimals, and its useful range depends on the
  extruder type.
- Applying and saving are separate acts; saving restarts the firmware.
- Multi-tool and single-tool machines must both look intentional.
- 708 × 480 px, finger input, LVGL primitives only, tokenised colours.

**Open — please design freely:**

- The whole visual language: single screen, cards, wizard, multi-step flow.
- Whether setup and result are one screen or two.
- How the wait is expressed — progress bar, phase list, temperature graph,
  animated illustration of what the nozzle is doing.
- How the number is presented and how it leaves the machine.
- How much explanation the idle state carries, and where it goes.
- Iconography and colour semantics; whether "sane value" is shown or implied.

**Two ideas worth exploring, not prescriptions:** a QR code carrying the value
(and the material/temperature context) turns a transcription problem into a phone
camera; and because the classic manual method is a *visual* judgement of a
printed pattern, showing a small illustration of what good and bad extrusion look
like would tell the user what the machine just decided on their behalf.

---

## 10. Deliverable

A design for the screen across states **A/B/D/E/F/G** in section 6 — plus the
**C** confirmation dialog — at **708 × 480**, dark theme, showing both the
multi-tool and single-tool variants of the setup state, plus a note on how it
degrades to a 480 × 272 panel. Keep it buildable with the LVGL primitives in
section 3 — no gradients on complex shapes, no blur, no SVG.
