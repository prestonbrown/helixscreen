# Panel Widgets (Developer Guide)

How home-dashboard widgets are built, and the reference pattern for making
one size itself responsively. The nozzle-temps widget is the exemplar to copy:
`src/ui/panel_widgets/nozzle_temps_widget.{h,cpp}`,
`src/ui/panel_widgets/nozzle_layout.h`, and its three XML components.

**Related**: `LAYOUT_SYSTEM.md` (the home grid that sizes tiles),
`LVGL9_XML_GUIDE.md` (bindings), `ARCHITECTURE.md` (subjects).

---

## The pieces of a widget

| Piece | Where | Notes |
|-------|-------|-------|
| Registry def | `src/ui/panel_widget_registry.cpp` | id, display name, icon, category, default/enabled, colspan/rowspan and their min/max, whether it may span half cells |
| Widget class | `src/ui/panel_widgets/<name>_widget.{h,cpp}` | extends `PanelWidget`; implements `attach()` / `detach()` / `on_size_changed()` |
| XML component | `ui_xml/components/panel_widget_<name>.xml` | the entire appearance |
| Row/child components | `ui_xml/components/<name>_*.xml` | repeated fragments the widget creates per item |
| Registration | `register_<name>_widget()` | factory + `register_widget_subjects()` (see below) |
| Placement | `assets/config/panel_widgets/<preset>/home.json` | per-printer-preset seeds; users rearrange at runtime |

## The reference pattern: measure, decide, publish, bind

A widget that sizes itself responsively is four layers with exactly one
responsibility each. `NozzleTempsWidget` (#1613) is the working instance.

**1. A pure decision function, no LVGL.** All sizing logic lives in a
header-only function next to the widget (`nozzle_layout.h`) taking measured
pixel inputs and returning a small verdict struct. It is unit-testable
without a display, and the tests' fixtures are the widths the widget really
measured, so the boundaries they pin are the live ones. Split the decision
the way the questions actually split — for nozzle-temps, the font tier is a
HEIGHT question (a stack that overflows the tile overflows it at any width)
and the label rung a WIDTH question (a row is one line tall whatever it
says, so label richness never costs height). When two axes both matter,
model both explicitly rather than folding one into constants.

**2. Measurement in the widget, one composer for render and measure.**
`on_size_changed()` measures text in the fonts the rows actually render, and
the strings it measures come from the same composer that `update_row_display`
renders — measurement and rendering cannot drift apart, or the ladder decides
on widths the row does not draw. Measure the values the rows are ACTUALLY
showing, not worst-case glyphs: an "888°" budget excludes normal-font rungs
from tiles where every real value fits.

**3. Publish the verdict as subjects.** One int subject per axis
(`nozzle_row_label_mode`, `nozzle_row_columns`, `nozzle_row_compact`),
registered via `register_widget_subjects("<id>", fn)` so they exist before
any XML that binds them is parsed — the parser permanently skips a binding
whose subject does not exist at parse time. `on_size_changed()` becomes
measure-and-publish with zero widget calls. Enum values ARE the subject ints
(`NozzleLabelMode`), so XML `ref_value`s cannot drift from C++.

**4. Bind every appearance in XML.** Row widths, container flow, label
visibility, fonts — all `bind_flag_if_*` / `bind_style_if_*` off the
subjects. A row created by a late rebuild reads the current values at
creation and agrees with its siblings with no seeding pass. Prefer one
threshold bind (`bind_flag_if_lt subject=... ref_value="2"`) over enumerated
equality pairs; it reads as the predicate it is.

### Degradation design

Let content degrade before identity does: the nozzle-temps value drops its
target half (one tap away in a detail view) before the row drops its tool
number, and the number before the icon. Budget each rung for what it
ACTUALLY draws at that rung — the target-hiding rungs measure the
current-only value. Re-decide when the value's width class changes (a
target setting or clearing), so a tile sized under idle values never draws
the wide value in the narrow rung.

### Engine contracts this pattern relies on

- A `<style>` carrying `flex_flow` must ALSO carry `layout="flex"` — the
  inline attribute path sets both, the style path does not, and a container
  without an active layout piles every child onto the first while
  `lv_obj_get_style_flex_flow` still reports the flow. Test layouts by
  asserting `LV_STYLE_LAYOUT` and child positions, not the flow value.
- Inline `style_*` attributes write LOCAL styles that outrank any bound
  style (declarative rule 6) — including `style_pad_all`, which shadows a
  per-state bound `pad_top`. Split the inline all-sides form when any side
  is bound.
- A measured-exact fit renders as an overlap: measurement and rendered width
  disagree by a few pixels, so keep a comfort margin on text-budgeted rungs
  and pin one-line labels (`long_mode="dots"`) when the stack model budgets
  one line.
- Bindable fonts on semantic `text_*` widgets work because the semantic font
  rides a shared ADDED style (#1614); do not reintroduce local font writes.

## Testing the pattern

- Pure decision tests on measured-live fixtures, every boundary exact
  (`[nozzle][layout]`).
- Widget tests asserting the BOUND OUTCOME: which label carries HIDDEN, the
  resolved width/flow/layout, distinct cell positions — not cached decisions
  (`[widget_size][nozzle_temps]`). Drive value-shape transitions through the
  real subjects to pin the re-decide.
- `test_widget_content_fits.cpp` sweeps every widget at every shipping
  geometry at its authored minimum; its baseline entries are the honest
  record of which tiles sit below a widget's floor.
