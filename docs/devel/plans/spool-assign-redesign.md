# Assign Spool / Edit Spool redesign

Approved design (Preston, 2026-11-20). Mockups: the "Spool Setup Redesign" canvas, row C
(C1-C4). Built on `release/1.1` first, then forward-ported to `main`: the current flow is
broken enough to ship in 1.1.

## Why

Setting up a spool on a lane today (AMS edit overlay, `ams_edit_overlay.xml` /
`ui_ams_edit_overlay.cpp`):

- "Not in your spools? Set it up" in the Spoolman picker drops into the local editor; Save
  writes only the lane. Creating the spool in Spoolman needs a separate small
  "Add to Spoolman + label" link that users do not find.
- The header Spoolman icon button is unlabeled and its route behaves differently from the
  picker entry (one commits on pick, the other returns to the editor).
- Four full-width dropdowns (Material, Variant, Brand, Product) waste the 724 px overlay.
- A new spool defaults to 0 g.

## The design

One screen per job, both inside the existing edit overlay.

### Assign Spool (lane has no spool)

Title **Assign Spool**, subtitle `Lane N · <unit>`. Header button **Scan Label** (QR), labeled.
No unlabeled Spoolman icon.

- **Left column (≈360 px): the describe form.** Material, Type (variant), Brand, Product as
  a 2x2 grid of dropdowns (Product hides when it has no choices: the grid drops to 3 cells).
  Color: swatches in an 8-wide grid that fills the column; the last swatch is **Custom…**
  and opens the existing color picker. Spool size chips: 1 kg (default), 750 g, 250 g,
  Partial… (keypad). Every field optional.
- **Right column: your spools.**
  - Form empty: **Recent spools** (Spoolman `last_used` order), each a row with a spool
    drawing whose filament ring shows remaining weight, name, `#id · N g left · when`, and a
    **Use** button. With Spoolman connected, a search field on top matches name, brand,
    color or `#ID`.
  - Any field set: the list narrows to **matches**. Exact matches first; fuzzy matches
    after them with a small `close: <what differs>` tag. Fuzzy = color by perceived
    distance on Spoolman hex (not name), material/brand/product loosely (case, `PLA` vs
    `PLA+`, "White" vs "Polar White"). Close matches are offered silently, never a prompt.
  - Bottom: **Not Listed? Add New Spool** (dashed). Becomes the primary action when nothing
    matches: **Add to Spoolman & Assign**, with a small secondary link
    "assign without adding to Spoolman" (the exception, not the rule).
  - No spools at all (fresh Spoolman, or none): **common spools to start from**, a 3x4 grid
    of PLA/PETG/ABS/ASA x Black/White/Gray; a tap fills the form. Shown until the user has
    a few spools.
- **Use** on a listed spool links it to the lane and closes. **Add to Spoolman & Assign**
  creates vendor/filament/spool in Spoolman (reuse `SpoolmanSlotSaver` CreateAndRebind),
  links it, and closes.

### Edit Spool (lane already has a spool)

Tapping a lane that has a spool opens straight to **Edit Spool** (subtitle
`Lane N · <name> · #id`). Same form, prefilled. Right: big spool drawing, Remaining with
− / value (tap for keypad) / +, `of <total> g · temps`. **Save** (primary; with a linked
spool it writes back to Spoolman, line under it "Saves to Spoolman spool #id"), **Clear
Lane** (red text). Header **Different Spool** goes to Assign Spool.

### After saving

Return to the AMS panel. Toast: `Lane N: <name>` / `Added to Spoolman as #id` with
**Print Label** and **Undo**. No separate done screen.

### Without Spoolman

Same screen: no search, no `#ID`, the list is spools previously used in this printer's
lanes (HelixScreen's own lane history), the common-spools grid when that is empty, and the
primary action is **Assign** (lane + HelixScreen record only).

### Wording

Title Case for titles and buttons, as the rest of the app ("Edit Spool", "Cool Down").
"Use", never "Load" (Load means feeding filament). No colloquial titles.

## Scope and constraints

- 1.1 first, then main. Keep it inside the AMS edit overlay and its helpers; the bypass /
  external spool menu opens the same screens.
- Declarative UI rules (`.claude/rules/declarative-ui.md`): layout in XML, data via
  subjects; list rows via `lv_xml_create` of a row component.
- Reuse: `SpoolmanSlotSaver`, the existing picker data load, the color picker, the spool
  drawing used by `ams_lane_spool`, filament catalog/presets for the common-spools grid.
- 800x480 and the smallest breakpoint must fit without clipping.

## Not in this change

Label printing itself (the toast button calls the existing print path if there is one,
else is omitted), QR scanning internals (the button opens the existing scanner).
