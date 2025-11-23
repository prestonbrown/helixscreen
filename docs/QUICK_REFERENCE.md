# LVGL 9 XML UI Quick Reference

Quick patterns and cheat sheets for daily development. For comprehensive documentation, see [LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md).

## Common Patterns

### Create XML Component with Reactive Binding

```cpp
// Header
lv_obj_t* ui_panel_xyz_create(lv_obj_t* parent);
void ui_panel_xyz_update(const char* text, int value);

// Implementation
static lv_subject_t text_subject, value_subject;
static char text_buf[128], value_buf[32];

lv_obj_t* ui_panel_xyz_create(lv_obj_t* parent) {
    // 1. Init subjects
    lv_subject_init_string(&text_subject, text_buf, NULL, sizeof(text_buf), "Initial");
    lv_subject_init_string(&value_subject, value_buf, NULL, sizeof(value_buf), "0");

    // 2. Register globally
    lv_xml_register_subject(NULL, "text_data", &text_subject);
    lv_xml_register_subject(NULL, "value_data", &value_subject);

    // 3. Create XML
    return (lv_obj_t*)lv_xml_create(parent, "panel_xyz", nullptr);
}

void ui_panel_xyz_update(const char* text, int value) {
    lv_subject_copy_string(&text_subject, text);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    lv_subject_copy_string(&value_subject, buf);
}
```

### XML Component Structure

```xml
<component>
    <view extends="lv_obj" width="100%" height="100%">
        <!-- Bound labels update automatically -->
        <lv_label bind_text="text_data" style_text_color="#text_primary"/>
        <lv_label bind_text="value_data" style_text_font="montserrat_28"/>
    </view>
</component>
```

### Component Instantiation with Names (CRITICAL)

**Always add explicit `name` attributes to component tags for findability:**

```xml
<!-- app_layout.xml -->
<lv_obj name="content_area">
  <!-- ✓ CORRECT - Explicit names on component tags -->
  <controls_panel name="controls_panel"/>
  <home_panel name="home_panel"/>
  <settings_panel name="settings_panel"/>
</lv_obj>
```

```cpp
// Now findable from C++
lv_obj_t* controls = lv_obj_find_by_name(parent, "controls_panel");
lv_obj_clear_flag(controls, LV_OBJ_FLAG_HIDDEN);
```

**Why:** Component names in `<view name="...">` definitions don't propagate to instantiation tags. Without explicit names, `lv_obj_find_by_name()` returns NULL.

## Custom Components

### Icon Component

**Custom Material Design icon widget with semantic properties for sizing and color variants.**

**Properties:**
- `src` - Material icon name (default: `"mat_home"`)
- `size` - Semantic size string: `xs`, `sm`, `md`, `lg`, `xl` (default: `"xl"`)
- `variant` - Color variant string: `primary`, `secondary`, `accent`, `disabled`, `none` (default: no recoloring)
- `color` - Custom color override (hex format: `"0xFF0000"` or `"#FF0000"`) - overrides `variant`

```xml
<!-- Basic usage with defaults (mat_home, 64px, no recolor) -->
<icon/>

<!-- Specify icon source -->
<icon src="mat_print"/>

<!-- Semantic sizes with color variants -->
<icon src="mat_heater" size="lg" variant="primary"/>
<icon src="mat_back" size="md" variant="secondary"/>
<icon src="mat_pause" size="sm" variant="disabled"/>

<!-- Custom color overrides variant -->
<icon src="mat_warning" size="lg" color="0xFFFF00"/>
<icon src="mat_error" size="md" color="#FF0000"/>
```

**Available Sizes:**
- `xs` - 16×16px @ scale 64 (UI elements)
- `sm` - 24×24px @ scale 96 (small buttons)
- `md` - 32×32px @ scale 128 (standard buttons)
- `lg` - 48×48px @ scale 192 (large UI elements)
- `xl` - 64×64px @ scale 256 (default, primary icons)

**Color Variants:**
- `primary` - Recolored with `#text_primary` (100% opacity)
- `secondary` - Recolored with `#text_secondary` (100% opacity)
- `accent` - Recolored with `#primary_color` (100% opacity)
- `disabled` - Recolored with `#text_secondary` (50% opacity)
- `none` - No recoloring (0% opacity, shows original icon colors)

**C++ Runtime API:**
```cpp
// Change icon source dynamically
ui_icon_set_source(icon_widget, "mat_wifi_strength_4");

// Change icon size at runtime
ui_icon_set_size(icon_widget, "lg");  // xs/sm/md/lg/xl

// Change color variant at runtime
ui_icon_set_variant(icon_widget, "accent");  // primary/secondary/accent/disabled/none

// Set custom color with opacity
ui_icon_set_color(icon_widget, lv_color_hex(0xFF0000), LV_OPA_COVER);
```

**Material Icon Names:**
All icons use `mat_` prefix: `mat_home`, `mat_print`, `mat_pause`, `mat_heater`, `mat_bed`, `mat_fan`, `mat_extruder`, `mat_cancel`, `mat_refresh`, `mat_back`, `mat_delete`, etc. See [material_icons.cpp](../src/material_icons.cpp) for complete list.

### ui_switch Component

**Custom switch widget extending lv_switch with semantic size presets for responsive UI design.**

**Properties:**
- `size` - Semantic size string: `tiny`, `small`, `medium`, `large` (default: LVGL defaults)
- `width` - Explicit width override (optional, overrides size preset)
- `height` - Explicit height override (optional, overrides size preset)
- `knob_pad` - Knob padding in pixels (optional, overrides size preset)
- `checked` - Initial state: `true` or `false` (default: `false`)

```xml
<!-- Basic usage with semantic size (recommended) -->
<ui_switch size="medium"/>

<!-- Different sizes -->
<ui_switch size="tiny"/>
<ui_switch size="small" checked="true"/>
<ui_switch size="medium"/>
<ui_switch size="large"/>

<!-- Progressive enhancement: size preset + selective override -->
<ui_switch size="medium" width="100"/>  <!-- Custom width, keeps medium height and knob_pad -->

<!-- Combine with standard LVGL properties -->
<ui_switch size="medium" style_bg_color="#ff0000" style_margin_right="10"/>
```

**Available Sizes (screen-size-aware):**

| Size | TINY (480×320) | SMALL (800×480) | LARGE (1280×720+) |
|------|----------------|-----------------|-------------------|
| `tiny` | 32×16px, pad=1 | 48×24px, pad=2 | 64×32px, pad=2 |
| `small` | 40×20px, pad=1 | 64×32px, pad=2 | 88×44px, pad=3 |
| `medium` | 48×24px, pad=2 | 80×40px, pad=3 | 112×56px, pad=4 |
| `large` | 56×28px, pad=2 | 88×44px, pad=3 | 128×64px, pad=4 |

### Step Progress Widget

**Reusable step-by-step progress indicator for wizards and multi-step operations.**

**Features:**
- Vertical and horizontal orientations
- Three visual states: PENDING (gray filled), ACTIVE (red filled), COMPLETED (green filled)
- Step numbers (1, 2, 3...) automatically toggle to checkmarks on completion
- Seamless 1px connector lines between steps, colored green from completed steps
- Dynamic updates via `ui_step_progress_set_current()`

**API Usage:**
```cpp
#include "ui_step_progress.h"

// Define steps
ui_step_t steps[] = {
    {"Nozzle heating", UI_STEP_STATE_COMPLETED},
    {"Prepare to retract", UI_STEP_STATE_ACTIVE},
    {"Retracting", UI_STEP_STATE_PENDING},
    {"Retract done", UI_STEP_STATE_PENDING}
};

// Create widget
lv_obj_t* progress = ui_step_progress_create(parent, steps, 4, false);  // false = vertical

// Update current step (advances to next)
ui_step_progress_set_current(progress, 2);  // Now on step 3
```

**Files:**
- `include/ui_step_progress.h` - Public API
- `src/ui_step_progress.cpp` - Implementation
- `ui_xml/step_progress_test.xml` - Test panel

## Layout Patterns

### Flex Layout (Navbar Pattern)

```xml
<view extends="lv_obj"
      flex_flow="column"
      style_flex_main_place="space_evenly"
      style_flex_cross_place="center"
      style_flex_track_place="center">

    <lv_button width="70" height="70">
        <lv_label align="center" text="#icon_name"/>
    </lv_button>
</view>
```

### Constants in globals.xml

```xml
<!-- globals.xml -->
<consts>
    <!-- Theme colors (read by C++ theme system) -->
    <color name="primary_color" value="..."/>
    <color name="secondary_color" value="..."/>
    <color name="text_primary" value="..."/>
    <color name="text_secondary" value="..."/>

    <!-- Semantic fonts (for manual widget styling) -->
    <str name="font_heading" value="montserrat_20"/>
    <str name="font_body" value="montserrat_16"/>
    <str name="font_small" value="montserrat_12"/>

    <!-- Layout constants -->
    <px name="nav_width" value="102"/>
    <px name="padding_normal" value="20"/>

    <!-- Icons -->
    <str name="icon_home" value=""/>  <!-- UTF-8 char -->
</consts>

<!-- Usage in XML -->
<lv_obj style_bg_color="#primary_color"/>
<lv_label style_text_font="#font_heading" style_text_color="#text_primary"/>
```

## Subject Types & Bindings

### Subject Initialization

```cpp
// String
char buf[128];
lv_subject_init_string(&subj, buf, NULL, sizeof(buf), "init");
lv_subject_copy_string(&subj, "new text");

// Integer
lv_subject_init_int(&subj, 0);
lv_subject_set_int(&subj, 42);

// Color
lv_subject_init_color(&subj, lv_color_hex(0xFF0000));
lv_subject_set_color(&subj, lv_color_hex(0x00FF00));

// Observer callback for image recoloring
static void image_color_observer(lv_observer_t* obs, lv_subject_t* subj) {
    lv_obj_t* image = (lv_obj_t*)lv_observer_get_target(obs);
    lv_color_t color = lv_subject_get_color(subj);
    lv_obj_set_style_img_recolor(image, color, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(image, 255, LV_PART_MAIN);
}

// Register observer
lv_subject_add_observer_obj(&color_subj, image_color_observer, image_widget, NULL);
```

### XML Binding Reference

| Widget | Binding | Subject Type |
|--------|---------|--------------|
| `lv_label` | `bind_text="name"` | String |
| `lv_slider` | `bind_value="name"` | Integer |
| `lv_arc` | `bind_value="name"` | Integer |
| `lv_dropdown` | `bind_value="name"` | Integer |

## Registration Order (CRITICAL)

```cpp
// ALWAYS follow this order:
lv_xml_register_font(...);                    // 1. Fonts
lv_xml_register_image(...);                   // 2. Images
lv_xml_component_register_from_file(...);     // 3. Components (globals first!)
lv_subject_init_string(...);                  // 4. Init subjects
lv_xml_register_subject(...);                 // 5. Register subjects
lv_xml_create(...);                           // 6. Create UI
```

## Style Properties Cheat Sheet

```xml
<!-- Layout -->
width="100" height="200" x="50" y="100"
flex_flow="row|column|row_wrap|column_wrap"
flex_grow="1"
align="center|top_left|bottom_right|left_mid|right_mid|..."

<!-- Colors & Opacity -->
style_bg_color="#hexcolor"
style_bg_opa="0%|50%|100%"
style_text_color="#hexcolor"

<!-- Spacing -->
style_pad_all="10"
style_pad_left="10" style_pad_right="10"
style_margin_top="10" style_margin_bottom="10"

<!-- Borders & Radius -->
style_border_width="2"
style_radius="8"
style_shadow_width="10"

<!-- Flex Alignment -->
style_flex_main_place="start|end|center|space_between|space_evenly|space_around"
style_flex_cross_place="start|end|center"
style_flex_track_place="start|end|center|space_between|space_evenly|space_around"
```

## Icon & Image Assets

### Material Design Icons (Navigation)

**Automated Workflow using `material_icons.py`:**

```bash
# List all registered Material Design icons
make material-icons-list

# Add new icons from Material Design Icons (download + convert + register)
make material-icons-add ICONS="wifi-strength-1 wifi-strength-2 lock"

# Convert existing SVG files to LVGL C arrays
make material-icons-convert SVGS="icon1.svg icon2.svg"
```

**What the automated workflow does:**
1. Downloads SVG from `google/material-design-icons` GitHub repo
2. Converts SVG → PNG (64x64, alpha preserved)
3. Converts PNG → LVGL C array (RGB565A8 format)
4. Adds declarations to `include/material_icons.h`
5. Registers in `src/material_icons.cpp`

**Manual Conversion (if needed):**

```bash
# 1. SVG to PNG with ImageMagick
# CRITICAL: Use -density 300 for sharp rendering when upscaling
magick -density 300 -background none icon.svg \
  -resize 64x64 -colorspace gray \
  -channel RGB -evaluate set 0 +channel \
  icon.png

# 2. PNG to LVGL 9 C array with LVGLImage.py
.venv/bin/python3 scripts/LVGLImage.py icon.png \
  --ofmt C --cf RGB565A8 -o assets/images/material --name icon
```

**Icon Format Requirements:**
- **RGB565A8 format** - 16-bit RGB + 8-bit alpha, works with `lv_obj_set_style_img_recolor()`
- **64x64 resolution** - Standard size for all Material Design icons
- **High-density rendering** - Use `-density 300` to avoid fuzzy upscaling from 24x24 SVGs
- **Black base color** - Icons must be black (graya(0,*)) for recoloring to work
- **Alpha channel preserved** - Smooth anti-aliased edges required for quality rendering

**Registration Pattern:**
```cpp
// Header (material_icons.h)
LV_IMG_DECLARE(home);
LV_IMG_DECLARE(print);

// Registration (material_icons.cpp)
void material_icons_register() {
    lv_xml_register_image(NULL, "mat_home", &home);
    lv_xml_register_image(NULL, "mat_print", &print);
}

// Usage in XML
<lv_image src="mat_home" align="center"/>
```

**Scaling & Recoloring in XML:**
```xml
<!-- XML scaling (256 = 100%) - PREFERRED METHOD -->
<lv_image src="mat_home"
          scale_x="128" scale_y="128"              <!-- 50% size (32px) -->
          style_image_recolor="#primary_color"     <!-- MUST use 'image' not 'img' -->
          style_image_recolor_opa="255"/>

<lv_image src="mat_home"
          scale_x="256" scale_y="256"              <!-- 100% size (64px) -->
          style_image_recolor="#text_secondary"
          style_image_recolor_opa="255"/>
```

**⚠️ CRITICAL Gotchas:**
- ❌ **zoom** - Doesn't exist in LVGL 9!
- ✅ **scale_x, scale_y** - Use these (256 = 100%)
- ❌ **style_img_recolor** - Parser ignores abbreviated 'img'!
- ✅ **style_image_recolor** - Must use full word 'image'

**C++ Scaling & Recoloring:**
```cpp
// Responsive scaling
lv_image_set_scale_x(icon, 128);  // 50% (32px)
lv_image_set_scale_y(icon, 128);
lv_image_set_scale_x(icon, 256);  // 100% (64px)
lv_image_set_scale_y(icon, 256);

// Dynamic recoloring (active = red, inactive = gray)
lv_obj_set_style_img_recolor(icon, UI_COLOR_PRIMARY, LV_PART_MAIN);
lv_obj_set_style_img_recolor_opa(icon, 255, LV_PART_MAIN);
```

### FontAwesome Icons (UI Content)

Generate icon constants for use in `globals.xml`:

```bash
# Regenerate icon constants after adding new icons
python3 scripts/generate-icon-consts.py

# This updates ui_xml/globals.xml with UTF-8 byte sequences
```

**Adding New FontAwesome Icons:**
1. Edit `package.json` to add Unicode codepoint to font range
2. Run `npm run convert-font-XX` (XX = 16/32/48/64)
3. Run `python3 scripts/generate-icon-consts.py`
4. Rebuild: `make`

**Usage in XML:**
```xml
<!-- globals.xml defines constants -->
<str name="icon_temperature" value=""/>  <!-- UTF-8 bytes -->

<!-- Use in components -->
<lv_label text="#icon_temperature" style_text_font="fa_icons_48"/>
```

**When to Use Which:**
- **Material Design Images**: Navigation, primary actions, needs recoloring
- **FontAwesome Fonts**: UI content, inline icons, text-based rendering

## Common Gotchas

❌ **Don't:** Use stack buffers for string subjects
```cpp
char buf[128];  // Stack - WRONG!
lv_subject_init_string(&subj, buf, ...);
```

✅ **Do:** Use static or heap buffers
```cpp
static char buf[128];  // Static - CORRECT
lv_subject_init_string(&subj, buf, ...);
```

---

❌ **Don't:** Use `flex_align` attribute in XML
```xml
<view flex_align="space_evenly center">
```

✅ **Do:** Use `style_flex_*_place` properties
```xml
<view style_flex_main_place="space_evenly" style_flex_cross_place="center">
```

---

❌ **Don't:** Register subjects after creating XML
```cpp
lv_xml_create(...);
lv_xml_register_subject(...);  // Too late!
```

✅ **Do:** Register subjects before XML creation
```cpp
lv_xml_register_subject(...);
lv_xml_create(...);
```

---

❌ **Don't:** Use abbreviated 'img' in style properties
```xml
<lv_image style_img_recolor="#ff0000"/>  <!-- Parser ignores this -->
```

✅ **Do:** Use full word 'image'
```xml
<lv_image style_image_recolor="#ff0000"/>  <!-- This works -->
```

---

## See Also

- **[LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md)** - Complete XML syntax and patterns
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System design and reactive patterns
- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Build system and daily workflow
- **[DOXYGEN_GUIDE.md](DOXYGEN_GUIDE.md)** - API documentation standards
