# Lane spool editor redesign

Replaces the one-screen Assign/Edit editor (`ui_xml/ams_edit_overlay.xml`, `src/ui/filament/ui_ams_edit_overlay.cpp`) with a lane summary plus a focused screen for each job. Agreed with Preston on 2026-11-21. Wireframes are in the session scratchpad, and the real XML screenshots go in the PR.

## Why

The current page stacks every job onto one screen: describe a spool, pick from Spoolman, set the weight, set lane settings, edit Spoolman details, restore RFID values. At 480x320 that crowds the controls, needs mode switches and disclosures, and scrolls.

Specific failures:
- Clear Lane is a 19 px text label 2-5 px under Save, and clears without asking.
- The unlabeled ⇄ icon is unclear.
- "Color 2: M..." is cut off.
- "Remaining" stands alone over a weight that can read "—".
- A cleared lane shows 500 g and unknown.
- Lane Settings opens below the fold and scrolls.
- The Assign form is a filter in one mode and the spool in the other, with no cue which.
- The size chips take 60 px, and the spool list is capped at 3.5 rows.
- "Reset to tag" is unclear, and the "Color" label is redundant.
- The color page's back target is only its 20 px arrow.

## Main uses, in order

1. A spool Spoolman already has goes in: pick it fast.
2. A new spool Spoolman doesn't know goes in: set it up quickly. Save it to Spoolman by default, with a checkbox to opt out.
3. The weight or color of the spool in the lane is off: fix it.
4. The same filament again, a fresh spool after a runout. This is the most common case.
5. An RFID-tagged spool goes in. The lane fills itself, and at most the weight needs correcting.
6. A spool comes out: clear the lane, and archive it in Spoolman if it's empty.
7. Lane settings (the tool a lane feeds, its runout backup). Rare.

## Screens

The lane's word always comes from `lane_label(noun, slot)` (`include/display_numbering.h`): Slot, Lane, Gate, Tool or Feeder. It is never hard-coded.

**Lane summary**, for a lane holding a spool. This is where the editor opens.
- Header: back, the lane's name and unit, and **New Spool** (words, no icon). Scan Label stays where Spoolman is set up.
- Spool card: a large spool drawing in its color, filled to what's left, with the product, the material and color name, and the temperatures. Tapping it opens the Filament screen.
- Weight: −/+ either side of "850 g of 1 kg", a drag bar under them, and a tap on the number for the keypad. There is no "Remaining" label. An unknown weight reads as a full spool of the lane's size, never "—".
- Lane line, "Tool T0 · Runout → Lane 3 ›". It appears only where a setting can change, so never on the U1. It opens the Lane Settings screen.
- Bottom: Clear Lane as an outlined danger button at bottom-left, and Save as primary at bottom-right.

**Empty lane**, after a runout or a clear.
- "<Lane> is empty", plus what it last held (swatch, product, material and color).
- **Same Filament, New Spool** (primary): one tap creates a full spool of that filament and assigns it, through Spoolman when Spoolman is set up.
- **Choose a Different Spool**: opens the New Spool list.

**New Spool list**
- The full width, sorted by most recently used, with search in the header.
- Each row shows a swatch, the name, the Spoolman #, what's left ("850 g left", or "Full" for an unused spool) and **Use**.
- A spool currently in another lane says so ("in Feeder 3"), and Use moves it.
- Bottom: **Not Listed? Add New Spool** opens the Filament screen preset to 1 kg, full, with Generic brand.

**Filament screen**
- Material, Type, Brand and Product dropdowns in a 2x2 grid.
- The color name alone, with no "Color" label, and only if it fits; a multi-color spool shows no per-segment label.
- Swatches, recents and the picker.
- Common spools (PLA, PETG, ABS, ASA in black, white and gray) are one-tap fills when adding.
- **Add to Spoolman** checkbox, on by default, shown only when adding and only when Spoolman is set up.
- An RFID bar, "Tag says <material> · <brand> · <color> [Use Tag]", only when the lane differs from its tag. It replaces "Reset to tag".
- Back returns to the summary with the changes held. There is no Done button: the summary's Save commits.
- Spoolman details (price, location, lot, notes) for a linked spool are a row on this screen that opens its own small screen.

**Lane Settings screen**
- Feeds Tool and Runout Backup dropdowns, and one sentence saying what a runout will do.
- A change applies immediately, because these are lane settings, not spool data.

**Clear Lane** asks first: "Clear <lane>?". For a Spoolman-linked spool it adds a checkbox, "Spool is empty, archive it in Spoolman". The checkbox is pre-checked when the lane ran out during a print or Spoolman's remaining weight is under 20 g. After the clear, the lane opens on the Empty screen.

**Color page header**: the readout (swatch, name, hex) goes back too, so the whole left of the header is one back target.

## Constraints

- No page scrolls except the New Spool list. Everything fits from 480x272 up to 800x480 by progressive sizing.
- Title Case. "Use", never "Load". Save commits an edit, and Done only closes a picker.
- Declarative UI rules (`.claude/rules/declarative-ui.md`): data in subjects, appearance in XML, list rows as components.
- Vendor names stay inside the backends. Lane-setting visibility comes from backend capability questions.
- Release/1.1 gets the same change (on 1.1 the files are flat under `src/ui/`).

## Open for the builder

- How each screen maps onto overlays: the summary, list and Filament screen as views of one overlay, or as separate overlays. Pick whichever keeps back navigation and the held edits simplest.
- The test plan: summary and empty-lane states per backend, clear with and without the archive checkbox, Use moving a spool between lanes, and quick add with and without Spoolman.
