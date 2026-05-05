# Modal Dialog System

Four-layer architecture for blocking user interactions:

```
Modal class (C++ RAII lifecycle, show/hide, button wiring)
  +-> ModalStack (singleton, z-order tracking, entrance/exit animations)
    +-> ui_dialog (XML custom widget, theme-aware card background)
      +-> Reusable XML components:
            modal_button_row  (divider + 2-button footer)
            modal_header      (icon + title row)
            modal_dialog      (generic title/message dialog)
```

## When to Use What

| Mechanism | Use Case |
|-----------|----------|
| **Modal** | Blocking user decision, confirmation, form input |
| **Overlay** | Full-screen secondary UI (can be "backed out" of) |
| **Panel** | Main navigation content |

- User must respond before continuing → **Modal**
- Replaces current screen but can back out → **Overlay** (`ui_nav_push_overlay()`)
- Primary navigation destination → **Panel**

## Three Ways to Create Modals

### 1. Confirmation/Alert Helpers (simplest, no custom XML)

```cpp
#include "ui_modal.h"

// Two-button confirmation
dialog_ = ui_modal_show_confirmation(
    lv_tr("Delete File?"),
    lv_tr("This cannot be undone."),
    ModalSeverity::Warning,
    lv_tr("Delete"),
    on_confirm_cb, on_cancel_cb, this);

// Single-button alert
ui_modal_show_alert(
    lv_tr("Tip of the Day"),
    lv_tr("You can long-press the home button..."),
    ModalSeverity::Info);
```

Severity levels: `Info` (blue), `Warning` (yellow), `Error` (red).

Use `ModalGuard` for RAII cleanup:
```cpp
#include "ui/ui_modal_guard.h"

class MyPanel {
    helix::ui::ModalGuard delete_dialog_;  // Auto-hides in destructor
};
```

### 2. Static Modal::show() (custom XML, no subclass)

```cpp
lv_obj_t* dialog = Modal::show("my_custom_modal");
lv_obj_t* btn = lv_obj_find_by_name(dialog, "btn_primary");
// Later:
Modal::hide(dialog);
```

### 3. Modal Subclass (custom XML + C++ logic)

```cpp
class PrintCancelModal : public Modal {
public:
    const char* get_name() const override { return "Print Cancel"; }
    const char* component_name() const override { return "print_cancel_confirm_modal"; }

protected:
    void on_show() override {
        wire_ok_button("btn_primary");       // → on_ok()
        wire_cancel_button("btn_secondary"); // → on_cancel()
    }

    void on_ok() override {
        if (on_confirm_cb_) on_confirm_cb_();
        hide();
    }
};
```

## Modal Subclass API

### Pure Virtuals
| Method | Purpose |
|--------|---------|
| `get_name()` | Human-readable name for logs |
| `component_name()` | XML component name for `lv_xml_create()` |

### Lifecycle Hooks (optional overrides)
| Hook | Default | When Called |
|------|---------|------------|
| `on_show()` | no-op | After modal created and visible |
| `on_hide()` | no-op | Before modal destroyed |
| `on_ok()` | `hide()` | Primary button clicked |
| `on_cancel()` | `hide()` | Secondary button clicked |
| `on_tertiary()` through `on_senary()` | `hide()` | Button 3-6 clicked |

### Button Wiring (in on_show)

```cpp
void on_show() override {
    wire_ok_button("btn_primary");         // → on_ok()
    wire_cancel_button("btn_secondary");   // → on_cancel()
    wire_tertiary_button("btn_tertiary");  // → on_tertiary()
}
```

**No XML callback attribute needed** on buttons when using wire_*_button(). The modal handles it.

### Protected Members
- `backdrop_` — full-screen backdrop overlay
- `dialog_` — the dialog card widget
- `parent_` — parent passed to show()
- `find_widget(name)` — convenience wrapper for `lv_obj_find_by_name(dialog_, name)`

## XML Template

```xml
<?xml version="1.0"?>
<component>
  <!-- NOTE: Backdrop created programmatically by Modal system -->
  <view name="my_modal"
        extends="ui_dialog" width="70%" height="content" align="center"
        flex_flow="column" style_flex_main_place="start" style_pad_gap="0">

    <modal_header icon_src="alert" icon_variant="warning"
                  title="Title" title_tag="Title"/>

    <lv_obj width="100%" height="content"
            style_pad_left="#space_lg" style_pad_right="#space_lg"
            style_pad_top="0" style_pad_bottom="#space_lg">
      <text_body name="dialog_message" width="100%" text="Message" long_mode="wrap"/>
    </lv_obj>

    <modal_button_row
        secondary_text="Cancel" secondary_callback="on_cancel"
        primary_text="Confirm" primary_callback="on_confirm"/>
  </view>
</component>
```

Key rules:
- Always `extends="ui_dialog"` (never plain `lv_obj` or `ui_card`)
- Never include backdrop in XML (created by Modal system)
- Use `modal_button_row` for standard two-button footers
- Use `modal_header` for icon + title rows
- Use design tokens for spacing

## Reusable XML Components

### modal_button_row
Two-button footer with divider.

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `primary_text` | string | "OK" | Primary (right) button label |
| `secondary_text` | string | "Cancel" | Secondary (left) button label |
| `primary_callback` | string | -- | XML callback name |
| `secondary_callback` | string | -- | XML callback name |
| `primary_bg_color` | string | "" | Override button color (e.g., `#danger`) |
| `show_secondary` | string | "true" | Show/hide secondary button |

### modal_header
Icon + title row.

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `icon_src` | string | "" | Icon name |
| `icon_variant` | string | "accent" | Color variant |
| `title` | string | "" | Header text |
| `title_tag` | string | "" | Translation tag |

### ui_dialog
Base container for modal cards. Provides theme-aware background, rounded corner clipping, disabled state at 50% opacity. Extends from this, not `lv_obj` or `ui_card`.

## Advanced Patterns

### Dynamic Content (SubjectManager)
```cpp
class AmsEditModal : public Modal {
    SubjectManager subjects_;
    void on_show() override { init_subjects(); }
    void on_hide() override { deinit_subjects(); }
};
```

### Many Buttons (up to 6)
```cpp
void on_show() override {
    wire_ok_button("btn_load");        // → on_ok()
    wire_cancel_button("btn_resume");  // → on_cancel()
    wire_tertiary_button("btn_cancel"); // → on_tertiary()
}
```

### Keyboard Input
```cpp
void on_show() override {
    lv_obj_t* textarea = find_widget("password_input");
    ui_modal_register_keyboard(dialog(), textarea);
}
```

### Custom Button Styling
```xml
<modal_button_row primary_bg_color="#danger" .../>
```

## Async Callback Safety

Modal provides `lifetime_` (AsyncLifetimeGuard) automatically:
```cpp
void MyModal::start_operation() {
    auto token = lifetime_.token();
    api->fetch([this, token]() {
        if (token.expired()) return;
        token.defer([this]() { lv_subject_set_int(&result_, 1); });
    });
}
```

`hide()` calls `lifetime_.invalidate()` before `on_hide()`.

## ModalStack Internals

- Entrance: 250ms scale (85%→100%) + fade in
- Exit: 150ms scale down + fade out, then destroy
- Don't interact with ModalStack directly — use Modal class API

## Legacy API

| Legacy | Current |
|--------|---------|
| `ui_modal_show(name)` | `Modal::show(name)` |
| `ui_modal_hide(dialog)` | `Modal::hide(dialog)` |
| `ui_modal_get_top()` | `Modal::get_top()` |
| `ui_modal_is_visible()` | `Modal::any_visible()` |
