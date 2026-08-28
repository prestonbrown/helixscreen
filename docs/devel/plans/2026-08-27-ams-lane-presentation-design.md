# AMS Lane Presentation — Design

Status: in flight. Delete this file when the work ships.

## Problem

How an AMS lane communicates its state is hand-written on four surfaces and they
do not agree. The rule that decides whether a lane shows filament, a ghost, or
nothing was reimplemented per surface, so the copies drifted; two of them read
`SlotInfo::is_present()`, which is false for `UNKNOWN` as well as `EMPTY`.

A first attempt (`3f6b76cf5`, `f578a5f82`) folded the copies onto one pure
function that returned a **rendering prescription** —
`{spool_opa, show_spool, show_placeholder, label}`. That was the wrong seam. It
forced every surface to draw the same way, which meant asking a 7px-wide bar to
express a dashed placeholder circle and a translated word. The result was a
ghosted fill one pixel tall (measured: `#4D4D51` at y=167, correct colour,
invisible) and a border tweak that made an ejected lane indistinguishable from a
present-but-weightless one.

Both commits are reverted by this design.

## What is shared, and what is not

The surfaces want the same **answer about the lane**, not the same pixels.

There are two renderings, not four surfaces:

| Rendering | Used by |
|-----------|---------|
| **Spool** | AMS panel (`ams_slot`), mini strip spool mode |
| **Bar**   | mini strip bar mode, overview mini-bars |

The two always share appearance within their family.

## Lane states

Three base states, plus two orthogonal decorations. Splitting them this way
matters: loaded-ness and error-ness are not alternatives to having filament,
they are marks laid over whatever the lane is already showing, and they come
from different inputs.

**Base state** — `classify_lane(status, has_identity)`:

| State | Spool rendering | Bar rendering |
|-------|-----------------|---------------|
| **Present** | spool at fill level | bar at fill level |
| **Ghosted** (ejected, identity retained) | whole cell ghosted, last known fill or assumed | same |
| **Empty** (no identity) | empty-spool placeholder + "Empty" | **nothing — the layout gap remains** |

**Decorations** — layered on any base state, each from its own input:

| Decoration | Source | Spool | Bar |
|------------|--------|-------|-----|
| **Active** | `slot_active_loaded` subject | glow + badge | bright border |
| **Error** | `status == BLOCKED \|\| slot.error` | glyph | glyph |

Fill level is a separate function again (`lane_fill_level`), so "tracked vs
untracked" never enters the state enum — a `Present` lane with no weight data
simply resolves to `ASSUMED_FILL_LEVEL`.

Two decisions worth naming:

- **Ghosting is a whole-cell modifier**, not per-element opacity. Spool, labels
  and percent dim together. This is what makes the state legible; dimming one
  element and not another is what produced the unreadable result above.
- **An empty lane on a bar draws nothing.** The gap stays, so lanes remain
  countable — the absence itself is the signal. The empty-spool placeholder and
  the word "Empty" belong to the spool rendering, which has room for them.

`UNKNOWN` is classified **exactly as `EMPTY`** — it therefore inherits the
identity split, giving `Ghosted` when the lane already carries an identity and
`Empty` when it does not. During startup that shows a known lane dimmed until
its status arrives, which is honest; treating `UNKNOWN` as `Present` would
briefly show filament in a lane that has none, which is not.

It is not a steady state on any backend: every
`SlotStatus::UNKNOWN` assignment is skeleton construction before firmware data
arrives (`ams_backend_qidi.cpp:72`, `ams_backend_snapmaker.cpp:263`,
`ams_backend_happy_hare.cpp:1325`, `ams_backend_ace.cpp:1317`,
`ams_backend_afc.cpp:4339`), plus one QIDI fallback for an unrecognised value
(`ams_backend_qidi.cpp:663`). Rendering it as `Empty` during startup is honest
and needs no special case.

## Reversing #1071

`SlotInfo::display_fill_level()` returns `0.0f` for any non-present lane. That
implements `a106413f6`, *"render an emptied lane's fill bar empty, not 75%, when
the Spoolman link is retained"* — the lane read as loaded.

This design gives a ghosted lane its last known fill back, because that fill is
what makes the ghost legible at all. The original complaint was a **full-strength**
75% bar with nothing to mark it as stale. A uniformly dimmed cell is a different
claim: the dimming is the disclaimer.

This is a deliberate reversal and must be pinned by a test asserting a ghosted
lane renders its fill **and** that the whole cell carries the ghost — the second
half is what makes the first half safe.

## Layers

### 1. Data — pure C++, no LVGL

```
classify_lane(SlotStatus, bool has_identity) -> LaneState {Present, Ghosted, Empty}
lane_fill_level(const SlotInfo&)             -> float
```

`Active` and `Error` are not part of `LaneState`; they ride their own subjects
and are applied by the chrome.

`ASSUMED_FILL_LEVEL` is a named constant replacing five bare `1.0f` literals
(`ams_types.h:986`, `ui_spool_canvas.cpp:151`, `:445`, `:547`).

Both are exhaustively testable without a display. This is what survives from
`ams_slot_presentation.h`, except it returns a state rather than a drawing.

### 2. Subjects — C++ writes data, never style

Per-lane subjects in `AmsState`. `status` / `material` / `color` / `fill` /
`active_loaded` already exist; add `lane_state[i]`.

### 3. Graphic widgets — C++, params in, pixels out

Two registered widgets: `ams_spool` (owns the 3D canvas vs flat rings choice
internally) and `ams_lane_bar`.

Custom widget attributes are **one-shot strings** — `spool_canvas_xml_apply()`
does `strtof(value)` at apply time and there is no generic `bind_<attr>`
facility. So the params are **subject names**, and each widget resolves and
observes its own:

```xml
<ams_spool bind_color="slot_color_${i}"
           bind_fill="slot_fill_${i}"
           bind_state="lane_state_${i}"/>
```

This is the pattern `ams_slot` already uses and CLAUDE.md's sanctioned shape for
custom widgets.

### 4. Chrome — XML

Everything around the graphic: material and percent labels, lane and tool
badges, layout, the "Empty" text, the error glyph, and one `bind_style` on the
cell root carrying the ghost.

`${i}` substitutes into any attribute value (`lv_xml.c:1907` scans for `${`
anywhere; `lv_xml.c:1953` resolves `i` to the repeat index), so bindings,
`bind_style` and `cond=` all work per-lane.

### 5. Containers — XML

`<repeat count="lane_count">` replaces the C++ create-and-wire loops in all three
places. `<if cond="...">` handles the `Empty` branch — structural, because one
arm draws nothing.

Bar-vs-spool mode stays a C++ pixel measurement that writes a `mini_mode`
subject; the XML branches on it.

## Performance

This is strictly less work than today. `<repeat>` re-expands only when
`lane_count` changes and per-lane subject changes update in place, whereas
`rebuild_spools()` tears down and rebuilds the whole strip behind a cached render
signature. Matters on CC1 (114 MB, swapping at idle).

## Deleted

- `create_spool_visual()` and the `spool_visual_set_*` family
- `style_slot_bar()` and `BarStyleParams`
- `rebuild_bars()` / `rebuild_spools()` create-and-wire loops
- `apply_slot_status()` / `apply_slot_material()` imperative styling
- `SlotPresentation` and `resolve_slot_presentation()`
- `SpoolCellData` / `SlotBarData` caches and their render signatures
- commits `3f6b76cf5` and `f578a5f82`

## Testing

- **Pure**: `classify_lane` exhaustive over every `SlotStatus` x identity;
  `lane_fill_level` over tracked / untracked / ghosted.
- **Ghost reversal**: a ghosted lane renders its fill AND the whole cell is
  dimmed. Both halves, or the #1071 regression returns.
- **Per rendering**: each of the three base states on the spool widget and the
  bar widget, each crossed with active and error set and clear — the
  decorations must not alter the base state's rendering.
- **Empty on a bar** draws nothing while the layout gap survives — lanes stay
  countable.
- Mutation-verify the ghost and empty branches; a test that passes with the
  branch removed is not a test.

## Open

- Per-lane payload for the context menu still needs `user_data` on generated
  items — a documented exception, unchanged by this design.
- Issue #1367 (`display_fill_level()` and `UNKNOWN`) is superseded in framing:
  its severity claim is wrong, and this design changes the function anyway. Fix
  or close it as part of the work.
