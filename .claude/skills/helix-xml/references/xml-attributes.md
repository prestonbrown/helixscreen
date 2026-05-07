# XML Attributes Reference

## Quick Rules

- `style_image_recolor` not `style_img_recolor` (full words, no abbreviations)
- `width="content"` not `width="LV_SIZE_CONTENT"` (XML string, not C constant)
- `flex_align` doesn't exist → use `style_flex_main_place` / `style_flex_cross_place`
- `zoom` doesn't exist → use `scale_x` / `scale_y` (256 = 100%)
- Unknown attributes silently ignored

## Base Object (lv_obj) — All widgets inherit these

### Layout

| Attr | Type | Notes |
|------|------|-------|
| `name` | str | For `lv_obj_find_by_name()` |
| `x`, `y` | size | px or % |
| `width`, `height` | size | px, %, or `"content"` |
| `align` | enum | `center`, `top_left`, `top_mid`, `top_right`, `bottom_*`, `left_mid`, `right_mid` |

### Flex

| Attr | Type | Notes |
|------|------|-------|
| `flex_flow` | enum | `row`, `column`, `row_wrap`, `column_wrap`, `*_reverse` |
| `flex_grow` | int | 0=fixed, 1+=grows |

### Flags (bool)
`hidden`, `clickable`, `checkable`, `scrollable`, `scroll_elastic`, `scroll_momentum`, `scroll_chain`, `ignore_layout`, `floating`, `overflow_visible`, `event_bubble`

### States (bool)
`checked`, `focused`, `disabled`, `pressed`, `hovered`

## Data Binding

```xml
<!-- Simple bindings -->
<lv_label bind_text="temp_subject"/>
<lv_label bind_text="temp" bind_text-fmt="%.1f°C"/>
<lv_slider bind_value="volume"/>

<!-- Conditional bindings (child elements) -->
<lv_obj>
    <bind_flag_if_eq subject="panel" flag="hidden" ref_value="0"/>
</lv_obj>
<lv_button>
    <bind_state_if_eq subject="power" state="disabled" ref_value="0"/>
</lv_button>
```

**Operators:** `bind_flag_if_eq`, `_not_eq`, `_gt`, `_ge`, `_lt`, `_le` (same for `bind_state_if_*` and `bind_style_if_*`)

### Parse-Time Conditional Hidden

| Attribute | Behavior |
|-----------|----------|
| `hidden_if_empty="$prop"` | Hides if prop is empty string |
| `hidden_if_prop_eq="$prop\|ref_value"` | Hides if prop equals ref_value |
| `hidden_if_prop_not_eq="$prop\|ref_value"` | Hides if prop does NOT equal ref_value |

These evaluate once at parse time — not reactive. For runtime visibility, use `bind_flag_if_*`.

## Style Attributes (style_* prefix)

### Size & Spacing
`style_radius`, `style_pad_all`, `style_pad_hor`, `style_pad_ver`, `style_pad_gap`, `style_pad_column`, `style_pad_row`, `style_margin_all`, `style_margin_hor`, `style_margin_ver`, `style_min_width`, `style_max_width`, `style_min_height`, `style_max_height`

### Background
`style_bg_color` (hex: `0xff0000`), `style_bg_opa` (0-255 or `"50%"`), `style_bg_grad_dir` (`none`/`hor`/`ver`), `style_bg_grad_color`

### Border & Shadow
`style_border_color`, `style_border_width`, `style_border_opa`, `style_border_side` (`none`/`top`/`bottom`/`left`/`right`/`full`), `style_shadow_width`, `style_shadow_color`, `style_shadow_opa`, `style_shadow_offset_x`, `style_shadow_offset_y`

### Text
`style_text_color`, `style_text_font`, `style_text_opa`, `style_text_align` (`left`/`right`/`center`/`auto`)

### Image
`style_image_recolor` (⚠️ `image` not `img`), `style_image_recolor_opa`

### Flex Layout
`style_flex_main_place`, `style_flex_cross_place`, `style_flex_track_place` — values: `start`, `end`, `center`, `space_between`, `space_around`, `space_evenly`

### Transforms
`style_transform_scale_x`/`y` (256=100%), `style_transform_rotation` (0.1° units, 900=90°), `style_translate_x`/`y`, `style_opa`

## Reusable Styles

Define in `<styles>`, apply with child `<style>`. Drop `style_` prefix in definitions.

```xml
<styles>
    <style name="btn" bg_color="0x2196f3" radius="8"/>
</styles>
<lv_button>
    <style name="btn"/>
    <style name="btn_pressed" selector="pressed"/>
</lv_button>
```

**Selectors:** `default`, `pressed`, `checked`, `focused`, `disabled`
**Parts:** `main`, `indicator`, `knob`
**Combine:** `selector="indicator:pressed"` | **Remove:** `bg_color="remove"` | **Constants:** `bg_color="#primary"`

## Widgets

### lv_label
`text`, `long_mode` (`wrap`/`scroll`/`dots`/`clip`), `bind_text`

### lv_image
`src`, `scale_x`/`scale_y` (256=100%), `rotation` (0.1°), `inner_align` (`center`/`stretch`/`tile`)

### lv_slider / lv_bar
`value` (`"50"` or `"50 true"` for animated), `range` (`"0 100"`), `mode` (`normal`/`range`/`symmetrical`), `bind_value`

### lv_arc
`value`, `range`, `angles` (`"0 270"`), `mode` (`normal`/`reverse`/`symmetrical`)

### lv_textarea
`text`, `placeholder`, `one_line`, `password_mode`

### lv_checkbox
`text`, `checked`

### lv_dropdown / lv_roller
`options` (`"A&#10;B&#10;C"` — use `&#10;` for newlines), `selected`, `bind_value`

### lv_buttonmatrix
```xml
<lv_buttonmatrix map="'1' '2' '3' '\n' '4' '5' '6'" one_checked="true"/>
```

## Event Callbacks

```xml
<lv_button>
    <event_cb trigger="clicked" callback="my_handler"/>
</lv_button>
```

Register: `lv_xml_register_event_cb(nullptr, "my_handler", fn)`

**Triggers:** `clicked`, `value_changed`, `pressed`, `released`, `ready`, `cancel`

## Enums Reference

| Type | Values |
|------|--------|
| align | `center`, `top_left`, `top_mid`, `top_right`, `bottom_*`, `left_mid`, `right_mid` |
| flex_flow | `row`, `column`, `row_wrap`, `column_wrap`, `*_reverse` |
| flex_align | `start`, `end`, `center`, `space_between`, `space_around`, `space_evenly` |
| dir | `none`, `top`, `bottom`, `left`, `right`, `hor`, `ver`, `all` |
| border_side | `none`, `top`, `bottom`, `left`, `right`, `full` |
| text_align | `left`, `right`, `center`, `auto` |
| blend_mode | `normal`, `additive`, `subtractive`, `multiply` |
