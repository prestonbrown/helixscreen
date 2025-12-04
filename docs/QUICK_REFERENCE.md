# LVGL 9 XML UI Quick Reference

Quick patterns and cheat sheets. For comprehensive docs, see [LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md).

---

## Class Patterns

**Panel:** See `include/ui_panel_motion.h` + `src/ui_panel_motion.cpp` for canonical example.

```cpp
class ExamplePanel : public PanelBase {
public:
    explicit ExamplePanel(lv_obj_t* parent);
    void show() override;
    void hide() override;
    lv_obj_t* get_root() const override { return root_; }
private:
    void init_subjects();  // Call BEFORE lv_xml_create()
    lv_obj_t* root_ = nullptr;
    lv_subject_t my_subject_{};
    char buf_[128]{};
};
```

**Manager:** Singleton with pluggable backend. See `include/network_manager.h`.

**Modal:** Backdrop + dialog. See `src/ui_wizard_*.cpp` for examples.

---

## Component Names (CRITICAL)

**Always add explicit `name` on component tags** - internal view names don't propagate:

```xml
<controls_panel name="controls_panel"/>  <!-- ✅ Findable -->
<controls_panel/>                        <!-- ❌ lv_obj_find_by_name returns NULL -->
```

---

## Icon Component

Font-based MDI icons (~50KB total vs ~4.6MB for images).

```xml
<icon src="home" size="lg" variant="accent"/>
<icon src="wifi" size="sm" color="#warning_color"/>
```

| Property | Values |
|----------|--------|
| `src` | Icon name (see `include/ui_icon_codepoints.h`) |
| `size` | `xs`=16px, `sm`=24px, `md`=32px, `lg`=48px, `xl`=64px (default) |
| `variant` | `primary`, `secondary`, `accent`, `disabled`, `warning` |
| `color` | Hex override (e.g., `"#FF0000"`) - overrides variant |

**C++ API:**
```cpp
ui_icon_set_source(icon, "wifi_strength_4");
ui_icon_set_size(icon, "lg");
ui_icon_set_variant(icon, "accent");
```

**Adding icons:** Find at [Pictogrammers MDI](https://pictogrammers.com/library/mdi/) → add to `ui_icon_codepoints.h` (sorted!) → add codepoint to `scripts/regen_mdi_fonts.sh` → run `./scripts/regen_mdi_fonts.sh`

---

## ui_switch Component

Screen-responsive switch with semantic sizes:

```xml
<ui_switch size="medium" checked="true"/>
```

| Size | SMALL screen | MEDIUM screen | LARGE screen |
|------|--------------|---------------|--------------|
| `tiny` | 32×16px | 48×24px | 64×32px |
| `small` | 40×20px | 64×32px | 88×44px |
| `medium` | 48×24px | 80×40px | 112×56px |
| `large` | 56×28px | 88×44px | 128×64px |

---

## Step Progress Widget

```cpp
#include "ui_step_progress.h"

ui_step_t steps[] = {
    {"Step 1", UI_STEP_STATE_COMPLETED},
    {"Step 2", UI_STEP_STATE_ACTIVE},
    {"Step 3", UI_STEP_STATE_PENDING}
};
lv_obj_t* progress = ui_step_progress_create(parent, steps, 3, false);  // false=vertical
ui_step_progress_set_current(progress, 2);  // Advance to step 3
```

---

## Responsive Design Tokens

**Screen breakpoints:** SMALL (≤480px), MEDIUM (481-800px), LARGE (>800px)

### Spacing (`#space_*`)

| Token | SMALL | MEDIUM | LARGE |
|-------|-------|--------|-------|
| `#space_xxs` | 2px | 3px | 4px |
| `#space_xs` | 4px | 5px | 6px |
| `#space_sm` | 6px | 7px | 8px |
| `#space_md` | 8px | 10px | 12px |
| `#space_lg` | 12px | 16px | 20px |
| `#space_xl` | 16px | 20px | 24px |

### Typography (`#font_*` or semantic components)

| Token | Component | SMALL | MEDIUM | LARGE |
|-------|-----------|-------|--------|-------|
| `#font_heading` | `<text_heading>` | montserrat_20 | montserrat_26 | montserrat_28 |
| `#font_body` | `<text_body>` | montserrat_14 | montserrat_18 | montserrat_20 |
| `#font_small` | `<text_small>` | montserrat_12 | montserrat_16 | montserrat_18 |

### Theme Colors

Define `mycolor_light` + `mycolor_dark` in globals.xml → use as `#mycolor`:

```xml
<lv_obj style_bg_color="#card_bg"/>  <!-- Auto-selects light/dark variant -->
```

### Adding Tokens

Add all variants to globals.xml - no C++ changes needed:
- **Colors:** `mycolor_light` + `mycolor_dark`
- **Spacing:** `myspace_small` + `myspace_medium` + `myspace_large`
- **Fonts:** `myfont_small` + `myfont_medium` + `myfont_large`

⚠️ Don't define base names (`space_lg`) in globals.xml - only variants.

---

## Subjects & Bindings

### Initialization (BEFORE lv_xml_create!)

```cpp
// String
static char buf[128];
lv_subject_init_string(&subj, buf, NULL, sizeof(buf), "init");
lv_xml_register_subject(nullptr, "my_text", &subj);

// Integer
lv_subject_init_int(&subj, 0);

// Color
lv_subject_init_color(&subj, lv_color_hex(0xFF0000));
```

### XML Bindings

| Widget | Binding | Subject Type |
|--------|---------|--------------|
| `lv_label` | `bind_text="name"` | String |
| `lv_slider` | `bind_value="name"` | Integer |
| `lv_arc` | `bind_value="name"` | Integer |

---

## Registration Order (CRITICAL)

```cpp
lv_xml_register_font(...);                    // 1. Fonts
lv_xml_register_image(...);                   // 2. Images
lv_xml_component_register_from_file(...);     // 3. Components (globals first!)
lv_subject_init_*(...);                       // 4. Init subjects
lv_xml_register_subject(...);                 // 5. Register subjects
lv_xml_create(...);                           // 6. Create UI
```

---

## Style Properties

⚠️ In `<styles>`: bare names (`bg_color`). On widgets: `style_` prefix (`style_bg_color`).

```xml
<styles>
    <style name="my_style" bg_color="#ff0000"/>  <!-- NO prefix -->
</styles>
<lv_obj style_bg_color="#card_bg"/>              <!-- WITH prefix -->

<!-- Part selectors -->
<lv_spinner style_arc_width:indicator="3"/>
<lv_slider style_bg_color:knob="#ffffff"/>
```

**Parts:** `main`, `indicator`, `knob`, `items`, `selected`, `cursor`, `scrollbar`

**Common properties:**
```xml
width="100" height="200" align="center"
flex_flow="row|column" flex_grow="1"
style_bg_color="#hex" style_bg_opa="50%"
style_pad_all="#space_md" style_radius="8"
style_flex_main_place="space_evenly"
style_flex_cross_place="center"
```

---

## Common Gotchas

| ❌ Wrong | ✅ Correct |
|----------|-----------|
| `char buf[128];` (stack) | `static char buf[128];` (static/heap) |
| `flex_align="..."` | `style_flex_main_place` + `style_flex_cross_place` |
| Register subjects after `lv_xml_create` | Register subjects BEFORE |
| `style_img_recolor` | `style_image_recolor` (full word) |

---

## See Also

- **[LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md)** - Complete XML reference
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System design
- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Build & workflow
