# LVGL 9.5 / helix-xml Attributes Reference

**Source:** `lib/helix-xml/src/xml/parsers/*.c` | **Updated:** 2026-07-22

---

## Quick Rules

- `style_image_recolor` not `style_img_recolor` (full words, no abbreviations)
- `width="content"` not `width="LV_SIZE_CONTENT"` (XML string, not C constant)
- `flex_align` doesn't exist → use `style_flex_main_place` / `style_flex_cross_place`
- `zoom` doesn't exist → use `scale_x` / `scale_y` (256 = 100%)
- Unknown attributes silently ignored

---

## Base Object (lv_obj)

All widgets inherit these.

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

### Data Binding

```xml
<!-- Simple bindings as attributes -->
<lv_label bind_text="temp_subject"/>
<lv_label bind_text="temp" bind_text-fmt="%.1f°C"/>
<lv_slider bind_value="volume"/>

<!-- Conditional bindings as child elements -->
<lv_obj>
    <bind_flag_if_eq subject="panel" flag="hidden" ref_value="0"/>
</lv_obj>
<lv_button>
    <bind_state_if_eq subject="power" state="disabled" ref_value="0"/>
</lv_button>
```

**Operators:** `bind_flag_if_eq`, `bind_flag_if_not_eq`, `bind_flag_if_gt`, `bind_flag_if_ge`, `bind_flag_if_lt`, `bind_flag_if_le` (same for `bind_state_*`)

**Expression-driven conditionals** (multi-subject / arithmetic conditions — see `LVGL9_XML_GUIDE.md` § "Expression Conditionals"):

```xml
<!-- Derived subject, kept in sync (declare inputs first) -->
<subjects>
    <int name="demo_temp" value="50"/>
    <int name="demo_threshold" value="70"/>
    <subject_expr name="demo_alarm" expr="demo_temp gt demo_threshold"/>
</subjects>

<lv_obj>
    <bind_flag_if cond="demo_alarm" flag="hidden" invert="true"/>
</lv_obj>
<ui_button>
    <bind_state_if cond="demo_alarm" state="disabled"/>
</ui_button>
<ui_card>
    <bind_style_if name="demo_alarm_style" cond="demo_alarm"/>
</ui_card>
```

| Tag/Attr | Notes |
|----------|-------|
| `<subject_expr name= expr=>` | Sibling of `<subject>`/`<int>` in `<subjects>`; derived int subject, inputs must be declared earlier |
| `<bind_flag_if cond= flag= invert=>` | Reactive flag add/remove driven by expression |
| `<bind_state_if cond= state= invert=>` | Reactive state add/remove driven by expression |
| `<bind_style_if cond= name= selector= parts= invert=>` | Reactive style enable/disable driven by expression |

**Grammar:** subject name, int literal, `( )` grouping; `== != < <= > >=` / word forms `eq ne lt le gt ge`; `&& \|\| !` / word forms `and or not`; `+ - * / %` (div/mod by zero → `0`). **House style = word forms** — `&&`/`<` need XML escaping (`&amp;&amp;`/`&lt;`), word forms don't.

**Looping** (`LVGL9_XML_GUIDE.md` § "Repeating fragments with `<repeat>`"):

```xml
<!-- Expand the body N times at load time; $i is the zero-based index -->
<lv_obj name="root">
    <repeat count="4">
        <lv_label name="lbl" text="$i"/>
    </repeat>
</lv_obj>
```

| Tag/Attr | Notes |
|----------|-------|
| `<repeat count=>` | Expands its body `count` times. `count` is a literal, a `#const`, or a subject name; a subject-bound `count` **reactively rebuilds** the expansion when the subject changes (async off-tree teardown — a reactive `<repeat>` must be its parent's last child or in its own container). Clamped to `[0, 256]`. Not nestable yet. |
| `$i` / `${…}` | Zero-based iteration index inside a `<repeat>` body. Bare `$i` is a whole-value substitution (`text="$i"`); `${i}` / `${prop}` splices a name into a larger string (`bind_text="slot_${i}_label"`). `${…}` also evaluates an **integer expression** and splices the result (`${i + 1}`, `${i * 84}`, `${base * scale}`, `${cols * 2}`) — operands are `i`, integer literals, numeric props, and subjects. **Resolve-once**: subject operands are read at creation and do not update reactively (use `bind_*` for live values). |

**Structural conditionals** (`LVGL9_XML_GUIDE.md` § "Structural conditionals with `<if>` / `<else>`"):

```xml
<!-- Creates ONLY the matching branch; false branch is never built -->
<lv_obj name="root">
    <if cond="c gt 0">
        <lv_obj name="t"/>
        <else/>
        <lv_obj name="f"/>
    </if>
</lv_obj>
```

| Tag/Attr | Notes |
|----------|-------|
| `<if cond=>` | `cond` is an expression string, same word-form grammar as `cond=` on `bind_flag_if`/`<subject_expr>`. Creates only the matching body — no subjects referenced = static, expands once at load, no observer; subjects referenced = reactive, rebuilds on any operand change. Reactive `<if>` must be its parent's last child or in its own container (same ordering constraint as `<repeat>`). Not nestable yet. |
| `<else>` | No attributes. Inline divider inside one `<if>…</if>`: everything before it is the true-body, everything after is the false-body. `<else/>` and `<else></else>` are identical. Optional — omitting it means "create nothing" for the false case. A second `<else/>` warns (first split wins); a stray `<else/>` outside any `<if>` warns and is ignored. |

---

## Style Attributes (style_* prefix)

### Size & Spacing

| Attr | Notes |
|------|-------|
| `style_radius` | Corner radius |
| `style_pad_all`, `style_pad_hor`, `style_pad_ver` | Padding |
| `style_pad_gap` | Flex/grid gap |
| `style_margin_all`, `style_margin_hor`, `style_margin_ver` | Margin |
| `style_min_width`, `style_max_width`, `style_min_height`, `style_max_height` | Constraints |

### Background

| Attr | Notes |
|------|-------|
| `style_bg_color` | Hex: `0xff0000` |
| `style_bg_opa` | 0-255 or `"50%"` |
| `style_bg_grad_dir` | `none`, `hor`, `ver` |
| `style_bg_grad_color` | Gradient end |

### Border & Shadow

| Attr | Notes |
|------|-------|
| `style_border_color`, `style_border_width`, `style_border_opa` | |
| `style_border_side` | `none`, `top`, `bottom`, `left`, `right`, `full` |
| `style_shadow_width`, `style_shadow_color`, `style_shadow_opa` | |
| `style_shadow_offset_x`, `style_shadow_offset_y` | |

### Text

| Attr | Notes |
|------|-------|
| `style_text_color`, `style_text_font`, `style_text_opa` | |
| `style_text_align` | `left`, `right`, `center`, `auto` |

### Image

| Attr | Notes |
|------|-------|
| `style_image_recolor` | ⚠️ `image` not `img` |
| `style_image_recolor_opa` | |

### Flex Layout

| Attr | Notes |
|------|-------|
| `style_flex_main_place` | Main axis: `start`, `end`, `center`, `space_between`, `space_around`, `space_evenly` |
| `style_flex_cross_place` | Cross axis alignment |
| `style_flex_track_place` | Track alignment — **needed to center items with explicit widths** (not just wrap!) |

### Transforms

| Attr | Notes |
|------|-------|
| `style_transform_scale_x`, `style_transform_scale_y` | 256=100%, 512=200% |
| `style_transform_rotation` | 0.1° units (900=90°) |
| `style_translate_x`, `style_translate_y` | Offset |
| `style_opa` | Overall opacity |

---

## Reusable Styles

Define in `<styles>`, apply with child `<style>`. Drop `style_` prefix.

```xml
<styles>
    <style name="btn" bg_color="0x2196f3" radius="8"/>
</styles>

<lv_button>
    <style name="btn"/>
    <style name="btn_pressed" selector="pressed"/>
</lv_button>
```

**Selectors:** `default`, `pressed`, `checked`, `focused`, `disabled` | **Parts:** `main`, `scrollbar`, `indicator`, `knob`, `selected`, `items`, `cursor`

**Combine:** `selector="indicator:pressed"` | **Remove:** `bg_color="remove"` | **Constants:** `bg_color="#primary"`

**`bind_style` / `bind_style_if_*` — `parts="main,indicator"`:** apply one style to multiple parts in one line (helix-xml extension). State bits from `selector` are preserved per part. See `LVGL9_XML_GUIDE.md` § "Applying One Style to Multiple Parts".

---

## Widgets

### lv_label

| Attr | Notes |
|------|-------|
| `text` | Content |
| `long_mode` | `wrap`, `scroll`, `dots`, `clip`. ⚠️ Inert without an explicit `width` — a content-width label is always one line. See `LVGL9_XML_GUIDE.md`, "Text never wraps inside a `flex_grow` column". |
| `bind_text` | `"subject 'format'"` |

### lv_image

| Attr | Notes |
|------|-------|
| `src` | Image name or path |
| `scale_x`, `scale_y` | 256=100% (⚠️ no `zoom`) |
| `rotation` | 0.1° units |
| `inner_align` | `center`, `stretch`, `tile` |

### lv_slider / lv_bar

| Attr | Notes |
|------|-------|
| `value` | `"50"` or `"50 true"` (animated) |
| `range` | `"0 100"` |
| `mode` | `normal`, `range`, `symmetrical` |
| `bind_value` | Subject name |

### lv_arc

| Attr | Notes |
|------|-------|
| `value`, `range` | Same as slider |
| `angles` | `"0 270"` (start end) |
| `mode` | `normal`, `reverse`, `symmetrical` |

### lv_textarea

| Attr | Notes |
|------|-------|
| `text`, `placeholder` | |
| `one_line`, `password_mode` | bool |

### lv_checkbox

| Attr | Notes |
|------|-------|
| `text` | Label |
| `checked` | bool state |

### lv_dropdown / lv_roller

| Attr | Notes |
|------|-------|
| `options` | `"A&#10;B&#10;C"` (use `&#10;` for newlines in XML!) |
| `selected` | Index |
| `bind_value` | Subject |

### lv_buttonmatrix

```xml
<lv_buttonmatrix map="'1' '2' '3' '\n' '4' '5' '6'" one_checked="true"/>
```

---

## Event Callbacks (9.4+)

```xml
<lv_button>
    <event_cb trigger="clicked" callback="my_handler"/>
</lv_button>
```

Register: `lv_xml_register_event_cb(nullptr, "my_handler", fn)`

**Triggers:** `clicked`, `value_changed`, `pressed`, `released`, `ready`, `cancel`

---

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
