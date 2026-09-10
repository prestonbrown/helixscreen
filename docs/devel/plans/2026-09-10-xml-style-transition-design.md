# XML Style Transitions: a CSS-style `transition` attribute

## Context

Nav bar buttons swap between a filled large icon and an outline small one the
instant `active_panel` changes. Smoothing that motion exposed a gap: the XML
engine has no way to declare animation at all. All ~80 animations in the app are
started from C++ and individually gated on `DisplaySettingsManager::get_animations_enabled()`.

`lib/helix-xml/` does carry a `<timeline>`/`<animation>` facility, but it triggers
on `lv_event`s only, no XML file uses it, and `docs/devel/HELIX_XML_FORK.md`
proposes it be used or deleted. That facility is the `@keyframes` analogue. The
piece that is missing is the `transition` analogue, which is both the shape a web
developer reaches for first and the shape a two-state toggle wants.

LVGL already implements the mechanism (`lv_style_set_transition`); `lv_theme_default`
uses it for press feedback today. This exposes it to XML.

## Engine facts a plan must respect (verified)

1. **LVGL's transition interpolator is a blacklist, not a whitelist.**
   `lib/lvgl/src/core/lv_obj_style.c#trans_anim_cb` switches on only the props that
   cannot interpolate; everything else falls through to a generic numeric lerp.
   - `LV_STYLE_TEXT_OPA` and `LV_STYLE_TRANSFORM_SCALE_X/Y` interpolate correctly.
   - Pointer props absent from the snap list (`BG_IMAGE_SRC`, `BG_GRAD`,
     `BITMAP_MASK_SRC`, `GRID_*_DSC_ARRAY`, `ARC_IMAGE_SRC`) have their low 32 bits
     arithmetically blended and the result is handed to the draw pass as a pointer.
     That is a crash.
   - `ARC_COLOR`, `LINE_COLOR` and `BG_IMAGE_RECOLOR` are absent from the color-mix
     block and bleed channels through `.num`.

   Nothing in `lib/lvgl/src/core/lv_obj_style.c#lv_obj_style_create_transition` validates. **The XML
   parser is the only place a bad prop can be caught.**

2. **Only the state being entered is scanned.** The state-change path
   (`lib/lvgl/src/core/lv_obj.c#update_obj_state`) skips a style whose state is not a subset of the new
   state, so a transition declared only on `:checked` is invisible when leaving
   checked. A transition on the base style (state 0) survives the filter in both
   directions.

3. **Styles are inline records on the scope.** `lv_xml_style_t` (`lib/helix-xml/src/xml/lv_xml_style.h`)
   holds its `lv_style_t` inline as an `lv_ll` node in `scope->style_ll`. Teardown in
   `lib/helix-xml/src/xml/lv_xml_component.c#component_scope_free` calls `lv_style_reset`,
   which frees nothing a property points at.

4. **Same-name registration reuses the record.** `lib/helix-xml/src/xml/lv_xml_style.c#lv_xml_register_style`
   reuses an existing record of the same name and re-runs the setters over it.
   `globals.xml` hot-reloads into a scope that is never retired, so every save
   re-enters this on the same record.

5. **A running transition does not retain the dsc.** `lib/lvgl/src/core/lv_obj_style.c#lv_obj_style_create_transition`
   copies duration, delay, path and prop out of the dsc before animating, so
   free-then-replace at set time is safe.

6. **XML-added styles do fire transitions.** The state-change path scans every style
   on the object; there is no theme-only path.

7. **An unrendered object snaps.** `lib/lvgl/src/core/lv_obj.c#update_obj_state` returns early from the transition path
   when the object has not been drawn yet, so state set during construction and
   binding does not animate. No startup-flash handling is needed.

8. **Equal endpoints are a silent no-op.** `lv_obj_style_create_transition` returns
   early when both endpoint values match, read through the full cascade including
   inheritance. `LV_STYLE_TEXT_OPA` is inheritable, so the state style must set it on
   the animated widget itself, not only on an ancestor.

9. **`STYLE_TRANSITION_MAX` caps transitioning props at 32** per state change across
   all styles on one object.

10. **`LV_STATE_CHECKED` is free on labels, not on buttons.** `lv_theme_default` has
    no label branch, so `<icon>` carries zero theme styles. Its button branch adds a
    secondary-palette background on `LV_STATE_CHECKED`, which would leak visually.

11. **Buttons already carry a theme transition** over background and transform props.
    `TEXT_OPA` and `TEXT_COLOR` are not in that set, so a text-opacity transition does
    not contend with it.

## The feature

### Shorthand

```
transition="<props> <duration> [easing] [delay]"
```

Properties are separated by `|`, matching the existing selector-token convention.
Duration and delay accept `200ms` or a bare `200`. Easing names map onto
`lv_anim_path_*`: `linear`, `ease_in`, `ease_out`, `ease_in_out`, `overshoot`,
`bounce`. Easing defaults to `linear`, delay to `0`.

```xml
<style name="nav_icon" transition="text_opa|transform_scale_x|transform_scale_y 200ms ease_out"/>
```

### Longhand

```xml
transition_props="text_opa"
transition_duration="#anim_fast"
transition_easing="ease_out"
transition_delay="0"
```

Each accepts a `#const`, which the shorthand cannot express positionally. **When both
forms appear on one style, each longhand attribute overrides that field of the
shorthand.** XML attribute order is not dependable, so the rule is stated rather than
derived from ordering as CSS does.

### Validation

`lib/helix-xml/src/xml/lv_xml_component.c#style_prop_anim_get_type` already classifies
props for `<animation>`. Extend that function rather than fork a twin. It needs two
corrections, both real bugs in it today:

- it rejects `RECOLOR` and `IMAGE_RECOLOR`, which LVGL mixes correctly
- it accepts `ARC_COLOR` and `LINE_COLOR`, which LVGL does not

Correcting it is safe: `<animation>` is its only current caller and no XML file uses
that tag.

An unknown or non-interpolatable prop is rejected with a warning naming both the prop
and the style. Silence here would ship a crash.

### dsc ownership

`lv_xml_style_t` gains ownership of the `lv_style_transition_dsc_t` and its
`lv_style_prop_t[]`.

- **Set path:** free any existing dsc and prop array before installing the new one.
  This is what keeps `globals.xml` hot reload from orphaning one dsc per save (fact 4),
  and fact 5 makes it safe.
- **Free path:** in the style walk in `component_scope_free`, alongside the existing
  `lv_style_reset`. Freeing there and nowhere else means the deferred and borrowed
  scope paths stay correct.
- Scope ownership matches the lifetime of a cross-scope borrowed style, which hands a
  raw `lv_style_t*` across scopes. Per-widget allocation would not.

### Global scale

```c
void lv_xml_set_transition_scale(int scale_256);   /* 256 = authored, 0 = off */
```

The engine keeps a registry of the dscs it allocated. Setting the scale walks them and
retimes in place, so the effective duration is `authored * scale / 256` and the change
applies without re-registering components. The name and units carry no app concepts;
this is the engine's `prefers-reduced-motion` equivalent.

HelixScreen wires it once at startup from the existing `settings_animations_enabled`
observer.

## Nav bar, the first consumer

- Both icons in a pair get `floating="true"` so they overlap instead of each taking
  flex space. Making both visible without this animates a layout reflow.
- `bind_state_if_eq subject="active_panel" state="checked"` on the **icons**, not the
  buttons (fact 10).
- The base style carries the `transition` (fact 2); the `:checked` style carries the
  target opacity and scale, set on the icon itself (fact 8).
- `transform_pivot_x/y` at 50% so scale grows from the centre.

## Non-goals

- **Per-property durations.** LVGL stores one dsc per style and selector, so
  `transition: transform 200ms, opacity 100ms` cannot map without synthesising extra
  styles.
- **Reviving `<timeline>`.** Whether it gains a subject trigger or gets deleted stays
  the open question `HELIX_XML_FORK.md` already poses.
- **Animating icon font size.** Sizes select among discrete bitmap fonts;
  `transform_scale` raster-scales an already-rasterised glyph, soft in flight and crisp
  at rest. No approach here changes that.
- **The 3-way controls button.** Its disabled state keeps today's behaviour.

## Testing

Engine, in `lib/helix-xml/`:
- shorthand parses to the right props, duration, easing and delay
- longhand overrides each shorthand field
- a pointer prop and `ARC_COLOR` are both rejected, with the style named
- registering the same style name twice frees the first dsc rather than orphaning it
- setting the scale retimes an already-registered dsc

App:
- the nav bar renders correctly in both states, at both orientations
- `HelixTestFixture` forces `settings_animations_enabled` to 0 suite-wide, so any test
  of the motion itself enables the subject explicitly and restores it

Mutation coverage is hand-written: `make mutate-diff` is blind on new files.

## Risks

- The 32-prop transition cap is per object per state change, shared with theme styles.
- `helix::ui::icon::set_opa` writes `text_opa` as a local style at state 0, which owns
  the default endpoint of any text-opacity transition on that icon. Nothing calls it on
  nav icons today, but an imperative caller would silently move one end.
- Declaring the same prop in two transitions on one object is order-dependent; the docs
  should say not to.
