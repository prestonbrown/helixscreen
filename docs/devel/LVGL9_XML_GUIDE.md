# LVGL 9 XML UI System - Complete Guide

Comprehensive guide to the declarative XML UI system with reactive data binding, based on practical experience building the HelixScreen UI. The XML engine lives in `lib/helix-xml/` — a permanent MIT-licensed fork of LVGL's XML engine taken at `a15dcbeb5` (`v9.4.0-358`), the last commit before v9.5 removed XML from core. It has no upstream; see `HELIX_XML_FORK.md`.

**Last Updated:** 2026-07-15

---

## Table of Contents

1. [Overview & Architecture](#overview--architecture)
2. [Project Structure](#project-structure)
3. [Core Concepts](#core-concepts)
4. [Layouts & Positioning](#layouts--positioning)
5. [Common UI Patterns](#common-ui-patterns)
6. [Responsive Design](#responsive-design)
7. [Styles & Theming](#styles--theming)
8. [Event Handling](#event-handling)
9. [Implementation Guide](#implementation-guide)
10. [Best Practices](#best-practices)
11. [Troubleshooting](#troubleshooting)

---

## Overview & Architecture

LVGL 9's XML system enables declarative UI development with reactive data binding through the Subject-Observer pattern. This separates UI layout (XML) from business logic (C++), similar to React or Vue.

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

### Reactive Data Binding is MANDATORY

**ALL UI updates MUST use reactive data binding. Direct widget manipulation is an anti-pattern.**

```xml
<!-- ✅ CORRECT - Reactive binding in XML -->
<lv_label bind_text="status_message"/>
<lv_button>
  <bind_flag_if_eq subject="connection_ready" flag="clickable" ref_value="1"/>
</lv_button>
```

```cpp
// ✅ CORRECT - Update subjects in C++
lv_subject_set_string(&status_message, "Connected");
lv_subject_set_int(&connection_ready, 1);
// UI updates automatically
```

---

## Project Structure

### HelixScreen Directory Layout

```
helixscreen/
├── ui_xml/                    # 60+ XML component definitions
│   ├── globals.xml            # Theme constants, responsive tokens
│   ├── app_layout.xml         # Root: navbar + content area
│   ├── navigation_bar.xml     # Vertical nav buttons
│   ├── *_panel.xml            # Main panels (home, controls, motion, etc.)
│   ├── *_overlay.xml          # Modal overlays
│   ├── *_modal.xml            # Dialog modals
│   └── icon.xml               # Icon custom widget
│                              # (text_heading/text_body/text_small/spinner are
│                              #  C++-registered widgets, not XML files — see below)
├── src/
│   ├── main.cpp               # Entry point, initialization
│   ├── xml_registration.cpp   # Component registration
│   └── ui/
│       ├── theme_manager.cpp  # Responsive token + theme registration
│       ├── ui_text.cpp        # text_heading/text_body/text_small widgets
│       ├── ui_spinner.cpp     # spinner widget
│       ├── ui_nav_manager.cpp # Navigation system
│       └── ui_panel_*.cpp     # Panel logic with subjects
├── include/
│   ├── ui_icon_codepoints.h   # MDI icon definitions
│   └── ui_*.h                 # Panel headers
├── assets/
│   ├── fonts/                 # MDI icon fonts, Montserrat
│   └── images/                # UI images
└── docs/devel/
    ├── LVGL9_XML_GUIDE.md             # This file
    └── LVGL9_XML_ATTRIBUTES_REFERENCE.md  # Quick-lookup cheatsheet
```

### Registration Flow (main.cpp + xml_registration.cpp)

```cpp
// 1. Register fonts
lv_xml_register_font(NULL, "montserrat_16", &lv_font_montserrat_16);
lv_xml_register_font(NULL, "montserrat_20", &lv_font_montserrat_20);

// 2. Register globals FIRST (constants must be available)
lv_xml_register_component_from_file("A:ui_xml/globals.xml");

// 3. Register responsive spacing tokens (both take the lv_display_t*)
theme_manager_register_responsive_spacing(display);  // Sets #space_md, #space_lg, etc.
theme_manager_register_responsive_fonts(display);    // Sets #font_body, etc.

// 4. Register components (order doesn't matter after globals)
lv_xml_register_component_from_file("A:ui_xml/icon.xml");
lv_xml_register_component_from_file("A:ui_xml/text_heading.xml");
lv_xml_register_component_from_file("A:ui_xml/home_panel.xml");
// ... etc
```

---

## Core Concepts

> **Engine semantics live in the engine repo.** Subjects, every binding element,
> the expression language, `<if>`/`<repeat>`, and the two silent-failure rules
> (`cond=` vs `<subject_expr>` resolution order; flag binds being two-way) are
> documented — and pinned by tests — in
> [`lib/helix-xml/docs/BINDINGS.md`](../../lib/helix-xml/docs/BINDINGS.md).
> Read that for what a construct *does*.
>
> What follows is HelixScreen's use of it: our widgets (`ui_button`, `ui_card`,
> `text_*`), our design tokens, our `ui_breakpoint` tiers, and the conventions
> that only make sense against this codebase.

### 1. XML Components

Components are reusable UI pieces defined with the `<component>` tag.

#### Basic Structure

```xml
<component>
    <!-- Optional: Component API (properties from parent) -->
    <api>
        <prop name="text" type="string" default="Click me"/>
        <prop name="enabled" type="bool" default="true"/>
    </api>

    <!-- Optional: Local constants -->
    <consts>
        <px name="button_size" value="36"/>
    </consts>

    <!-- Optional: Local styles (NO style_ prefix!) -->
    <styles>
        <style name="style_base" bg_color="0x333" text_color="0xfff"/>
    </styles>

    <!-- The actual UI definition -->
    <view extends="lv_button" width="#button_size">
        <!-- Use API props with $ prefix -->
        <lv_label text="$text" align="center"/>
        <style name="style_base"/>
    </view>
</component>
```

#### Property Types

| Type | Description | Example |
|------|-------------|---------|
| `string` | Text values | `default="Hello"` |
| `int` | Integer numbers | `default="42"` |
| `bool` | true/false | `default="true"` |
| `color` | Hex colors | `default="0xff4444"` |
| `subject` | Subject references | For data binding |

### 2. Subjects (Reactive Data)

Subjects are observable data containers that automatically update bound widgets.

#### Subject Types

```cpp
lv_subject_init_string()  // String data (text labels)
lv_subject_init_int()     // Integer data (sliders, counters)
lv_subject_init_pointer() // Pointer data (custom objects)
lv_subject_init_color()   // Color data (dynamic theming)
```

#### Subject Lifecycle

```cpp
// 1. Create subject in C++
static lv_subject_t status_subject;
static char status_buffer[128];

// 2. Initialize with default value
lv_subject_init_string(&status_subject, status_buffer, NULL,
                       sizeof(status_buffer), "Initial status");

// 3. Register globally (BEFORE creating XML)
lv_xml_register_subject(NULL, "status_text", &status_subject);

// 4. Create XML (widgets automatically bind)
lv_obj_t* panel = lv_xml_create(parent, "home_panel", nullptr);

// 5. Update subject (all bound widgets update automatically)
lv_subject_copy_string(&status_subject, "New status");
```

**CRITICAL:** Register subjects BEFORE creating XML components that bind to them.

#### Static Buffers Required

```cpp
// ✅ CORRECT - Static or heap-allocated
static char status_buffer[128];
lv_subject_init_string(&subject, status_buffer, NULL, sizeof(status_buffer), "Initial");

// ❌ WRONG - Stack-allocated (will be destroyed)
char buffer[128];  // DANGER: Goes out of scope!
lv_subject_init_string(&subject, buffer, ...);
```

### 3. Data Binding

#### Sigil Conventions

LVGL XML uses prefix sigils to distinguish different value types:

| Sigil | Meaning | Example | Context |
|-------|---------|---------|---------|
| `#` | Design token / const | `style_pad_all="#space_md"` | Spacing, colors, sizes |
| `$` | Component prop | `text="$primary_text"` | Inside component templates |
| `$i` | `<repeat>` loop index | `text="$i"` | Inside a `<repeat>` body (see [Repeating fragments](#repeating-fragments-with-repeat)) |
| `${expr}` | Embedded composition / integer expression | `bind_text="slot_${i + 1}_label"`, `style_translate_x="${i * 84}"` | Splices a bare name (`${i}`, `${grp}`) or evaluates an integer expression and splices the result. See [Repeating fragments](#repeating-fragments-with-repeat) |
| `@` | Subject binding | `text="@my_subject"` | Reactive data on `ui_button` |

The `@` prefix on `ui_button`'s `text` attribute marks a value as a subject reference (reactive) vs. a literal string (static). Alternatively, `bind_text` always treats its value as a subject name (no `@` needed). See [ui_button](#ui_button) for details.

#### Simple Attribute Bindings

```xml
<!-- Bind label text to string subject -->
<lv_label bind_text="status_text"/>

<!-- Bind with format string -->
<lv_label bind_text="temp_value" bind_text-fmt="%.1f°C"/>

<!-- Bind slider value to integer subject -->
<lv_slider bind_value="volume" range="0 100"/>
```

> **Reactive color:** there is no `bind_style_<prop>` attribute handler. To make a
> color react to a subject, define two `<style>`s and swap them with a
> `bind_style_if_eq` / `bind_style_if` on the widget (see the "Reactive styles"
> section below), or drive the color through a themed token.

> **Note:** Standard LVGL widgets (`lv_label`, `lv_slider`) resolve `bind_text` directly as a subject name. The `@` prefix convention is specific to `ui_button`, which needs to disambiguate between literal button labels and subject references.

#### Inline Text Content

Widgets that understand `text=` also accept inline element content, HTML-style:

```xml
<text_muted>Print speed</text_muted>
<!-- equivalent to: -->
<text_muted text="Print speed" translation_tag="Print speed"/>
```

**Inline text is translatable by default.** The literal string is used as the
translation key (and as the fallback when no translation exists), and the label
re-resolves on language change -- same behavior as the `label=`/`label_tag=`
pairs on setting rows. `make translation-sync` extracts inline text
automatically. Use `text="..."` instead when a string must stay untranslated
(versions, IPs, device names).

Rules:

- **Attributes win.** If the element also has `text=`, `bind_text=`, or
  `translation_tag=`, the inline text is dropped with a runtime warning.
- **Whitespace collapses HTML-style.** Leading/trailing whitespace is trimmed
  and internal runs (including newlines -- even explicit `&#10;`) collapse to a
  single space. For multi-line label text, use `text="Line1&#10;Line2"`.
- **`$prop` / `#const` resolve** exactly like attribute values (whole-value):
  `<text_muted>$title</text_muted>` works inside a component view.
- **Literal text starting with `$` or `#` is not supported.** It's parsed as a
  prop/const reference; if the name doesn't resolve, the text is dropped with
  a warning instead of rendering literally. Use `text="$5.00"` for a literal
  string that happens to start with one of these sigils.
- **Mixed content is allowed**: text before/after child elements applies to the
  containing element; children are unaffected.
- Widgets that don't understand `text` ignore inline content silently, like any
  unknown attribute.
- Inline text on the root `<view>` element of a component is not supported --
  it's silently dropped. Use `text=`/`bind_text=` on the view's opening tag, or
  put the inline text on a child element instead.

#### Conditional Flag Bindings (Show/Hide)

```xml
<lv_obj>
    <!-- Hide when current_step == 1 -->
    <bind_flag_if_eq subject="current_step" flag="hidden" ref_value="1"/>

    <!-- Disable when level >= 100 -->
    <bind_flag_if_ge subject="level" flag="disabled" ref_value="100"/>
</lv_obj>
```

**Available Operators:**

| Element | Condition |
|---------|-----------|
| `<bind_flag_if_eq>` | `subject == ref_value` |
| `<bind_flag_if_not_eq>` | `subject != ref_value` |
| `<bind_flag_if_gt>` | `subject > ref_value` |
| `<bind_flag_if_ge>` | `subject >= ref_value` |
| `<bind_flag_if_lt>` | `subject < ref_value` |
| `<bind_flag_if_le>` | `subject <= ref_value` |

**Supported Flags:** `hidden`, `clickable`, `checkable`, `scrollable`, `disabled`, `ignore_layout`, `floating`

#### Conditional State Bindings

Control visual states (disabled styling, checked styling):

```xml
<lv_button>
    <!-- Disable when WiFi is off -->
    <bind_state_if_eq subject="wifi_enabled" state="disabled" ref_value="0"/>
</lv_button>

<lv_checkbox>
    <!-- Check when dark mode is on -->
    <bind_state_if_eq subject="dark_mode" state="checked" ref_value="1"/>
</lv_checkbox>
```

**Difference:** Flags control behavior; States control visual appearance.

#### Conditional Style Bindings

Apply entire style objects conditionally:

```xml
<styles>
    <style name="temp_normal" text_color="0xffffff"/>
    <style name="temp_warning" text_color="0xffaa00"/>
    <style name="temp_critical" text_color="0xff0000"/>
</styles>

<lv_label bind_text="temperature">
    <bind_style name="temp_normal" subject="temp_state" ref_value="0"/>
    <bind_style name="temp_warning" subject="temp_state" ref_value="1"/>
    <bind_style name="temp_critical" subject="temp_state" ref_value="2"/>
</lv_label>
```

**⚠️ CRITICAL: Style Priority**

Inline style attributes (e.g., `style_bg_color="#card_bg"`) have **higher priority** than `bind_style` in LVGL's style cascade. If you set an inline style on an element, `bind_style` cannot override that property.

```xml
<!-- ❌ WRONG - inline bg_color will override bind_style -->
<lv_button style_bg_color="#card_bg">
    <bind_style name="active_style" subject="is_active" ref_value="1"/>
</lv_button>

<!-- ✅ CORRECT - use TWO bind_styles, no inline bg_color -->
<lv_button>
    <bind_style name="inactive_style" subject="is_active" ref_value="0"/>
    <bind_style name="active_style" subject="is_active" ref_value="1"/>
</lv_button>
```

**Rule:** When using `bind_style` for reactive visual changes, do NOT set inline style attributes for the properties you want to change reactively.

**⚠️ Moving `flex_flow` into a style? Set `layout="flex"` there too.**

The inline `flex_flow="column"` attribute sets *two* things — `LV_STYLE_FLEX_FLOW` **and**
`LV_STYLE_LAYOUT`. A `<style>` sets only the property you name. So obeying the rule above and
lifting `flex_flow` out of the element into two bound styles silently removes the layout, and
a flex container with a flow but no layout runs **no layout at all**: every child lands
stacked on the container's origin, same x, same y. Nothing warns.

```xml
<!-- ❌ WRONG - children all pile up at the origin -->
<style name="list_micro" flex_flow="row_wrap"/>
<style name="list_wide"  flex_flow="column"/>

<!-- ✅ CORRECT -->
<style name="list_micro" layout="flex" flex_flow="row_wrap"/>
<style name="list_wide"  layout="flex" flex_flow="column"/>
```

Symptom to recognise: `helix-screen ctl geom <container> 3` reports every child at identical
`x`/`y`, and the container's height collapses to one row. See
`ui_xml/ams_environment_overlay.xml` for a worked example (#1192).

#### Applying One Style to Multiple Parts (`parts=...`)

For widgets with several parts that should share the same reactive style — arc background+indicator at the same stroke width, slider track+indicator+knob at the same color — `bind_style` and every `bind_style_if_*` variant accept a `parts="..."` attribute that takes a comma-separated list of part names:

| Part name | LV_PART_* |
|-----------|-----------|
| `main` | `LV_PART_MAIN` |
| `scrollbar` | `LV_PART_SCROLLBAR` |
| `indicator` | `LV_PART_INDICATOR` |
| `knob` | `LV_PART_KNOB` |
| `selected` | `LV_PART_SELECTED` |
| `items` | `LV_PART_ITEMS` |
| `cursor` | `LV_PART_CURSOR` |

```xml
<styles>
    <style name="arc_w_8" arc_width="8"/>
</styles>

<!-- One line applies arc_w_8 to LV_PART_MAIN AND LV_PART_INDICATOR. -->
<lv_arc>
    <bind_style name="arc_w_8" parts="main,indicator"
                subject="arc_thickness_tier" ref_value="2"/>
</lv_arc>
```

State bits from `selector` (if present) are preserved across each part — `parts="main,indicator" selector="pressed"` applies the style to both parts in the pressed state.

**Without `parts`**, the existing `selector="indicator"` form (single part + optional state) still works. `parts` is an opt-in extension for the multi-part case; it's helix-xml's extension over upstream LVGL XML.

Max 8 parts per attribute (more than enough — even sliders only have 3-4).

#### Conditional Style Bindings with Comparison Operators

`bind_style_if_*` elements apply a style only when the subject value matches a comparison condition. Unlike `bind_style` (which only does exact match), these support all six comparison operators:

| Element | Condition |
|---------|-----------|
| `<bind_style_if_eq>` | `subject == ref_value` |
| `<bind_style_if_not_eq>` | `subject != ref_value` |
| `<bind_style_if_gt>` | `subject > ref_value` |
| `<bind_style_if_ge>` | `subject >= ref_value` |
| `<bind_style_if_lt>` | `subject < ref_value` |
| `<bind_style_if_le>` | `subject <= ref_value` |

Attributes are the same as `bind_style`: `name` (style name), `subject` (subject name), `ref_value` (comparison value), and optional `selector` (part+state selector) or `parts` (comma-list of parts — see "Applying One Style to Multiple Parts" above).

```xml
<styles>
  <style name="pad_micro" pad_left="8" pad_right="8"/>
  <style name="pad_standard" pad_left="16" pad_right="16"/>
</styles>

<lv_obj>
  <!-- Compact padding on Micro breakpoint (index 0) -->
  <bind_style_if_eq name="pad_micro" subject="ui_breakpoint" ref_value="0"/>
  <!-- Standard padding on Tiny and above (index >= 1) -->
  <bind_style_if_ge name="pad_standard" subject="ui_breakpoint" ref_value="1"/>
</lv_obj>
```

**Why use `bind_style_if_*` instead of `bind_style`?** The `bind_style` element only matches exact values, so you need one `bind_style` per possible value. With `bind_style_if_ge`, a single element covers all breakpoints above a threshold. This is essential for responsive styling where you have 7 breakpoint tiers.

**CRITICAL: Remove inline styles when using `bind_style_if_*`.** The same priority rule applies as with `bind_style` -- inline `style_*` attributes always win over added styles. When switching padding responsively, do NOT set `style_pad_left` on the element; use two `bind_style_if_*` elements instead.

#### Expression Conditionals

Every `bind_flag_if_*` / `bind_state_if_*` / `bind_style_if_*` variant above compares **one** subject against **one** `ref_value`. When a condition needs to combine multiple subjects (`error_flag OR temp > threshold`) or do arithmetic, use the expression evaluator instead of stacking several single-subject binds or writing a hand-rolled C++ derived subject.

The evaluator is an integer-only expression language over subjects: nonzero is truthy, the result is always an int, division/modulo by zero evaluate to `0` instead of crashing. It's exposed through four constructs:

**1. `<subject_expr>` — a derived subject, kept in sync**

Inside a component's `<subjects>` block, `<subject_expr name="X" expr="EXPR"/>` creates an int subject `X` that recomputes and updates automatically whenever any subject referenced by `EXPR` changes. It's a sibling of `<subject>`/`<int>` entries, and it can itself be referenced by widget bindings just like any other subject.

**Every subject referenced by `expr` must already be declared** before the `<subject_expr>` line — either globally (C++-registered *and registered early enough*, see below) or earlier in the same `<subjects>` block. Forward references don't compile (the XML parser logs a warning and the derived subject is silently not registered).

##### Which phase registers the subject decides which construct you can use

`<subject_expr>` resolves its operands when the **component is registered**
(Phase 8c, `register_xml_components()`); `cond=` resolves when the **view is
created**. So an expression over a subject that some `init_subjects()` creates
at Phase 9a compiles only in the `cond=` form — as a `<subject_expr>` it
silently never registers.

Full rule, both directions, and the tests that pin it:
[`lib/helix-xml/docs/BINDINGS.md` § *When each is resolved*](../../lib/helix-xml/docs/BINDINGS.md#when-each-is-resolved--the-trap).
HelixScreen phases are in
[`architecture/01-declarative-ui.md`](architecture/01-declarative-ui.md).
Worked examples in this tree: `ui_xml/temp_graph_overlay.xml`
(`temp_graph_mode`) and `ui_xml/header_bar.xml` (`any_tool_z_dirty`).

**2. `<bind_flag_if cond="EXPR" flag="FLAG" invert="true|false"/>`** — child of any object. Adds `flag` when `EXPR` is truthy, removes it when falsy. `invert="true"` flips that (apply when falsy) — the common case for `flag="hidden"` when the markup wants to read as "show when `cond`" instead of "hide when `cond`":

```xml
<lv_obj>
    <bind_flag_if cond="demo_alarm" flag="hidden" invert="true"/>
    <text_heading text="ALARM"/>
</lv_obj>

<!-- Direct multi-subject expression, no subject_expr needed -->
<lv_obj>
    <bind_flag_if cond="demo_temp gt demo_threshold" flag="hidden" invert="true"/>
    <text_body text="Temp is over threshold"/>
</lv_obj>
```

**3. `<bind_state_if cond="EXPR" state="STATE" invert="..."/>`** — same semantics, toggling an `lv_state_t` (e.g. `disabled`, `checked`) instead of a flag:

```xml
<ui_button text="Action">
    <bind_state_if cond="demo_alarm" state="disabled"/>
</ui_button>
```

**4. `<bind_style_if cond="EXPR" name="STYLE" selector="..." parts="..." invert="..."/>`** — same expression-driven pattern for style enable/disable, mirroring `bind_style_if_eq` (`name`, `selector`/`parts` behave identically — see "Applying One Style to Multiple Parts" above):

```xml
<styles>
    <style name="demo_alarm_style" bg_color="#warning" bg_opa="255"/>
</styles>
<ui_card>
    <bind_style_if name="demo_alarm_style" cond="demo_alarm"/>
    <text_body text="Styled by bind_style_if"/>
</ui_card>
```

**Grammar** (integer-only; nonzero = truthy):

| Category | Operators |
|----------|-----------|
| Operands | subject name, integer literal, `( expr )` for grouping |
| Comparison | `== != < <= > >=` or word forms `eq ne lt le gt ge` |
| Boolean | `&& \|\| !` or word forms `and or not` |
| Arithmetic | `+ - * /` (divide-by-zero → `0`), `%` (mod-by-zero → `0`) |

Both symbolic and word forms tokenize identically — `a && b` and `a and b` compile to the same expression.

**House style: use word forms (`and`/`or`/`not`/`gt`/`lt`/...).** `&&` and `<` are XML metacharacters — inside an XML attribute value they must be written as `&amp;&amp;` and `&lt;`, which is unreadable and easy to get wrong. Word forms need no escaping:

```xml
<!-- ✅ Preferred: no escaping needed -->
<bind_flag_if cond="demo_error or demo_temp gt demo_threshold" flag="hidden" invert="true"/>

<!-- Also valid, but requires XML entity escaping -->
<bind_flag_if cond="demo_error &amp;&amp; demo_temp &gt; demo_threshold" flag="hidden"/>
```

A full working demo of all four constructs (sliders/switch driving `demo_temp`, `demo_threshold`, `demo_error`, all four binding types reacting live) is in `ui_xml/test_panel.xml` (reachable via `helix-screen ctl navigate test`) — use it as the canonical reference and a live testbed when writing new expressions.

#### Parse-Time Conditional Hidden Attributes

These attributes hide an element at parse time based on a resolved prop value. Unlike `bind_flag_if_*` (which is reactive and requires a subject), these evaluate once when the XML is parsed and are useful for component props.

| Attribute | Behavior |
|-----------|----------|
| `hidden_if_empty="$prop"` | Hides the element if the resolved prop value is an empty string |
| `hidden_if_prop_eq="$prop\|ref_value"` | Hides the element if the resolved prop equals ref_value (pipe-delimited) |
| `hidden_if_prop_not_eq="$prop\|ref_value"` | Hides the element if the resolved prop does NOT equal ref_value |

```xml
<api>
  <prop name="description" type="string" default=""/>
  <prop name="mode" type="string" default="basic"/>
</api>

<!-- Hidden when no description is provided -->
<icon src="info_outline" hidden_if_empty="$description"/>

<!-- Hidden when mode is "advanced" -->
<lv_obj hidden_if_prop_eq="$mode|advanced">
  <text_body text="Basic mode content"/>
</lv_obj>

<!-- Hidden when mode is NOT "advanced" -->
<lv_obj hidden_if_prop_not_eq="$mode|advanced">
  <text_body text="Advanced mode content"/>
</lv_obj>
```

These are parse-time only -- the hidden state does not change after creation. For reactive visibility that responds to subject changes at runtime, use `bind_flag_if_*` instead.

#### Binding Limitations

**❌ No `bind_text_if_eq`** - use multiple labels with `bind_flag_if_*` for conditional text.

**✅ Compound conditions are supported** via the expression evaluator (see "Expression Conditionals" above) — `cond="a or b gt c"` on `bind_flag_if`/`bind_state_if`/`bind_style_if`, or a `<subject_expr>` derived subject for a condition reused in multiple places. This replaces stacking several single-subject `bind_flag_if_*` elements or writing a hand-rolled C++ derived subject for "OR of two subjects" type logic.

**Reuse alone does not pick `<subject_expr>`** — check the phase table above first. If any referenced subject comes from an `init_subjects()` (Phase 9a), the derived subject silently never registers and you must repeat the `cond=` at each site instead.

**❌ Never put two flag bindings for the same flag on one widget.** The flag
binds are two-way — a non-matching `bind_flag_if_eq` actively *removes* the flag
rather than abstaining — so two of them do not AND, they overwrite each other.
Combine into one expression (`cond="can and dirty"`) instead. Two binds for one
flag are safe only on different widgets, which is why the broken form can look
correct in nested markup. Worked example and the tests that pin it:
[`lib/helix-xml/docs/BINDINGS.md` § *The flag binds are two-way*](../../lib/helix-xml/docs/BINDINGS.md#the-flag-binds-are-two-way).

#### Repeating fragments with `<repeat>`

`<repeat count="N">…body…</repeat>` expands its body `N` times at load time, so a fixed-size list of widgets becomes XML structure instead of a C++ create-and-wire loop. Inside the body, the bare sigil `$i` resolves to the zero-based iteration index.

```xml
<lv_obj name="root">
  <repeat count="4">
    <lv_label name="lbl" text="$i" style_pad_all="#space_sm"/>
  </repeat>
</lv_obj>
<!-- root now has 4 labels reading "0", "1", "2", "3" -->
```

`count` accepts three forms:

| Form | Example | Meaning |
|------|---------|---------|
| Literal | `count="4"` | A fixed integer (clamped to `[0, 256]`), resolved once at load time. The expansion never changes. |
| `#const` | `count="#rows"` | A component `<const>` value, resolved once at load time. |
| Subject name | `count="row_count"` | **Reactive.** Expands to the subject's current value at load time, then re-expands automatically every time the subject changes — teardown of the old items and creation of the new ones happens on an async, off-tree-reparent path (no synchronous deletion inside the observer callback). |

Each iteration re-resolves the body against pristine attribute values, so `$i`, `$param`, and `#const` references all yield independent per-iteration results — the labels above each get their own index, not a shared last value.

> ⚠️ **Subject-bound `<repeat>` MUST be the last child of its parent, or the only child of a dedicated container.** On rebuild, the old expansion's roots are detached and the new ones are created fresh — and LVGL always appends a freshly-created child to the *end* of its parent's child list. If a subject-bound `<repeat>` shares a parent with static siblings that come after it in the document, those siblings stay put but the rebuilt repeat items land *after* them, silently reordering the layout every time the count changes. A literal or `#const` `count` never rebuilds, so this only matters for subject-bound `count`. Fix: give the `<repeat>` its own container (an `<lv_obj>` wrapper with no other children), or make it the last element inside its parent. See `ui_xml/test_panel.xml` "XML Repeat Demo" for a worked example of both the fixed and subject-bound forms side by side.

##### Self-wiring indexed subjects with `${name}`

The bare `$i` sigil is a whole-value substitution: `text="$i"` becomes the index, but `text="slot_$i"` does not splice. To compose the index (or a component prop) **into a larger string**, use the embedded `${name}` sigil. This is what lets a repeated widget bind to its own per-iteration subject:

```xml
<lv_obj name="root">
  <repeat count="3">
    <lv_label name="lbl" bind_text="demo_${i}_v"/>
  </repeat>
</lv_obj>
<!-- three labels bind to subjects demo_0_v, demo_1_v, demo_2_v -->
```

`${i}` resolves to the loop index; any other `${name}` resolves against the component's props (passed attributes first, then the `<prop>` default). Both can appear in the same value, so a component with `<prop name="grp"/>` instantiated as `<my_row grp="fan"/>` can bind `bind_text="status_${grp}_${i}_x"` → `status_fan_0_x`, `status_fan_1_x`, … The C++ side is responsible for registering those indexed subjects; an unresolved `${name}` splices empty and logs a warning.

`${…}` also evaluates **integer expressions** and splices the result as text: `${i + 1}` (1-based names), `${i * 84}` (computed numeric attributes like `style_translate_x`), `${base * scale}` (subject operands), `${cols * 2}` (a numeric component prop). A single bare name (`${i}`, `${grp}`) still means name-substitution; a token containing operators is evaluated. Operands: the loop index `i`, integer literals, numeric props, and subjects; the grammar and word forms are the same as [expression conditionals](#expression-conditionals). Division/modulo by zero and any unresolvable or malformed expression splice empty and log a warning.

> ⚠️ **Resolve-once.** A `${expr}` is evaluated **once, when the widget is created** — subject operands are read at that moment and the composed value does **not** update if the subject changes later. A `<repeat count="subject">` rebuild re-runs composition; a standalone attribute does not. For a value that must track a subject live, use a `bind_*` binding, not composition.

`<repeat>` is intercepted directly by the XML view parser (it creates no widget of its own), so its body must be well-formed markup that would be valid where the `<repeat>` sits. Nesting `<repeat>` inside another `<repeat>` is not yet supported.

#### Structural conditionals with `<if>` / `<else>`

`<if cond="EXPR"> …true-body… <else/> …false-body… </if>` creates **only** the body that matches `cond` — the other branch is never built. This is different from `bind_flag hidden` / `cond=` on `bind_flag_if`, which build every branch up front and then toggle visibility: cheap for light subtrees, wasteful for an expensive one (a whole card, a chart, an alternate layout). Use `<if>` when the *creation* itself is the cost you want to avoid; keep `bind_flag`/`cond=` for cheap show/hide.

```xml
<subjects><subject name="c" type="int" value="1"/></subjects>
<lv_obj name="root">
  <if cond="c gt 0">
    <lv_obj name="t"/>
    <else/>
    <lv_obj name="f"/>
  </if>
</lv_obj>
<!-- c > 0: root's only child is "t". c <= 0: root's only child is "f". -->
```
(adapted from `lib/helix-xml/tests/cases/test_if_else.c`)

`<else/>` is an inline divider *inside* the single matched `<if>…</if>` block, not a separate sibling tag: everything before it is the true-body, everything after it (up to `</if>`) is the false-body. Both spellings behave identically — self-closing `<else/>` and empty-element `<else></else>` — the split point is the `<else>` open tag; the marker's own open/close events aren't part of either body. `<else>` is optional: `<if cond="X">…</if>` with no `<else>` creates nothing when `cond` is false, and the component still loads. A second `<else/>` inside one `<if>` is a mistake — it warns and the first split wins. A stray `<else/>` with no enclosing `<if>` also warns and is ignored; the component still loads.

`cond` uses the same word-form expression grammar as [Expression Conditionals](#expression-conditionals) — subject names, int literals, `and`/`or`/`not`, `eq`/`ne`/`lt`/`le`/`gt`/`ge`, arithmetic operators. A cond with **no subject operands is static**: it's evaluated once at load time and the losing branch is never created — no observer. A cond that **references one or more subjects is reactive**: it fires immediately for the initial build, then re-evaluates and rebuilds (tears down the current branch, builds the other) on every change to *any* referenced subject — `cond="a and b gt c"` rebuilds whether `a`, `b`, or `c` changes.

> ⚠️ **A reactively-rebuilt `<if>` must be the last child of its parent, or the only child of a dedicated container** — the same ordering constraint as [`<repeat>`](#repeating-fragments-with-repeat). On rebuild, LVGL appends the freshly-built body to the *end* of the parent's child list, so static siblings that come after the `<if>` in the document stay put while the rebuilt body lands after them, silently reordering the layout on every flip. A static `<if>` never rebuilds, so this only matters for a subject-referencing `cond`.

Nested `<if>` (an `<if>` inside another `<if>`/`<repeat>` body) is not yet supported, same as nested `<repeat>`.

### 4. Observer Cleanup in DELETE Handlers

**CRITICAL:** When using `lv_label_bind_text()` with subjects in heap-allocated per-widget data, you must clean up observers before freeing.

```cpp
// ✅ CORRECT - Track and remove observers
struct MyWidgetData {
    lv_subject_t text_subject;
    char text_buf[32];
    lv_observer_t* text_observer = nullptr;  // Track it!
};

// When binding:
data->text_observer = lv_label_bind_text(label, &data->text_subject, "%s");

// In DELETE handler:
static void on_delete(lv_event_t* e) {
    MyWidgetData* data = get_data(e);
    if (data->text_observer) {
        lv_observer_remove(data->text_observer);  // Remove first!
    }
    delete data;  // Now safe
}
```

### Custom Widget on_delete Cleanup Ordering

When a custom widget owns subjects AND has child labels bound to external subjects, the `on_delete` handler **must** detach children from all subjects before deiniting owned subjects:

```cpp
static void on_delete(lv_event_t* e) {
    // 1. Detach child labels from ALL subjects (external + owned)
    if (data->current_label)
        lv_obj_remove_from_subject(data->current_label, nullptr);
    if (data->target_label)
        lv_obj_remove_from_subject(data->target_label, nullptr);

    // 2. NOW safe to deinit owned subjects
    lv_subject_deinit(&data->owned_subject);
}
```

**Why:** `lv_subject_deinit()` frees observer memory. If child labels still have `unsubscribe_on_delete_cb` events referencing those observers, LVGL's cascading child deletion will walk freed memory. `lv_obj_remove_from_subject(label, nullptr)` removes ALL observer connections from a label, including the `unsubscribe_on_delete_cb` events.

---

## Layouts & Positioning

### lv_obj Defaults (HelixScreen Theme)

Our theme system sets these defaults on all `lv_obj` containers:

| Property | Default Value | Notes |
|----------|---------------|-------|
| `width` | `content` | Shrinks to content size |
| `height` | `content` | Shrinks to content size |
| `border_width` | `0` | No border by default |
| `bg_opa` | `0` | Transparent background |
| `pad_all` | `0` | No internal padding |
| `scrollable` | **`true`** | **NOT overridden by our theme** - this is LVGL's own default (`LV_OBJ_FLAG_SCROLLABLE`) and it is ON. Write `scrollable="false"` explicitly on any container that is not a real scroll region. |

This means `lv_obj` acts as a pure layout container *visually* by default - no background, border, or padding unless explicitly added. Behaviorally it is not inert: it is still scrollable, so it can absorb drags and qualify for a page-scroll gutter. Turn that off with `scrollable="false"` unless the container is meant to scroll.

```xml
<!-- These are equivalent in HelixScreen -->
<lv_obj flex_flow="row">...</lv_obj>
<lv_obj flex_flow="row" height="content" style_border_width="0" style_bg_opa="0" style_pad_all="0">...</lv_obj>

<!-- ...but neither of the above is scroll-inert. A pure layout wrapper wants: -->
<lv_obj flex_flow="row" scrollable="false">...</lv_obj>
```

`helix-screen ctl geom <name>` reports the `scrollable` flag and the scroll extents, so it
tells you directly whether a container is scrollable (see `HELIXCTL.md:566-567`).

### Flex Layout (Flexbox)

Best for 1D layouts (single row/column or wrapping).

#### Flex Flow Options

```xml
<lv_obj flex_flow="row"/>           <!-- Horizontal left to right -->
<lv_obj flex_flow="column"/>        <!-- Vertical top to bottom -->
<lv_obj flex_flow="row_reverse"/>   <!-- Right to left -->
<lv_obj flex_flow="column_reverse"/><!-- Bottom to top -->
<lv_obj flex_flow="row_wrap"/>      <!-- Wrap to new rows -->
<lv_obj flex_flow="column_wrap"/>   <!-- Wrap to new columns -->
```

#### `flex_grow` does NOT compose with `*_wrap`

An item with `flex_grow` contributes a base size of **zero** when LVGL decides
which track an item belongs to. So `flex_grow="1"` on wrapping children means
everything fits one track and **nothing ever wraps** — the wrap silently stops
happening and the items just shrink.

```xml
<!-- ❌ never wraps: grow zeroes the base width used for track fitting -->
<lv_obj flex_flow="row_wrap">
  <ui_button width="48%" flex_grow="1"/>   <!-- x4 -> all on one row -->
</lv_obj>

<!-- ✅ wraps 2x2; the container distributes the leftover instead -->
<lv_obj flex_flow="row_wrap" style_flex_main_place="space_between">
  <ui_button width="48%"/>                 <!-- x4 -> two rows of two -->
</lv_obj>
```

To wrap *and* fill the row edge to edge, size the items with a percentage that
forces the wrap (two at 48% fit, three don't) and let
`style_flex_main_place="space_between"` absorb the remainder into the gap.
See `ui_xml/calibration_pid_panel.xml` (material presets).

#### Percentage height inside a content-sized parent collapses

`height="100%"` on a child of a `height="content"` parent is circular — the
parent sizes to the child, the child sizes to the parent, and **both resolve to
zero**. The children still lay out, at zero height, usually outside the parent's
box where they get clipped away and look like they were never created.

```xml
<!-- ❌ both collapse; the labels render outside the card and vanish -->
<lv_obj height="content" flex_flow="row">
  <lv_obj height="100%" flex_grow="1"><text_xs text="$print_time"/></lv_obj>
</lv_obj>

<!-- ✅ -->
<lv_obj height="content" flex_flow="row">
  <lv_obj height="content" flex_grow="1"><text_xs text="$print_time"/></lv_obj>
</lv_obj>
```

Symptom to recognise: `ctl geom` reports the row at a few px (its padding alone)
and the children at `h=0`. Real example: prestonbrown/helixscreen#1208.

#### Flex Alignment (Three Properties — You Need ALL THREE to Center!)

| Property | Controls | CSS Equivalent |
|----------|----------|----------------|
| `style_flex_main_place` | Main axis distribution (vertical in column) | `justify-content` |
| `style_flex_cross_place` | Cross axis alignment (horizontal in column) | `align-items` |
| `style_flex_track_place` | Track alignment — **required to center items with explicit widths** | `align-content` |

**GOTCHA:** Unlike CSS, LVGL needs `style_flex_track_place="center"` even without flex wrap.
Without it, children with explicit widths (e.g., `width="80%"`) will be left-aligned even if
`style_flex_cross_place="center"` is set. Always use all three for centering:

```xml
<!-- ✅ CORRECT — fully centered column layout -->
<lv_obj flex_flow="column"
        style_flex_main_place="center"
        style_flex_cross_place="center"
        style_flex_track_place="center">

<!-- ❌ WRONG — children with explicit widths won't center horizontally -->
<lv_obj flex_flow="column"
        style_flex_main_place="center"
        style_flex_cross_place="center">

<!-- ❌ WRONG - flex_align is silently ignored -->
<lv_obj flex_flow="row" flex_align="center center center"/>
```

#### Alignment Values

| Value | Behavior |
|-------|----------|
| `start` | Beginning (left/top) |
| `center` | Centered |
| `end` | End (right/bottom) |
| `space_evenly` | Equal space around all |
| `space_around` | Equal space, double at edges |
| `space_between` | No edge space, even gaps |

#### Flex Grow

Children with `flex_grow` expand to fill remaining space:

```xml
<lv_obj flex_flow="row" width="100%">
    <lv_label text="Left"/>                  <!-- Fixed size -->
    <lv_obj flex_grow="1"/>                  <!-- Expands -->
    <lv_label text="Right"/>                 <!-- Fixed size -->
</lv_obj>

<!-- Equal distribution -->
<lv_obj flex_flow="row">
    <lv_obj flex_grow="1">33%</lv_obj>
    <lv_obj flex_grow="1">33%</lv_obj>
    <lv_obj flex_grow="1">33%</lv_obj>
</lv_obj>
```

#### CRITICAL: Parent Height Required

When using `flex_grow`, the parent MUST have explicit height:

```xml
<!-- ✅ Parent needs height="100%" for flex_grow to work -->
<lv_obj flex_flow="row" height="100%">
    <lv_obj flex_grow="3" height="100%">Left</lv_obj>
    <lv_obj flex_grow="7" height="100%">Right</lv_obj>
</lv_obj>
```

#### Flex Gaps

```xml
<lv_obj flex_flow="row"
        style_pad_column="10"   <!-- Horizontal gap -->
        style_pad_row="5">      <!-- Vertical gap (if wrapping) -->
```

### Centering Techniques

```xml
<!-- Text: BOTH required -->
<lv_label text="Centered" style_text_align="center" width="100%"/>

<!-- Flex centering -->
<lv_obj flex_flow="column" height="100%"
        style_flex_main_place="center" style_flex_cross_place="center">
    <lv_label text="Centered"/>
</lv_obj>

<!-- Single child: use align, NOT flex (flex conflicts with align) -->
<lv_obj width="100%" height="100%">
    <lv_obj align="center">Perfectly centered</lv_obj>
</lv_obj>
```

---

## Common UI Patterns

### Icon Component

Font-based icons using Material Design Icons (MDI):

```xml
<!-- Basic icon -->
<icon src="home" size="lg"/>

<!-- With color variant -->
<icon src="heater" size="lg" variant="accent"/>

<!-- Clickable icon button -->
<lv_button width="60" height="60" style_bg_opa="0">
    <icon src="back" size="md" variant="primary"/>
    <event_cb trigger="clicked" callback="back_clicked"/>
</lv_button>
```

**Sizes:** `xs` (16px), `sm` (24px), `md` (32px), `lg` (48px), `xl` (64px)

**Variants:** `primary`, `secondary`, `accent`, `disabled`, `warning`

**Adding Icons:**
1. Find icon at [Pictogrammers MDI](https://pictogrammers.com/library/mdi/)
2. Add codepoint to `include/ui_icon_codepoints.h`
3. Add to `scripts/regen_mdi_fonts.sh`
4. Run `make regen-fonts`

### Semantic Typography

**ALWAYS use semantic text components instead of `<lv_label>` with hardcoded fonts.**

```xml
<!-- ✅ CORRECT - Semantic components -->
<text_heading text="WiFi"/>
<text_body text="Connected"/>
<text_small text="192.168.1.150"/>

<!-- ❌ WRONG - Hardcoded fonts -->
<lv_label text="WiFi" style_text_font="montserrat_20"/>
```

| Component | Purpose | Responsive Sizing |
|-----------|---------|-------------------|
| `<text_heading>` | Section titles | 20px / 26px / 28px |
| `<text_body>` | Primary content | 14px / 18px / 20px |
| `<text_small>` | Captions | 12px / 16px / 18px |

All support `bind_text`, `align`, `style_text_color`, etc.

### Spinner (Loading Indicator)

```xml
<!-- Large spinner for modals -->
<spinner size="lg"/>

<!-- Medium for inline loading -->
<spinner size="md"/>

<!-- Small for status indicators -->
<spinner size="sm"/>
```

### Custom Semantic Widgets

HelixScreen provides semantic widgets with built-in defaults. **Don't redundantly specify defaults!**

#### ui_card

Container with card styling from `theme_core`.

```xml
<!-- ✅ CORRECT - Minimal, uses defaults -->
<ui_card name="my_card" width="100%" height="200">
    <text_body text="Card content"/>
</ui_card>

<!-- ❌ WRONG - Redundant, border_radius is already a default -->
<ui_card style_radius="#border_radius">
```

**Built-in defaults:** `card_bg` background, `border_radius` corners, border from theme

#### ui_button

Semantic button with variant-based styling and auto-contrast text.

```xml
<!-- Primary action button -->
<ui_button variant="primary" text="Save"/>

<!-- Secondary button -->
<ui_button variant="secondary" text="Cancel"/>

<!-- Ghost (transparent) for toolbars -->
<ui_button variant="ghost" icon="settings"/>

<!-- Destructive action -->
<ui_button variant="danger" text="Delete"/>

<!-- Icon + text -->
<ui_button variant="primary" icon="check" text="Confirm"/>
```

**Variants:** `primary`, `secondary`, `danger`, `success`, `tertiary`, `warning`, `ghost`, `transparent`, `outline` (an unknown variant falls back to `primary`)

**Built-in defaults:** Responsive `button_height` (48/52/72px), `border_radius`, auto-contrast text color

**Reactive text with subject binding:**

`ui_button` supports two ways to bind text to a subject:

```xml
<!-- Literal text (static) -->
<ui_button text="Save"/>

<!-- Subject binding via text= with '@' prefix -->
<ui_button text="@my_button_text_subject"/>

<!-- Subject binding via bind_text (LVGL standard — always a subject, no '@' needed) -->
<ui_button bind_text="my_button_text_subject"/>
```

Both `text="@subject"` and `bind_text="subject"` produce identical reactive bindings. Use whichever reads better in context. `bind_text` is the LVGL-standard attribute and always expects a subject name. `text` with `@` prefix is syntactic sugar for the same thing.

When bound to a subject, the button label updates automatically, and a deferred invalidation ensures the button background repaints correctly (avoids partial-redraw artifacts).

#### divider_vertical / divider_horizontal

Visual separators with theme-aware colors.

```xml
<divider_vertical height="80%"/>
<divider_horizontal width="100%"/>
```

**Built-in defaults:** 1px width/height, `text_muted` color at 50% opacity

#### ui_markdown

Markdown viewer widget that renders markdown content as native LVGL widgets. Wraps the `lv_markdown` library (which uses md4c for parsing) and automatically applies theme-aware styling from design tokens.

```xml
<!-- Dynamic content via subject binding -->
<ui_markdown bind_text="update_release_notes" width="100%"/>

<!-- Static content -->
<ui_markdown text="# Hello\nSome **bold** text" width="100%"/>
```

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| `bind_text` | string | Binds to a string subject for dynamic markdown content |
| `text` | string | Sets static markdown content directly |
| `name` | string | Widget name for `lv_obj_find_by_name()` lookup |
| `width` | size | Width (typically `100%`). Height is always `LV_SIZE_CONTENT` |

All standard `lv_obj` attributes (`width`, `height`, `align`, `hidden`, etc.) are also supported.

**Supported Markdown Elements:**

- Headings (H1-H6)
- Bold (`**bold**`), italic (`*italic*`), bold-italic (`***both***`)
- Inline code (`` `code` ``)
- Fenced code blocks (` ``` `)
- Unordered lists (`- item`) with nesting
- Ordered lists (`1. item`) with nesting
- Blockquotes (`> quote`)
- Horizontal rules (`---`)

**Theme-Aware Styling:**

The widget automatically picks up colors, fonts, and spacing from the active theme. No manual styling is needed. The mapping is:

| Element | Font Token | Color Token |
|---------|-----------|-------------|
| Body text | `font_body` | `text` |
| H1 | `font_heading` | `primary` |
| H2 | `font_heading` | `secondary` |
| H3-H4 | `font_body` | `text` |
| H5-H6 | `font_small` | `text_muted` |
| Inline code | `font_small` | `text` on `elevated_bg` |
| Code blocks | `font_small` | `text` on `elevated_bg` |
| Blockquote border | -- | `primary` |
| Horizontal rule | -- | `text_muted` |

Spacing uses `space_sm` (paragraph), `space_xxs` (line), and `space_lg` (list indent).

Bold and italic use faux rendering (letter spacing for bold, underline for italic) since separate bold/italic font files are not shipped.

**Usage Pattern -- Scrollable Container:**

The widget uses `LV_SIZE_CONTENT` for height, growing to fit its content. For long content, wrap it in a scrollable container:

```xml
<ui_card width="100%" height="400" style_pad_all="#space_lg">
  <lv_obj width="100%" height="100%" scrollable="true"
          style_pad_all="0" style_border_width="0" style_bg_opa="0" style_radius="0">
    <ui_markdown name="my_markdown" width="100%" bind_text="my_content"/>
  </lv_obj>
</ui_card>
```

This pattern is used by the test panel. Another approach uses `flex_grow` to fill available space (used by the telemetry info modal):

```xml
<lv_obj width="100%" flex_grow="1"
        style_pad_left="#space_lg" style_pad_right="#space_lg"
        scrollable="true" scroll_snap_y="none">
  <ui_markdown name="info_text" width="100%" bind_text="my_subject"/>
</lv_obj>
```

**Setting Content from C++:**

For subject-bound widgets, update the subject and the widget updates automatically. For programmatic setup (e.g., the test panel), use `lv_markdown_set_text()` directly:

```cpp
lv_obj_t* md = lv_obj_find_by_name(lv_screen_active(), "my_markdown");
lv_markdown_set_text(md, "# Title\nSome **bold** markdown content.");
```

**Registration:**

The widget is registered via `ui_markdown_init()` in `xml_registration.cpp`. This must be called after `lv_xml_init()` and after the theme is initialized. No XML file registration is needed -- it is a custom C++ widget, not an XML component.

**Limitations and Gotchas:**

- No image/link support -- markdown images and hyperlinks are not rendered
- LVGL spangroups do not support per-span background styles, so inline code background color (`code_bg_color`) has no visible effect
- Theme changes at runtime do not automatically re-style existing markdown widgets (the style is applied at creation time)
- The `text` attribute in XML does not support literal newlines; use `\n` for line breaks in static content
- When using `bind_text`, the observer does not use `ObserverGuard` -- the observer is cleaned up automatically when the widget is deleted via LVGL's built-in observer-object tracking

#### Widget Defaults Quick Reference

| Widget | Don't Specify (Built-in) |
|--------|--------------------------|
| `ui_card` | `style_radius`, `style_bg_color`, `style_border_*` |
| `ui_button` | `style_radius`, `style_bg_color`, `style_height`, text color |
| `text_*` | `style_text_font`, `style_text_color` |
| `icon` | Font selection |
| `divider_*` | `style_bg_color`, width/height (1px) |
| `ui_markdown` | All styling (theme-aware fonts, colors, spacing) |

---

## Responsive Design

HelixScreen has a responsive design token system with 7 breakpoints, semantic spacing, responsive fonts, and more. See the **[UI Contributor Guide](UI_CONTRIBUTOR_GUIDE.md)** for the complete reference — it covers breakpoints, spacing tokens, font tokens, component tokens, color system, and how to add new tokens.

Quick summary for reference:
- **Breakpoints** are selected from the narrow axis, `min(width, height)`, not the height: MICRO (≤272), TINY (273-390), SMALL (391-460), MEDIUM (461-550), LARGE (551-700), XLARGE (701-1000), XXLARGE (>1000)
- **Spacing:** `#space_xxs` through `#space_2xl` — always use tokens, never hardcoded pixels
- **Fonts:** Use `<text_heading>`, `<text_body>`, `<text_small>`, `<text_xs>` components
- **Colors:** Use `#token_name` in XML (e.g., `style_bg_color="#card_bg"`)

---

## Styles & Theming

### Defining Styles

**CRITICAL:** Inside `<styles>`, do NOT use `style_` prefix!

```xml
<styles>
    <!-- ✅ CORRECT - No prefix in style definitions -->
    <style name="style_button" bg_color="0x111" radius="8" pad_all="12"/>

    <!-- ❌ WRONG - style_ prefix doesn't work here -->
    <style name="bad_style" style_bg_color="0x111"/>
</styles>
```

**`flex_flow` in a `<style>` is inert without `layout="flex"`.** Setting the flow
alone is a no-op: `lv_obj_set_flex_flow()` sets *both* `LAYOUT` and `FLEX_FLOW`,
but a `<style>` only applies the properties you name. A container with a flow and
no layout runs no layout at all — every child stacks at the content origin, on
top of each other.

```xml
<!-- ❌ no layout runs; children pile up at the top-left -->
<style name="card_row" flex_flow="row"/>

<!-- ✅ -->
<style name="card_row" layout="flex" flex_flow="row"/>
```

This matters whenever a layout is switched per breakpoint via `bind_style_if_*`,
since the flow can only live in the style — an inline `flex_flow` attribute would
beat the bound style (see Rule 6). Examples:
`ui_xml/components/lock_screen.xml`, `ui_xml/ams_environment_overlay.xml`.

### Applying Styles

```xml
<!-- By name -->
<lv_button>
    <style name="style_button"/>
</lv_button>

<!-- With state selector -->
<lv_button>
    <style name="style_base"/>
    <style name="style_pressed" selector="pressed"/>
</lv_button>

<!-- Inline (USE style_ prefix) -->
<lv_button style_bg_color="0x111" style_radius="8"/>
```

### Part Selectors

Many widgets have styleable parts:

```xml
<!-- Style slider knob separately -->
<lv_slider style_bg_color="#333333"
           style_bg_color:indicator="#primary_color"
           style_bg_color:knob="#ffffff"/>

<!-- Hide spinner background track -->
<lv_spinner style_arc_opa:main="0"/>
```

| Part | Widgets |
|------|---------|
| `main` | All (background) |
| `indicator` | slider, bar, arc, spinner |
| `knob` | slider, arc |
| `items` | dropdown, roller |
| `scrollbar` | Scrollable containers |

### Theme Colors (C++ API)

```cpp
// ✅ For theme tokens - handles light/dark mode:
lv_color_t bg = theme_manager_get_color("card_bg");
lv_color_t ok = theme_manager_get_color("success_color");

// ✅ For literal hex strings:
lv_color_t custom = theme_manager_parse_hex_color("#FF4444");

// ❌ WRONG - parse_hex_color doesn't look up tokens:
// lv_color_t bg = theme_manager_parse_hex_color("#card_bg");  // Garbage!
```

---

## Event Handling

### The Mandatory Pattern

Events MUST be declared in XML and registered in C++. **NEVER use `lv_obj_add_event_cb()`**.

**Step 1: Declare in XML**

```xml
<lv_button name="my_button">
    <event_cb trigger="clicked" callback="on_my_button_clicked"/>
    <text_body text="Click Me"/>
</lv_button>

<!-- Multiple events -->
<lv_slider name="my_slider">
    <event_cb trigger="value_changed" callback="on_slider_changed"/>
    <event_cb trigger="released" callback="on_slider_released"/>
</lv_slider>
```

**Step 2: Register in init_subjects() (BEFORE XML creation)**

```cpp
void MyPanel::init_subjects() {
    // Register callbacks BEFORE XML is created
    lv_xml_register_event_cb(nullptr, "on_my_button_clicked", on_click_cb);
    lv_xml_register_event_cb(nullptr, "on_slider_changed", on_slider_cb);
}
```

**Step 3: Implement callback**

```cpp
static void on_click_cb(lv_event_t* e) {
    spdlog::info("Button clicked!");
}

// Or use lambda
lv_xml_register_event_cb(nullptr, "on_slider_changed", [](lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_current_target(e);
    int value = lv_slider_get_value(slider);
    spdlog::info("Slider: {}", value);
});
```

### Common Triggers

| Trigger | When Fired |
|---------|------------|
| `clicked` | Button click (press + release) |
| `value_changed` | Slider, dropdown, switch |
| `pressed` | Object pressed down |
| `released` | Object released |
| `long_pressed` | Long press detected |
| `focused` | Object gains focus |
| `ready` | Text area complete |

---

## Implementation Guide

### Step-by-Step Pattern

#### 1. Create XML Layout

ui_xml/example_panel.xml:

```xml
<component>
    <view extends="lv_obj" width="100%" height="100%" style_bg_color="#overlay_bg">
        <!-- Bound to subject -->
        <text_body bind_text="example_status"/>

        <!-- Conditional visibility -->
        <lv_obj name="loading_view">
            <bind_flag_if_eq subject="panel_state" flag="hidden" ref_value="1"/>
            <spinner size="lg"/>
        </lv_obj>

        <lv_obj name="content_view">
            <bind_flag_if_not_eq subject="panel_state" flag="hidden" ref_value="1"/>
            <lv_button>
                <event_cb trigger="clicked" callback="on_action_clicked"/>
                <text_body text="Action"/>
            </lv_button>
        </lv_obj>
    </view>
</component>
```

#### 2. Create C++ Wrapper

include/example_panel.h:

```cpp
#pragma once
#include "lvgl/lvgl.h"

class ExamplePanel {
public:
    static void init_subjects();
    static lv_obj_t* create(lv_obj_t* parent);
    static void update_status(const char* msg);
    static void show_loading();
    static void show_content();
};
```

src/example_panel.cpp:

```cpp
#include "example_panel.h"
#include <spdlog/spdlog.h>

static lv_subject_t status_subject;
static lv_subject_t state_subject;
static char status_buffer[128];

void ExamplePanel::init_subjects() {
    // Initialize subjects
    lv_subject_init_string(&status_subject, status_buffer, NULL,
                           sizeof(status_buffer), "Ready");
    lv_subject_init_int(&state_subject, 0);

    // Register subjects
    lv_xml_register_subject(NULL, "example_status", &status_subject);
    lv_xml_register_subject(NULL, "panel_state", &state_subject);

    // Register event callbacks
    lv_xml_register_event_cb(nullptr, "on_action_clicked", [](lv_event_t* e) {
        spdlog::info("Action clicked!");
    });
}

lv_obj_t* ExamplePanel::create(lv_obj_t* parent) {
    return lv_xml_create(parent, "example_panel", nullptr);
}

void ExamplePanel::update_status(const char* msg) {
    lv_subject_copy_string(&status_subject, msg);
}

void ExamplePanel::show_loading() {
    lv_subject_set_int(&state_subject, 0);
}

void ExamplePanel::show_content() {
    lv_subject_set_int(&state_subject, 1);
}
```

#### 3. Register and Use

In `main.cpp`:

```cpp
// 1. Register component
lv_xml_register_component_from_file("A:ui_xml/example_panel.xml");

// 2. Initialize subjects (BEFORE creating XML)
ExamplePanel::init_subjects();

// 3. Create panel
lv_obj_t* panel = ExamplePanel::create(screen);

// 4. Update (triggers reactive updates)
ExamplePanel::update_status("Loading...");
ExamplePanel::show_loading();
```

---

## Best Practices

### Widget Lookup - Use Names

```xml
<lv_label name="temperature_display" bind_text="temp"/>
```

```cpp
// ✅ CORRECT - Name-based (resilient)
lv_obj_t* w = lv_obj_find_by_name(parent, "temperature_display");

// ❌ WRONG - Index-based (fragile)
lv_obj_t* w = lv_obj_get_child(parent, 3);
```

### Component Names Required

```xml
<!-- ❌ WRONG - Component not findable -->
<controls_panel/>

<!-- ✅ CORRECT - Explicit name -->
<controls_panel name="controls_panel"/>
```

### Widget Naming Strategy

Widgets **must have names** when:
1. **C++ lookup** - Referenced via `lv_obj_find_by_name()`
2. **Interactive types** - `lv_button`, `lv_slider`, `lv_dropdown`, `lv_spinner`, `lv_textarea`
3. **Subject binding** - Has `bind_text=`, `bind_value=` attributes

Widgets **can safely omit names**:
- **Layout containers** - Pure flexbox structure: `<lv_obj flex_flow="row" style_pad_gap="...">`
- **Spacers/dividers** - One-pixel separators: `<lv_obj width="100%" height="1">`
- **Static labels** - No binding, not looked up: `<lv_label text="Section Title"/>`
- **Decorative buttons** - `clickable="false"` placeholders

```xml
<!-- These DON'T need names (decorative) -->
<lv_obj flex_flow="row" style_pad_gap="#space_md">
  <lv_obj width="1" height="100%" style_bg_color="#text_muted"/>
  <lv_label text="Settings"/>
</lv_obj>

<!-- These DO need names (interactive/bound) -->
<lv_button name="save_btn">
<lv_label name="status_display" bind_text="status_subject"/>
```

> **Note:** The audit script (`scripts/audit_codebase.sh`, P5 section) uses smart detection
> to only warn on truly interactive unnamed widgets. Decorative containers are ignored.

### Banned Patterns

| Pattern | Why Banned | Alternative |
|---------|------------|-------------|
| `lv_obj_add_event_cb()` | Tight coupling | XML `<event_cb>` |
| `lv_label_set_text()` | Bypasses binding | `bind_text` subject |
| `lv_obj_add_flag(HIDDEN)` | Visibility is UI | `<bind_flag_if_eq>` |
| `lv_obj_set_style_*()` | Styling in XML | Design tokens |

### Acceptable Exceptions

1. `LV_EVENT_DELETE` cleanup
2. Widget pool recycling (virtual scroll)
3. Chart data points
4. Animations
5. One-time `setup()` widget lookup

---

## Troubleshooting

### Critical Gotchas

#### 1. SIZE_CONTENT Syntax

```xml
<!-- ✅ CORRECT -->
<lv_obj width="content" height="content"/>

<!-- ❌ WRONG - Parses as 0! -->
<lv_obj width="LV_SIZE_CONTENT"/>
```

#### 2. No zoom Attribute

```xml
<!-- ❌ WRONG - zoom doesn't exist -->
<lv_image src="icon" zoom="128"/>

<!-- ✅ CORRECT - use scale (256 = 100%) -->
<lv_image src="icon" scale_x="128" scale_y="128"/>
```

#### 3. Full Words, Not Abbreviations

```xml
<!-- ❌ WRONG -->
<lv_image style_img_recolor="#ff0000"/>

<!-- ✅ CORRECT -->
<lv_image style_image_recolor="#ff0000"/>
```

#### 4. Dropdown Newlines

```xml
<!-- ✅ CORRECT - XML entity -->
<lv_dropdown options="A&#10;B&#10;C"/>

<!-- ❌ WRONG - Literal \n doesn't work -->
<lv_dropdown options="A\nB\nC"/>
```

#### 5. Complex Layouts Need lv_obj_update_layout()

Grid layouts or dynamic content with SIZE_CONTENT may need an explicit layout update:

```cpp
lv_obj_t* panel = lv_xml_create(parent, "complex_panel", NULL);
lv_obj_update_layout(panel);  // Required for grid layouts
```

**Note:** SIZE_CONTENT disables flex wrapping - use explicit width if you need `row_wrap`.

#### 6. lv_bar value=0 Bug (Upstream)

Bar shows FULL instead of empty when created with `cur_value=0` and XML sets `value=0`. `lv_bar_set_value()` returns early without invalidation because old == new. Workaround: set to 1 then 0.

```cpp
lv_bar_set_value(bar, 1, LV_ANIM_OFF);
lv_bar_set_value(bar, 0, LV_ANIM_OFF);
```

### Debugging Checklist

When layouts don't work:

- [ ] Label has `style_text_align="center"` AND `width="100%"`?
- [ ] Parent has `flex_flow` set?
- [ ] Using `style_flex_main_place` (NOT `flex_align`)?
- [ ] Children have `flex_grow="1"`?
- [ ] Container has `height="100%"`?
- [ ] No mixing absolute positioning with flex?

### Visual Debugging

Add temporary background colors:

```xml
<lv_obj style_bg_color="#ff0000" style_bg_opa="100%">
    <!-- Check actual size -->
</lv_obj>
```

---

## Quick Reference

### API Functions

```cpp
// Component registration
lv_xml_register_component_from_file("A:path/file.xml");

// Subject registration
lv_xml_register_subject(NULL, "name", &subject);

// Event callback registration
lv_xml_register_event_cb(nullptr, "callback_name", function);

// Font registration
lv_xml_register_font(NULL, "font_name", &font);

// Constant registration
lv_xml_register_const(scope, "name", "value");

// Create component
lv_obj_t* obj = lv_xml_create(parent, "component_name", nullptr);

// Find widget by name
lv_obj_t* w = lv_obj_find_by_name(parent, "widget_name");
```

---

## Resources

- **Subject-Observer:** https://docs.lvgl.io/master/details/auxiliary-modules/observer/
- **Fork origin and licensing:** `HELIX_XML_FORK.md`
- **Upstream XML docs:** LVGL removed XML from core in v9.5 and now sells it as LVGL Pro. The old
  `docs.lvgl.io/master/details/xml/` link redirects to https://lvgl.io/docs/pro/syntax, which
  documents a different, closed engine — it is *not* authoritative for helix-xml syntax. Read it
  for background only, and never read LVGL Pro source (see `HELIX_XML_FORK.md` § Clean-room rule).
- **Quick Reference:** `LVGL9_XML_ATTRIBUTES_REFERENCE.md`
- **Example Panels:** `ui_xml/bed_mesh_panel.xml` (gold standard)

