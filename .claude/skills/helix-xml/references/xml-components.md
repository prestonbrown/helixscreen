# XML Components — API, Props, Custom Semantic Widgets

## Component Definition

```xml
<component>
    <api>                                    <!-- Optional: properties from parent -->
        <prop name="text" type="string" default="Click me"/>
        <prop name="enabled" type="bool" default="true"/>
        <prop name="color" type="color" default="0xff4444"/>
        <prop name="data" type="subject"/>   <!-- Subject reference -->
        <prop name="count" type="int" default="0"/>
    </api>
    <consts>                                 <!-- Optional: local constants -->
        <px name="button_size" value="36"/>
    </consts>
    <styles>                                 <!-- Optional: local styles (NO style_ prefix) -->
        <style name="base" bg_color="0x333" text_color="0xfff"/>
    </styles>
    <view extends="lv_button" width="#button_size">   <!-- UI tree -->
        <lv_label text="$text" align="center"/>       <!-- $ prefix for props -->
        <style name="base"/>
    </view>
</component>
```

### Property Types
| Type | Description | Example |
|------|-------------|---------|
| `string` | Text values | `default="Hello"` |
| `int` | Integer numbers | `default="42"` |
| `bool` | true/false | `default="true"` |
| `color` | Hex colors | `default="0xff4444"` |
| `subject` | Subject references | For data binding |

## Component Usage

Components are instantiated by their registered name:

```xml
<!-- In parent XML -->
<home_panel name="home_panel"/>
<icon src="home" size="lg"/>
<text_heading text="Title"/>
```

### Passing Props

```xml
<!-- Static values -->
<my_card title="Status" color="0xff0000"/>

<!-- Bind to subject -->
<my_component data="my_subject"/>

<!-- Use component constants with # -->
<lv_obj width="#button_size"/>
```

## Custom Semantic Widgets

### ui_card
Container with card styling from theme_core.

**Don't specify:** `style_radius`, `style_bg_color`, `style_border_*`
```xml
<ui_card name="my_card" width="100%" height="200">
    <text_body text="Content"/>
</ui_card>
```

### ui_button
Semantic button with variant-based styling and auto-contrast text.

**Variants:** `primary`, `secondary`, `ghost`, `destructive`
**Don't specify:** `style_radius`, `style_bg_color`, `style_height`, text color
```xml
<ui_button variant="primary" text="Save"/>
<ui_button variant="ghost" icon="settings"/>
<ui_button variant="primary" icon="check" text="Confirm"/>
<!-- Reactive text -->
<ui_button text="@my_subject"/>          <!-- @ prefix = subject -->
<ui_button bind_text="my_subject"/>      <!-- bind_text = always subject -->
```

### text_heading / text_body / text_small
Responsive semantic typography.

**Don't specify:** `style_text_font`, `style_text_color`

| Component | Purpose | Responsive Sizing |
|-----------|---------|-------------------|
| `text_heading` | Section titles | 20px / 26px / 28px |
| `text_body` | Primary content | 14px / 18px / 20px |
| `text_small` | Captions | 12px / 16px / 18px |

All support `bind_text`, `align`, `style_text_color`, etc.

### icon
Font-based icons (Material Design Icons).

**Sizes:** `xs` (16), `sm` (24), `md` (32), `lg` (48), `xl` (64)
**Variants:** `primary`, `secondary`, `accent`, `disabled`, `warning`
**Don't specify:** font selection

### divider_vertical / divider_horizontal
Visual separators. **Don't specify:** `style_bg_color`, width/height (1px default).

### spinner
Loading indicator. Sizes: `sm`, `md`, `lg`.

### ui_markdown
Markdown → native LVGL widgets. Theme-aware fonts/colors/spacing.
```xml
<ui_markdown bind_text="content" width="100%"/>
```
**Don't specify:** any styling (all theme-aware).

## Widget Defaults Quick Reference

| Widget | Don't Specify (Built-in) |
|--------|--------------------------|
| `ui_card` | `style_radius`, `style_bg_color`, `style_border_*` |
| `ui_button` | `style_radius`, `style_bg_color`, `style_height`, text color |
| `text_*` | `style_text_font`, `style_text_color` |
| `icon` | Font selection |
| `divider_*` | `style_bg_color`, width/height |
| `ui_markdown` | All styling |

## Component Registration

Components must be registered in C++ before use:

```cpp
// In xml_registration.cpp
lv_xml_register_component_from_file("A:ui_xml/my_component.xml");
```

Order matters for dependencies — register custom widgets and sub-components before components that use them. After `globals.xml`, order is generally flexible.

## Component Naming

Always add `name` attribute:
```xml
<!-- ❌ WRONG -->
<controls_panel/>
<!-- ✅ CORRECT -->
<controls_panel name="controls_panel"/>
```

Widgets need names when: C++ lookup via `lv_obj_find_by_name()`, interactive types (buttons/sliders/dropdowns), subject binding (`bind_text`, `bind_value`).

Can omit names for: layout containers, spacers, static labels, decorative elements.
