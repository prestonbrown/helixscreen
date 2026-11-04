# Theme System

> **For contributors:** If you're doing layout or styling work, start with the **[UI Contributor Guide](UI_CONTRIBUTOR_GUIDE.md)** instead. This document covers the internal architecture of the theme system — the table-driven `ThemeManager`, shared `lv_style_t` objects, and extending the system with new themed widgets.

## Overview

The reactive theme system enables **live theme switching** (dark/light modes) without recreating widgets. When the user toggles dark mode or previews a theme, all UI elements update instantly.

### Key Principle

> **DATA in C++, APPEARANCE in XML, Shared Styles connect them.**

> **Note:** The theme system handles colors, spacing tokens, and typography. For structural layout changes across different screen shapes (ultrawide, portrait, tiny), see the [Layout System](LAYOUT_SYSTEM.md) — themes and layouts are independent and any combination works.

- **Data** (C++): Printer state, temperatures, positions
- **Appearance** (XML): Layout, colors via tokens, spacing via tokens
- **Shared Styles** (`ThemeManager`): a fixed table of LVGL `lv_style_t` objects — one per `StyleRole` — that widgets add by reference. Re-theming reconfigures the styles in place, so every widget that added them redraws with new colors.

### What Problem It Solves

**Before:** Changing themes required restarting the app or manually updating hundreds of widgets.

**After:** Call `theme_manager_apply_theme(theme, dark)` → `ThemeManager` reconfigures every style in its table against the new palette → `lv_obj_report_style_change(nullptr)` triggers LVGL's style cascade → every widget that added a shared style redraws with new colors. Widgets with baked-in inline styles from XML are then re-styled by a widget-tree walk (`theme_manager_refresh_widget_tree` + `theme_apply_current_palette_to_tree`).

---

## Architecture

The system has two cooperating pieces, both fronted by `theme_manager.h`:

| Piece | File(s) | Responsibility |
|-------|---------|----------------|
| **`ThemeManager`** (singleton) | `src/ui/theme/theme_manager_new.cpp`, style configs in `src/ui/style_configs.cpp` | Owns the fixed table of shared `lv_style_t` objects (one per `StyleRole`), reconfigures them against the active `ThemePalette`, and fires `lv_obj_report_style_change()` |
| **Free functions** (`theme_manager_*`) | `src/ui/theme/theme_manager.cpp` and the `src/ui/theme_*.cpp` files beside it | Token lookup, XML constant registration, responsive spacing/font constants, theme loading, and the unified `theme_manager_apply_theme()` re-theme entry point |

`ThemeManager` replaces the deleted theme_core.c. Instead of a getter per style, roles are enumerated in the `StyleRole` enum and stored in a `std::array<StyleEntry, StyleRole::COUNT>`. Each `StyleEntry` binds a role to its `lv_style_t` and a `StyleConfigureFn` (`configure_card`, `configure_button_primary`, … in `style_configs.cpp`) that writes palette colors into the style.

### When to Use Each

| Task | Use |
|------|-----|
| Get a color token in C++ | `theme_manager_get_color("card_bg")` |
| Get a color from the live palette struct | `ThemeManager::instance().get_color("card_bg")` |
| Get responsive spacing | `theme_manager_get_spacing("space_lg")` |
| Get responsive font | `theme_manager_get_font("font_body")` |
| Toggle dark/light mode | `theme_manager_toggle_dark_mode()` |
| Apply a whole theme (or preview) | `theme_manager_apply_theme(theme, dark)` |
| Check current mode | `theme_manager_is_dark_mode()` |
| Add a shared style to a custom widget | `ThemeManager::instance().get_style(StyleRole::Card)` |

### Data Flow

```
Theme JSON (nord.json, etc.)
    ↓
theme_manager.cpp  (theme_loader parses JSON → ThemeData/ModePalette,
                    builds a ThemePalette, registers _light/_dark XML consts)
    ↓
ThemeManager::apply_palette(palette)
    ↓
For each StyleEntry: lv_style_reset() + entry.configure(&style, palette)
    ↓
Widgets that called get_style(role) + lv_obj_add_style(...) reference the shared styles
```

Standard LVGL widgets (buttons, textareas, dropdowns, switches, sliders, …) receive their
shared styles automatically from the LVGL theme apply callback `helix_theme_apply()` in
`theme_lvgl_apply.cpp`, keyed on widget class. Widget parts not covered by `StyleRole`
(checkbox box, switch track/knob, slider track/knob) use file-static styles configured by
`init_extra_styles()` / `update_handle_styles()` in the same file.

### How Changes Propagate

```
theme_manager_apply_theme(theme, dark)   ← also reached via theme_manager_toggle_dark_mode()
    ↓
theme_update_colors() → ThemeManager::set_palettes() → apply_palette()
    ↓
Every style in the table is reset + reconfigured against the new palette
    ↓
lv_obj_report_style_change(nullptr)      ← CRITICAL: invalidates LVGL style caches / cascade
    ↓
Re-register XML color/property consts; update screen bg
    ↓
lv_xml_reapply_style_tokens() + lv_xml_reapply_token_styles()  ← <style> and inline #token colors re-resolve
    ↓
theme_manager_refresh_widget_tree()      ← lv_obj_refresh_style() on every widget (picks up inline styles)
    ↓
theme_apply_current_palette_to_tree()    ← re-colors widgets with baked XML inline colors
    ↓
theme_manager_notify_change()            ← bumps the theme-changed subject (generation counter)
```

### Authored Inline Colors

helix-xml records every inline `style_*` color an XML author writes, per object, property
and selector: a global token (`style_text_color="#text_muted"`) with its name, a literal
(`style_bg_color="0x000000"`, `"#FFFFFF"`) without one. Two things follow on a theme switch:

- `lv_xml_reapply_token_styles()` rewrites each token color with its const's new value, unless
  C++ has changed that color since the XML wrote it (an error-red label, a selection
  highlight keeps its value).
- The palette walker (`src/ui/theme/theme_live_recolor.cpp`) writes every local color through one
  helper that skips a property+selector `lv_xml_obj_has_authored_style()` reports. A token
  is already right after the re-apply, and a literal such as the QR scanner's white status
  text stays as written. Widgets with no inline color are themed by the walker as before.
- `lv_xml_reapply_style_tokens()` rewrites the `#token` colors of every named `<style>` in
  place, so widgets using the style, and ones built from it later, take the new mode.
- The walker also leaves a label's text color, and a button's bg and border colors, alone
  when a style someone chose provides it (a bound style, on or off at the time, a component
  `<style>`, one added from C++), so the motion rail's idle `zone_tab_pill_idle` keeps
  `#card_bg`. ThemeManager's shared semantic text styles and its button styles (every
  `StyleRole::Button*`) do not count: a plain `text_body` on a dark ancestor still turns
  white, and a gray `ui_button` still takes `elevated_bg`.

A color passed through a component `$prop` is resolved at the instance tag, where the token
name is lost, so it counts as a literal: kept, never re-applied. Colors set from C++ are not
recorded and stay the walker's to theme.

---

## Color System

### Token Naming Conventions

All colors are referenced as tokens with `#` prefix in XML:

| Token | Purpose |
|-------|---------|
| `#screen_bg` | Main application background |
| `#overlay_bg` | Sidebar/panel backgrounds |
| `#card_bg` | Card surfaces |
| `#elevated_bg` | Elevated/control surfaces (dialogs, inputs) |
| `#border` | Borders and dividers |
| `#text` | Primary text |
| `#text_muted` | Secondary/dimmed text |
| `#text_subtle` | Hint/tertiary text |
| `#primary` | Primary accent color |
| `#secondary` | Secondary accent |
| `#tertiary` | Tertiary accent |
| `#info` | Info state (blue) |
| `#success` | Success state (green) |
| `#warning` | Warning state (amber) |
| `#danger` | Error/danger (red) |
| `#focus` | Focus ring outline |

### Light/Dark Variants

Theme-aware colors resolve to a light or dark value depending on the current mode. Note
the `_light`/`_dark` **suffix convention is internal to the XML const registration** — it
is how `theme_tokens_register.cpp` registers each palette color into the `ui_xml` global scope
(e.g. `card_bg_light`, `card_bg_dark`). The **theme JSON itself does not use suffixed
keys**; it nests `"light": { ... }` and `"dark": { ... }` objects. The system automatically
selects the right one:

```xml
<!-- In your XML - just use the base name -->
<lv_obj style_bg_color="#card_bg"/>

<!-- At runtime, resolves to card_bg_light or card_bg_dark based on mode -->
```

Internally:
```cpp
// theme_tokens_register.cpp checks for both variants
const char* light_str = lv_xml_get_const_silent(nullptr, "card_bg_light");
const char* dark_str = lv_xml_get_const_silent(nullptr, "card_bg_dark");
// Returns appropriate value based on use_dark_mode flag
```

### Color Functions

| Function | Use Case |
|----------|----------|
| `theme_manager_get_color("card_bg")` | Get themed color token (handles _light/_dark) |
| `theme_manager_parse_hex_color("#FF0000")` | Parse hex string only (NOT for tokens) |

**Common mistake:** Using `theme_manager_parse_hex_color()` with tokens. It only parses hex strings.

```cpp
// WRONG - parse_hex_color doesn't handle tokens
lv_color_t bg = theme_manager_parse_hex_color("#card_bg");

// CORRECT - get_color resolves tokens
lv_color_t bg = theme_manager_get_color("card_bg");
```

---

## Responsive Sizing

### The Breakpoint System

Every responsive value requires three core variants: `_small`, `_medium`, `_large`. Optional `_micro`, `_tiny`, `_xlarge` and `_xxlarge` variants can be added where values differ. The system selects a tier automatically.

**Breakpoints (7 tiers, shared by both axis ladders):**
| Suffix | Axis range | Target Devices | Fallback |
|--------|------------|----------------|----------|
| `_micro` | ≤272px | 480×272 | → `_tiny` → `_small` |
| `_tiny` | 273-390px | 480×320 | → `_small` |
| `_small` | 391-460px | 480×400, 1920×440 | required |
| `_medium` | 461-550px | 800×480 | required |
| `_large` | 551-700px | 1024×600 | required |
| `_xlarge` | 701-1000px | 1280×720, 1024×768 | → `_large` |
| `_xxlarge` | >1000px | 1440p, 4K | → `_xlarge` → `_large` |

**Two ladders feed that one table** (`src/ui/theme/theme_responsive.cpp`):

| Ladder | Scalar | Used by |
|--------|--------|---------|
| Cramped axis (default) | `responsive_dimension()` = `min(width, height)` | Fonts, spacing, widths, everything axis-neutral |
| Vertical axis | `responsive_vertical_dimension()` = `height` | The height tokens listed in `VERTICAL_AXIS_TOKENS` |

On a landscape display the narrow axis usually *is* the height, which is why "height-based" looked right for years; on a portrait panel it is the width. Because `min(w, h) == h` on landscape and square displays, the two ladders only diverge in portrait. A 320×1480 panel resolves `#space_lg` from 320 (`_tiny`, 8px) and `#button_height` from 1480 (`_xxlarge`, 96px).

`theme_manager_resolve_px_tokens()` is the single place that applies this policy; startup registration and the resize path both apply its output, so the two can never disagree. The `ui_breakpoint` subject keeps holding the **cramped** tier — all XML `ref_value` bindings are written against it. Tier boundaries live in `breakpoint_for()` (`include/ui_breakpoint.h`).

Full explanation, worked example, and the rule for classifying a new token: [UI_CONTRIBUTOR_GUIDE.md § Screen Breakpoints](UI_CONTRIBUTOR_GUIDE.md#2-screen-breakpoints). Background: prestonbrown/helixscreen#1209.

### The high-DPI UI scale factor

Breakpoints are chosen from **pixel** counts, so resolution stands in for physical size. That proxy holds across printer panels and fails on phone-class ones: a 1080x2400 handset at ~405 DPI picks XXLARGE and draws a 192px grid cell, which measures 12mm instead of the ~30mm the tier intends. Proportions stay right while everything shrinks physically in lockstep.

`DisplayMetrics` (`include/display_metrics.h`) resolves an effective DPI and turns it into a multiplicative scale applied **on top of** the tier ladder. Breakpoint selection itself still runs on raw pixels, so no panel can change tier and a regression is structurally impossible rather than merely unlikely.

```
scale = 1.0                            for dpi <= 225
scale = 1.0 + (dpi - 225) * 0.0032118  above, capped at 2.0
```

The deadband covers every measured printer (CC1 131, SonicPad 170, CB1 187, Pi 206, K1C/K2+ 218, AD5M 221 true DPI), so all of them resolve to exactly 1.0 and are bit-identical to an unscaled build. A 405 DPI panel drawn at its native resolution resolves to 1.578. The Android SDL driver never draws a phone that way: above 225 DPI it renders at half the native resolution or less and upscales, so the curve sees the logical pixel's DPI (a 480 DPI Galaxy Z Flip 7 at 1260x540 is 240, scale 1.048).

**What the scale multiplies**

| Thing | Where |
|-------|-------|
| Every `<px>` token, responsive and static alike | `theme_manager_scale_px_token()`, shared by both registration paths |
| Grid cell track edge | `GridLayout::get_dimensions()` |
| Font tokens | remapped to the nearest **larger** linked face by `DisplayMetrics::scaled_font_name()` |

Static (non-suffixed) tokens scale too. They are fixed-size boxes — icon badges, chips, swatches, column widths — holding scaled text, so exempting them just makes glyphs overflow their containers. The one exemption is any token whose name ends `_opacity`: those are declared `<px>` but hold an 0-255 alpha, and multiplying one sails past opaque.

**LVGL's own DPI**

`Application` sets `lv_display_set_dpi()` to `LV_DPI_DEF * scale` — never a kernel-reported value. LVGL derives its internal padding from DPI via `LV_DPX`, so this keeps its chrome (and the handful of `lv_dpx()` call sites in `src/`) growing in step with the design tokens. The two paths are disjoint — tokens reach widgets as style attributes, `LV_DPX` only feeds LVGL's own theme — so they add rather than compound. At scale 1.0 the expression is exactly `LV_DPI_DEF`.

**Why the kernel is never trusted**

Reported physical size is wrong or absent on 6 of the 8 devices in the test fleet, and the wrong ones look plausible: the Raspberry Pi rig reports `154x86mm`, the exact published active area of the official 7" touchscreen, for a panel that is really ~115mm. `simpledrm` synthesizes exactly 96 DPI when the bootloader gives it nothing. No connector in the fleet exposes EDID at all.

So DPI is resolved from trusted sources only, most trusted first:

| Source | Notes |
|--------|-------|
| `--dpi <n>` / stored override | Always wins; ignored if outside the 60-700 plausibility band |
| Platform measurement | Android only (`SDL_GetDisplayDPI`), converted to the DPI of the logical surface the SDL driver draws and upscales (`DisplayMetrics::logical_dpi`), still plausibility-checked |
| Known-panel table | Integrated panels keyed on platform: `ad5m`, `cc1`, `k1`, `k2`. Platforms driving a user-chosen display (`pi`, `pi32`, `x86`) deliberately have no entry |
| Fallback | The 160 DPI authoring reference, i.e. scale 1.0 |

Startup logs which source won: `[Application] Display metrics: dpi=... (source=...) -> ui_scale=..., lvgl_dpi=...`.

**Fonts above the authored ladder**

The tier ladder tops out at `noto_sans_40` (26 for light), so the scale needs larger faces to step into: `noto_sans_48/64`, `noto_sans_bold_48/64`, `noto_sans_light_32/40`. These live in `FONTS_XXLARGE` only (`mk/fonts.mk`), and no printer platform in `mk/cross.mk` declares the xxlarge tier, so the constrained targets link none of them. Android builds via CMake, which globs `assets/fonts/*.c`, and picks them up automatically. A scaled face is adopted only after `lv_xml_get_font_silent()` confirms it is linked, so a build that pruned them degrades to the unscaled tier font rather than losing text.

Testing note: `[content_fits]` cannot cover the scale, because the scale is process-wide while the sweep varies geometry per row. Verify scaled rendering against a running instance: `-s 1080x2400 --dpi 405`.

### Spacing Tokens

Defined in `ui_xml/globals.xml`:

| Token | Small | Medium | Large | Use Case |
|-------|-------|--------|-------|----------|
| `#space_xxs` | 2px | 3px | 4px | Keypad rows, compact icon gaps |
| `#space_xs` | 4px | 5px | 6px | Button icon+text gaps, dense info |
| `#space_sm` | 6px | 7px | 8px | Tight layouts, minor separations |
| `#space_md` | 8px | 10px | 12px | Standard flex gaps, compact padding |
| `#space_lg` | 12px | 16px | 20px | Container padding, major sections |
| `#space_xl` | 16px | 20px | 24px | Emphasis cards, major separations |
| `#space_2xl` | 24px | 32px | 40px | Toast/overlay offsets |

### How It Works

```xml
<!-- In globals.xml - define triplet variants -->
<px name="space_lg_small" value="12"/>
<px name="space_lg_medium" value="16"/>
<px name="space_lg_large" value="20"/>

<!-- At runtime, theme_manager registers base name -->
lv_xml_register_const(scope, "space_lg", "16");  // On 800x480 screen

<!-- In your XML - use base name -->
<lv_obj style_pad_all="#space_lg"/>
```

**Critical:** Do NOT define base constants (`space_lg`) in globals.xml. Only define the triplet variants. The theme_manager registers the base name at runtime. LVGL ignores duplicate registrations, so pre-defining base names breaks responsive overrides.

### Font Tokens

| Token | Small | Medium | Large |
|-------|-------|--------|-------|
| `#font_heading` | noto_sans_20 | noto_sans_26 | noto_sans_28 |
| `#font_body` | noto_sans_14 | noto_sans_18 | noto_sans_20 |
| `#font_small` | noto_sans_light_12 | noto_sans_light_16 | noto_sans_light_18 |
| `#font_xs` | noto_sans_light_10 | noto_sans_light_12 | noto_sans_light_14 |

### Font tier registration

Font tokens differ from `px` tokens in one way that matters: the face a token names
has to have been registered with LVGL before the token can point at it, and not every
tier is registered at once.

- **Build time** — `FONT_TIERS` (`mk/cross.mk`) decides which faces are linked at all,
  and `HELIX_MAX_FONT_TIER` is derived from it. `theme_manager` uses that ceiling to
  tell a font pruned by tier from a font missing because of a build bug. See
  `BUILD_SYSTEM.md`.
- **Startup** — `AssetManager::register_fonts()` registers the current tier and below,
  skipping larger tiers to save `.rodata`.
- **Runtime** — `AssetManager::register_fonts_for_tier()` is re-entrant. A rising tier
  registers the delta; a same-or-lower tier is a no-op; nothing is ever unregistered,
  because live widgets hold pointers into those static faces.

**Ordering invariant.** `theme_manager_refresh_layout_constants()` must register the
tier *before* re-pointing the font tokens. Reversed, every raised token names a face
that was never registered and the existence check in
`theme_manager_register_responsive_fonts()` silently bounces it back down to `_large`.

Font token registration uses `lv_xml_update_const()`, not `lv_xml_register_const()`.
Const registration is first-write-wins, so on a second pass -- a runtime breakpoint
change or a theme reload -- registration would keep the startup value and the refresh
would be inert. These base tokens have no `globals.xml` declaration to protect; they
exist only because that function derives them.

Note what this does **not** do: `style_text_font` is resolved to a concrete
`lv_font_t*` at parse time and baked into the widget's style, so re-pointing a token
does not restyle widgets that already exist. The same has always been true of `px`
tokens. Widgets built after the refresh pick up the new tier; the eagerly-built root
panels do not, short of `NavigationManager::rebuild_active_views()`.

---

## Pre-Themed Widgets

These semantic widgets apply shared theme styles automatically. Use them instead of raw LVGL widgets.

### Typography

| Widget | Font | Text Style | Use Case |
|--------|------|-----------|----------|
| `<text_heading>` | font_heading | Muted | Section titles |
| `<text_body>` | font_body | Primary | Body paragraphs |
| `<text_muted>` | font_body | Muted | Secondary metadata |
| `<text_small>` | font_small | Muted | Helper text |
| `<text_xs>` | font_xs | Muted | Compact info, badges |
| `<text_button>` | font_body | Primary | Button labels (centered) |

**Attributes:**
- `text` - Label text (supports `bind_text="subject_name"`)
- `stroke_width` - Text outline thickness
- `stroke_color` - Outline color (hex)

```xml
<text_heading text="Section Title"/>
<text_body text="Description paragraph"/>
<text_muted text="Last updated: 5m ago"/>
<text_small text="Helper text"/>
```

### Containers

#### ui_card
Standard card surface with themed background, border, and radius.

```xml
<ui_card width="200" height="150">
  <text_body text="Card content"/>
</ui_card>
```

Adds the shared `StyleRole::Card` style (`ThemeManager::instance().get_style(StyleRole::Card)`) → `card_bg` color, `border` color, theme radius. See `src/ui/components/ui_card.cpp`.

#### ui_dialog
Modal/overlay container with elevated surface color.

```xml
<ui_dialog width="80%" height="60%">
  <text_heading text="Dialog Title"/>
  <!-- content -->
</ui_dialog>
```

Adds the shared `StyleRole::Dialog` style → `elevated_bg` color. See `src/ui/components/ui_dialog.cpp`.

### Buttons

`<ui_button>` supports multiple variants with auto-contrast text:

| Variant | Background | Use Case |
|---------|-----------|----------|
| `primary` | Primary accent | Main actions |
| `secondary` | Surface control | Secondary actions |
| `danger` | Danger red | Destructive actions |
| `success` | Success green | Confirmations |
| `warning` | Warning amber | Caution actions |
| `tertiary` | Tertiary accent | Tertiary actions |
| `ghost` | Transparent | Subtle actions |

**Attributes:**
- `variant` - Button style (default: "primary")
- `text` - Button label
- `icon` - MDI icon name
- `icon_position` - "left" (default) or "right"

```xml
<ui_button variant="primary" text="Save"/>
<ui_button variant="danger" text="Delete" icon="trash_can"/>
<ui_button variant="ghost" text="Cancel"/>
<ui_button icon="settings"/>  <!-- Icon only -->
```

**Auto-contrast:** Text color automatically adjusts based on background luminance. Dark backgrounds get light text, light backgrounds get dark text. This updates reactively when themes change.

### Icons

`<icon>` widget with color variants and sizes:

**Color Variants:**
| Variant | Color Source |
|---------|-------------|
| `text` | Primary text color |
| `muted` | Muted text color |
| `primary` | Primary accent |
| `secondary` | Secondary accent |
| `tertiary` | Tertiary accent |
| `success` | Success green |
| `warning` | Warning amber |
| `danger` | Danger red |
| `info` | Info blue |
| `disabled` | Text color at 50% opacity |

**Size Variants:**

Icon sizes map directly to fixed-size icon fonts (not responsive):

| Size | Font | Typical Use |
|------|------|-------------|
| `xs` | mdi_icons_16 | Inline with small text |
| `sm` | mdi_icons_24 | Buttons, list items |
| `md` | mdi_icons_32 | Card headers |
| `lg` | mdi_icons_48 | Status indicators |
| `xl` | mdi_icons_64 | Navigation, hero icons |

> **Note:** For responsive icon sizing, use the `icon_size` token (`size="#icon_size"`) which selects one of these named sizes per breakpoint (`icon_size_*` in `globals.xml`): `md` for micro, tiny and small; `lg` for medium; `xl` for large, xlarge and xxlarge. `sm` is never selected automatically — 24px is illegible on a 272px screen.

**Attributes:**
- `src` - MDI icon name ("home", "settings", "wifi")
- `size` - "xs", "sm", "md", "lg", "xl"
- `variant` - Color variant
- `color` - Custom color override (hex)

```xml
<icon src="home" size="lg" variant="primary"/>
<icon src="warning" size="md" variant="warning"/>
<icon src="settings" color="#FF0000"/>  <!-- Custom color -->
```

**C++ API:**
```cpp
helix::ui::icon::set_source(icon, "check");
helix::ui::icon::set_size(icon, "md");
helix::ui::icon::set_variant(icon, "success");
```

### Status Indicators

#### ui_spinner
Indeterminate loading spinner with themed arc color.

Spinner sizes are **responsive** - the pixel values vary by breakpoint (narrow axis):

| Size | Small (391-460) | Medium (461-550) | Large (551-700) | XLarge (701-1000) | XXLarge (>1000) |
|------|-----------------|-------------------|-----------------|-------------------|-----------------|
| `xs` | 12px / 2px arc | 14px / 2px arc | 16px / 2px arc | 18px / 2px arc | 20px / 2px arc |
| `sm` | 16px / 2px arc | 18px / 2px arc | 20px / 2px arc | 24px / 2px arc | 28px / 2px arc |
| `md` | 24px / 2px arc | 28px / 3px arc | 32px / 3px arc | 40px / 4px arc | 48px / 4px arc |
| `lg` | 48px / 3px arc | 56px / 4px arc | 64px / 4px arc | 80px / 5px arc | 96px / 6px arc |

> Note: no `_micro` or `_tiny` spinner variants are defined, so Micro and Tiny both resolve to the Small column. The `xs`/`sm` arc widths (`spinner_arc_xs`, `spinner_arc_sm`) are plain constants with no variants — 2px on every tier.

```xml
<spinner size="lg"/>
<spinner size="md" align="center"/>
```

Adds the shared `StyleRole::Spinner` style → primary accent color. See `src/ui/components/ui_spinner.cpp`.

#### severity_card
Status card with severity-colored border:

| Severity | Border Color |
|----------|-------------|
| `info` | Info blue |
| `success` | Success green |
| `warning` | Warning amber |
| `error` | Danger red |

```xml
<severity_card severity="warning">
  <text_body text="Nozzle temperature is high"/>
</severity_card>
```

Adds one of the shared `StyleRole::SeverityInfo` / `SeveritySuccess` / `SeverityWarning` / `SeverityDanger` styles, selected by the `severity` attribute. See `src/ui/components/ui_severity_card.cpp`.

### Layout

#### Dividers

```xml
<divider_horizontal/>  <!-- Full-width horizontal line -->
<divider_vertical/>    <!-- Full-height vertical line -->
```

Use `border` color from theme.

---

## XML Usage Examples

### Using Color Tokens

```xml
<!-- Inline color tokens: resolved from the palette at parse time, and re-colored
     on theme change by the widget-tree palette walk -->
<lv_obj style_bg_color="#card_bg" style_border_color="#border"/>
```

For a reactive surface that updates automatically without a tree walk, use a semantic widget
(`<ui_card>`, `<ui_dialog>`) — it adds the corresponding shared `StyleRole` style, which the
`ThemeManager` reconfigures in place on every theme change.

### Using Spacing Tokens

```xml
<lv_obj style_pad_all="#space_lg" style_pad_gap="#space_md"/>

<!-- Flex container with responsive gap -->
<lv_obj style_flex_flow="row" style_pad_gap="#space_sm">
  <ui_button text="A"/>
  <ui_button text="B"/>
</lv_obj>
```

### Using Pre-Themed Widgets

```xml
<ui_card width="100%" height="content">
  <text_heading text="Temperature"/>
  <text_body bind_text="nozzle_temp_subject"/>
  <ui_button variant="primary" text="Heat" icon="fire"/>
</ui_card>
```

### Semantic Widget Composition

```xml
<ui_dialog>
  <text_heading text="Confirm Action"/>
  <divider_horizontal/>
  <text_body text="Are you sure you want to proceed?"/>
  <lv_obj style_flex_flow="row" style_pad_gap="#space_md">
    <ui_button variant="ghost" text="Cancel"/>
    <ui_button variant="danger" text="Delete"/>
  </lv_obj>
</ui_dialog>
```

---

## C++ Integration

### When to Add Shared Styles Directly

Use `ThemeManager::instance().get_style(StyleRole::X)` when creating **custom widgets** that
need reactive theming. The pointer refers into the manager's style table; because re-theming
reconfigures that same style object in place, any widget that added it updates automatically.

```cpp
// In your custom widget's create handler
auto& tm = ThemeManager::instance();
lv_style_t* card_style = tm.get_style(StyleRole::Card);  // never null after init
lv_obj_add_style(obj, card_style, LV_PART_MAIN);
```

This is exactly the pattern the built-in widgets use — see `ui_card.cpp` (`StyleRole::Card`),
`ui_dialog.cpp` (`StyleRole::Dialog`), `ui_button.cpp` (variant → `StyleRole::Button*`),
`ui_text.cpp` (`StyleRole::TextPrimary` / `TextMuted`), and `ui_severity_card.cpp`.

### Available Style Roles

`StyleRole` (in `include/theme_manager.h`) is the complete list of shared styles. Highlights:

- **Surfaces:** `Card`, `Dialog`, `ObjBase`, `InputBg`
- **States:** `Disabled`, `Pressed`, `Focused`
- **Text:** `TextPrimary`, `TextMuted`, `TextSubtle`
- **Icons:** `IconText`, `IconPrimary`, `IconSecondary`, `IconTertiary`, `IconInfo`, `IconSuccess`, `IconWarning`, `IconDanger`
- **Buttons:** `Button`, `ButtonPrimary`, `ButtonSecondary`, `ButtonTertiary`, `ButtonDanger`, `ButtonGhost`, `ButtonTransparent`, `ButtonOutline`, `ButtonSuccess`, `ButtonWarning`, `ButtonDisabled`, `ButtonPressed`
- **Status:** `Spinner`, `Arc`, `SeverityInfo`, `SeveritySuccess`, `SeverityWarning`, `SeverityDanger`
- **Inputs:** `Dropdown`, `Checkbox`, `Switch`, `Slider`

Each role's colors are written by a matching `configure_*` function in `src/ui/style_configs.cpp`.

### When to Use theme_manager Tokens

Use the free `theme_manager_*` functions when you need **values** for dynamic styling
(custom drawing, layout math) rather than a shared style object:

```cpp
// Get themed color (resolves _light/_dark variant for the current mode)
lv_color_t primary = theme_manager_get_color("primary");

// Get spacing for custom layout
int32_t padding = theme_manager_get_spacing("space_lg");

// Get font for custom text
const lv_font_t* font = theme_manager_get_font("font_body");

// Pick readable text for a given background
lv_color_t text = theme_manager_get_contrast_color(bg_color);
```

### Listening for Theme Changes

For widgets that need custom update logic beyond a shared style, observe the theme-changed
subject (a monotonically increasing generation counter) exposed by
`theme_manager_get_changed_subject()`. Use an `ObserverGuard` member so cleanup is automatic
(see `src/ui/controls/ui_heating_animator.cpp`):

```cpp
lv_subject_t* theme_subject = theme_manager_get_changed_subject();
if (theme_subject) {
    theme_observer_ = ObserverGuard(theme_subject, theme_change_cb, this);
}
```

The observer fires after every `theme_manager_apply_theme()` (toggle, theme switch, preview).
Widgets that only need color updates should add a shared `StyleRole` style instead — those
refresh automatically without an observer.

---

## Glass appearance

**Settings > Appearance > Glass Effects** layers depth and lighting over whatever theme is
active. It is not a theme: every color glass draws is the widget's own color mixed toward white
or black (`include/glass_shading.h`), never another hue, so it is safe on any palette and on a
widget whose bg color was set locally.

**Effective glass = the gate passed && the setting is on.** `settings_glass_effects`
(`/display/glass_effects`) is the saved choice; `glass_available` is set only by the gate. While
either is 0, no hook is installed and the shared styles are configured exactly as in flat mode.

### How it draws

Glass rewrites draw tasks, not styles (`include/glass_painter.h`). `glass_attach(obj)` sets
`LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS` and handles `LV_EVENT_DRAW_TASK_ADDED`: each opaque flat
fill becomes the family's gradient over the fill's resolved color, and each border becomes the
family's rim; an outline (a focus ring) keeps its own color. A translucent fill (judged by the
part's own bg_opa, so a disabled button's faded fill is still shaded) or one that already has a
gradient is left alone. While glass is
effective, `glass_set_effective()` attaches every widget on every screen and layer, and the
theme apply callback (`helix_theme_apply`) attaches each widget as it is created, so hot reload
and new panels are covered. `ThemeManager::set_glass()` adds the soft drop shadows and the 1px
rim width to the Button, Card, Dialog and Pressed styles (`src/ui/style_configs.cpp`), and keeps
them across dark/light, theme loads and previews.

### Families

A widget part's family comes from the shared style it carries (`glass_family_of`):

| Family | Look | Who |
|---|---|---|
| Raised | light top edge fading to a darker bottom; flips while pressed | `Button` and the solid `Button*` roles |
| Surface | a corner light from the part's own top-left, `kGlassGlowRadiusPx` across | `Card`, `Dialog` |
| Recessed | dark top fading to the base | tracks, troughs, inputs |
| None | flat | `ButtonGhost`, `ButtonTransparent`, `ButtonOutline`, everything unclassified |

`glass_mark(obj, family)` overrides the styles. **Color that identifies something stays flat**:
a filament swatch, an LED color chip, any widget whose bg comes from user or filament data calls
`glass_mark(obj, GlassFamily::None)`. Content stays flat for the same reason: images, canvases,
camera, gcode and 3D previews, bed mesh and graphs are never classified.

### The gate

`include/glass_gate.h`. `decide_glass_gate()` fails outright below 24 bpp (the display's
color format, so a 32 bpp build on a 16 bpp framebuffer fails), on a tier
`full_style_effects_allowed()` rejects, on a software-rotated display, and on a NanoVG build
(`LV_USE_DRAW_NANOVG`), whose gradient renderer reads only the first two stops. Any other board is
measured once per board fingerprint (`include/board_fingerprint.h`, shared with the screensaver
gate) and app version: `kGlassProbeDelayMs` after boot, `measure_glass_frame_ms()` draws a glass
frame, drop shadows included, into a hidden canvas, and a median at or under `kGlassFrameBudgetMs` passes. The record is
stored at `/display/glass_probe`; a record from another version or board is ignored and the board
is measured again. `HELIX_GLASS=1`/`0` forces the verdict (`docs/devel/ENVIRONMENT_VARIABLES.md`).
The ESP32 firmware draws 16 bpp and does not compile glass.

---

## Quick Reference

### Adding a New Themed Widget

1. **Add a role** to the `StyleRole` enum in `include/theme_manager.h` (before `COUNT`).

2. **Write its configure function** in `src/ui/style_configs.cpp` — declare it in the
   `style_configs` namespace block and implement it to write palette colors into the style:
   ```cpp
   void configure_my_style(lv_style_t* s, const ThemePalette& p) {
       lv_style_set_bg_color(s, p.card_bg);
       lv_style_set_bg_opa(s, LV_OPA_COVER);
       // ...borders, radius, text color from p as needed
   }
   ```

3. **Bind role → configure fn** in `ThemeManager::register_style_configs()`
   (`src/ui/theme/theme_manager_new.cpp`):
   ```cpp
   styles_[static_cast<size_t>(StyleRole::MyStyle)].configure = configure_my_style;
   ```
   The manager inits and reconfigures every table entry automatically — there is no separate
   init/update/preview step to touch.

4. **Add the shared style** in your widget's create handler:
   ```cpp
   lv_obj_add_style(obj, ThemeManager::instance().get_style(StyleRole::MyStyle), LV_PART_MAIN);
   ```

5. **Register the widget** with the XML parser:
   ```cpp
   lv_xml_register_widget("my_widget", my_widget_create, my_widget_apply);
   ```

Widget *parts* not modeled as a `StyleRole` (e.g. a custom knob or indicator) follow the
file-static pattern in `theme_lvgl_apply.cpp` instead — add an `lv_style_t`, configure it in
`init_extra_styles()` / `update_handle_styles()`, and attach it in `helix_theme_apply()`.

### Adding a New Color Token

1. Add to theme JSON files (`assets/config/themes/defaults/*.json`):
   ```json
   "dark": { "my_color": "#hexvalue" },
   "light": { "my_color": "#hexvalue" }
   ```

2. If semantic (part of 16-color palette), add to `ModePalette` in `theme_loader.h`

3. Reference in XML: `style_bg_color="#my_color"`

### Adding Responsive Spacing

1. Add triplet to `ui_xml/globals.xml`:
   ```xml
   <px name="my_space_small" value="8"/>
   <px name="my_space_medium" value="12"/>
   <px name="my_space_large" value="16"/>
   ```

   The declaration MUST be at the top level of `ui_xml/` — discovery does not recurse into `ui_xml/components/`, `ui_xml/portrait/` or any other subdirectory, so a suffixed token declared there is never registered and every `#reference` to it silently resolves to nothing. Enforced by `scripts/check_responsive_token_scope.py` (prestonbrown/helixscreen#1211).

2. Classify the axis. Heights, top/bottom padding and vertical maxima go in `VERTICAL_AXIS_TOKENS` (`src/ui/theme/theme_responsive.cpp`); widths and anything axis-neutral (all `space_*`) need no change. If in doubt, neutral.

3. Use in XML: `style_pad_all="#my_space"`

4. Use in C++: `theme_manager_get_spacing("my_space")`

---

## File Reference

| File | Purpose |
|------|---------|
| `include/theme_manager.h` | `ThemeManager` class, `StyleRole` enum, `ThemePalette` struct, free `theme_manager_*` API |
| `src/ui/theme/theme_manager_new.cpp` | `ThemeManager` implementation: style table, `apply_palette()`, `set_dark_mode()`, `get_style()`, `get_color()`, preview |
| `src/ui/style_configs.cpp` | `configure_*` functions — one per `StyleRole`, writes palette colors into each shared style |
| `src/ui/theme/theme_manager.cpp` | Core state (`ThemeRuntime`, `ThemeSubjects`), `theme_manager_init()`/`deinit()`, `theme_manager_apply_theme()`, change broadcast, color lookups |
| `src/ui/theme/theme_token_scan.cpp` | Expat discovery of `<color>`/`<px>`/`<string>` tokens in `ui_xml/` and the constant-set validator |
| `src/ui/theme/theme_tokens_register.cpp` | XML constant registration: semantic colors, theme properties, static constants, swatch subjects |
| `src/ui/theme/theme_responsive.cpp` | Breakpoint classification, responsive px tokens, overlay width constants, resize refresh |
| `src/ui/theme/theme_fonts.cpp` | Responsive font tokens, `is_icon_font()`, `theme_manager_get_font()` |
| `src/ui/theme/theme_lvgl_apply.cpp` | `helix_theme_apply()`, shared widget-part styles, palette conversions |
| `src/ui/theme/theme_live_recolor.cpp` | Palette walk over a live widget tree and the colour-swap maps (`set_swap_maps()`) |
| `src/ui/theme/theme_color_math.cpp` | Hex parsing, brightness, saturation, WCAG contrast helpers |
| `include/glass_shading.h`, `include/glass_painter.h`, `include/glass_gate.h` | Glass appearance: the derived stops, the draw-task painter and families, the gate and live switching |
| `src/ui/overlay_geometry.cpp` | Nav width ladder, overlay widths and heights, `ui_set_overlay_geometry()` |
| `src/ui/theme/theme_manager_internal.h` | `helix::theme_detail`: what these files share. Not public API |
| `src/ui/theme/theme_loader.cpp`, `include/theme_loader.h` | Parses theme JSON into `ThemeData` / `ModePalette` |
| `ui_xml/globals.xml` | Spacing, font, and icon token definitions |
| `assets/config/themes/defaults/*.json` | Theme color definitions |
