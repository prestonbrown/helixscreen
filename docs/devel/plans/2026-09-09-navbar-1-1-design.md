# Nav bar improvements for 1.1

## Origin

PR [#977](https://github.com/prestonbrown/helixscreen/pull/977) by just-trey proposed
outline/filled icon duality plus size differentiation for the nav bar. The design was
adopted; the implementation is redone on current main because the branch predates roughly
5,200 commits and the three subsystems it touches (the icon size ladder, the icon fonts,
the nav layout) all moved underneath it.

Credit rides with the idea: `Co-authored-by` on the icon commit, which feeds
`CONTRIBUTORS.txt` via `make update-contributors`, plus a `THANKS.md` entry under
"Code & features".

## Goals

1. An inactive nav button is distinguishable from the active one by more than color.
2. The landscape and portrait nav bars stop being two hand-maintained copies.
3. The nav bar follows a live orientation flip rather than latching at startup.

## Non-goals

Everything else in [#1261](https://github.com/prestonbrown/helixscreen/issues/1261) stays
there: [#1214](https://github.com/prestonbrown/helixscreen/issues/1214) restyle-on-resize,
home grid reflow (#1215, #1216), the Android manifest change, and the Display & Sound
orientation control.

Also explicitly not doing: extracting the five nav buttons into a parameterized
`nav_button` component. `${param}` interpolation into `name=` makes it possible
(`lib/helix-xml/src/xml/lv_xml.c#xml_value_has_compose`), but once the landscape and
portrait files are one file, five buttons with different glyphs and different panel
indices are five different buttons, not duplication. Parameterizing them behind `${key}`
would cost readability for no drift benefit.

---

## Commit 1: nav icon state differentiation

### Token

`icon_size_nav_inactive_*` goes in `ui_xml/navigation_bar.xml`'s own `<consts>`, beside
the `nav_width_*` ladder it already owns. Not `globals.xml`: that file carries an explicit
note that `nav_width` is defined in `navigation_bar.xml`, so nav-scoped responsive tokens
have a documented home. Placement does not affect resolution, since
`src/ui/theme_manager.cpp#theme_manager_parse_all_xml_for_suffix` scans every top-level
`ui_xml/*.xml` and registers the bare base name into the `globals` scope.

| tier | `icon_size` | `icon_size_nav_inactive` | active -> inactive px |
|---|---|---|---|
| micro | md | md | 24 -> 24 |
| tiny | md | sm | 32 -> 24 |
| small | md | sm | 32 -> 24 |
| medium | lg | md | 48 -> 32 |
| large | lg | md | 48 -> 32 |
| xlarge | lg | md | 64 -> 48 |
| xxlarge | lg | md | 80 -> 64 |

Two deliberate calls:

- **The top three tiers use `md`, not `lg`.** PR #977 used `lg`, which was one rung below
  `icon_size` on its base but is the *same* rung on current main, because
  `icon_size_large/xlarge/xxlarge` was retuned from `xl` to `lg`. `lg` would make the
  size differentiation inert on exactly those tiers.
- **micro opts out of the size delta.** One rung down is `icon_font_sm_micro`, a 16px
  face, and a 16px outline glyph has thin strokes inside a 42px nav strip on a 480x272
  panel. micro differentiates by glyph and color only.

### Glyphs

| Name | Codepoint | Note |
|---|---|---|
| `home_outline` | F06A1 | new |
| `cog_outline` | F08BB | new |
| `settings_outline` | F08BB | alias, mirrors the existing `settings` -> `cog` pair |
| `filament_outline` | F0E5C | new |

`settings_outline` costs no font weight and keeps the settings button semantically named
on both halves rather than pairing `settings` with `cog_outline`.

MDI has no `tune-outline` and no outline variant of `dots-vertical`, so **Controls and
Advanced cannot take the outline treatment**. They differentiate by size and color only.
This non-uniformity is accepted rather than substituting worse-fitting glyphs.

### XML

Both nav files (until commit 2 makes it one) get: home, filament and settings inactive
icons switched to the outline glyph and `#icon_size_nav_inactive`; controls and advanced
inactive icons keep their glyph and take `#icon_size_nav_inactive`.

Two things from #977 are deliberately not carried over:

- **Disabled icons stay filled at `#icon_size`.** Disabled means disconnected or Klippy
  not ready, which is a third state rather than a flavor of inactive, and
  `variant="disabled"` already sets it apart at 50% opacity. #977 shrank them and made
  filament's outline while controls' stayed filled, encoding a distinction that means
  nothing.
- **The printer-switcher badge keeps `#icon_size`.** It has no active counterpart, so
  shrinking it makes it permanently smaller than everything else, and its 8x8
  `nav_printer_dot` is `align="top_right"` on a content-sized wrap, so the dot's overlap
  fraction grows as the icon shrinks.

`variant` stays `secondary`, not `muted`. `src/ui/ui_variant.cpp` maps `MUTED` to
`StyleRole::TextMuted` while every other icon variant uses an `Icon*` role, so moving five
nav icons onto a text-governed color would put them out of reach of icon theming. A
dedicated `IconMuted` role is a separate change if we want a different gray.

### The gate

A new case in `tests/unit/` resolves both `icon_size_<suffix>` and
`icon_size_nav_inactive_<suffix>` through the `icon_font_<rung>_<suffix>` ladder for each
of the seven suffixes, and asserts the inactive face is never larger than the active one,
and is strictly smaller everywhere except micro. It uses
`theme_manager_parse_all_xml_for_suffix` the way
`tests/unit/test_theme_token_table.cpp` does, so it needs no display.

This is the thing that would have caught the original drift: #977 was correct when
written and silently became wrong when the `icon_size` ladder was retuned, with nothing
failing.

### Regen chain

In order: `make regen-fonts` (against main's current 251-codepoint list, never merging a
branch's `.c` files), `make regen-icon-consts`, `make regen-tokens`,
`scripts/format-xml.py`, then `make test-run`.

`make regen-tokens` is required because `tests/unit/test_theme_token_table.cpp` compares
the committed `src/generated/theme_token_table.cpp` against a live scan, and its `TYPES[]`
includes `"string"`. Adding seven tokens without regenerating fails it. Nothing auto-fixes
this; `gen_theme_tokens.py --check` exists but no hook or workflow calls it.

### Mutation proof

`make mutate-diff` is blind outside `src/` and `include/`, so the XML half is hand-mutated:
set `icon_size_nav_inactive_large` back to `lg` and confirm the new gate goes red before
trusting its green.

---

## Commit 2: unify `navigation_bar.xml`

`ui_xml/navigation_bar.xml` and `ui_xml/portrait/navigation_bar.xml` are 149 and 125 lines
with 111 aligned lines byte-identical. Comment-stripped and tag-normalized, the entire
delta is the `<consts>` block plus nine opening tags. There is **zero behavioral
divergence**: all 25 `bind_*` elements, all 24 widget names, both `event_cb` callbacks,
every icon `src` and every `ref_value` match byte for byte, which is why
`scripts/check_variant_parity.py` passes on the pair today.

### Approach

`bind_style_if_eq` on `ui_is_portrait`, with the orientation-varying attributes moved into
two styles. A `<style>` can carry `flex_flow`, `flex_main_place`, `width`, `height`,
`min_width`, `min_height` and every `pad_*` prop
(`lib/helix-xml/src/xml/lv_xml_style.c#lv_xml_register_style`), which covers everything the
two roots disagree on. `ui_xml/setting_dropdown_row.xml` is the shipped exemplar, swapping
root padding off `ui_breakpoint`; ten `ui_xml` files use the idiom.

```xml
<styles>
  <style name="nav_bar_landscape" width="#nav_width" height="100%" flex_flow="column"
         pad_top="#space_xl" pad_bottom="#space_xl" pad_left="0" pad_right="0"/>
  <style name="nav_bar_portrait"  width="100%" height="#button_height_lg" flex_flow="row"
         pad_left="#space_xl" pad_right="#space_xl" pad_top="0" pad_bottom="0"/>
  <style name="nav_btn_landscape" width="100%" height="content"/>
  <style name="nav_btn_portrait"  width="content" height="100%"/>
</styles>
<view extends="lv_obj" ...>
  <bind_style_if_eq     name="nav_bar_portrait"  subject="ui_is_portrait" ref_value="1"/>
  <bind_style_if_not_eq name="nav_bar_landscape" subject="ui_is_portrait" ref_value="1"/>
```

`ui_xml/portrait/navigation_bar.xml` is deleted.

Because `bind_style_if_eq` is observer-backed, the bar re-styles on a live `ui_is_portrait`
change with no rebuild, no const re-pointing, and no variant-directory re-resolution. That
is #1261's gap 1 for this file, closed. #1214 does not bite: applying a style at runtime is
exactly the mechanism that *does* update already-built widgets, and the icon font never
needs to change because `responsive_dimension()` is `min(width, height)` and therefore
rotation-invariant.

### Constraints that must not be broken

- **`nav_width_*` stays in a top-level `ui_xml/*.xml`.** `scripts/gen_theme_tokens.py` does
  not recurse. Moving that block into `ui_xml/components/` makes `#nav_width` unresolvable, and the
  lookup in `src/ui/theme_manager.cpp` then takes its hardcoded 94px fallback in *both*
  orientations. Lint-gated by `scripts/check_responsive_token_scope.py`.
- **Per-button `flex_flow="column"` is identical in both files and stays fixed.** It stacks
  the icon inside the button and has nothing to do with the bar's axis. This is the
  attribute an axis-swap sweep eats by accident.
- **`nav_btn_print_select` exists in no XML and must stay absent.** The `button_names[]`
  arrays in `src/ui/ui_nav_manager.cpp#register_nav_buttons` are positional by panel id, so
  index order is load-bearing. Both call sites handle the null lookup.
- **`style_flex_main_place` is real drift** (`start` landscape, `center` portrait), and is
  currently inert because every child carries `flex_grow="1"` so there is never free
  main-axis space. Pick `center` deliberately and say so in the commit.

---

## Commit 3: unify `app_layout.xml`

The two app layouts differ in root `flex_flow` and in child order: landscape is
`flex_flow="row"` with the navbar first, portrait is `flex_flow="column"` with the navbar
last. Those two differences cancel, because `column_reverse` is supported
(`lib/helix-xml/src/xml/lv_xml_base_types.c#lv_xml_flex_flow_to_enum`):

| child order | `row` | `column` | `column_reverse` |
|---|---|---|---|
| `[navbar, content_area]` | navbar left | navbar top | navbar bottom |

So one child order `[navbar, content_area]` serves both, with the root's `flex_flow`
swapped by the same `bind_style_if_eq` mechanism as commit 2.
`ui_xml/portrait/app_layout.xml` is deleted.

This commit is separable. If it turns out to disturb panel sizing, commits 1 and 2 stand
alone.

---

## Testing

- **Unit:** the icon-size gate from commit 1. `tests/unit/test_navigation.cpp` currently
  loads the **landscape** file only, because no fixture calls `LayoutManager::init()` and
  it defaults to STANDARD. The portrait file has **zero coverage today**, so consolidating
  puts it under test for free. Add a case that flips `ui_is_portrait` and re-asserts the
  same visibility invariants.
- **Live:** headless via the pinned-socket recipe, then `ctl geom navbar` before and after
  `ctl set ui_is_portrait 1`. That is exact, where a screenshot only proves what a scroll
  position exposed.
- **Gates:** `scripts/check_variant_parity.py` loses two pairs and must still exit 0;
  `format-xml.py --check`; `make test-run` green.

## Doc debt this creates

`README.md` claims portrait has dedicated layouts for the app shell and navigation bar.
That stops being true. Also referencing these files:
`docs/devel/LAYOUT_SYSTEM.md`, `docs/devel/UI_CONTRIBUTOR_GUIDE.md`,
`docs/devel/HELIXCTL.md` (cites `ui_xml/navigation_bar.xml#nav_btn_edit_add`),
`docs/devel/MULTI_PRINTER.md` (inline XML snippet),
`docs/devel/architecture/10-theme-tokens-layout.md`.

`tests/shell/test_responsive_token_scope_gate.bats` uses `portrait/navigation_bar` as a
synthetic fixture it creates itself, so deleting the real file does not affect it.

## Open questions

- just-trey was asked on #977 whether he agrees with micro opting out of the size delta and
  with `md` at the top three tiers. Not blocking; the values are defensible either way.
- How he wants to be named in `THANKS.md`.
