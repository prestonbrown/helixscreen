---
name: helix-xml
description: >
  HelixScreen XML UI engine — triggers when editing files in ui_xml/, lib/helix-xml/, src/xml_registration.cpp,
  or include/ headers that register subjects/components/events. Also for creating modal dialogs via ModalStack
  (src/ui/modals/), defining custom XML widgets/components in ui_xml/components/, writing C++ code that binds
  lv_subject_t to XML layouts, working with design tokens (#space_md, #card_bg) in XML, or using
  bind_text/bind_flag/bind_style reactive bindings. Covers the full XML→Subject→C++ pipeline, declarative
  UI rules, component system, event registration, and gotchas specific to this engine.
---

# HelixScreen XML UI Engine (helix-xml)

The helix-xml library (`lib/helix-xml/`) provides the declarative XML UI system for HelixScreen. XML layouts live in `ui_xml/`, components are registered in `src/xml_registration.cpp`, and C++ subjects are bound via `lv_xml_register_subject()`. The engine implements a Subject-Observer pipeline:

```
XML Layout (components) → bind_text/bind_value/bind_flag → Subjects (C++) → UI auto-updates
```

## Core Architecture

### Registration Flow (startup order matters)

```cpp
// 1. Fonts
lv_xml_register_font(NULL, "montserrat_16", &lv_font_montserrat_16);
// 2. Globals FIRST (constants must exist before components reference them)
lv_xml_register_component_from_file("A:ui_xml/globals.xml");
// 3. Responsive tokens
ui_theme_register_responsive_spacing();  // #space_md, etc.
ui_theme_register_responsive_fonts();    // #font_body, etc.
// 4. Components (order doesn't matter after globals)
lv_xml_register_component_from_file("A:ui_xml/home_panel.xml");
```

### Component Structure

```xml
<component>
    <api>                          <!-- Properties from parent -->
        <prop name="text" type="string" default="Click me"/>
    </api>
    <consts>                       <!-- Local constants -->
        <px name="size" value="36"/>
    </consts>
    <styles>                       <!-- NO style_ prefix here! -->
        <style name="base" bg_color="0x333" text_color="0xfff"/>
    </styles>
    <view extends="lv_button" width="#size">   <!-- The UI tree -->
        <lv_label text="$text" align="center"/>
        <style name="base"/>
    </view>
</component>
```

### Sigils

| Sigil | Meaning | Example |
|-------|---------|---------|
| `#` | Design token / const | `style_pad_all="#space_md"` |
| `$` | Component prop | `text="$label_text"` |
| `@` | Subject binding (ui_button only) | `text="@status"` |

For standard widgets, `bind_text="subject_name"` always treats its value as a subject (no `@` needed).

### Subject Lifecycle

```cpp
static lv_subject_t subj;
static char buf[128];  // MUST be static/heap — never stack
lv_subject_init_string(&subj, buf, NULL, sizeof(buf), "Initial");
lv_xml_register_subject(NULL, "status_text", &subj);  // BEFORE creating XML
// Later: UI updates automatically
lv_subject_copy_string(&subj, "New status");
```

Subject types: `lv_subject_init_string()`, `lv_subject_init_int()`, `lv_subject_init_pointer()`, `lv_subject_init_color()`.

### Data Binding (XML side)

```xml
<!-- Text binding -->
<lv_label bind_text="status"/>
<lv_label bind_text="temp" bind_text-fmt="%.1f°C"/>

<!-- Conditional visibility -->
<lv_obj>
    <bind_flag_if_eq subject="step" flag="hidden" ref_value="1"/>
</lv_obj>

<!-- Conditional style (inline styles OVERRIDE bind_style!) -->
<styles>
    <style name="active" bg_color="0x00ff00"/>
    <style name="inactive" bg_color="0xff0000"/>
</styles>
<lv_button>
    <bind_style name="inactive" subject="is_on" ref_value="0"/>
    <bind_style name="active" subject="is_on" ref_value="1"/>
</lv_button>
```

Comparison operators: `bind_flag_if_eq`, `_not_eq`, `_gt`, `_ge`, `_lt`, `_le` (same for `bind_state_if_*` and `bind_style_if_*`).

Flags: `hidden`, `clickable`, `checkable`, `scrollable`, `disabled`, `ignore_layout`, `floating`.

## Declarative UI Rules (MANDATORY)

These rules are non-negotiable in this codebase:

1. **ALL UI updates via reactive subjects** — never `lv_label_set_text()`, `lv_obj_add_flag(HIDDEN)`, `lv_obj_set_style_*()`, or `lv_obj_add_state()`
2. **Events declared in XML** with `<event_cb trigger="clicked" callback="name"/>`, registered in C++ via `lv_xml_register_event_cb()`. NEVER use `lv_obj_add_event_cb()`
3. **Inline styles override bind_style** — when using reactive style changes, do NOT set inline `style_*` attributes for those properties. Use two `bind_style` entries instead
4. **Register subjects BEFORE creating XML** that binds to them
5. **Subject string buffers MUST be static or heap** — stack-allocated buffers cause use-after-free
6. **Use design tokens** (`#card_bg`, `#space_md`) not hardcoded pixels/colors
7. **Semantic widgets** (`<text_heading>`, `<text_body>`, `<text_small>`) not raw `<lv_label>` with hardcoded fonts
8. **Component names required**: `<controls_panel name="controls_panel"/>` not `<controls_panel/>`

Exceptions (acceptable direct access): `LV_EVENT_DELETE` cleanup, widget pool recycling, chart data, animations, one-time `setup()`.

## Flex Layout

Three properties needed to center (unlike CSS):

```xml
<lv_obj flex_flow="column" height="100%"
        style_flex_main_place="center"
        style_flex_cross_place="center"
        style_flex_track_place="center">
```

- `flex_grow="1"` expands to fill; parent MUST have explicit `height="100%"`
- `width="content"` not `width="LV_SIZE_CONTENT"` (which parses as 0)
- `style_pad_column` / `style_pad_row` for flex gaps

## Events

```xml
<lv_button name="btn">
    <event_cb trigger="clicked" callback="on_btn_clicked"/>
</lv_button>
```
```cpp
// Register BEFORE XML creation
lv_xml_register_event_cb(nullptr, "on_btn_clicked", [](lv_event_t* e) {
    // handle
});
```

Triggers: `clicked`, `value_changed`, `pressed`, `released`, `long_pressed`, `focused`, `ready`.

## Gotchas

- Unknown XML attributes are **silently ignored** — typos produce no error
- `style_image_recolor` not `style_img_recolor` (full words only)
- No `zoom` attribute → use `scale_x`/`scale_y` (256 = 100%)
- No `flex_align` attribute → use `style_flex_main_place`/`style_flex_cross_place`/`style_flex_track_place`
- Dropdown options: `<lv_dropdown options="A&#10;B&#10;C"/>` (XML entity for newline)
- `lv_bar` value=0 bug: set to 1 then 0 as workaround
- XML `<subjects>` block shadows global C++ subjects — don't declare in both places
- Unregistered components are silently empty — no error, no log

## Observer Cleanup

Track observers for heap-allocated widget data:
```cpp
data->text_observer = lv_label_bind_text(label, &subject, "%s");
// In DELETE handler:
lv_observer_remove(data->text_observer);
delete data;
```

For custom widgets with owned subjects, detach children BEFORE deiniting:
```cpp
lv_obj_remove_from_subject(label, nullptr);  // removes ALL observers
lv_subject_deinit(&owned_subject);
```

## File Index

| File | Content |
|------|---------|
| [references/xml-guide.md](references/xml-guide.md) | Widgets, layouts, styles, responsive design, implementation patterns |
| [references/xml-attributes.md](references/xml-attributes.md) | Complete attribute reference for all XML widgets |
| [references/xml-components.md](references/xml-components.md) | Component API, props, custom semantic widgets |
| [references/modal-system.md](references/modal-system.md) | Modal dialog system — helpers, subclasses, XML templates |
| [references/declarative-ui-rules.md](references/declarative-ui-rules.md) | Reactive-first rules, threading safety, banned patterns |
