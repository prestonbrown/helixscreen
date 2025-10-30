# Session Handoff Document

**Last Updated:** 2025-10-29
**Current Focus:** Wizard container foundation ready for screen implementation

---

## 🎯 Active Work & Next Priorities

### ✅ Recently Completed

**Wizard Container Redesign** (Completed 2025-10-29)
- ✅ Clean three-region layout (header/content/footer) using runtime constants
- ✅ Responsive design: TINY (480x320), SMALL (800x480), LARGE (1280x720+)
- ✅ Single XML template adapts via C++ screen detection
- ✅ Horizontal header layout (progress + title) saves vertical space
- ✅ Reactive navigation with conditional back button visibility
- ✅ Subject-driven text updates (Next → Finish on last step)
- ✅ Simplified API: init_subjects → register_constants → create → navigate
- Commit: `b915469`

**WiFi System** (Production Ready 2025-10-28)
- Backend abstraction: macOS (CoreWLAN) + Linux (wpa_supplicant) + Mock
- Security hardened, comprehensive error handling, real hardware validated
- See git history for detailed implementation (commits `6025c3c` through `8886bf6`)

### Next Priorities

1. **Wizard Screens Implementation**
   - WiFi setup screen (SSID list + password input)
   - Moonraker connection screen (host/port/API key)
   - Printer identification (type selection from 32 presets)
   - Hardware configuration (bed/hotend/fan/LED selection)
   - Summary/confirmation screen

2. **Wizard Flow Logic**
   - Screen switching mechanism (load into wizard_content container)
   - State persistence between steps
   - Validation and error handling
   - Completion callback integration

3. **Future Enhancements**
   - **Phase 2**: Node.js/npm optional dependencies (make font generation optional)
   - **UI Testing**: Fix multiple fixture segfaults (1/10 tests passing)
   - **Additional Features**: As defined in `docs/ROADMAP.md`

---

## 📋 Critical Architecture Patterns (Essential How-To Reference)

### Pattern #0: Flex Layout Height Requirements 🚨 CRITICAL

**When using `flex_grow` on children, parent MUST have explicit height:**

```xml
<!-- BROKEN: Parent has no height -->
<lv_obj flex_flow="row">
    <lv_obj flex_grow="3">Left (30%)</lv_obj>
    <lv_obj flex_grow="7">Right (70%)</lv_obj>
</lv_obj>
<!-- Result: Columns collapse to 0 height -->

<!-- CORRECT: Two-column pattern (30/70 split) -->
<view height="100%" flex_flow="column">
    <lv_obj width="100%" flex_grow="1" flex_flow="column">
        <lv_obj width="100%" flex_grow="1" flex_flow="row">
            <!-- BOTH columns MUST have height="100%" -->
            <lv_obj flex_grow="3" height="100%"
                    flex_flow="column" scrollable="true" scroll_dir="VER">
                <lv_obj height="100">Card 1</lv_obj>
                <lv_obj height="100">Card 2</lv_obj>
            </lv_obj>
            <lv_obj flex_grow="7" height="100%"
                    scrollable="true" scroll_dir="VER">
                <!-- Content -->
            </lv_obj>
        </lv_obj>
    </lv_obj>
</view>
```

**Critical Checks:**
1. Parent has explicit height (`height="300"`, `height="100%"`, or `flex_grow="1"`)
2. ALL columns have `height="100%"` (row height = tallest child)
3. Every level has sizing (wrapper → row → columns)
4. Cards use fixed heights (`height="100"`), NOT `LV_SIZE_CONTENT` in nested flex

**Diagnostic:** Add `style_bg_color="#ff0000"` to visualize bounds

**Reference:** `docs/LVGL9_XML_GUIDE.md:634-716`, `.claude/agents/widget-maker.md:107-149`, `.claude/agents/ui-reviewer.md:101-152`

### Pattern #1: Runtime Constants for Responsive Design

**Use case:** Single XML template that adapts to different screen sizes

```cpp
// C++ - Detect screen size and override constants BEFORE creating XML
int width = lv_display_get_horizontal_resolution(lv_display_get_default());
lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");

if (width < 600) {  // TINY
    lv_xml_register_const(scope, "wizard_padding", "6");
    lv_xml_register_const(scope, "wizard_gap", "4");
} else if (width < 900) {  // SMALL
    lv_xml_register_const(scope, "wizard_padding", "12");
    lv_xml_register_const(scope, "wizard_gap", "8");
} else {  // LARGE
    lv_xml_register_const(scope, "wizard_padding", "20");
    lv_xml_register_const(scope, "wizard_gap", "12");
}

// XML - Uses runtime-modified constants
<lv_obj style_pad_all="#wizard_padding" style_pad_column="#wizard_gap">
```

**Why:** One XML template adapts to any screen size without duplication or C++ layout manipulation

**Files:** `ui_wizard.cpp:71-124`, `wizard_container.xml`, `globals.xml:119-125`

### Pattern #2: Custom Switch Widget

**Available:** `<ui_switch>` registered for XML use

```xml
<ui_switch name="my_toggle" checked="true"/>
<ui_switch orientation="horizontal"/>  <!-- auto|horizontal|vertical -->
```

**Supports:** All standard `lv_obj` properties (width, height, style_*)

**Files:** `include/ui_switch.h`, `src/ui_switch.cpp`, `src/main.cpp:643`

### Pattern #3: Navigation History Stack

**When to use:** Overlay panels (motion, temps, extrusion, keypad)

```cpp
ui_nav_push_overlay(motion_panel);  // Shows overlay, saves history
if (!ui_nav_go_back()) { /* fallback */ }
```

**Files:** `ui_nav.h:54-62`, `ui_nav.cpp:250-327`

### Pattern #4: Global Keyboard for Textareas

```cpp
// One-time init in main.cpp (already done)
ui_keyboard_init(lv_screen_active());

// For each textarea
ui_keyboard_register_textarea(my_textarea);  // Auto show/hide on focus
```

**Files:** `include/ui_keyboard.h`, `src/ui_keyboard.cpp`

### Pattern #5: Subject Initialization Order

**MUST initialize subjects BEFORE creating XML:**

```cpp
lv_xml_register_component_from_file("A:/ui_xml/my_panel.xml");
ui_my_panel_init_subjects();  // FIRST
lv_xml_create(screen, "my_panel", NULL);  // AFTER
```

### Pattern #6: Component Instantiation Names

**Always add explicit `name` attributes:**

```xml
<!-- WRONG --><my_panel/>
<!-- CORRECT --><my_panel name="my_panel"/>
```

**Why:** Component `<view name="...">` doesn't propagate to instantiation

### Pattern #7: Image Scaling in Flex Layouts

```cpp
lv_obj_update_layout(container);  // Force layout calculation FIRST
ui_image_scale_to_cover(img, container);
```

**Why:** LVGL uses deferred layout - containers report 0x0 until forced

**Files:** `ui_utils.cpp:213-276`, `ui_panel_print_status.cpp:249-314`

### Pattern #8: Logging Policy

**ALWAYS use spdlog, NEVER printf/cout/LV_LOG:**

```cpp
#include <spdlog/spdlog.h>
spdlog::info("Operation complete: {}", value);  // fmt-style formatting
spdlog::error("Failed: {}", (int)enum_val);     // Cast enums
```

**Reference:** `CLAUDE.md:77-134`

### Pattern #9: Copyright Headers

**ALL new files MUST include GPL v3 header**

**Reference:** `docs/COPYRIGHT_HEADERS.md`

### Pattern #10: UI Testing Infrastructure

**See `docs/UI_TESTING.md` for complete UI testing guide**

**Quick reference:**
- `UITest::click(widget)` - Simulate touch at widget center
- `UITest::find_by_name(parent, "name")` - Widget lookup
- `UITest::wait_until(condition, timeout)` - Async condition wait
- Run tests: `./build/bin/run_tests "[tag]"`

**Files:** `tests/ui_test_utils.h/cpp`, `tests/unit/test_wizard_wifi_ui.cpp`

---

## 🔧 Known Issues & Gotchas

### Node.js/npm Dependency for Icon Generation 📦

**Current State:** canvas 3.2.0 requires Node.js v22 compatibility

**Dependencies:**
- Node.js v22.20.0+ (for canvas pre-built binaries)
- npm packages: lv_font_conv, lv_img_conv
- Canvas native dependencies: cairo, pango, libpng, libjpeg, librsvg

**When Required:** Only when regenerating fonts/icons (`make generate-fonts`, `make material-icons-convert`)

**Phase 2 TODO:** Make npm optional for regular builds (check if fonts exist before requiring lv_font_conv)

**Reference:** `package.json`, `mk/fonts.mk`, Phase 1 commit `83a867a`

### UI Testing Known Issues 🐛

**See `docs/UI_TESTING.md` for comprehensive testing documentation**

**Critical Issues:**
1. Multiple fixture instances cause segfaults (only 1/10 WiFi tests passing)
2. Virtual input events don't trigger ui_switch VALUE_CHANGED events

**Status:** UI testing is a deferred project - documented for future work

### LVGL 9 XML Roller Options ⚠️ WORKAROUND

**Problem:** LVGL 9 XML roller parser fails with `options="'item1\nitem2' normal"` syntax

**Workaround:** Set roller options programmatically in C++:
```cpp
lv_roller_set_options(roller, "Item 1\nItem 2\nItem 3", LV_ROLLER_MODE_NORMAL);
```

**Status:** Applied to wizard step 3 printer selection (32 printer types)

**Files:** `src/ui_wizard.cpp:352-387`

### LVGL 9 XML Flag Syntax ✅ FIXED

**NEVER use `flag_` prefix:**
- ❌ `flag_hidden="true"` → ✅ `hidden="true"`
- ❌ `flag_clickable="true"` → ✅ `clickable="true"`

**Status:** All XML files fixed (2025-10-24)

### LV_SIZE_CONTENT in Nested Flex

**Problem:** Evaluates to 0 before `lv_obj_update_layout()` is called

**Solutions:**
1. Call `lv_obj_update_layout()` after creation (timing sensitive)
2. Use explicit pixel dimensions (recommended)
3. Use `style_min_height`/`style_min_width` for cards

**Reference:** `docs/LVGL9_XML_GUIDE.md:705-708`

---

**Rule:** When work is complete, REMOVE it from HANDOFF immediately. Keep this document lean and current.
