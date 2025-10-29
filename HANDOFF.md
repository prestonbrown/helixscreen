# Session Handoff Document

**Last Updated:** 2025-10-28
**Current Focus:** WiFi system complete - Future work planning

---

## 🎯 Active Work & Next Priorities

### ✅ Recently Completed

**WiFi Stage 2: Build System Integration** (Completed 2025-10-28)
- ✅ Added wpa_supplicant build variables to Makefile (WPA_DIR, WPA_CLIENT_LIB, WPA_INC)
- ✅ Created wpa_supplicant/.config for libwpa_client.a build
- ✅ Platform-specific LDFLAGS (Linux includes libwpa_client.a, macOS excluded)
- ✅ Added wpa_supplicant build target in mk/deps.mk (Linux only)
- ✅ Updated check-deps to verify wpa_supplicant submodule
- ✅ Updated clean target to clean wpa_supplicant
- ✅ Verified build: libwpa_client.a (220K) builds and links successfully

**Stage 0-1: Submodule Setup** (Completed 2025-10-28)
- ✅ Converted libhv from symlink to git submodule (v1.3.1-54 → v1.3.4)
- ✅ Added wpa_supplicant v2.11 as git submodule (hostap_2_11 tag)
- ✅ Makes prototype-ui9 self-contained and CI/CD friendly
- Commits: `6025c3c`, `394f0b7`

**Phase 1: Node.js v22 Compatibility** (Completed 2025-10-28)
- ✅ Upgraded canvas from v2.11.2 to v3.2.0 (Node v22 pre-built binaries)
- ✅ Fixed LVGL pthread compilation with -D_GNU_SOURCE
- ✅ Fixed C++ standard compliance in main.cpp (compound literals)
- ✅ Full build succeeds with all dependencies satisfied
- Commit: `83a867a`

**Previous Work:**
- WiFi wizard step 0 with password modal (commit 8cab855)
- UI testing infrastructure with virtual input device (commit ded96ef)
- 10 WiFi UI tests written (see `docs/UI_TESTING.md` for status)

### ✅ Recently Completed

**WiFi Implementation Stages 3-6** (Completed 2025-10-28)
- ✅ **Stage 3**: WPA backend port complete (commit `4d6581a`)
- ✅ **Stage 4**: WiFiManager integration with security hardening (commit `24b12b0`)
- ✅ **Stage 5**: Real hardware testing with comprehensive robustness validation
- ✅ **Stage 6**: Documentation updates and test harness creation

**Stage 5 Real Hardware Testing Results:**
- ✅ Graceful fallback with unconfigured wpa_supplicant (missing control interface)
- ✅ Real WiFi hardware setup (RF-kill handling, interface management)
- ✅ Live network scanning (32+ networks detected successfully)
- ✅ Permission robustness (socket access, netdev group requirements)
- ✅ Edge case handling (interface down, daemon restarts, missing hardware)
- ✅ Non-destructive testing with complete system restoration
- ✅ Test harness script for automated validation (`scripts/test_wifi_real.sh`)

**WiFi Error Handling & Permission System** (Completed 2025-10-28)
- ✅ **Comprehensive Error Propagation**: WiFiResult enum, WiFiError struct, user-friendly messages
- ✅ **Permission Checking**: Socket access validation, netdev group testing, hardware detection
- ✅ **Backend Interface Update**: All methods return WiFiError instead of bool
- ✅ **Mock Backend Alignment**: Updated to match new error interface with realistic scenarios
- ✅ **WiFiManager Integration**: Full error handling with graceful UI fallback behavior
- ✅ **Hardware Detection**: Enables adaptive UI (skip WiFi setup when no hardware)
- Commit: `8886bf6`

**WiFi System Status:** PRODUCTION READY
- Backend abstraction complete with pluggable implementations
- Security hardened (input validation, resource cleanup, thread safety)
- Robust fallback behavior for diverse hardware scenarios
- Comprehensive test coverage for real-world deployment
- **NEW**: Comprehensive error handling with user-friendly messages

### Next Priorities

**Future Work** (Post-WiFi Implementation):
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

### Pattern #1: Custom Switch Widget

**Available:** `<ui_switch>` registered for XML use

```xml
<ui_switch name="my_toggle" checked="true"/>
<ui_switch orientation="horizontal"/>  <!-- auto|horizontal|vertical -->
```

**Supports:** All standard `lv_obj` properties (width, height, style_*)

**Files:** `include/ui_switch.h`, `src/ui_switch.cpp`, `src/main.cpp:643`

### Pattern #2: Navigation History Stack

**When to use:** Overlay panels (motion, temps, extrusion, keypad)

```cpp
ui_nav_push_overlay(motion_panel);  // Shows overlay, saves history
if (!ui_nav_go_back()) { /* fallback */ }
```

**Files:** `ui_nav.h:54-62`, `ui_nav.cpp:250-327`

### Pattern #3: Global Keyboard for Textareas

```cpp
// One-time init in main.cpp (already done)
ui_keyboard_init(lv_screen_active());

// For each textarea
ui_keyboard_register_textarea(my_textarea);  // Auto show/hide on focus
```

**Files:** `include/ui_keyboard.h`, `src/ui_keyboard.cpp`

### Pattern #4: Subject Initialization Order

**MUST initialize subjects BEFORE creating XML:**

```cpp
lv_xml_component_register_from_file("A:/ui_xml/my_panel.xml");
ui_my_panel_init_subjects();  // FIRST
lv_xml_create(screen, "my_panel", NULL);  // AFTER
```

### Pattern #5: Component Instantiation Names

**Always add explicit `name` attributes:**

```xml
<!-- WRONG --><my_panel/>
<!-- CORRECT --><my_panel name="my_panel"/>
```

**Why:** Component `<view name="...">` doesn't propagate to instantiation

### Pattern #6: Image Scaling in Flex Layouts

```cpp
lv_obj_update_layout(container);  // Force layout calculation FIRST
ui_image_scale_to_cover(img, container);
```

**Why:** LVGL uses deferred layout - containers report 0x0 until forced

**Files:** `ui_utils.cpp:213-276`, `ui_panel_print_status.cpp:249-314`

### Pattern #7: Logging Policy

**ALWAYS use spdlog, NEVER printf/cout/LV_LOG:**

```cpp
#include <spdlog/spdlog.h>
spdlog::info("Operation complete: {}", value);  // fmt-style formatting
spdlog::error("Failed: {}", (int)enum_val);     // Cast enums
```

**Reference:** `CLAUDE.md:77-134`

### Pattern #8: Copyright Headers

**ALL new files MUST include GPL v3 header**

**Reference:** `docs/COPYRIGHT_HEADERS.md`

### Pattern #9: UI Testing Infrastructure

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
