# Modal System (Developer Guide)

How the modal dialog system works internally, how to create new modals, and how to migrate old patterns to the standardized system.

**Key files**: `include/ui_modal.h`, `src/ui/ui_modal.cpp`, `src/ui/ui_dialog.cpp`, `ui_xml/modal_dialog.xml`

---

## Architecture Overview

The modal system has four layers:

```
Modal class (C++ RAII lifecycle, show/hide, button wiring)
  |
  +-> ModalStack (singleton, z-order tracking, entrance/exit animations)
  |
  +-> ui_dialog (XML custom widget, theme-aware card background)
  |
  +-> Reusable XML components:
        modal_button_row  (divider + 2-button footer, optional 3rd action)
        modal_header      (icon + title row, optional close button)
        modal_dialog      (generic title/message dialog)
```

### How It Works

1. **C++ calls `Modal::show()` or `modal.show(parent)`**
2. A full-screen **backdrop** is created programmatically (semi-transparent overlay)
3. The XML component is instantiated via `lv_xml_create()` inside the backdrop
4. The dialog gets an **entrance animation** (scale + fade)
5. The **ModalStack** tracks the backdrop/dialog pair for z-ordering
6. On hide, an **exit animation** plays, then both backdrop and dialog are destroyed

Backdrops are always created in C++ -- never in XML. This avoids the old pattern of inline XML backdrops that caused double-backdrop bugs and inconsistent behavior.

---

## When to Use What

| Mechanism | Use Case | Examples |
|-----------|----------|---------|
| **Modal** | Blocking user decision, confirmation, form input | Print cancel, Z-offset save, WiFi password, AMS edit |
| **Overlay** | Full-screen or near-full-screen secondary UI | Network settings, print tune, theme editor |
| **Panel** | Main navigation content | Home, controls, settings, print select |

**Rules of thumb:**
- If the user must respond before continuing, use a **Modal**
- If it replaces the current screen but can be "backed out" of, use an **Overlay** (`NavigationManager::instance().push_overlay()`)
- If it's a primary navigation destination, use a **Panel**

---

## Three Ways to Create Modals

The system supports three approaches, from simplest to most flexible.

### 1. Confirmation/Alert Helpers (no subclass, no custom XML)

For standard "title + message + buttons" dialogs, use the helper functions. These use the built-in `modal_dialog.xml` component.

> **Namespace:** every modal helper below lives in `namespace helix::ui` (declared in
> `include/ui_modal.h`). There is no `ui_modal_*` prefix. Snippets fully-qualify
> the calls; add a `using namespace helix::ui;` if you prefer the short form.

```cpp
#include "ui_modal.h"

// Confirmation dialog (two buttons: confirm + cancel). The callbacks are
// std::function, the dialog closes itself on a press, and owner_token gates
// all three callbacks.
helix::ui::ConfirmOptions opts;
opts.on_cancel = [this] { cancel_the_thing(); };
opts.owner_token = lifetime_.token();
dialog_ = helix::ui::modal_confirm(
    lv_tr("Delete File?"),
    lv_tr("This cannot be undone."),
    ModalSeverity::Warning,
    lv_tr("Delete"),
    [this] { delete_the_file(); },
    opts);

// Alert dialog (single OK button).
helix::ui::modal_alert(
    lv_tr("Tip of the Day"),
    lv_tr("You can long-press the home button..."),
    ModalSeverity::Info);
```

`helix::ui::modal_confirm()` returns the dialog widget pointer for callers that dismiss it programmatically. Store it in a `ModalGuard` for RAII:

```cpp
#include "ui/ui_modal_guard.h"

class MyPanel {
    helix::ui::ModalGuard delete_dialog_;  // Auto-hides in destructor

    void show_delete() {
        delete_dialog_ = helix::ui::modal_confirm(...);
    }
};
```

> **Pass `on_dismiss` whenever the caller holds state the buttons are meant to resolve.**
> Backdrop click, ESC and `Modal::rebuild_top()` (XML hot reload, dev builds only) close
> the dialog without either button being pressed, so `on_confirm`/`on_cancel` never run. A
> re-entry guard, a pending-request flag, a stored dialog pointer or a queue entry left
> behind by that leaks, and the feature silently stops working with no log line to say why.
>
> The helpers are backed by an internal `ConfirmationModal` instance, so `on_hide()` is the
> always-fires resolve point and `on_dismiss` is called from it - deferred to the next tick,
> so a callback that closes another dialog cannot re-enter the teardown it was called from.
>
> `on_dismiss` means "closed by something other than you": every close that is not the
> caller's own `Modal::hide()` reports - a backdrop tap, ESC, a hot-reload rebuild, even a
> button press whose side carries no callback. The caller's own programmatic `hide()` does
> NOT fire it (each `hide()` entry point defaults to `Programmatic`, which `on_hide()` gates
> on), so closing the dialog from teardown cannot arm a deferred callback against a
> destructor that has already run. Nulling the stored handle first is still good hygiene,
> but it is no longer the only thing standing between a close and a use-after-free.
>
> **If the capture can die before the dialog, pass a lifetime token too.** The dialog
> outlives its exit animation, and a `std::function` capturing a panel that has since been
> destroyed is a use-after-free. Hand it a token from the owner's `AsyncLifetimeGuard`
> (`lifetime_.token()`) and the call is skipped once the owner is gone.
>
> **New code should prefer `modal_confirm()` / `modal_alert()`.** They take `std::function`
> throughout, attach nothing of the caller's to a widget, and close the dialog themselves -
> so a callback never has to guess at `Modal::get_top()` to dismiss the dialog it was fired
> from. Their `owner_token` also gates **all three** callbacks, not just the dismissal,
> which became possible when the `lv_event_cb_t` forms were retired: those callbacks were
> invoked by LVGL straight off the button, so their `void*` still has to outlive the dialog
> on its own.

**Severity levels** control the header icon:
- `ModalSeverity::Info` -- blue info icon
- `ModalSeverity::Warning` -- yellow alert icon
- `ModalSeverity::Error` -- red octagon icon

### 2. Static `Modal::show()` (no subclass, custom XML)

For modals with custom XML layout but no complex C++ logic:

```cpp
// Show a custom XML modal
lv_obj_t* dialog = Modal::show("my_custom_modal");

// Wire up buttons or do work with the dialog...
lv_obj_t* btn = lv_obj_find_by_name(dialog, "btn_primary");

// Later, hide it
Modal::hide(dialog);
```

### 3. Modal Subclass (custom XML + C++ logic)

For modals with complex behavior, subclass `Modal`:

```cpp
// ui_print_cancel_modal.h
class PrintCancelModal : public Modal {
public:
    using ConfirmCallback = std::function<void()>;

    const char* get_name() const override { return "Print Cancel"; }
    const char* component_name() const override { return "print_cancel_confirm_modal"; }

    void set_on_confirm(ConfirmCallback cb) { on_confirm_cb_ = std::move(cb); }

protected:
    void on_show() override {
        wire_ok_button("btn_primary");       // "Stop" button
        wire_cancel_button("btn_secondary"); // "Keep Printing" button
    }

    void on_ok() override {
        if (on_confirm_cb_) on_confirm_cb_();
        hide();
    }

private:
    ConfirmCallback on_confirm_cb_;
};
```

Usage:

```cpp
// In the panel that owns the modal
PrintCancelModal cancel_modal_;

void show_cancel_dialog() {
    cancel_modal_.set_on_confirm([this]() { execute_cancel(); });
    cancel_modal_.show(lv_screen_active());
}
```

The Modal destructor auto-hides if visible, so storing a `Modal` subclass as a member provides RAII cleanup for free.

---

## Modal Subclass API Reference

### Pure Virtuals (must implement)

| Method | Purpose |
|--------|---------|
| `get_name()` | Human-readable name for log messages |
| `component_name()` | XML component name passed to `lv_xml_create()` |

### Lifecycle Hooks (optional overrides)

| Hook | Default | When Called |
|------|---------|------------|
| `on_show()` | no-op | After modal is created and visible |
| `on_hide()` | no-op | Before modal is destroyed |
| `on_ok()` | `hide()` | When primary button is clicked |
| `on_cancel()` | `hide()` | When secondary button is clicked |
| `on_tertiary()` | `hide()` | Third button clicked |
| `on_quaternary()` | `hide()` | Fourth button clicked |
| `on_quinary()` | `hide()` | Fifth button clicked |
| `on_senary()` | `hide()` | Sixth button clicked |

### Button Wiring Helpers

Call these in `on_show()` to connect XML buttons to the hook methods:

```cpp
void on_show() override {
    wire_ok_button("btn_primary");         // -> on_ok()
    wire_cancel_button("btn_secondary");   // -> on_cancel()
    wire_tertiary_button("btn_tertiary");  // -> on_tertiary()
    // ...etc
}
```

The button names must match `name="..."` attributes in your XML. These use `lv_obj_find_by_name()` internally.

**`wire_*_button()` is self-sufficient.** Each call:
1. Sets `user_data` on the button to `this` (the Modal instance)
2. Adds a direct `LV_EVENT_CLICKED` handler that routes to the corresponding virtual method

This means **no XML callback attribute is needed** on the button. `modal_button_row` can omit `primary_callback` / `secondary_callback` entirely when using a Modal subclass:

```xml
<!-- Preferred for Modal subclasses — no callback attributes needed -->
<modal_button_row primary_text="Save" secondary_text="Cancel"/>
```

The generic XML callbacks (`on_modal_ok_clicked`, `on_modal_cancel_clicked`) are retained for the static `Modal::show("component")` API, which doesn't use `wire_*_button()`.

If both an XML callback and `wire_*_button()` are present on the same button, the handler may fire twice — this is safe because `hide()` guards against double-execution.

### Protected Members

| Member | Type | Purpose |
|--------|------|---------|
| `backdrop_` | `lv_obj_t*` | The full-screen backdrop overlay |
| `dialog_` | `lv_obj_t*` | The dialog card widget |
| `parent_` | `lv_obj_t*` | Parent passed to `show()` |

### Helper: `find_widget(name)`

Convenience wrapper for `lv_obj_find_by_name(dialog_, name)`. Use in `on_show()` for custom widget access.

---

## XML Components

### `ui_dialog` (Custom Widget)

The base container for all modal dialog cards. Registered as a custom LVGL XML widget that provides:

- Theme-aware background color (adapts to light/dark mode via `ThemeManager`)
- Zero padding, zero border, zero shadow by default
- Rounded corner clipping (for full-bleed bottom buttons)
- Disabled state at 50% opacity
- `LV_OBJ_FLAG_USER_1` flag for context-aware input styling. `ThemeManager`
  answers "am I inside a dialog" by walking an object's parents looking for this
  bit, so **nothing else may set it**, on any object, for any reason. It is one
  of only four user flag bits; see the ledger in
  [chapter 09 — Home panel widgets](architecture/09-home-widgets.md) before claiming
  one

Usage in XML:

```xml
<view name="my_modal"
      extends="ui_dialog" width="70%" height="content" align="center"
      flex_flow="column" style_flex_main_place="start" style_pad_gap="0">
  <!-- content here -->
</view>
```

### `modal_button_row`

Reusable button footer with divider. Provides the standard "secondary | primary" layout, plus an optional third (tertiary) leading action.

**API props:**

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `primary_text` | string | "OK" | Primary (right) button label |
| `secondary_text` | string | "Cancel" | Secondary (left) button label |
| `primary_callback` | string | -- | Registered XML callback name |
| `secondary_callback` | string | -- | Registered XML callback name |
| `primary_tag` / `secondary_tag` | string | "" | Translation tag for the label |
| `primary_icon` / `secondary_icon` | string | "" | Optional leading icon name |
| `primary_variant` | string | "primary" | Primary button style variant (`primary`, `danger`, ...) |
| `show_secondary` | string | "true" | Show/hide secondary button |
| `tertiary_text` | string | "" | Optional third (leading) action label |
| `tertiary_callback` | string | "" | Registered XML callback for the tertiary button |
| `tertiary_tag` | string | "" | Translation tag for the tertiary label |
| `tertiary_icon` | string | "" | Optional icon for the tertiary button |
| `tertiary_variant` | string | "secondary" | Tertiary button style variant |
| `hide_tertiary` | string | "true" | Hidden by default; pass `"false"` to reveal it |

Use `primary_variant="danger"` (not a color override) for destructive primaries. The
tertiary button is hidden by default so existing two-button callers are unaffected; a
hidden flex child drops out of layout entirely.

Usage in XML:

```xml
<modal_button_row
    secondary_text="Cancel" secondary_callback="on_my_cancel"
    primary_text="Delete" primary_callback="on_my_confirm"
    primary_variant="danger"/>
```

The component renders as (tertiary revealed):

```
+--[divider_horizontal]-------------------+
| [Reset]  [Cancel]    |    [Delete]      |
+-----------------------------------------+
```

Buttons are edge-to-edge with zero radius, matching the `modal_dialog.xml` style.

### `modal_header`

Reusable icon + title row for modal headers.

**API props:**

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `icon_src` | string | "" | Icon name (e.g., "alert", "alert_octagon") |
| `icon_variant` | string | "accent" | Icon color variant |
| `hide_icon` | string | "false" | Title-only modals pass `"true"` to drop the leading icon |
| `title` | string | "" | Header text |
| `title_tag` | string | "" | Translation tag |
| `title_subject` | string | -- | Bind the title to a subject (attribute dropped when omitted) |
| `hide_close` | string | "true" | Pass `"false"` to show a close (X) button |
| `close_callback` | string | "" | Registered XML callback fired by the close button |

Usage in XML:

```xml
<modal_header icon_src="alert_octagon" icon_variant="danger"
              title="Factory Reset" title_tag="Factory Reset"/>
```

### `modal_dialog`

The generic title + message dialog used by `helix::ui::modal_confirm()` and `helix::ui::modal_alert()`. Uses subject bindings for dynamic content:

- `dialog_severity` -- controls which icon is shown (0=info, 1=warning, 2=error)
- `dialog_show_cancel` -- toggles cancel button visibility
- `dialog_primary_text` -- primary button label
- `dialog_cancel_text` -- cancel button label

You rarely interact with `modal_dialog` directly. Use `helix::ui::modal_confirm()` or `helix::ui::modal_alert()` instead.

---

## Standard Modal XML Template

When creating a new modal with custom layout, follow this template:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- NOTE: Backdrop created programmatically by Modal system -->
<component>
  <view name="my_feature_modal"
        extends="ui_dialog" width="70%" height="content" align="center"
        flex_flow="column" style_flex_main_place="start" style_pad_gap="0">

    <!-- Header (option A: use modal_header component) -->
    <modal_header icon_src="alert" icon_variant="warning"
                  title="My Title" title_tag="My Title"/>

    <!-- Content area -->
    <lv_obj width="100%" height="content"
            style_pad_left="#space_lg" style_pad_right="#space_lg"
            style_pad_top="0" style_pad_bottom="#space_lg">
      <text_body name="dialog_message" width="100%"
                 text="Some message here" long_mode="wrap"/>
    </lv_obj>

    <!-- Button row -->
    <modal_button_row
        secondary_text="Cancel" secondary_callback="on_my_cancel"
        primary_text="Confirm" primary_callback="on_my_confirm"/>
  </view>
</component>
```

Key rules:
- Always `extends="ui_dialog"` (never plain `lv_obj` or `ui_card`)
- Never include a backdrop in XML (comment: "Backdrop created programmatically by Modal system")
- Use `modal_button_row` for standard two-button footers
- Use `modal_header` for icon + title rows (or build custom headers)
- Use design tokens for all spacing (`#space_lg`, `#space_md`, etc.)
- Cap the card at `style_max_height="85%"` and the scroll area at a
  `#dialog_content_*` token — see the next section

### Height budget: the `#dialog_content_*` ladder family

The card cap and the content cap are ONE piece of arithmetic, shared by every
modal: the card is `height="content"` capped at **85% of the screen**, and the
scrollable body is capped at a token whose per-breakpoint values were measured
as (85% cap − that shape's chrome). There is no flex-shrink in LVGL, so a card
whose children total more than its cap clips its LAST child off the bottom —
the button row — and the modal cannot be dismissed.

Pick the token by the card's chrome shape (values in `ui_xml/globals.xml`):

| Token | Card shape | Measured on |
|-------|------------|-------------|
| `#dialog_content_max` | header + divider + scroll area + divider + ONE button row | `modal_dialog` |
| `#dialog_content_pinned_max` | …plus ONE pinned block below the scroll area (a diagram, a status row) | `ams_loading_error_modal` |
| `#dialog_content_tall_chrome_max` | …plus a SECOND button row with its divider | `klipper_recovery_dialog` |

- Prefer moving content INSIDE the scroll container over pinning it — then
  `#dialog_content_max` is correct by construction.
- Never raise a card above 85% to fit extra chrome (klipper_recovery carried
  90% for a while; #1277 ported it back onto the tall-chrome token). The
  chrome-budget lint gate (`scripts/check_modal_chrome_budget.py`) flags both
  the raised cap and an unbudgeted block below a scroll area.
- A shape beyond one extra block (action_prompt's diagram + wrapping rows +
  footer) fits no single ladder: measure it at every breakpoint with
  `ctl demo` + `ctl geom` and mark the file `MODAL_CHROME_OK`.

---

## ModalGuard (RAII for Static API)

When using `Modal::show()` or a `modal_confirm()` whose handle you keep, the returned `lv_obj_t*` must eventually be hidden. `ModalGuard` automates this:

```cpp
#include "ui/ui_modal_guard.h"

class ControlsPanel {
    helix::ui::ModalGuard motors_dialog_;
    helix::ui::ModalGuard z_offset_dialog_;

    void confirm_disable_motors() {
        // ModalGuard::operator= hides any previous dialog first
        motors_dialog_ = helix::ui::modal_confirm(
            lv_tr("Disable Motors?"),
            lv_tr("Release all stepper motors."),
            ModalSeverity::Warning, lv_tr("Disable"),
            on_confirm, on_cancel, this);
    }
};
// Panel destructor -> ModalGuard destructor -> helix::ui::modal_hide() called automatically
```

`ModalGuard` supports move semantics, assignment from raw `lv_obj_t*`, explicit `hide()`, and `release()` to take ownership.

> **`ModalGuard` guarantees the dialog gets hidden, not that it is gone.** It calls the
> static `Modal::hide()`, which starts a 150ms exit animation - so the sequence in the
> comment above is: the panel destructor runs the guard, and the dialog then outlives the
> panel. For tier-2 dialogs whose buttons a caller hand-wired with `wire_button_with()`,
> that means whatever those callbacks capture must outlive the dialog too.
>
> `Modal::hide()` disarms the tree on the way out (`Modal::disarm_tree`), so a click can no
> longer reach those callbacks. What it does not do is make the pointer valid: a teardown
> that never goes through `hide()` - the screen being deleted out from under the dialog, or
> `ModalStack::clear()` at shutdown - leaves the tree fully armed. The helper forms
> (`modal_confirm()`/`modal_alert()`) avoid the question entirely: their callbacks are
> std::function copies gated by `owner_token`, not pointers attached to widgets.
>
> Despite the name this is not the `ObserverGuard` bargain: `ObserverGuard` detaches the
> observer so nothing can dispatch afterwards, while `ModalGuard` only asks the dialog to
> close. If the callbacks capture something that dies with the owner, own a `Modal`
> subclass and let its teardown run instead.

---

## ModalStack Internals

`ModalStack` is a singleton that tracks all active modals for:

- **Z-ordering**: Multiple stacked modals maintain correct visual order
- **Top-modal queries**: `Modal::get_top()` returns the topmost dialog
- **Animation state**: `mark_exiting()` prevents double-hide during exit animation
- **Backdrop-to-dialog mapping**: Links each backdrop to its dialog
- **Owner tracking**: Records the `Modal*` that shows each dialog, so a dialog can be traced back to the C++ instance behind it

You should not interact with `ModalStack` directly. Use the `Modal` class API instead.

### Why the stack records an owner

The two `hide` overloads do different amounts of work. Instance `Modal::hide()` runs the
full teardown — `lifetime_.invalidate()`, `user_data` clearing, `on_hide()`. Static
`Modal::hide(lv_obj_t*)` only animates the widgets away.

Most callers reach for the static one as `Modal::hide(Modal::get_top())` and cannot know
whether the dialog on top belongs to a `Modal` subclass. Without the owner, hiding a
subclass that way skipped `on_hide()`: the instance leaked, any `active_instance_` static
stayed non-null, and `backdrop_`/`dialog_` dangled. The static overload now looks the
owner up and delegates, so both spellings tear a modal down identically **when the dialog
has an owner**.

The confirmation and alert helpers are owned. Each one builds an internal
`ConfirmationModal` (`src/ui/ui_modal.cpp`), so `on_hide()` runs, `lifetime_` is
invalidated, and a dismissal is reported through `on_dismiss` - see "Three Ways to Create
Modals" above.

One-shot modals are owned by their stack entry. `Modal::show_owned()` (or the
`assume_ownership()` call inside a class's own show helper) hands the instance to
the entry that tracks its widgets; the entry frees it when it goes - one tick
after the exit animation on a normal close, immediately in `clear()`. That
replaced the self-delete-from-on_hide() idiom across every one-shot modal
(InfoQrModal, CrashReportModal, PreflightCheckModal, ...), whose only free path
was `on_hide()`: any teardown that bypassed `hide()` - `ModalStack::clear()` at a
soft restart, `hide()`'s untracked-backdrop early return - leaked the C++ object
(prestonbrown/helixscreen#1382). A modal that is reshown stays owned by its
member or `unique_ptr` holder and uses plain `show()`.

The bare static `Modal::show()` factory is the shape with no owner: it pushes with
`owner = nullptr`, so there is nothing to delegate to and the static teardown is all that
runs. It never reaches `on_hide()`, and there is no `lifetime_` to invalidate because
there is no instance to own one. **A dialog shown that way cannot tell anyone it was
dismissed.**

`Modal::rebuild_top()` (the `HELIX_HOT_RELOAD=1` path) uses the owner differently — an
instance-backed modal is hidden rather than rebuilt, because re-creating it from XML alone
would skip `on_show()` and the subclass's button wiring and leave a dialog whose buttons do
nothing.

Instance `hide()` clears the owner before invoking `on_hide()`, so a hook that itself calls
the static overload is not delegated straight back into the same `hide()`. An owned
instance is not freed at that point - only when the entry leaves - so `on_hide()` bodies
must not assume destruction has begun.

### Animations

- **Entrance**: 250ms scale (85% to 100%) + fade in, with slight overshoot bounce
- **Exit**: 150ms scale down + fade out, then backdrop and dialog are destroyed
- Constants match `globals.xml` animation tokens (`anim_normal`, `anim_fast`)

---

## Advanced Patterns

### Modals with Dynamic Content (SpoolEditModal)

For modals that manage their own subjects and complex state:

```cpp
class SpoolEditModal : public Modal {
    SubjectManager subjects_;       // RAII subject lifecycle
    lv_subject_t color_subject_;    // Bound to XML elements
    char color_buf_[32] = {0};      // String buffer for subject

    const char* get_name() const override { return "Edit Spool Modal"; }
    const char* component_name() const override { return "spoolman_edit_modal"; }

    void on_show() override {
        init_subjects();
        update_ui();
    }

    void on_hide() override {
        deinit_subjects();
    }
};
```

> **Note:** The AMS slot editor is NOT a modal anymore — it is `AmsEditOverlay`,
> a NavigationManager overlay hosting four internal views selected by the
> `ams_edit_view` subject: overview (spool card + "Change filament" row),
> Spoolman spool picker, a unified spool-edit view (identity + color +
> logistics, `VIEW_SPOOL_EDIT`), and the color view. `SpoolEditModal`,
> `ColorPicker`, and `FilamentCatalogPickerModal` remain standalone modals for
> their other consumers (SpoolmanPanel, LED/theme pickers, FilamentPanel
> presets).

### Modals with Many Buttons (RunoutGuidanceModal)

The hook system supports up to 6 buttons. Wire each to a named hook:

```cpp
void on_show() override {
    wire_ok_button("btn_load_filament");        // -> on_ok()
    wire_cancel_button("btn_resume");           // -> on_cancel()
    wire_tertiary_button("btn_cancel_print");   // -> on_tertiary()
    wire_quaternary_button("btn_unload");       // -> on_quaternary()
    wire_quinary_button("btn_purge");           // -> on_quinary()
    wire_senary_button("btn_ok");               // -> on_senary()
}
```

Some buttons can choose not to hide the modal (useful for "purge" that can be repeated):

```cpp
void on_quinary() override {
    if (on_purge_) on_purge_();
    // Don't call hide() - user may want to purge multiple times
}
```

**Subject-driven conditional sections.** The six button hooks are only half the story - which
buttons and rows are even visible is driven by XML bindings on component-scoped subjects that
`RunoutGuidanceModal` sets before each `show()`, not by hiding/showing widgets from C++:

- `runout_autofeed_capable` (0/1) - set from the active backend's `recovers_filament_on_resume()`.
  When 1 (autofeed, e.g. Snapmaker U1), the manual Load/Unload/Purge row is hidden entirely and
  the message changes to "Refill the spool, then Resume." because Resume alone recovers the
  runout.
- `print_state_enum` - also hides the manual row while `== 1` (printing; Load/Unload/Purge mid-print
  destroys the print), and swaps the Close button row for a Cancel Print / Resume Print row while
  `== 2` (paused).
- `runout_is_advisory` (0/1) - swaps the warning icon (`alert`) for a neutral one (`filament`)
  when the dialog was opened by a deliberate user tap rather than an actual runout. Owned by
  `RunoutGuidanceModal`; every show site must call `set_advisory()` itself since the subject is
  static and outlives any one show - inheriting the previous call's value would latch it.

All three live in `ui_xml/runout_guidance_modal.xml` as `<bind_flag_if_eq>` /
`<bind_flag_if>` bindings, not `on_show()` visibility toggles - see rule 2 in the root
`CLAUDE.md`'s declarative-UI table.

### Modals with Keyboard Input (WiFi Password)

Use `helix::ui::modal_register_keyboard()` to attach a keyboard to a textarea inside a modal:

```cpp
void on_show() override {
    lv_obj_t* textarea = find_widget("password_input");
    helix::ui::modal_register_keyboard(dialog(), textarea);
}
```

### Modals with Custom Button Styling

In XML, use `primary_variant` on `modal_button_row` for destructive actions:

```xml
<modal_button_row
    secondary_text="Keep Printing"
    secondary_callback="on_dismiss"
    primary_text="Stop"
    primary_callback="on_confirm"
    primary_variant="danger"/>
```

---

## Migration Guide (Old Pattern to New Pattern)

The cc046ad2 refactor converted 9 modals. Here is the pattern transformation:

### XML: Inline Backdrop to ui_dialog

**Before** (inline backdrop in XML):
```xml
<component>
  <view name="my_modal_backdrop"
        extends="lv_obj" width="100%" height="100%"
        style_bg_opa="180" style_border_width="0"
        style_radius="0" clickable="true">
    <lv_obj width="400" height="200" align="center"
            style_radius="#border_radius" style_pad_all="#space_2xl"
            flex_flow="column">
      <!-- content -->
      <lv_obj width="100%" height="#button_height" flex_flow="row"
              style_pad_gap="#space_lg">
        <ui_button name="btn_cancel" width="160" text="Cancel">
          <event_cb trigger="clicked" callback="on_cancel"/>
        </ui_button>
        <ui_button name="btn_confirm" width="160" text="Confirm">
          <event_cb trigger="clicked" callback="on_confirm"/>
        </ui_button>
      </lv_obj>
    </lv_obj>
  </view>
</component>
```

**After** (ui_dialog + modal_button_row):
```xml
<component>
  <!-- NOTE: Backdrop created programmatically by Modal system -->
  <view name="my_modal"
        extends="ui_dialog" width="70%" height="content" align="center"
        flex_flow="column" style_flex_main_place="start" style_pad_gap="0">
    <!-- content -->
    <modal_button_row
        secondary_text="Cancel" secondary_callback="on_cancel"
        primary_text="Confirm" primary_callback="on_confirm"/>
  </view>
</component>
```

### C++: Manual Show/Hide to Modal System

**Before** (manual backdrop + hidden flag toggling):
```cpp
// Showing
lv_obj_t* backdrop = lv_obj_find_by_name(screen, "my_modal_backdrop");
lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);

// Hiding
lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
```

**After** (Modal system):
```cpp
// Showing
lv_obj_t* dialog = helix::ui::modal_show("my_modal");

// Hiding
helix::ui::modal_hide(dialog);
```

### C++: Manual Button Wiring to Confirmation Helper

**Before** (18+ lines):
```cpp
const char* attrs[] = {"title", "Delete?", "message", "Cannot be undone.", nullptr};
helix::ui::modal_configure(ModalSeverity::Warning, true, "Delete", "Cancel");
dialog_ = helix::ui::modal_show("modal_dialog", attrs);
if (!dialog_) return;
lv_obj_t* cancel = lv_obj_find_by_name(dialog_, "btn_secondary");
if (cancel) lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, this);
lv_obj_t* confirm = lv_obj_find_by_name(dialog_, "btn_primary");
if (confirm) lv_obj_add_event_cb(confirm, on_confirm, LV_EVENT_CLICKED, this);
```

**After** (single call):
```cpp
helix::ui::ConfirmOptions opts;
opts.on_cancel = on_cancel;
dialog_ = helix::ui::modal_confirm(
    "Delete?", "Cannot be undone.",
    ModalSeverity::Warning, "Delete", on_confirm, opts);
```

### C++: extends="ui_card" to extends="ui_dialog"

If your XML used `extends="ui_card"`, simply change to `extends="ui_dialog"`. The `ui_dialog` widget provides the correct theme-aware background, corner clipping, and context flag.

---

## Checklist: Adding a New Modal

1. **Choose approach**: Helper function, static `Modal::show()`, or subclass? Pick a
   subclass when a dismissal has to be observable (the caller holds a guard, flag or
   pending entry that the buttons are supposed to clear), or when the callbacks capture
   something shorter-lived than the dialog. Helper otherwise. This is a lifetime question,
   not a question of how many buttons the dialog has.
2. **Create XML** in `ui_xml/` using `extends="ui_dialog"` and `modal_button_row`
3. **Register XML** in `src/xml_registration.cpp` (components must be registered before they're used by other components)
4. **Register callbacks** via `lv_xml_register_event_cb()` in your C++ code
5. **If subclass**: Create header in `include/`, implement `get_name()` and `component_name()`
6. **Wire buttons** in `on_show()` using `wire_ok_button()` / `wire_cancel_button()`
7. **Store the modal** as a member (subclass) or in a `ModalGuard` (static API)
8. **Test**: Modal should auto-hide when parent panel is destroyed

---

## Async Callback Safety

Modal subclasses that make asynchronous API calls (WebSocket, HTTP, Moonraker) need protection against callbacks arriving after the modal is dismissed.

The `Modal` base class provides `lifetime_` (a `helix::AsyncLifetimeGuard`) that handles this automatically:

```cpp
void MyModal::start_operation() {
    auto token = lifetime_.token();
    api->fetch([this, token]() {
        if (token.expired()) return;       // Modal dismissed
        lifetime_.defer([this]() {         // Safe main-thread update
            lv_subject_set_int(&result_subject_, 1);
        });
    });
}
```

> Inside a subclass this is `lifetime_`, invalidated for you by `hide()`. The helpers take
> the *caller's* token instead - `owner_token`
> on `modal_confirm()` / `modal_alert()` - because the thing that can die early there is the
> caller, not the modal. Only the bare static `Modal::show()` factory has no protection at
> all, which is one reason not to reach for it.

`Modal::hide()` calls `lifetime_.invalidate()` before `on_hide()`, so all outstanding tokens expire automatically. Your `on_hide()` override does **not** need to manually invalidate — just handle observer cleanup and state reset.

For cancel-and-retry scenarios (e.g., user clicks "Test Connection" again while a test is in flight), call `lifetime_.invalidate()` explicitly before starting the new operation:

```cpp
void MyModal::handle_retry() {
    lifetime_.invalidate();               // Cancel previous callbacks
    auto token = lifetime_.token();       // Fresh token
    api->test([this, token]() { ... });
}
```

See `include/async_lifetime_guard.h` for the full API documentation.

---

## Free-function Wrappers

The `helix::ui::modal_*()` free functions are thin inline wrappers around the `Modal`
class, provided for convenience. (There is no `ui_modal_*` prefix — those aliases were
removed.) All live in `include/ui_modal.h`, `namespace helix::ui`:

| Free function | Underlying |
|--------|---------|
| `helix::ui::modal_show(name)` | `Modal::show(name)` |
| `helix::ui::modal_hide(dialog)` | `Modal::hide(dialog)` |
| `helix::ui::modal_get_top()` | `Modal::get_top()` |
| `Modal::any_visible()` | (static method; no free-function wrapper) |
| `helix::ui::modal_init_subjects()` | subject registration |
| `helix::ui::modal_configure(...)` | configures the shared `modal_dialog` |

Use the `Modal::` class methods or the `helix::ui::modal_confirm()` / `helix::ui::modal_alert()` helpers; the `lv_event_cb_t` spellings (`modal_show_confirmation()` / `modal_show_alert()`) are gone. Subject to the rule in "Three Ways to Create Modals": if the dialog needs custom content rather than title + message + buttons, own a `Modal` subclass instead.
