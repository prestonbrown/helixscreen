# LVGL 9 XML UI System - Complete Guide

This comprehensive guide covers LVGL 9's declarative XML UI system with reactive data binding, based on LVGL 9.4 documentation and practical experience building the HelixScreen UI prototype.

**Last Updated:** 2025-10-28

---

## Table of Contents

1. [Overview & Architecture](#overview--architecture)
2. [Project Structure](#project-structure)
3. [Core Concepts](#core-concepts)
4. [Layouts & Positioning](#layouts--positioning)
5. [Common UI Patterns](#common-ui-patterns)
6. [Responsive Design Patterns](#responsive-design-patterns)
7. [Styles & Theming](#styles--theming)
8. [Event Handling](#event-handling)
9. [Implementation Guide](#implementation-guide)
10. [Best Practices](#best-practices)
11. [Troubleshooting](#troubleshooting)
12. [Resources](#resources)

---

## Overview & Architecture

LVGL 9's XML system enables declarative UI development with reactive data binding through the Subject-Observer pattern. This approach separates UI layout (XML) from business logic (C++), similar to modern web frameworks like React or Vue.

### Architecture Diagram

```
┌─────────────────┐
│  XML Component  │ ← Declarative UI layout
│  (home_panel)   │
└────────┬────────┘
         │ bind_text="subject_name"
         ↓
┌─────────────────┐
│    Subjects     │ ← Reactive data (strings, ints, colors)
│  (status_text)  │
└────────┬────────┘
         │ lv_subject_copy_string()
         ↓
┌─────────────────┐
│  C++ Wrapper    │ ← Business logic & state updates
│ (ui_panel_*.cpp)│
└─────────────────┘
```

### Key Benefits

**XML Approach:**
- Declarative and concise
- Separation of concerns (layout vs. logic)
- Reactive data binding (automatic UI updates)
- Rapid iteration (no recompile for layout changes)
- Designer-friendly

**vs. Traditional C++ Approach:**
- Less verbose (3 lines XML vs. 15 lines C++)
- No manual widget management
- Type-safe at runtime through subject system
- Theme-able with global constants

### ⚠️ CRITICAL: Reactive Data Binding is Mandatory

**ALL UI updates MUST use reactive data binding. Direct widget manipulation from C++ is an anti-pattern.**

```xml
<!-- ✅ CORRECT - Reactive binding in XML -->
<lv_label bind_text="status_message"/>
<lv_button>
  <lv_obj-bind_flag_if_eq subject="connection_ready" flag="clickable" ref_value="1"/>
</lv_button>
```

```cpp
// ✅ CORRECT - Update subjects in C++
lv_subject_set_string(&status_message, "Connected");
lv_subject_set_int(&connection_ready, 1);
// UI updates automatically - zero widget manipulation
```

```cpp
// ❌ WRONG - Direct widget manipulation (DO NOT DO THIS)
lv_obj_t* label = lv_obj_find_by_name(screen, "status_label");
lv_label_set_text(label, "Connected");  // ANTI-PATTERN
```

**Why reactive bindings are mandatory:**
- ✅ Complete separation of UI and logic
- ✅ UI automatically stays synchronized with state
- ✅ Testable without UI present
- ✅ Multiple widgets can react to same data change
- ❌ Direct manipulation breaks on XML layout changes
- ❌ Creates tight coupling between C++ and widget names

**See ARCHITECTURE.md "Reactive-First Principle" for complete guide.**

---

## Project Structure

### Standard LVGL Editor Project Layout

```
project_name/
├── project.xml          # Project config (targets, display sizes)
├── globals.xml          # Global resources (fonts, images, subjects, consts)
├── screens/             # Screen XML files (<screen> tag)
├── components/          # Reusable components (<component> tag)
├── widgets/             # Custom widget definitions
├── ui_project_name.c    # User code (calls init_gen)
└── ui_project_name_gen.c # Generated code (auto-created by LVGL Editor)
```

### HelixScreen Prototype Structure

```
prototype-ui9/
├── ui_xml/
│   ├── globals.xml            # Theme constants + auto-generated icons
│   ├── app_layout.xml         # Root: navbar + content area
│   ├── navigation_bar.xml     # 5-button vertical navigation
│   └── *_panel.xml            # Panel components (home, controls, etc.)
├── src/
│   ├── main.cpp               # Entry point, initialization
│   ├── ui_nav.cpp             # Navigation system
│   └── ui_panel_*.cpp         # Panel logic with subject management
├── include/
│   ├── ui_nav.h
│   ├── ui_panel_*.h
│   └── ui_fonts.h             # FontAwesome icon definitions
├── assets/
│   ├── fonts/                 # Custom fonts (FontAwesome 6)
│   └── images/                # UI images (printer, filament spool)
└── docs/
    └── LVGL9_XML_GUIDE.md     # This file
```

---

## Core Concepts

### 1. XML Components

Components are reusable UI pieces defined with the `<component>` tag.

#### Basic Structure

```xml
<component>
    <!-- Optional: Component API (properties passed from parent) -->
    <api>
        <!-- Simple properties -->
        <prop name="text" type="string" default="Click me"/>
        <prop name="enabled" type="bool" default="true"/>
        <prop name="count" type="int" default="0"/>

        <!-- Multi-parameter property (e.g., bind_text with format) -->
        <prop name="temperature">
            <param name="temperature" type="subject"/>
            <param name="fmt" type="string" default="%.1f°C"/>
        </prop>

        <!-- Enum property (predefined values) -->
        <enumdef name="button_size">
            <enum name="small" value="32"/>
            <enum name="medium" value="48"/>
            <enum name="large" value="64"/>
        </enumdef>
        <prop name="size" type="button_size" default="medium"/>
    </api>

    <!-- Optional: Local constants -->
    <consts>
        <int name="button_size" value="36"/>
    </consts>

    <!-- Optional: Local styles -->
    <styles>
        <style name="style_base" bg_color="0x333" text_color="0xfff"/>
    </styles>

    <!-- The actual UI definition -->
    <view extends="lv_button" width="#button_size">
        <!-- Use API properties with $ prefix -->
        <lv_label text="$text" align="center"/>

        <!-- Reference local or global constants with # prefix -->
        <style name="style_base"/>
    </view>
</component>
```

#### Globals.xml Structure

The `globals.xml` file contains all shared resources:

```xml
<component>
    <consts>
        <!-- Colors -->
        <color name="bg_dark" value="0x1a1a1a"/>
        <color name="panel_bg" value="0x111410"/>
        <color name="text_primary" value="0xffffff"/>
        <color name="primary_color" value="0xff4444"/>

        <!-- Dimensions -->
        <px name="nav_width" value="102"/>
        <px name="padding_normal" value="20"/>
        <px name="accent_bar_width" value="4"/>
        <percent name="card_width" value="45%"/>

        <!-- Icon strings (auto-generated) -->
        <str name="icon_home" value=""/>  <!-- UTF-8 char -->
    </consts>

    <!-- Optional: Global styles -->
    <styles>
        <style name="style_button" bg_color="0x111" radius="8"/>
    </styles>

    <view extends="lv_obj"/>
</component>
```

**Using Constants:**
```xml
<!-- Reference with # prefix -->
<lv_obj width="#nav_width" style_bg_color="#primary_color"/>
```

### 1b. Custom Component API (Advanced)

The `<api>` tag defines the interface for components, creating reusable widgets with custom properties.

#### Property Types

**Simple types:**
- `string` - Text values
- `int` - Integer numbers
- `bool` - true/false
- `color` - Color values (hex)
- `subject` - Subject references
- `float` - Floating point numbers

**Complex types:**
- Multi-parameter properties (see below)
- Enum definitions (predefined value sets)
- Element accessors (for widget internals)

#### Multi-Parameter Properties

When a property needs multiple inputs (like format strings):

```xml
<api>
    <prop name="bind_temperature">
        <param name="bind_temperature" type="subject"/>
        <param name="fmt" type="string" default="%.1f"/>
    </prop>
</api>

<view>
    <!-- Usage: parameters become hyphenated attributes -->
    <lv_label bind_temperature="temp_subject" bind_temperature-fmt="%.2f°C"/>
</view>
```

**Pattern:** Property name repeated as first param; others use `property-param` notation.

#### Enum Definitions

Define allowed values for properties:

```xml
<api>
    <enumdef name="align_mode">
        <enum name="left" value="0"/>
        <enum name="center" value="1"/>
        <enum name="right" value="2"/>
    </enumdef>
    <prop name="alignment" type="align_mode" default="center"/>
</api>

<view>
    <!-- String names map to C enum values -->
    <lv_label alignment="left"/>  <!-- Uses value 0 -->
</view>
```

#### Element Definitions (Widget Internals)

For widgets with complex internal structure (like chart series):

```xml
<api>
    <!-- Add-type elements (dynamic creation) -->
    <element name="series" type="lv_chart_series_t" access="add">
        <arg name="color" type="color"/>
        <prop name="width" type="int" default="2"/>
    </element>

    <!-- Get-type elements (access pre-existing parts) -->
    <element name="indicator" type="lv_obj_t" access="get">
        <arg name="index" type="int"/>
        <prop name="color" type="color"/>
    </element>

    <!-- Set-type elements (indexed access, like table cells) -->
    <element name="cell" access="set">
        <arg name="row" type="int"/>
        <arg name="col" type="int"/>
        <prop name="value" type="string"/>
    </element>
</api>

<view extends="lv_chart">
    <!-- Usage: widget-element notation -->
    <lv_chart-series color="#ff0000" width="3"/>
    <lv_chart-series color="#00ff00" width="2"/>
</view>
```

**Access patterns:**
- `add` - Create multiple dynamically (chart series, list items)
- `get` - Access pre-existing parts (slider indicator, bar parts)
- `set` - Indexed access (table cells, matrix elements)
- `custom` - Map to arbitrary C function

#### Component vs Widget API

**Components** (simple):
- Only `<prop>` tags (no `<param>`, `<enumdef>`, `<element>`)
- Generate single `create()` function with all props as arguments
- Properties forward to child widgets

**Widgets** (rich):
- Full API model with all descriptor types
- Map to C setter functions and internal structures
- Built-in widgets already have parsers

#### API Best Practices

1. **Default values** - Always provide sensible defaults
2. **Type safety** - Use enums instead of raw integers
3. **Clear names** - Property names should be self-documenting
4. **Multi-param sparingly** - Use only when truly needed
5. **Document in comments** - Add `help="..."` attribute to props

### 2. Subjects (Reactive Data)

Subjects are observable data containers that automatically update all bound widgets when changed.

#### Subject Types

```c
lv_subject_init_string()  // String data (text labels)
lv_subject_init_int()     // Integer data (sliders, counters)
lv_subject_init_pointer() // Pointer data (custom objects)
lv_subject_init_color()   // Color data (dynamic theming)
```

#### Subject Lifecycle

```c
// 1. Create subject in C++
static lv_subject_t status_subject;
static char status_buffer[128];

// 2. Initialize with default value
lv_subject_init_string(&status_subject, status_buffer, NULL,
                       sizeof(status_buffer), "Initial status");

// 3. Register globally (before creating XML)
lv_xml_register_subject(NULL, "status_text", &status_subject);

// 4. Create XML (widgets automatically bind)
lv_obj_t* panel = lv_xml_create(parent, "home_panel", NULL);

// 5. Update subject (all bound widgets update automatically)
lv_subject_copy_string(&status_subject, "New status");
```

**⚠️ CRITICAL:** Register subjects BEFORE creating XML components that bind to them.

#### Scope: Global vs. Component

**Global scope** (`NULL` parameter):
- Subjects accessible to all XML components
- Use for shared data (status, temperature, active panel)

**Component scope** (non-NULL parameter):
- Subjects only accessible within specific component
- Use for component-internal state

### 3. Runtime Constants & Dynamic Configuration

XML constants can be registered and modified from C++ **before** widget creation, enabling responsive design without XML duplication.

#### Runtime Constant API

**Available Functions (lv_xml.h):**

```cpp
// Register or update a constant value
lv_result_t lv_xml_register_const(lv_xml_component_scope_t* scope,
                                   const char* name,
                                   const char* value);

// Retrieve a constant value
const char* lv_xml_get_const(lv_xml_component_scope_t* scope,
                              const char* name);
```

#### How Constants Work

**Storage:** Constants are stored in a linked list per component scope (global or component-specific).

**Resolution Timing:** Constants are resolved **once at parse time** when `lv_xml_create()` is called, NOT at render time.

**Critical Implication:** You can modify constants before widget creation, but modifying them afterward won't affect already-created widgets.

#### Responsive Design Pattern

Use runtime constant modification for screen-size adaptation:

```cpp
// Detect screen size at startup (BEFORE creating wizard)
int width = lv_display_get_horizontal_resolution(lv_display_get_default());

// Calculate responsive values
const char* padding_value;
const char* gap_value;
const char* header_height;

if (width < 600) {          // TINY (480x320)
    padding_value = "6";
    gap_value = "4";
    header_height = "28";
} else if (width < 900) {   // SMALL (800x480)
    padding_value = "12";
    gap_value = "8";
    header_height = "32";
} else {                    // LARGE (1024x600+)
    padding_value = "20";
    gap_value = "12";
    header_height = "40";
}

// Get globals scope and register constants BEFORE creating widgets
lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
lv_xml_register_const(scope, "wizard_padding", padding_value);
lv_xml_register_const(scope, "wizard_gap", gap_value);
lv_xml_register_const(scope, "wizard_header_height", header_height);

// NOW create wizard - it picks up the responsive constants
lv_obj_t* wizard = lv_xml_create(parent, "wizard_container", NULL);
```

**XML Usage:**

```xml
<!-- globals.xml - Define defaults (overridden by C++ at runtime) -->
<consts>
    <px name="wizard_padding" value="12"/>      <!-- Default for SMALL -->
    <px name="wizard_gap" value="8"/>
    <px name="wizard_header_height" value="32"/>
</consts>

<!-- wizard_container.xml - Uses constants reactively -->
<view name="wizard_container">
    <lv_obj width="100%" height="100%" flex_flow="column">
        <lv_obj name="wizard_header"
                width="100%" height="LV_SIZE_CONTENT"
                style_min_height="#wizard_header_height"
                style_pad_all="#wizard_padding"
                style_pad_gap="#wizard_gap">
            <!-- Header content -->
        </lv_obj>

        <lv_obj name="wizard_content" flex_grow="1"
                style_pad_all="#wizard_padding">
            <!-- Content area -->
        </lv_obj>
    </lv_obj>
</view>
```

#### Scope Hierarchy

**Global scope (NULL):**
```cpp
lv_xml_register_const(NULL, "primary_color", "0xff4444");  // Available to all components
```

**Component scope:**
```cpp
lv_xml_component_scope_t* scope = lv_xml_component_get_scope("my_component");
lv_xml_register_const(scope, "local_padding", "10");  // Only in my_component
```

**Lookup order:** Component scope first, then falls back to globals.

#### Limitations

❌ **Cannot modify after widget creation:** Constants are resolved once at parse time
❌ **No arithmetic:** Cannot use `calc(#padding * 2)` expressions
❌ **String values only:** All constants are stored as strings, converted during parsing

✅ **Best for:** Screen-size adaptation, theme switching, configuration-driven UIs

### 4. Data Binding

XML widgets can bind to subjects using **attribute bindings** (simple) or **child element bindings** (complex/conditional).

#### Simple Attribute Bindings

For straightforward data binding, use `bind_*` attributes:

```xml
<!-- Bind label text to string subject -->
<lv_label bind_text="status_text" style_text_color="#text_primary"/>

<!-- Bind with format string (multi-parameter property) -->
<lv_label bind_text="temp_value" bind_text-fmt="%.1f°C"/>
<!-- Format syntax: C printf format strings -->
<!-- %d=integer, %f=float, %s=string, %.1f=1 decimal place -->

<!-- Bind slider value to integer subject -->
<lv_slider bind_value="volume" min_value="0" max_value="100"/>

<!-- Bind arc value -->
<lv_arc bind_value="progress" min_value="0" max_value="100"/>

<!-- Reactive color binding -->
<lv_label bind_style_text_color="icon_color" text="#icon_home"/>

<!-- Bind image source (dynamic image switching) -->
<lv_image bind_src="current_icon"/>
```

**Naming pattern:** `bind_<property>` where `<property>` is the widget property name.

#### Advanced Child Element Bindings

For conditional logic and complex bindings, use child elements. LVGL 9.4 provides three types of conditional bindings: **flag**, **state**, and **style**.

##### A. Conditional Flag Bindings (Show/Hide, Enable/Disable)

Control widget flags based on subject values:

```xml
<lv_obj>
    <!-- Hide when current_step == 1 -->
    <lv_obj-bind_flag_if_eq subject="current_step" flag="hidden" ref_value="1"/>

    <!-- Disable when level >= 100 -->
    <lv_obj-bind_flag_if_ge subject="level" flag="disabled" ref_value="100"/>

    <!-- Make scrollable when count > 5 -->
    <lv_obj-bind_flag_if_gt subject="item_count" flag="scrollable" ref_value="5"/>
</lv_obj>
```

**Available Conditional Operators:**

| Element | Condition | Behavior |
|---------|-----------|----------|
| `<lv_obj-bind_flag_if_eq>` | `subject == ref_value` | Set flag when equal, clear otherwise |
| `<lv_obj-bind_flag_if_not_eq>` | `subject != ref_value` | Set flag when not equal, clear otherwise |
| `<lv_obj-bind_flag_if_gt>` | `subject > ref_value` | Set flag when greater than |
| `<lv_obj-bind_flag_if_ge>` | `subject >= ref_value` | Set flag when greater or equal |
| `<lv_obj-bind_flag_if_lt>` | `subject < ref_value` | Set flag when less than |
| `<lv_obj-bind_flag_if_le>` | `subject <= ref_value` | Set flag when less or equal |

**Supported Flags (Common):**

```xml
<!-- Visibility -->
<lv_obj-bind_flag_if_eq subject="visible" flag="hidden" ref_value="0"/>

<!-- Interaction -->
<lv_obj-bind_flag_if_ge subject="level" flag="clickable" ref_value="100"/>

<!-- Scrolling -->
<lv_obj-bind_flag_if_lt subject="size" flag="scrollable" ref_value="10"/>

<!-- All LV_OBJ_FLAG_* flags supported: -->
<!-- hidden, clickable, click_focusable, checkable, scrollable, -->
<!-- scroll_elastic, scroll_momentum, scroll_one, scroll_chain_hor, -->
<!-- scroll_chain_ver, scroll_on_focus, scroll_with_arrow, snappable, -->
<!-- press_lock, event_bubble, gesture_bubble, adv_hittest, -->
<!-- ignore_layout, floating, send_draw_task_events, overflow_visible, -->
<!-- flex_in_new_track, layout_1, layout_2, widget_1, widget_2, -->
<!-- user_1, user_2, user_3, user_4 -->
```

**Real-World Example (Wizard Back Button):**

```xml
<!-- Back button hidden on first step -->
<lv_button name="btn_back" width="#wizard_button_width" height="#button_height">
    <lv_label text="Back"/>
    <event_cb trigger="clicked" callback="on_back_clicked"/>
    <lv_obj-bind_flag_if_eq subject="current_step" flag="hidden" ref_value="1"/>
</lv_button>
```

##### B. Conditional State Bindings (Visual States)

Control widget states (pressed, checked, disabled, focused) based on subject values:

```xml
<lv_obj>
    <!-- Disable button when WiFi is disabled -->
    <lv_obj-bind_state_if_eq subject="wifi_enabled" state="disabled" ref_value="0"/>

    <!-- Check toggle when dark mode is on -->
    <lv_obj-bind_state_if_eq subject="dark_mode" state="checked" ref_value="1"/>

    <!-- Focus input when ready -->
    <lv_obj-bind_state_if_eq subject="input_ready" state="focused" ref_value="1"/>
</lv_obj>
```

**Available Conditional Operators:**

| Element | Condition |
|---------|-----------|
| `<lv_obj-bind_state_if_eq>` | `subject == ref_value` |
| `<lv_obj-bind_state_if_not_eq>` | `subject != ref_value` |
| `<lv_obj-bind_state_if_gt>` | `subject > ref_value` |
| `<lv_obj-bind_state_if_ge>` | `subject >= ref_value` |
| `<lv_obj-bind_state_if_lt>` | `subject < ref_value` |
| `<lv_obj-bind_state_if_le>` | `subject <= ref_value` |

**Supported States:**

```xml
<!-- State values: disabled, checked, focused, pressed, edited, -->
<!-- focus_key, scrolled, scroll_on_focus -->

<!-- Example: Disable connect button when no network selected -->
<lv_button>
    <lv_label text="Connect"/>
    <lv_obj-bind_state_if_eq subject="network_selected" state="disabled" ref_value="0"/>
</lv_button>
```

**Difference from Flag Bindings:**

- **Flags** control widget behavior (hidden, clickable, scrollable)
- **States** control visual appearance tied to LVGL's state system (disabled styling, checked styling, etc.)

##### C. Conditional Style Bindings (Whole Style Objects)

Apply entire style objects conditionally based on subject values:

```xml
<styles>
    <style name="style_error" bg_color="0xff0000" text_color="0xffffff"/>
    <style name="style_success" bg_color="0x00ff00" text_color="0x000000"/>
    <style name="style_warning" bg_color="0xffaa00" text_color="0x000000"/>
</styles>

<lv_obj>
    <!-- Apply error style when error_code == 1 -->
    <lv_obj-bind_style name="style_error" subject="error_code" ref_value="1" selector="main"/>

    <!-- Apply success style when status == 0 -->
    <lv_obj-bind_style name="style_success" subject="status" ref_value="0" selector="main"/>

    <!-- Apply warning style when temp >= 80 -->
    <lv_obj-bind_style name="style_warning" subject="temp" ref_value="80" selector="main"/>
</lv_obj>
```

**Attributes:**

- `name` - Style name from `<styles>` section (required)
- `subject` - Subject name (required)
- `ref_value` - Integer value for equality check (required)
- `selector` - Style selector: `main`, `pressed`, `checked`, etc. (optional, defaults to `main`)

**⚠️ LIMITATION:** Only equality check (`==`) is supported for style bindings. No `gt`, `lt`, `ne`, etc.

**Why:** Style bindings apply entire pre-defined style objects, not individual properties.

**Real-World Example (Temperature Warning):**

```xml
<styles>
    <style name="temp_normal" text_color="0xffffff"/>
    <style name="temp_warning" text_color="0xffaa00"/>
    <style name="temp_critical" text_color="0xff0000"/>
</styles>

<lv_label bind_text="temperature">
    <!-- Different colors for different temperature ranges -->
    <lv_obj-bind_style name="temp_normal" subject="temp_state" ref_value="0" selector="main"/>
    <lv_obj-bind_style name="temp_warning" subject="temp_state" ref_value="1" selector="main"/>
    <lv_obj-bind_style name="temp_critical" subject="temp_state" ref_value="2" selector="main"/>
</lv_label>
```

```cpp
// C++ updates state based on temperature
void update_temp_state(int temp) {
    int state = (temp < 200) ? 0 : (temp < 250) ? 1 : 2;
    lv_subject_set_int(&temp_state_subject, state);
}
```

##### D. Reactive Style Property Bindings (LVGL 9.4+)

**NEW in LVGL 9.4+:** Bind individual style properties directly to subject values for dynamic, reactive styling.

Unlike conditional style bindings that apply entire style objects, style property bindings allow you to bind a single style property (like opacity, color, width) to a subject's value. The widget's style automatically updates whenever the subject changes.

**XML Syntax:**

```xml
<lv_slider>
    <!-- Bind knob opacity to subject value (0-255) -->
    <bind_style_prop prop="bg_opa" selector="knob|pressed" subject="slider_knob_opa"/>

    <!-- Bind background color to subject -->
    <bind_style_prop prop="bg_color" selector="main" subject="theme_bg_color"/>

    <!-- Bind border width to subject -->
    <bind_style_prop prop="border_width" selector="main" subject="focus_border_width"/>
</lv_slider>
```

**C++ API (Alternative):**

```cpp
// Bind style property to subject
lv_obj_bind_style_prop(slider, LV_STYLE_BG_OPA, LV_PART_KNOB | LV_STATE_PRESSED, &slider_knob_opa);
lv_obj_bind_style_prop(obj, LV_STYLE_BG_COLOR, LV_PART_MAIN, &theme_bg_color);

// Update subject → widget automatically updates
lv_subject_set_int(&slider_knob_opa, 128);  // Slider becomes semi-transparent
lv_subject_set_color(&theme_bg_color, lv_color_hex(0xFF5733));  // Background changes
```

**Attributes:**

- `prop` - Style property name (e.g., `bg_opa`, `bg_color`, `border_width`) (required)
- `selector` - Style selector (e.g., `main`, `knob|pressed`, `indicator`) (optional, defaults to `main`)
- `subject` - Subject name (required)

**Use Cases:**

1. **Dynamic opacity controls** - Bind slider opacity to user preference subject
2. **Theme switching** - Bind colors to theme subjects (dark/light mode)
3. **Responsive sizing** - Bind widths/heights to screen size subjects
4. **State-driven styling** - Bind border colors/widths to connection status subjects
5. **Data visualization** - Bind colors to value ranges (e.g., temperature → color gradient)

**Example: Dynamic Slider Styling**

```xml
<component name="dynamic_slider">
    <view extends="lv_slider">
        <!-- Opacity controlled by subject -->
        <bind_style_prop prop="bg_opa" selector="main" subject="slider_main_opa"/>
        <bind_style_prop prop="bg_opa" selector="indicator" subject="slider_indicator_opa"/>
        <bind_style_prop prop="bg_opa" selector="knob" subject="slider_knob_opa"/>

        <!-- Colors controlled by subjects -->
        <bind_style_prop prop="bg_color" selector="indicator" subject="slider_active_color"/>
    </view>
</component>
```

```cpp
// Initialize subjects
static lv_subject_t slider_knob_opa;
static lv_subject_t slider_active_color;

lv_subject_init_int(&slider_knob_opa, 255);  // Start fully opaque
lv_subject_init_color(&slider_active_color, lv_color_hex(0x00FF00));  // Green

// Change dynamically based on state
lv_subject_set_int(&slider_knob_opa, 128);  // Make semi-transparent
lv_subject_set_color(&slider_active_color, lv_color_hex(0xFF0000));  // Change to red
```

**Example: Dark/Light Theme Toggle**

```xml
<!-- Multiple widgets bind to same theme subjects -->
<lv_obj>
    <bind_style_prop prop="bg_color" selector="main" subject="theme_bg"/>
    <bind_style_prop prop="border_color" selector="main" subject="theme_border"/>

    <lv_label text="Hello World">
        <bind_style_prop prop="text_color" selector="main" subject="theme_text"/>
    </lv_label>
</lv_obj>
```

```cpp
// Toggle between light and dark themes
void set_dark_theme(bool dark) {
    lv_color_t bg = dark ? lv_color_hex(0x1a1a1a) : lv_color_hex(0xffffff);
    lv_color_t text = dark ? lv_color_hex(0xe0e0e0) : lv_color_hex(0x000000);
    lv_color_t border = dark ? lv_color_hex(0x404040) : lv_color_hex(0xcccccc);

    lv_subject_set_color(&theme_bg, bg);
    lv_subject_set_color(&theme_text, text);
    lv_subject_set_color(&theme_border, border);
    // All bound widgets update automatically!
}
```

**Advantages over Conditional Style Bindings:**

| Feature | Style Property Binding | Conditional Style Binding |
|---------|------------------------|---------------------------|
| Granularity | Single property | Entire style object |
| Subject type | Any value (int, color, etc.) | Integer equality only |
| Conditionals | No conditions (direct binding) | Equality check (`==`) |
| Use case | Reactive value mapping | State-based styling |
| Multiple properties | Need multiple bindings | One binding per style |

**When to use Style Property Bindings:**
- ✅ Direct value-to-style mapping (e.g., slider value → opacity)
- ✅ Theme color variables (e.g., primary color subject)
- ✅ Continuous value changes (e.g., temperature → color gradient)
- ✅ Multiple widgets sharing same style value

**When to use Conditional Style Bindings:**
- ✅ Discrete state changes (e.g., normal/warning/error states)
- ✅ Applying predefined style presets
- ✅ Complex multi-property changes based on single condition

##### E. Conditional Binding Limitations

**❌ Text Conditionals DO NOT EXIST:**

There is no `bind_text_if_eq` or similar. To show different text based on conditions, use multiple labels with flag bindings:

```xml
<!-- Workaround: Multiple labels with conditional visibility -->
<lv_label text="Idle">
    <lv_obj-bind_flag_if_not_eq subject="state" flag="hidden" ref_value="0"/>
</lv_label>
<lv_label text="Active">
    <lv_obj-bind_flag_if_not_eq subject="state" flag="hidden" ref_value="1"/>
</lv_label>
<lv_label text="Error">
    <lv_obj-bind_flag_if_not_eq subject="state" flag="hidden" ref_value="2"/>
</lv_label>
```

**❌ CONDITIONAL Style Property Bindings DO NOT EXIST:**

You cannot bind individual style properties **conditionally** (like `bind_style_pad_all_if_eq`).

**However**, you CAN bind style properties **reactively** using `<bind_style_prop>` (see section D above):

```xml
<!-- ❌ DOESN'T WORK - Conditional style property binding: -->
<lv_obj bind_style_pad_all_if_eq="subject" value="20" ref_value="1"/>

<!-- ✅ WORKS - Reactive style property binding (LVGL 9.4+): -->
<lv_obj>
    <bind_style_prop prop="pad_all" selector="main" subject="padding_value"/>
    <!-- Widget padding reactively updates when padding_value subject changes -->
</lv_obj>

<!-- ✅ ALSO WORKS - Conditional whole style objects: -->
<styles>
    <style name="large_padding" style_pad_all="20"/>
    <style name="small_padding" style_pad_all="5"/>
</styles>

<lv_obj>
    <lv_obj-bind_style name="large_padding" subject="size_mode" ref_value="1" selector="main"/>
    <lv_obj-bind_style name="small_padding" subject="size_mode" ref_value="0" selector="main"/>
</lv_obj>
```

**Key Distinction:**
- ❌ **Conditional + Style Property** = Not supported (no `if_eq`, `if_gt`, etc. on individual properties)
- ✅ **Reactive Style Property** = Supported (direct binding with `<bind_style_prop>`)
- ✅ **Conditional + Whole Style** = Supported (with `<lv_obj-bind_style>`)

**❌ Subjects Cannot Be Used Directly in Attributes:**

```xml
<!-- ❌ DOESN'T WORK: -->
<lv_obj style_pad_all="subject:padding_value"/>

<!-- ✅ WORKS - Use bind_style_prop (LVGL 9.4+): -->
<lv_obj>
    <bind_style_prop prop="pad_all" selector="main" subject="padding_value"/>
</lv_obj>

<!-- ✅ ALSO WORKS - Use runtime constants: -->
<lv_obj style_pad_all="#dynamic_padding"/>
```

##### E. When to Use Each Binding Type

**Use attribute bindings (`bind_text`, `bind_value`):**
- Simple direct data display
- No conditional logic needed
- One subject → one property

**Use flag bindings (`<lv_obj-bind_flag_if_*>`):**
- Show/hide widgets conditionally
- Enable/disable interaction
- Control scrolling, layout behavior

**Use state bindings (`<lv_obj-bind_state_if_*>`):**
- Visual state changes (disabled styling, checked styling)
- State-driven UI feedback

**Use style bindings (`<lv_obj-bind_style>`):**
- Apply complex style combinations
- Theme switching
- Multi-property visual changes

**Use C++ observers for:**
- Individual style property updates not covered by style objects
- Complex multi-subject logic
- Calculations or transformations

#### Defining Subjects in globals.xml (Optional)

While subjects can be defined in XML, it's recommended to initialize them in C++ for better control:

```xml
<!-- In globals.xml (optional - for documentation) -->
<subjects>
    <int name="volume" value="10"/>
    <string name="status" value="Ready"/>
    <float name="temp" value="25.0"/>
</subjects>
```

**Note:** If you define subjects in XML, they'll be registered with empty/default values before C++ initialization. Prefer C++ initialization for predictable behavior.

---

## Layouts & Positioning

LVGL 9 provides two powerful layout systems inspired by CSS: **Flex** (flexbox) and **Grid**. Layouts automatically position children, overriding manual positioning.

### Layout Control Flags

These flags affect how widgets participate in layouts:

```xml
<!-- Exclude from layout calculations -->
<lv_obj hidden="true"/>  <!-- LV_OBJ_FLAG_HIDDEN -->

<!-- Remove from layout but keep manual position -->
<lv_obj ignore_layout="true"/>  <!-- LV_OBJ_FLAG_IGNORE_LAYOUT -->

<!-- Floating (like ignore_layout + excluded from content size) -->
<lv_obj floating="true"/>  <!-- LV_OBJ_FLAG_FLOATING -->
```

---

### Flex Layout (Flexbox)

Flex arranges children in rows or columns with wrapping, spacing, and proportional growth. Best for **1D layouts** (single row/column or wrapping).

#### Flex Flow Options (VERIFIED in lvgl/src/others/xml/lv_xml_base_types.c)

```xml
<!-- Basic flow (no wrapping) -->
<lv_obj flex_flow="row">           <!-- Horizontal left to right -->
<lv_obj flex_flow="column">        <!-- Vertical top to bottom -->
<lv_obj flex_flow="row_reverse">   <!-- Horizontal right to left -->
<lv_obj flex_flow="column_reverse"><!-- Vertical bottom to top -->

<!-- With wrapping (creates multiple tracks) -->
<lv_obj flex_flow="row_wrap">              <!-- Wrap to new rows -->
<lv_obj flex_flow="column_wrap">           <!-- Wrap to new columns -->
<lv_obj flex_flow="row_wrap_reverse">      <!-- Wrap reversed -->
<lv_obj flex_flow="column_wrap_reverse">   <!-- Wrap reversed -->
```

#### Flex Alignment - Three Parameters

Flex uses **three alignment properties** to control positioning:

| Property | Controls | CSS Equivalent |
|----------|----------|----------------|
| `style_flex_main_place` | Item distribution along **main axis** | `justify-content` |
| `style_flex_cross_place` | Item alignment along **cross axis** | `align-items` |
| `style_flex_track_place` | Track distribution (multi-track wrapping) | `align-content` |

**⚠️ CRITICAL:** Never use `flex_align` attribute - it doesn't exist in LVGL 9 XML!

**✓ CORRECT XML usage:**
```xml
<lv_obj flex_flow="row"
        style_flex_main_place="center"
        style_flex_cross_place="center"
        style_flex_track_place="start">
    <!-- Children -->
</lv_obj>
```

#### Alignment Values (All Three Properties)

| Value | Behavior | Available For |
|-------|----------|---------------|
| `start` | Beginning (left/top, RTL-aware) | All three |
| `center` | Centered | All three |
| `end` | End (right/bottom, RTL-aware) | All three |
| `space_evenly` | Equal space around all items | `main_place`, `track_place` |
| `space_around` | Equal space, double at edges | `main_place`, `track_place` |
| `space_between` | No edge space, even gaps | `main_place`, `track_place` |

**Note:** `space_*` values don't apply to `cross_place` (items don't "distribute" perpendicularly).

#### Axis Direction Reference

**For `flex_flow="row"` (horizontal):**
- `style_flex_main_place` → **horizontal** distribution (left/center/right)
- `style_flex_cross_place` → **vertical** alignment (top/center/bottom)

**For `flex_flow="column"` (vertical):**
- `style_flex_main_place` → **vertical** distribution (top/center/bottom)
- `style_flex_cross_place` → **horizontal** alignment (left/center/right)

#### Flex Grow - Proportional Expansion

Children with `flex_grow` expand to fill remaining space proportionally.

**How it works:**
- Available space ÷ total grow weight = space per unit
- Each child gets (available space × its grow value) ÷ total grow weight

**Example:** 400px available, grow values (1, 1, 2):
- First two: 100px each (400 × 1 ÷ 4)
- Third: 200px (400 × 2 ÷ 4)

```xml
<lv_obj flex_flow="row" width="400">
    <lv_label text="Left"/>                      <!-- Fixed size -->
    <lv_label text="Center" flex_grow="1"/>      <!-- Takes remaining space -->
    <lv_label text="Right"/>                     <!-- Fixed size -->
</lv_obj>
```

**Equal distribution:**
```xml
<lv_obj flex_flow="row">
    <lv_obj flex_grow="1">33%</lv_obj>
    <lv_obj flex_grow="1">33%</lv_obj>
    <lv_obj flex_grow="1">33%</lv_obj>
</lv_obj>
```

**Weighted distribution:**
```xml
<lv_obj flex_flow="row">
    <lv_obj flex_grow="1">25%</lv_obj>
    <lv_obj flex_grow="2">50%</lv_obj>
    <lv_obj flex_grow="1">25%</lv_obj>
</lv_obj>
```

**Disable grow:** Set `flex_grow="0"` (default)

#### Forcing New Track (Line Break)

Use `flex_in_new_track="true"` to force an item to start a new row/column:

```xml
<lv_obj flex_flow="row_wrap">
    <lv_button>Item 1</lv_button>
    <lv_button>Item 2</lv_button>
    <lv_button flex_in_new_track="true">Item 3</lv_button>  <!-- New row -->
    <lv_button>Item 4</lv_button>
</lv_obj>
```

**C flag:** `LV_OBJ_FLAG_FLEX_IN_NEW_TRACK`

#### Flex Gaps (Spacing)

Control spacing between items using padding style properties:

```xml
<lv_obj flex_flow="row"
        style_pad_column="10"   <!-- 10px horizontal gap -->
        style_pad_row="5">      <!-- 5px vertical gap (if wrapping) -->
    <!-- Children -->
</lv_obj>
```

#### 🚨 CRITICAL: Flex Layout Height Requirements

**ESSENTIAL RULE:** When using `flex_grow` on children, **the parent MUST have an explicit height dimension**.

Without an explicit parent height, `flex_grow` children will collapse to 0 size or cause unexpected layout behavior.

**❌ BROKEN - Parent has no height:**
```xml
<lv_obj flex_flow="row">  <!-- No height specified -->
    <lv_obj flex_grow="3">Left column (30%)</lv_obj>
    <lv_obj flex_grow="7">Right column (70%)</lv_obj>
</lv_obj>
<!-- Result: Columns collapse or have unpredictable heights -->
```

**✓ CORRECT - Parent has explicit height:**
```xml
<!-- Option 1: Fixed height -->
<lv_obj flex_flow="row" height="300">
    <lv_obj flex_grow="3" height="100%">Left (30%)</lv_obj>
    <lv_obj flex_grow="7" height="100%">Right (70%)</lv_obj>
</lv_obj>

<!-- Option 2: flex_grow from grandparent -->
<lv_obj flex_flow="column" height="100%">
    <lv_obj flex_flow="row" flex_grow="1">  <!-- Expands to fill parent -->
        <lv_obj flex_grow="3" height="100%">Left (30%)</lv_obj>
        <lv_obj flex_grow="7" height="100%">Right (70%)</lv_obj>
    </lv_obj>
</lv_obj>
```

**💡 Two-Column Layout Pattern (30/70 split):**

This pattern is used extensively (wizard screens, settings pages, etc.):

```xml
<!-- CORRECT hierarchy for two-column layouts -->
<view height="100%" flex_flow="column">
    <!-- Top wrapper must expand to fill parent -->
    <lv_obj width="100%" flex_grow="1" flex_flow="column">

        <!-- Two-column row must expand within wrapper -->
        <lv_obj width="100%" flex_grow="1" flex_flow="row">

            <!-- LEFT COLUMN (30%) - must have height="100%" -->
            <lv_obj flex_grow="3" height="100%"
                    flex_flow="column"
                    scrollable="true" scroll_dir="VER">
                <!-- Cards with fixed heights -->
                <lv_obj height="100">Card 1</lv_obj>
                <lv_obj height="100">Card 2</lv_obj>
            </lv_obj>

            <!-- RIGHT COLUMN (70%) - must have height="100%" -->
            <lv_obj flex_grow="7" height="100%"
                    scrollable="true" scroll_dir="VER">
                <!-- Content -->
            </lv_obj>
        </lv_obj>
    </lv_obj>
</view>
```

**⚠️ Common Pitfalls:**

1. **Row height constrained by shortest column:**
   - In `flex_flow="row"`, row height = tallest child
   - If one column is short, it constrains the entire row
   - **Solution:** Add `height="100%"` to ALL columns

2. **LV_SIZE_CONTENT in nested flex:**
   - `LV_SIZE_CONTENT` evaluates to 0 before `lv_obj_update_layout()` is called
   - In deeply nested flex layouts, this causes collapse
   - **Solution:** Use fixed heights or `style_min_height` for cards

3. **Missing flex_grow chain:**
   - Every level needs proper sizing: `wrapper → row → columns`
   - Missing `flex_grow` at any level breaks the chain
   - **Solution:** Trace from root to leaf, ensure each level expands

**Diagnostic tip:** Add temporary `style_bg_color="#ff0000"` to containers to visualize their actual bounds.

---

### Grid Layout

Grid arranges children in a **2D table structure** with explicit rows and columns. Best for **structured layouts** where items align both horizontally and vertically.

#### When to Use Grid vs Flex

**Use Grid when:**
- You need precise 2D alignment (rows AND columns)
- Items should align across multiple rows
- You have a table-like structure
- You need cells that span multiple rows/columns

**Use Flex when:**
- Single-direction flow (row or column)
- Wrapping is simple (no cross-alignment needed)
- Dynamic number of items
- You need proportional growth (flex_grow)

#### Defining Grid Structure

Grids use **descriptor arrays** to define column/row sizes:

```xml
<!-- Not directly supported in XML - use C++ or component API -->
```

**⚠️ IMPORTANT:** Grid definitions require C API or custom component `<api>` tags. LVGL XML doesn't have direct `style_grid_column_dsc_array` attribute support.

**C API approach:**
```cpp
// Define grid structure in C++
static int32_t col_dsc[] = {100, 200, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
static int32_t row_dsc[] = {50, 50, 50, LV_GRID_TEMPLATE_LAST};

lv_obj_set_style_grid_column_dsc_array(container, col_dsc, 0);
lv_obj_set_style_grid_row_dsc_array(container, row_dsc, 0);
lv_obj_set_layout(container, LV_LAYOUT_GRID);
```

**Track sizing options:**
- **Fixed pixels:** `100` (100px wide/tall)
- **LV_GRID_CONTENT:** Size to fit largest child in that track
- **LV_GRID_FR(n):** Fractional units (distribute free space proportionally)

**Example:** `{100, LV_GRID_FR(1), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST}`
- Column 1: 100px fixed
- Column 2: 1/3 of remaining space
- Column 3: 2/3 of remaining space

#### Placing Grid Children

Position children in grid cells using C API:

```cpp
lv_obj_set_grid_cell(
    child,
    LV_GRID_ALIGN_STRETCH,  // col_align
    0,                       // col_pos (0-indexed)
    2,                       // col_span (spans 2 columns)
    LV_GRID_ALIGN_CENTER,   // row_align
    1,                       // row_pos
    1                        // row_span
);
```

**Cell alignment values:**
- `LV_GRID_ALIGN_START` - Left/top (default)
- `LV_GRID_ALIGN_CENTER` - Centered in cell
- `LV_GRID_ALIGN_END` - Right/bottom
- `LV_GRID_ALIGN_STRETCH` - Fill entire cell

#### Grid Track Alignment

When free space exists in the grid container, control track distribution:

```cpp
lv_obj_set_grid_align(
    container,
    LV_GRID_ALIGN_SPACE_EVENLY,  // column alignment
    LV_GRID_ALIGN_CENTER          // row alignment
);
```

**Available values:** Same as flex alignment - `START`, `END`, `CENTER`, `SPACE_EVENLY`, `SPACE_AROUND`, `SPACE_BETWEEN`

#### Grid Gaps (Spacing)

```cpp
lv_obj_set_style_pad_column(container, 10, 0);  // 10px column gap
lv_obj_set_style_pad_row(container, 5, 0);      // 5px row gap
```

**XML equivalent (if grid setup done in C):**
```xml
<lv_obj style_pad_column="10" style_pad_row="5">
    <!-- Grid children -->
</lv_obj>
```

#### Sub-Grids (Inheritance)

Setting grid descriptors to `NULL` makes a child inherit its parent's grid:

```cpp
lv_obj_set_grid_dsc_array(child, NULL, NULL);  // Inherit parent grid
```

**Use case:** Create wrapper objects that span multiple parent cells while their children align to the parent's grid.

---

### Centering Techniques

Based on extensive testing with the home panel status card.

#### Horizontal Centering

**Text/Icons in Labels:**

```xml
<lv_label
    text="Content"
    style_text_align="center"
    width="100%"/>
```

**Key points:**
- `style_text_align="center"` centers text within the label
- `width="100%"` makes label span full container width
- **Both attributes required** - `style_text_align` alone won't work

**Containers with Flex:**

```xml
<lv_obj flex_flow="row" style_flex_main_place="center">
    <lv_label text="Centered content"/>
</lv_obj>
```

#### Vertical Centering

**Single-Level:**

```xml
<lv_obj flex_flow="column"
        style_flex_main_place="center"
        style_flex_cross_place="center">
    <lv_label text="Vertically centered"/>
</lv_obj>
```

**Multi-Level Nested Centering:**

```xml
<lv_obj flex_grow="1" height="100%"
        flex_flow="column"
        style_flex_main_place="center"
        style_flex_cross_place="center">
    <lv_label text="Icon" style_text_align="center" width="100%"/>
    <lv_label text="Text" style_text_align="center" width="100%"/>
</lv_obj>
```

**Key points:**
- Container needs `height="100%"` to enable vertical centering
- `style_flex_main_place="center"` centers along main axis (vertical for column)
- `style_flex_cross_place="center"` centers along cross axis (horizontal for column)
- Inner labels use `style_text_align="center"` + `width="100%"`

#### Common Centering Combinations

```xml
<!-- Center children both horizontally and vertically (row layout) -->
<lv_obj flex_flow="row"
        style_flex_main_place="center"
        style_flex_cross_place="center">
    <!-- Children centered in both directions -->
</lv_obj>

<!-- Evenly distribute horizontally, center vertically (row layout) -->
<lv_obj flex_flow="row"
        style_flex_main_place="space_evenly"
        style_flex_cross_place="center">
    <!-- Children spread out horizontally, centered vertically -->
</lv_obj>

<!-- Stack vertically, center horizontally (column layout) -->
<lv_obj flex_flow="column"
        style_flex_main_place="center"
        style_flex_cross_place="center">
    <!-- Children stacked vertically, centered horizontally -->
</lv_obj>
```

#### Widget Alignment (Alternative to Flex)

For centering children within a parent without flex:

```xml
<lv_button>
    <lv_label align="center" text="Click me"/>  <!-- Equivalent to lv_obj_center() -->
</lv_button>
```

**Alignment options:** `center`, `top_left`, `top_mid`, `top_right`, `bottom_left`, `bottom_mid`, `bottom_right`, `left_mid`, `right_mid`

#### ⚠️ CRITICAL: Flex Layout Can Conflict with `align="center"`

**Problem:** When a parent has `flex_flow` set, it can interfere with child widget's `align="center"`, causing misalignment.

**Symptoms:**
- Widget with `align="center"` appears off-center (horizontally or vertically)
- Centering works in one direction but not the other
- Changing `style_flex_main_place` values doesn't fix the issue

**Root Cause:** Flex layout positioning overrides absolute `align="center"` positioning.

**Solution:** Remove `flex_flow` from parent container when you have a single child that needs true center alignment.

**Example from motion_panel.xml (jog pad centering):**

```xml
<!-- ❌ DOESN'T WORK - flex conflicts with align="center" -->
<lv_obj name="left_column" width="65%" height="100%"
        flex_flow="column" style_flex_main_place="center">
    <lv_obj name="jog_pad" align="center">
        <!-- Jog pad appears off-center -->
    </lv_obj>
</lv_obj>

<!-- ✅ WORKS - no flex, pure absolute positioning -->
<lv_obj name="left_column" width="65%" height="100%"
        style_bg_opa="0" style_pad_all="0">
    <!-- NO flex_flow attribute -->
    <lv_obj name="jog_pad" align="center">
        <!-- Jog pad perfectly centered both H and V -->
    </lv_obj>
</lv_obj>
```

**When to use each approach:**

**Use flex layout** when:
- Parent has multiple children to arrange
- You need responsive distribution (`space_between`, `space_evenly`)
- Children need `flex_grow` behavior

**Use `align="center"` without flex** when:
- Parent has a single child
- You need perfect center alignment in both directions
- Simple absolute positioning is sufficient

**Reference:** See `ui_xml/motion_panel.xml:50-68` for working example.

### Dividers in Flex Layouts

**Vertical dividers** (in row layouts):

```xml
<lv_obj flex_flow="row" style_flex_cross_place="center">
    <lv_obj flex_grow="1">Section 1</lv_obj>

    <!-- Vertical divider -->
    <lv_obj width="1"
            style_align_self="stretch"
            style_bg_color="#text_secondary"
            style_bg_opa="50%"
            style_border_width="0"/>

    <lv_obj flex_grow="1">Section 2</lv_obj>
</lv_obj>
```

**Key points:**
- Use `width="1"` for vertical dividers in row layouts
- Use `style_align_self="stretch"` to extend full height
- Dividers participate in flex layout as children
- Set `style_border_width="0"` to avoid border rendering

---

## Common UI Patterns

### Vertical Accent Bar (Leading Edge Indicator)

A thin vertical colored bar positioned to the left of text, commonly used in material design to draw attention to important content.

**Visual:**
```
| Important message here
```

**Implementation:**

```xml
<!-- Container with flex-grow to fill available space -->
<lv_obj flex_grow="1" style_bg_opa="0%" style_border_width="0" style_pad_all="0"
        flex_flow="row" style_flex_cross_place="center" style_pad_gap="#accent_bar_gap">

    <!-- Vertical accent bar -->
    <lv_obj width="#accent_bar_width" height="#accent_bar_height"
            style_bg_color="#primary_color" style_bg_opa="255"
            style_radius="2" style_border_width="0"/>

    <!-- Content text (flex-grows to fill remaining width) -->
    <lv_label flex_grow="1" bind_text="status_text"
              style_text_color="#text_primary" style_text_font="montserrat_20"/>
</lv_obj>
```

**Required Constants in globals.xml:**

```xml
<px name="accent_bar_width" value="4"/>      <!-- Bar thickness -->
<px name="accent_bar_height" value="28"/>    <!-- Bar height -->
<px name="accent_bar_gap" value="18"/>       <!-- Spacing between bar and text -->
```

**When to use:**
- Status messages or notifications
- Section headers that need emphasis
- Call-to-action text
- Important state information

**Alternative: Full-height bar using `style_align_self="stretch"`:**

```xml
<!-- Bar automatically matches container height -->
<lv_obj width="#accent_bar_width"
        style_align_self="stretch"
        style_bg_color="#primary_color"
        style_bg_opa="255"/>
```

### Icon Component

**Modern approach using reusable icon component with semantic sizing:**

```xml
<!-- Basic icon with default size (64px) -->
<icon src="mat_home"/>

<!-- Sized icon with color variant -->
<icon src="mat_heater" size="lg" variant="accent"/>

<!-- Clickable icon button -->
<lv_button width="60" height="60"
           style_bg_opa="0"
           style_border_width="0"
           style_shadow_width="0">
    <icon src="mat_back" size="md" variant="primary"/>
    <lv_event-call_function trigger="clicked" callback="back_clicked"/>
</lv_button>

<!-- Icon with custom recolor (override variant) -->
<icon src="mat_delete" size="md" variant="none"
      style_image_recolor="#warning_color"
      style_image_recolor_opa="100%"/>
```

**Semantic sizes:** `xs` (16px), `sm` (24px), `md` (32px), `lg` (48px), `xl` (64px), `xxl` (96px)

**Color variants:** `primary`, `secondary`, `accent`, `disabled`, `none`

**Benefits:**
- Clean, semantic API (size="md" vs explicit pixel values)
- Automatic icon scaling (no manual scale calculation)
- Consistent color theming via variants
- Encapsulated in reusable component

**C++ Setup (main.cpp):**
```cpp
#include "ui_icon.h"

lv_display_t* display = lv_sdl_window_create(...);
ui_icon_init_auto_scale(display);  // Initialize once at startup
```

### Legacy Pattern: Direct Image/Label Icons

For reference, the old approach before icon component:

```xml
<!-- Direct lv_image with manual scaling -->
<lv_image src="mat_home" width="64" height="64" scale="256"/>

<!-- FontAwesome with label -->
<lv_label text="#icon_home" style_text_font="fa_icons_64" align="center"/>
```

**Why migrate to icon component:**
- Reduces XML verbosity
- Eliminates scale calculation errors
- Provides semantic sizing
- Enables global theme changes via variants

### Reactive Counter with Buttons

Uses subjects for automatic UI updates:

```xml
<!-- In globals.xml -->
<subjects>
    <int name="counter" value="0"/>
</subjects>

<!-- In component -->
<lv_obj flex_flow="row" style_flex_cross_place="center">
    <lv_button>
        <lv_label text="-"/>
        <subject_increment_event subject="counter" step="-1" min="0"/>
    </lv_button>

    <lv_label bind_text="counter" bind_text-fmt="%d" flex_grow="1" style_text_align="center"/>

    <lv_button>
        <lv_label text="+"/>
        <subject_increment_event subject="counter" step="1" max="100"/>
    </lv_button>
</lv_obj>
```

### Tabbed Navigation

Show/hide panels based on active tab subject:

```xml
<!-- Tab buttons -->
<lv_obj flex_flow="row">
    <lv_button flex_grow="1">
        <lv_label text="Home"/>
        <subject_set_int_event subject="active_tab" value="0"/>
    </lv_button>
    <lv_button flex_grow="1">
        <lv_label text="Settings"/>
        <subject_set_int_event subject="active_tab" value="1"/>
    </lv_button>
</lv_obj>

<!-- Home tab content -->
<lv_obj>
    <bind_flag_if_eq subject="active_tab" flag="hidden" ref_value="1"/>
    <!-- Home content -->
</lv_obj>

<!-- Settings tab content -->
<lv_obj>
    <bind_flag_if_eq subject="active_tab" flag="hidden" ref_value="0"/>
    <!-- Settings content -->
</lv_obj>
```

### Status Card with Icon Stacks

Vertical stacking of icon and label, centered (using icon component):

```xml
<lv_obj flex_grow="1" height="100%"
        flex_flow="column"
        style_flex_main_place="center"
        style_flex_cross_place="center">
    <!-- Icon on top (using component) -->
    <icon src="mat_heater" size="lg" variant="secondary"/>

    <!-- Text below -->
    <lv_label bind_text="temp_text"
              style_text_font="montserrat_16"
              style_text_align="center"
              width="100%"/>
</lv_obj>
```

**Legacy approach with lv_label:**
```xml
<!-- Old pattern (still works, but verbose) -->
<lv_label text="#icon_temperature"
          style_text_font="fa_icons_48"
          style_text_align="center"
          width="100%"/>
```

---

## Responsive Design Patterns

Responsive design ensures UI layouts adapt gracefully across different screen sizes using mobile-first principles and flex layouts.

### Mobile-First Approach

Design for the smallest screen first (480×320), then let flex layout scale up naturally.

**Screen Size Reference:**
- **Tiny**: 480×320 (height: 320px) - **Design for this first**
- **Small**: 800×480 (height: 480px)
- **Medium**: 1024×600 (height: 600px)
- **Large**: 1280×720 (height: 720px)

### Key Principles

#### 1. Use Semantic Constants Only

Never hardcode pixel values in XML. Use constants from `globals.xml`:

```xml
<!-- globals.xml -->
<consts>
    <px name="padding_small" value="8"/>
    <px name="padding_medium" value="12"/>
    <px name="padding_large" value="20"/>
    <px name="gap_tiny" value="4"/>
    <px name="gap_small" value="8"/>
    <px name="gap_normal" value="12"/>
    <px name="gap_large" value="20"/>
</consts>

<!-- Usage in components -->
<lv_obj style_pad_all="#padding_medium" style_pad_gap="#gap_small"/>
```

#### 2. Flex Layout for Responsiveness

Use flex layouts that adapt to available space:

```xml
<!-- Container fills available space -->
<lv_obj flex_grow="1" flex_flow="column">
    <!-- Rows distribute evenly -->
    <lv_obj flex_grow="1" flex_flow="row"
            style_flex_main_place="space_evenly">
        <!-- Buttons/cells expand to fill -->
        <lv_button flex_grow="1" height="100%"/>
        <lv_button flex_grow="1" height="100%"/>
    </lv_obj>
</lv_obj>
```

#### 3. Mobile-First Font Sizes

Choose fonts that work on tiny screens - larger screens get the same compact fonts:

```xml
<!-- Prefer smaller, readable fonts -->
<lv_label style_text_font="montserrat_16"/>  <!-- Good for tiny screens -->
<lv_label style_text_font="montserrat_20"/>  <!-- Still readable on tiny -->

<!-- Avoid large fonts that don't scale down -->
<lv_label style_text_font="montserrat_28"/>  <!-- Too big for tiny screens -->
```

### C++ Component Pattern for Dynamic Responsiveness

When XML constants aren't sufficient, use C++ component wrappers for dynamic responsive behavior:

```cpp
// src/ui_responsive_utils.cpp
lv_coord_t ui_get_responsive_header_height(lv_coord_t screen_height) {
    if (screen_height >= 600) return 60;      // Large/medium
    else if (screen_height >= 480) return 48; // Small
    else return 40;                           // Tiny
}

void ui_component_header_bar_setup(lv_obj_t* header, lv_obj_t* screen) {
    lv_coord_t height = ui_get_responsive_header_height(lv_obj_get_height(screen));
    lv_obj_set_height(header, height);
}
```

**Usage in panels:**
```cpp
void ui_panel_motion_setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    lv_obj_t* header = lv_obj_find_by_name(panel, "motion_header");
    if (header) {
        ui_component_header_bar_setup(header, parent_screen);
    }
}
```

### Testing Responsive Layouts

Test panels at all screen sizes:

```bash
# Test specific panel at all sizes
for size in tiny small medium large; do
  ./build/bin/helix-ui-proto -s $size -p panel-name
done

# Or use screenshot script for visual comparison
for size in tiny small medium large; do
  ./scripts/screenshot.sh helix-ui-proto panel-$size -s $size
done
```

### Common Responsive Patterns

#### Equal Distribution Grid

```xml
<lv_obj flex_flow="row" width="100%" height="100%">
    <lv_obj flex_grow="1" height="100%" style_pad_all="#padding_small">
        <!-- Content adapts to available space -->
    </lv_obj>
    <lv_obj flex_grow="1" height="100%" style_pad_all="#padding_small">
        <!-- Content adapts to available space -->
    </lv_obj>
</lv_obj>
```

#### Responsive Button Grid

```xml
<lv_obj flex_flow="column" style_pad_gap="#gap_small">
    <lv_obj flex_grow="1" flex_flow="row" style_pad_gap="#gap_small">
        <lv_button flex_grow="1" height="100%">1</lv_button>
        <lv_button flex_grow="1" height="100%">2</lv_button>
        <lv_button flex_grow="1" height="100%">3</lv_button>
    </lv_obj>
    <lv_obj flex_grow="1" flex_flow="row" style_pad_gap="#gap_small">
        <lv_button flex_grow="1" height="100%">4</lv_button>
        <lv_button flex_grow="1" height="100%">5</lv_button>
        <lv_button flex_grow="1" height="100%">6</lv_button>
    </lv_obj>
</lv_obj>
```

---

## Styles & Theming

### Defining Styles

**Global styles** (in globals.xml):

```xml
<styles>
    <style name="style_button"
           bg_color="0x111"
           text_color="0xfff"
           radius="8"
           pad_all="12"/>

    <style name="style_card"
           bg_color="0x202020"
           radius="8"
           border_width="0"/>
</styles>
```

**Local styles** (in component):

```xml
<component>
    <styles>
        <style name="style_dark" bg_color="0x333"/>
    </styles>

    <view>
        <lv_obj>
            <style name="style_dark"/>
        </lv_obj>
    </view>
</component>
```

### Applying Styles

**By name:**

```xml
<lv_button>
    <style name="style_button"/>
</lv_button>
```

**With selector (state-based):**

```xml
<lv_button>
    <style name="style_base"/>
    <style name="style_pressed" selector="pressed"/>
    <style name="style_checked" selector="checked"/>
</lv_button>
```

**Inline styles:**

```xml
<lv_button style_bg_color="0x111" style_radius="8" style_pad_all="12"/>
```

### Conditional Style Binding

Dynamically apply styles based on subject value:

```xml
<lv_obj>
    <bind_style name="style_dark" subject="dark_theme" ref_value="1"/>
</lv_obj>
```

---

## Event Handling

### Custom Callback Events - Use `<lv_event-call_function>` Element

**IMPORTANT DISCREPANCY**: The LVGL online documentation references `<event_cb>`, but the actual LVGL 9 source code uses `<lv_event-call_function>` as the element name (see `lvgl/src/others/xml/lv_xml.c:113`). Use `<lv_event-call_function>` in your XML.

**⚠️ TODO**: Future refactor may be needed if LVGL standardizes on the `<event_cb>` name.

**Correct approach:**

**Step 1: Register callback in C code (BEFORE loading XML):**
```c
// Register callback with LVGL XML system
lv_xml_register_event_cb(NULL, "my_handler", my_handler_function);

// Standard LVGL event handler signature
static void my_handler_function(lv_event_t* e) {
    const char* user_data = lv_event_get_user_data(e);
    // Handle event
    LV_LOG_USER("Button clicked!");
}
```

**Step 2: Add event callback in XML using correct element name:**
```xml
<lv_button name="my_button">
    <lv_label text="Click me"/>
    <!-- Use lv_event-call_function, NOT event_cb -->
    <lv_event-call_function trigger="clicked" callback="my_handler" user_data="optional_data"/>
</lv_button>
```

**Official Documentation:** https://docs.lvgl.io/master/details/xml/ui_elements/events.html (Note: docs use `<event_cb>` but source code uses `<lv_event-call_function>`)

### XML Event Elements - Supported Types

#### Event Triggers

Common trigger values:
- `"clicked"` - Button click (press + release)
- `"pressed"` - Button pressed down
- `"released"` - Button released
- `"long_pressed"` - Long press detected
- `"long_pressed_repeat"` - Held down (repeats)
- `"all"` - Any event

**Example with user data:**
```xml
<lv_button>
    <lv_label text="Click"/>
    <event_cb callback="my_handler" trigger="clicked" user_data="button_1"/>
</lv_button>
```

**C Code Registration (MUST happen BEFORE loading XML):**
```c
// Register callback with LVGL XML system
lv_xml_register_event_cb(NULL, "my_handler", my_handler_function);

// Standard LVGL event handler signature
static void my_handler_function(lv_event_t* e) {
    const char* user_data = lv_event_get_user_data(e);

    // Handle event
    LV_LOG_USER("Button clicked: %s", user_data);
}
```

#### Subject Manipulation Events

**Set Subject Value:**
```xml
<lv_button>
    <subject_set_int_event trigger="clicked" subject="counter" value="0"/>
</lv_button>
```

**Increment/Decrement Subject:**
```xml
<lv_button>
    <lv_label text="+"/>
    <subject_increment_event trigger="clicked" subject="volume" step="1" max="100"/>
</lv_button>

<lv_button>
    <lv_label text="-"/>
    <subject_increment_event trigger="clicked" subject="volume" step="-1" min="0"/>
</lv_button>
```

#### Screen Navigation Events

**Load Existing Screen:**
```xml
<lv_button>
    <screen_load_event trigger="clicked" screen="settings_screen"
                       anim_type="fade_in" duration="300"/>
</lv_button>
```

**Create Screen Dynamically:**
```xml
<lv_button>
    <screen_create_event trigger="clicked" screen="dialog_screen"/>
</lv_button>
```

---

## Implementation Guide

### Step-by-Step Pattern

#### Step 1: Define XML Layout

Create component in `ui_xml/component_name.xml`:

```xml
<component>
    <view extends="lv_obj" width="100%" height="100%" style_bg_color="#panel_bg">
        <!-- Status label bound to subject -->
        <lv_label bind_text="status_message" style_text_font="montserrat_20"/>

        <!-- Temperature label bound to subject -->
        <lv_label bind_text="temp_value" style_text_font="montserrat_28"/>
    </view>
</component>
```

#### Step 2: Create C++ Wrapper

**Header** (`include/ui_component_name.h`):

```cpp
#pragma once
#include "lvgl/lvgl.h"

// Initialize subjects (call before creating XML)
void ui_component_name_init_subjects();

// Create component with reactive data binding
lv_obj_t* ui_component_name_create(lv_obj_t* parent);

// Update component state
void ui_component_name_update(const char* status, int temp);
```

**Implementation** (`src/ui_component_name.cpp`):

```cpp
#include "ui_component_name.h"
#include <cstdio>
#include <cstring>

static lv_obj_t* component = nullptr;

// Subjects for reactive binding
static lv_subject_t status_subject;
static lv_subject_t temp_subject;
static char status_buffer[128];
static char temp_buffer[32];

void ui_component_name_init_subjects() {
    // Initialize subjects with default values
    lv_subject_init_string(&status_subject, status_buffer, NULL,
                           sizeof(status_buffer), "Initial status");
    lv_subject_init_string(&temp_subject, temp_buffer, NULL,
                           sizeof(temp_buffer), "25 °C");

    // Register subjects globally (NULL = global scope)
    lv_xml_register_subject(NULL, "status_message", &status_subject);
    lv_xml_register_subject(NULL, "temp_value", &temp_subject);
}

lv_obj_t* ui_component_name_create(lv_obj_t* parent) {
    // Create XML component (automatically binds to registered subjects)
    component = (lv_obj_t*)lv_xml_create(parent, "component_name", nullptr);
    return component;
}

void ui_component_name_update(const char* status, int temp) {
    // Update subjects - all bound widgets update automatically
    if (status) {
        lv_subject_copy_string(&status_subject, status);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d °C", temp);
    lv_subject_copy_string(&temp_subject, buf);
}
```

#### Step 3: Register and Use

In `main.cpp`:

```cpp
// 1. Register fonts, images (from ui_fonts.h, ui_images.h)
lv_xml_register_font(NULL, "montserrat_20", &lv_font_montserrat_20);

// 2. Register XML components (globals FIRST)
lv_xml_component_register_from_file("A:/path/to/ui_xml/globals.xml");
lv_xml_component_register_from_file("A:/path/to/ui_xml/component_name.xml");

// 3. Initialize subjects (BEFORE creating XML)
ui_component_name_init_subjects();

// 4. Create component (bindings happen automatically)
lv_obj_t* panel = ui_component_name_create(screen);

// 5. Update component state (triggers reactive updates)
ui_component_name_update("Printing...", 210);
```

### Working Example: Home Panel

**Files:**
- `ui_xml/home_panel.xml` - Layout with cards, status text, printer image
- `src/ui_panel_home.cpp` - Subject management and update API
- `include/ui_panel_home.h` - Public interface

**Subjects:**
- `status_text` - Status message (string)
- `temp_text` - Temperature display (string)

**XML Bindings:**
```xml
<lv_label bind_text="status_text" style_text_color="#text_primary"/>
<lv_label bind_text="temp_text" style_text_font="montserrat_16"/>
```

**Update API:**
```cpp
ui_panel_home_init_subjects();           // Call once during startup
ui_panel_home_update("Ready", 25);       // Updates both labels instantly
```

---

## Best Practices

### Development Guidelines

1. **One subject per updatable value** - Don't share subjects across unrelated widgets
2. **Initialize → Register → Create** - Follow strict ordering for subjects
3. **Use const references** - Pass string data efficiently to update functions
4. **Descriptive subject names** - `"printer_status"` not `"data1"`
5. **Component-local wrappers** - Each XML component gets its own C++ wrapper
6. **Name-based widget lookup** - Use `lv_obj_find_by_name()` instead of child indices
7. **Global constants** - Define all dimensions, colors, and sizes in `globals.xml`
8. **Avoid magic numbers** - Use named constants like `#padding_normal` instead of `"20"`

### Widget Lookup: ALWAYS Use Names

**✓ CORRECT - Name-based lookup (resilient to layout changes):**

```xml
<lv_label name="temperature_display" bind_text="temp_text"/>
```

```cpp
lv_obj_t* widget = lv_obj_find_by_name(parent, "temperature_display");
```

**✗ WRONG - Index-based lookup (fragile, breaks when layout changes):**

```cpp
lv_obj_t* widget = lv_obj_get_child(parent, 3);  // DON'T DO THIS
```

**Benefits:**
- Layout changes (reordering, adding/removing widgets) won't break code
- Self-documenting - widget name shows intent
- Works seamlessly with XML `name` attributes

### Component Instantiation: Always Add Explicit Names

**CRITICAL:** When instantiating XML components, always add explicit `name` attributes to make them findable with `lv_obj_find_by_name()`.

**✗ WRONG - Component tag without name (not findable):**

```xml
<!-- app_layout.xml -->
<lv_obj>
  <controls_panel/>  <!-- Component tag without name attribute -->
</lv_obj>
```

```cpp
// This will FAIL - returns NULL
lv_obj_t* controls = lv_obj_find_by_name(parent, "controls_panel");
```

**✓ CORRECT - Component tag with explicit name:**

```xml
<!-- app_layout.xml -->
<lv_obj name="content_area">
  <controls_panel name="controls_panel"/>  <!-- Explicit name attribute -->
  <home_panel name="home_panel"/>
  <settings_panel name="settings_panel"/>
</lv_obj>
```

```cpp
// This WORKS - finds the component
lv_obj_t* controls = lv_obj_find_by_name(parent, "controls_panel");
if (controls) {
    lv_obj_clear_flag(controls, LV_OBJ_FLAG_HIDDEN);
}
```

**Why this matters:**
- Component names defined in `<view name="...">` inside the component definition are **not** automatically applied to component instantiation tags
- Without explicit names, components cannot be found with `lv_obj_find_by_name()` from other panels
- This pattern enables clean panel-to-panel communication (e.g., motion panel showing controls panel when back button is pressed)

**When to use:**
- ✓ Always add names to component instantiation tags in layout files
- ✓ Use names when components need to be shown/hidden from other panels
- ✓ Use names when components need to be referenced in C++ code

### Subject Management

**String subject buffers must be persistent:**

```cpp
// ✓ CORRECT - Static or heap-allocated
static char status_buffer[128];
lv_subject_init_string(&subject, status_buffer, NULL, sizeof(status_buffer), "Initial");

// ✗ WRONG - Stack-allocated (will be destroyed)
char buffer[128];
lv_subject_init_string(&subject, buffer, ...);
```

### Theme System

**Centralize all UI constants:**

```xml
<!-- In globals.xml -->
<consts>
    <color name="primary_color" value="0xff4444"/>
    <px name="padding_normal" value="20"/>
    <px name="accent_bar_width" value="4"/>
</consts>
```

**Benefits:**
- Single source of truth for colors and dimensions
- Easy to adjust theme project-wide
- Consistency across all components

### Auto-Generated Icon Constants

FontAwesome icons are auto-generated to avoid UTF-8 encoding issues:

```bash
# Regenerate after adding icons to ui_fonts.h
python3 scripts/generate-icon-consts.py
```

The script reads icon definitions from `include/ui_fonts.h` and writes UTF-8 byte sequences to `globals.xml`.

**Usage in XML:**
```xml
<lv_label text="#icon_home" style_text_font="fa_icons_64"/>
```

---

## Troubleshooting

### Critical XML Attribute Gotchas ⚠️

**IMPORTANT:** LVGL 9 XML parser has several common pitfalls that cause silent failures:

#### 1. zoom Attribute Doesn't Exist

**Problem:** Using `zoom="..."` on `lv_image` elements has no effect
```xml
<!-- ❌ WRONG - zoom doesn't exist in LVGL 9 -->
<lv_image src="my_icon" zoom="128"/>
```

**Solution:** Use `scale_x` and `scale_y` where 256 = 100%
```xml
<!-- ✅ CORRECT - scale_x and scale_y (256 = 100%) -->
<lv_image src="my_icon" scale_x="128" scale_y="128"/>  <!-- 50% size -->
<lv_image src="my_icon" scale_x="512" scale_y="512"/>  <!-- 200% size -->
```

**Valid lv_image Attributes:**
- `src` - image source
- `inner_align` - alignment within widget bounds
- `rotation` - rotation angle (0-3600, in 0.1 degree units)
- `scale_x`, `scale_y` - scaling (256 = 100%)
- `pivot` - rotation pivot point

Source: `/lvgl/src/others/xml/parsers/lv_xml_image_parser.c:63-72`

#### 2. Must Use Full Words, Not Abbreviations

**Problem:** Using `style_img_recolor` instead of `style_image_recolor`
```xml
<!-- ❌ WRONG - 'img' abbreviation not recognized -->
<lv_image src="icon" style_img_recolor="#ff0000" style_img_recolor_opa="255"/>
```

**Solution:** Always use full words in XML attribute names
```xml
<!-- ✅ CORRECT - full word 'image' required -->
<lv_image src="icon" style_image_recolor="#ff0000" style_image_recolor_opa="255"/>
```

**Common Mistakes:**
- `style_img_recolor` → `style_image_recolor` ✅
- `style_txt_color` → `style_text_color` ✅
- `style_bg_color` → `style_bg_color` ✅ (already correct)

Source: `/lvgl/src/others/xml/lv_xml_style.c:240-241`

#### 3. Silent Failures

**Problem:** Unknown attributes are silently ignored without warnings

**Solution:** When XML attributes don't work:
1. Check parser source code in `/lvgl/src/others/xml/parsers/`
2. Verify exact attribute names (no abbreviations)
3. Check if attribute exists for that widget type
4. Test with known-working examples first

### Common Issues

#### ⚠️ LV_SIZE_CONTENT Evaluation to Zero (CRITICAL)

**Issue:** `LV_SIZE_CONTENT` frequently evaluates to 0, causing widgets to be invisible or improperly sized.

**Symptoms:**
- Labels with `width="LV_SIZE_CONTENT"` render with zero width (invisible)
- Containers with `height="LV_SIZE_CONTENT"` collapse to 0 height
- Flex layouts show children at (0,0) with no size

**Root Cause - Deferred Layout Calculation:**

LVGL 9 uses **deferred layout** where sizes aren't calculated immediately when set. Layout updates happen in multiple passes:

1. Children are processed first (recursively)
2. Parent's size is refreshed via `lv_obj_refr_size()`
3. Layout is applied to children

When `lv_obj_refr_size()` calculates `LV_SIZE_CONTENT`, it reads child coordinates to determine the bounding box. **If children haven't been laid out yet**, their coordinates are still at defaults (0x0), causing the parent to calculate size as 0.

**Source:** `lvgl/src/core/lv_obj_pos.c:1077-1170, 1224-1247`

**Verified Test Results** (from `test_size_content.cpp`):

```
Test 1: Label with LV_SIZE_CONTENT
  BEFORE lv_obj_update_layout(): width=0, height=0
  AFTER lv_obj_update_layout():  width=72, height=16  ✓

Test 2: Container with LV_SIZE_CONTENT + child
  BEFORE adding child:           width=0, height=0
  AFTER adding child (no update): width=0, height=0
  AFTER lv_obj_update_layout():  width=125, height=60 ✓

Test 4: Flex container with LV_SIZE_CONTENT
  BEFORE lv_obj_update_layout(): width=0, height=0
  AFTER lv_obj_update_layout():  width=234, height=62 ✓

Test 5: Nested LV_SIZE_CONTENT
  BEFORE update:                 Outer=0x0, Inner=0x0
  AFTER update:                  Outer=204x104, Inner=160x60 ✓
```

**When LV_SIZE_CONTENT Evaluates to Zero:**

1. **Container queried before layout completes** - Layout update deferred via `layout_inv` flag
2. **Container created but children not yet added** - No children to measure
3. **All children hidden or floating** - Calculation skips them, returns only intrinsic size

**When LV_SIZE_CONTENT DOES Work (Verified):**

1. **After `lv_obj_update_layout()` is called** - Forces immediate layout calculation
2. **Widgets with intrinsic size** - Labels, buttons, checkboxes, images default to LV_SIZE_CONTENT
3. **Flex containers** - Works correctly with flex_flow when layout is updated
4. **Nested SIZE_CONTENT** - Parent and child both with SIZE_CONTENT work when updated
5. **Percentage children** - LVGL 9 handles this intelligently (see Test 3: calculated 88x94, not 0)

**Solution: Our Application Calls lv_obj_update_layout() Strategically**

LV_SIZE_CONTENT is **ENCOURAGED** for appropriate use cases. Our application ensures layout updates happen at key points:

```cpp
// Main UI initialization (main.cpp:671)
lv_obj_t* app_layout = lv_xml_create(screen, "app_layout", NULL);
lv_obj_update_layout(screen);  // Ensures all SIZE_CONTENT widgets sized

// Dynamic component creation
lv_obj_t* panel = lv_xml_create(parent, "my_panel", NULL);
lv_obj_update_layout(panel);  // Force layout calculation
int32_t width = lv_obj_get_width(panel);  // Now accurate
```

**When to Use LV_SIZE_CONTENT (Encouraged ✅):**

```xml
<!-- ✅ EXCELLENT - Flex container auto-sizes to children -->
<lv_obj flex_flow="row" width="LV_SIZE_CONTENT" height="LV_SIZE_CONTENT"
        style_pad_all="#padding_small">
    <lv_button>Action</lv_button>
    <lv_button>Cancel</lv_button>
</lv_obj>

<!-- ✅ GOOD - Label sizes to dynamic text -->
<lv_label text="$status_message"
          width="LV_SIZE_CONTENT"
          height="LV_SIZE_CONTENT"/>

<!-- ✅ GOOD - Vertical stack sizes to content -->
<lv_obj flex_flow="column" width="100%" height="LV_SIZE_CONTENT">
    <!-- Dynamic children added in C++ -->
</lv_obj>
```

**When to Use Explicit Dimensions:**

```xml
<!-- ✓ Fixed-size containers (nav bar, headers) -->
<lv_obj width="#nav_width" height="100%"/>

<!-- ✓ Grid layouts requiring precise cell sizing -->
<lv_obj width="200" height="150"/>

<!-- ✓ Percentage-based responsive layouts -->
<lv_obj width="33%" height="100%"/>
```

**Circular Dependency Handling:**

LVGL 9 has intelligent circular dependency prevention:
- Parent has `width/height = LV_SIZE_CONTENT`
- Child has `width/height = "100%"` (percentage)

**In LVGL 8:** Child was sized to 0 to prevent infinite loops
**In LVGL 9:** Sizes both correctly using sophisticated layout solver (verified in test_size_content.cpp Test 3)

**Source:** `lvgl/src/core/lv_obj_pos.c:114-123, 145-155`

**Best Practices:**

1. **Use LV_SIZE_CONTENT freely in XML** - Our application ensures layout updates at strategic points

2. **For C++ dynamic creation:** ALWAYS call `lv_obj_update_layout()` after creating widgets
   ```cpp
   lv_obj_t* container = lv_xml_create(parent, "component", NULL);
   lv_obj_update_layout(container);  // CRITICAL - ensures SIZE_CONTENT calculates
   int32_t width = lv_obj_get_width(container);  // Now accurate
   ```

3. **Troubleshooting layout issues:** If you encounter sizing problems (0x0, collapsed, invisible):
   - **BE SKEPTICAL** that `lv_obj_update_layout()` has been called recently enough
   - Test by adding layout update after XML creation
   - Check test_size_content.cpp for diagnostic patterns

4. **Prefer semantic constants** for explicit dimensions:
   ```xml
   <!-- ✅ Use globals.xml constants -->
   <lv_obj width="#nav_width" height="#button_height" style_bg_color="#panel_bg"/>

   <!-- ❌ Avoid magic numbers -->
   <lv_obj width="102" height="48" style_bg_color="0x1a1a1a"/>
   ```

5. **Strategic layout update locations** in our codebase:
   - Main UI: main.cpp:671 (after app_layout creation)
   - Print select: ui_panel_print_select.cpp:515, 675
   - Print status: ui_panel_print_status.cpp:326
   - Step progress: ui_step_progress.cpp:300, 343

#### "No constant was found with name X"

**Cause:** Constant not defined in globals.xml or not registered before component creation.

**Fix:** Ensure `globals.xml` is registered first:
```cpp
lv_xml_component_register_from_file(".../globals.xml");  // FIRST
lv_xml_component_register_from_file(".../other.xml");    // Then others
```

#### Labels not updating when subject changes

**Cause:** Subject not registered before XML creation, or wrong subject name.

**Fix:**
1. Check registration order (initialize → register → create XML)
2. Verify `bind_text="subject_name"` matches registered name
3. Add debug printf() in update function to verify calls

#### Centering not working

**Cause 1:** Using `flex_align` instead of `style_flex_*` properties.

**Fix:**
```xml
<!-- ✗ DOESN'T WORK -->
<lv_obj flex_align="center center center">

<!-- ✓ WORKS -->
<lv_obj style_flex_main_place="center" style_flex_cross_place="center">
```

**Cause 2:** Flex layout conflicts with `align="center"` attribute.

**Fix:** Remove `flex_flow` from parent when centering a single child:
```xml
<!-- ✗ DOESN'T WORK - flex overrides align -->
<lv_obj flex_flow="column" style_flex_main_place="center">
    <lv_obj align="center"><!-- off-center --></lv_obj>
</lv_obj>

<!-- ✓ WORKS - no flex, pure absolute positioning -->
<lv_obj>
    <lv_obj align="center"><!-- perfectly centered --></lv_obj>
</lv_obj>
```

See "Flex Layout Can Conflict with align="center"" section for details.

#### Vertical centering fails

**Cause:** Container missing `height="100%"`.

**Fix:**
```xml
<lv_obj flex_flow="column" height="100%"
        style_flex_main_place="center"
        style_flex_cross_place="center">
```

#### Icons not centered in buttons

**Cause:** Missing `align="center"` on label.

**Fix:**
```xml
<lv_button>
    <lv_label align="center" text="#icon_home"/>
</lv_button>
```

#### Flex items not distributing evenly

**Cause:** Not using `flex_grow` on children.

**Fix:**
```xml
<lv_obj flex_flow="row">
    <lv_obj flex_grow="1">Item 1</lv_obj>
    <lv_obj flex_grow="1">Item 2</lv_obj>
    <lv_obj flex_grow="1">Item 3</lv_obj>
</lv_obj>
```

### Debugging Checklist

When layouts aren't working:

- [ ] Does the label have `style_text_align="center"` **AND** `width="100%"`?
- [ ] Does the parent container have `flex_flow` set correctly (row/column)?
- [ ] Does the parent use `style_flex_main_place` and `style_flex_cross_place` (NOT `flex_align`)?
- [ ] Are children using `flex_grow="1"` for equal space distribution?
- [ ] Does the container have `height="100%"` (required for vertical centering)?
- [ ] Have you added debug background colors to visualize actual container sizes?
- [ ] Are you avoiding mixing absolute positioning with flex layouts?
- [ ] Have you verified no `flex_align` attributes remain (they're silently ignored)?

### Visual Debugging

Unlike web browsers, there's no inspect tool. Use temporary background colors:

```xml
<lv_obj style_bg_color="#ff0000" style_bg_opa="100%">
    <!-- Check if this container has the expected size -->
</lv_obj>
```

### What DOESN'T Work in LVGL 9 XML

❌ **`flex_align` attribute** - Silently ignored, use `style_flex_*` instead
❌ **`style_flex_grow`** - Doesn't exist, use `flex_grow` attribute instead
❌ **Mixing explicit percentage widths with `flex_grow`** - Use one or the other
❌ **Stack-allocated string buffers for subjects** - Must be static or heap-allocated
❌ **Creating XML before registering subjects** - Bindings will fail silently

---

## Resources

### Official Documentation

- **LVGL XML Docs:** https://docs.lvgl.io/master/details/xml/
- **Subject-Observer:** https://docs.lvgl.io/master/details/auxiliary-modules/observer/observer.html
- **LVGL Events:** https://docs.lvgl.io/latest/en/html/details/xml/ui_elements/events.html
- **LVGL Editor Tutorials:** https://github.com/lvgl/lvgl_editor/tree/master/tutorials

### Project Examples

- **Navigation bar:** `ui_xml/navigation_bar.xml`
- **Home panel:** `ui_xml/home_panel.xml` + `src/ui_panel_home.cpp`
- **App layout:** `ui_xml/app_layout.xml`
- **Global constants:** `ui_xml/globals.xml`

### Related Project Documentation

- **CLAUDE.md** - Claude Code context for this project
- **STATUS.md** - Documentation guide and architectural decisions
- **HANDOFF.md** - Current work status and priorities
- **README.md** - Project overview and quick start
- **ROADMAP.md** - Planned features and milestones

---

## Document History

**2025-01-29:** Added verified LVGL 9.4 API patterns:
- Section 3: Runtime Constants & Dynamic Configuration - Complete API reference with responsive design pattern
- Section 4: Advanced Child Element Bindings - Expanded with state bindings, style bindings, and limitations
- Conditional Binding Limitations - Documented what doesn't exist (text conditionals, style property conditionals)
- Real-world examples for wizard containers and responsive UIs
- Source code verified (lvgl/src/others/xml/)

**2025-01-11:** Initial consolidated guide combining:
- LVGL_XML_REFERENCE.md - XML syntax reference
- LVGL9_CENTERING_GUIDE.md - Centering techniques and flex_align discovery
- XML_UI_SYSTEM.md - Architecture and implementation patterns
- Vertical Accent Bar Pattern - New UI pattern documentation

**Contributors:** HelixScreen development team, based on LVGL 9.4 documentation and extensive prototype testing.
