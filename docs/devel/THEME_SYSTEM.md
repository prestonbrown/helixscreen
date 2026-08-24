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
| **`ThemeManager`** (singleton) | `src/ui/theme_manager_new.cpp`, style configs in `src/ui/style_configs.cpp` | Owns the fixed table of shared `lv_style_t` objects (one per `StyleRole`), reconfigures them against the active `ThemePalette`, and fires `lv_obj_report_style_change()` |
| **Free functions** (`theme_manager_*`) | `src/ui/theme_manager.cpp` | Token lookup, XML constant registration, responsive spacing/font constants, theme loading, and the unified `theme_manager_apply_theme()` re-theme entry point |

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
`theme_manager.cpp`, keyed on widget class. Widget parts not covered by `StyleRole`
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
theme_manager_refresh_widget_tree()      ← lv_obj_refresh_style() on every widget (picks up inline styles)
    ↓
theme_apply_current_palette_to_tree()    ← re-colors widgets with baked XML inline colors
    ↓
theme_manager_notify_change()            ← bumps the theme-changed subject (generation counter)
```

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
is how `theme_manager.cpp` registers each palette color into the `ui_xml` global scope
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
// theme_manager.cpp checks for both variants
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

**Two ladders feed that one table** (`src/ui/theme_manager.cpp`):

| Ladder | Scalar | Used by |
|--------|--------|---------|
| Cramped axis (default) | `responsive_dimension()` = `min(width, height)` | Fonts, spacing, widths, everything axis-neutral |
| Vertical axis | `responsive_vertical_dimension()` = `height` | The height tokens listed in `VERTICAL_AXIS_TOKENS` |

On a landscape display the narrow axis usually *is* the height, which is why "height-based" looked right for years; on a portrait panel it is the width. Because `min(w, h) == h` on landscape and square displays, the two ladders only diverge in portrait. A 320×1480 panel resolves `#space_lg` from 320 (`_tiny`, 8px) and `#button_height` from 1480 (`_xxlarge`, 96px).

`theme_manager_resolve_px_tokens()` is the single place that applies this policy; startup registration and the resize path both apply its output, so the two can never disagree. The `ui_breakpoint` subject keeps holding the **cramped** tier — all XML `ref_value` bindings are written against it. Tier boundaries live in `breakpoint_for()` (`include/ui_breakpoint.h`).

Full explanation, worked example, and the rule for classifying a new token: [UI_CONTRIBUTOR_GUIDE.md § Screen Breakpoints](UI_CONTRIBUTOR_GUIDE.md#2-screen-breakpoints). Background: prestonbrown/helixscreen#1209.

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

Adds the shared `StyleRole::Card` style (`ThemeManager::instance().get_style(StyleRole::Card)`) → `card_bg` color, `border` color, theme radius. See `src/ui/ui_card.cpp`.

#### ui_dialog
Modal/overlay container with elevated surface color.

```xml
<ui_dialog width="80%" height="60%">
  <text_heading text="Dialog Title"/>
  <!-- content -->
</ui_dialog>
```

Adds the shared `StyleRole::Dialog` style → `elevated_bg` color. See `src/ui/ui_dialog.cpp`.

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
ui_icon_set_source(icon, "check");
ui_icon_set_size(icon, "md");
ui_icon_set_variant(icon, "success");
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

Adds the shared `StyleRole::Spinner` style → primary accent color. See `src/ui/ui_spinner.cpp`.

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

Adds one of the shared `StyleRole::SeverityInfo` / `SeveritySuccess` / `SeverityWarning` / `SeverityDanger` styles, selected by the `severity` attribute. See `src/ui/ui_severity_card.cpp`.

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
(see `src/ui/ui_heating_animator.cpp`):

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
   (`src/ui/theme_manager_new.cpp`):
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
file-static pattern in `theme_manager.cpp` instead — add an `lv_style_t`, configure it in
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

2. Classify the axis. Heights, top/bottom padding and vertical maxima go in `VERTICAL_AXIS_TOKENS` (`src/ui/theme_manager.cpp`); widths and anything axis-neutral (all `space_*`) need no change. If in doubt, neutral.

3. Use in XML: `style_pad_all="#my_space"`

4. Use in C++: `theme_manager_get_spacing("my_space")`

---

## File Reference

| File | Purpose |
|------|---------|
| `include/theme_manager.h` | `ThemeManager` class, `StyleRole` enum, `ThemePalette` struct, free `theme_manager_*` API |
| `src/ui/theme_manager_new.cpp` | `ThemeManager` implementation: style table, `apply_palette()`, `set_dark_mode()`, `get_style()`, `get_color()`, preview |
| `src/ui/style_configs.cpp` | `configure_*` functions — one per `StyleRole`, writes palette colors into each shared style |
| `src/ui/theme_manager.cpp` | Free functions: token/const registration, responsive spacing/fonts, `helix_theme_apply()`, `theme_manager_apply_theme()`, widget-tree refresh, contrast helpers |
| `src/ui/theme_loader.cpp`, `include/theme_loader.h` | Parses theme JSON into `ThemeData` / `ModePalette` |
| `ui_xml/globals.xml` | Spacing, font, and icon token definitions |
| `assets/config/themes/defaults/*.json` | Theme color definitions |
