# XML Guide — Widgets, Layouts, Styles, Responsive Design

## Layout

### lv_obj Defaults (HelixScreen Theme)

| Property | Default | Notes |
|----------|---------|-------|
| `width` | `content` | Shrinks to content |
| `height` | `content` | Shrinks to content |
| `border_width` | `0` | No border |
| `bg_opa` | `0` | Transparent |
| `pad_all` | `0` | No padding |

### Flex Layout

```xml
<lv_obj flex_flow="row"/>           <!-- row, column, row_wrap, column_wrap, *_reverse -->
```

Three alignment properties (all three needed to center):
- `style_flex_main_place` — main axis (like justify-content)
- `style_flex_cross_place` — cross axis (like align-items)
- `style_flex_track_place` — track alignment (needed even without wrap for centering explicit-width children)

Values: `start`, `end`, `center`, `space_evenly`, `space_around`, `space_between`

```xml
<lv_obj flex_flow="row" width="100%">
    <lv_label text="Left"/>
    <lv_obj flex_grow="1"/>          <!-- Expands to fill -->
    <lv_label text="Right"/>
</lv_obj>
```

**Parent MUST have explicit height for flex_grow to work.**

### Centering

```xml
<!-- Text centering -->
<lv_label text="Centered" style_text_align="center" width="100%"/>

<!-- Flex centering (all three!) -->
<lv_obj flex_flow="column" height="100%"
        style_flex_main_place="center" style_flex_cross_place="center" style_flex_track_place="center">
    <lv_label text="Centered"/>
</lv_obj>

<!-- Single child: use align -->
<lv_obj width="100%" height="100%">
    <lv_obj align="center">Centered</lv_obj>
</lv_obj>
```

## Widget Quick Reference

### Icon Component
```xml
<icon src="home" size="lg"/>         <!-- xs:16, sm:24, md:32, lg:48, xl:64 -->
<icon src="heater" size="lg" variant="accent"/>  <!-- primary, secondary, accent, disabled, warning -->
```

### Semantic Typography (ALWAYS use these, not raw lv_label)
```xml
<text_heading text="Title"/>         <!-- 20/26/28px responsive -->
<text_body text="Content"/>          <!-- 14/18/20px responsive -->
<text_small text="Caption"/>         <!-- 12/16/18px responsive -->
```

### ui_card
Container with card styling. Don't redundantly specify `style_radius`, `style_bg_color`.
```xml
<ui_card name="my_card" width="100%" height="200">
    <text_body text="Content"/>
</ui_card>
```

### ui_button
Semantic button with variant styling. Supports `text="@subject"` or `bind_text="subject"` for reactive text.
```xml
<ui_button variant="primary" text="Save"/>
<ui_button variant="ghost" icon="settings"/>
<!-- variants: primary, secondary, ghost, destructive -->
```

### ui_markdown
Renders markdown as native LVGL widgets. Supports `bind_text` for dynamic content.
```xml
<ui_markdown bind_text="release_notes" width="100%"/>
```

### dividers
```xml
<divider_vertical height="80%"/>
<divider_horizontal width="100%"/>
```

## Styles

### Defining (NO style_ prefix inside <styles>)
```xml
<styles>
    <style name="btn" bg_color="0x2196f3" radius="8" pad_all="12"/>
</styles>
```

### Applying
```xml
<lv_button>
    <style name="btn"/>                        <!-- Default -->
    <style name="btn_pressed" selector="pressed"/>  <!-- State selector -->
</lv_button>
<!-- Inline (WITH style_ prefix) -->
<lv_button style_bg_color="0x111" style_radius="8"/>
```

### Part Selectors
Style widget parts: `main`, `indicator`, `knob`, `items`, `scrollbar`
```xml
<lv_slider style_bg_color="#333" style_bg_color:indicator="#primary" style_bg_color:knob="#fff"/>
```

### State Selectors
`default`, `pressed`, `checked`, `focused`, `disabled`. Combine: `selector="indicator:pressed"`.

## Responsive Design

- **Breakpoints** (height-based): TINY (≤390), SMALL (391-460), MEDIUM (461-550), LARGE (551-700), XLARGE (>700)
- **Spacing tokens**: `#space_xxs` through `#space_2xl` — always use tokens, never pixels
- **Fonts**: Use semantic components (`<text_heading>`, `<text_body>`, `<text_small>`)
- **Colors**: Use token names (`#card_bg`, `#primary_color`)

## Implementation Workflow

1. **Create XML layout** in `ui_xml/panel.xml`
2. **Create C++ wrapper** with `init_subjects()` (subjects + callbacks) and `create()` (calls `lv_xml_create`)
3. **Register** component in `xml_registration.cpp`
4. **Register subjects BEFORE creating XML**
5. **Update via subjects** — UI reacts automatically

```cpp
// C++ wrapper pattern
void MyPanel::init_subjects() {
    lv_subject_init_string(&status_subject, buf, NULL, sizeof(buf), "Ready");
    lv_xml_register_subject(NULL, "status", &status_subject);
    lv_xml_register_event_cb(nullptr, "on_click", [](lv_event_t* e) { /*...*/ });
}

lv_obj_t* MyPanel::create(lv_obj_t* parent) {
    return lv_xml_create(parent, "my_panel", nullptr);
}

void MyPanel::update(const char* msg) {
    lv_subject_copy_string(&status_subject, msg);
}
```

## Theme Colors (C++ API)
```cpp
lv_color_t bg = ui_theme_get_color("card_bg");       // Token lookup
lv_color_t ok = ui_theme_parse_color("#FF4444");     // Literal hex only
```

## Widget Lookup
```cpp
lv_obj_t* w = lv_obj_find_by_name(parent, "widget_name");  // ✅ Name-based
lv_obj_t* w = lv_obj_get_child(parent, 3);                  // ❌ Index-based (fragile)
```
