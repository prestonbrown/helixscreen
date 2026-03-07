# Multi-Printer Phase 4: Printer Management Settings — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a "Printers" settings row, a full-screen PrinterListOverlay for managing printers (switch/delete/add), wizard cancel recovery, and manual testing of the add-printer wizard flow.

**Architecture:** Settings "Printers" row opens existing PrinterManagerOverlay. A new "Manage Printers" button at the bottom of that overlay pushes a new PrinterListOverlay. The list overlay shows all configured printers with switch/delete/add actions. Wizard cancel recovery hooks into the wizard's back-from-first-step path.

**Tech Stack:** LVGL 9.5, XML declarative UI, OverlayBase pattern, ContextMenu patterns for dynamic list building, Config v3 CRUD API, soft restart machinery.

**Reference docs:** `docs/devel/UI_CONTRIBUTOR_GUIDE.md`, `docs/devel/LVGL9_XML_GUIDE.md`, `docs/devel/MODAL_SYSTEM.md`

---

## Task 1: Settings "Printers" Row

Add a new `setting_action_row` to the settings panel that opens the PrinterManagerOverlay.

**Files:**
- Modify: `ui_xml/settings_panel.xml` — add row at top of PRINTER section
- Modify: `src/ui/ui_panel_settings.cpp` — add callback + handler

**Step 1: Add settings row XML**

In `ui_xml/settings_panel.xml`, immediately after the `<setting_section_header title="PRINTER" .../>` line (before the AMS container), insert:

```xml
<setting_action_row name="row_printers"
                    label="Printers" label_tag="Printers" icon="printer_3d"
                    description="Manage configured printers"
                    description_tag="Manage configured printers" callback="on_printers_clicked"/>
```

**Step 2: Register callback in SettingsPanel**

In `src/ui/ui_panel_settings.cpp`, inside `register_xml_callbacks({...})`, add:

```cpp
{"on_printers_clicked", on_printers_clicked},
```

**Step 3: Add static callback and handler**

In `src/ui/ui_panel_settings.cpp`, add the static callback (follow existing pattern like `on_display_settings_clicked`):

```cpp
void SettingsPanel::on_printers_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_printers_clicked");
    get_global_settings_panel().handle_printers_clicked();
    LVGL_SAFE_EVENT_CB_END();
}
```

Add the handler (opens the existing PrinterManagerOverlay):

```cpp
void SettingsPanel::handle_printers_clicked() {
    spdlog::debug("[{}] Printers clicked - opening Printer Manager", get_name());
    auto& overlay = get_printer_manager_overlay();
    overlay.show(parent_screen_);
}
```

Add declarations to the header — static callback declaration alongside the others, and `handle_printers_clicked()` as a private method.

**Step 4: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 5: Visual test**

No rebuild needed for XML changes (L031). Launch:
```bash
./build/bin/helix-screen --test -vv
```
Navigate to Settings → PRINTER section. "Printers" row should appear at the top. Tapping it should open the Printer Manager overlay.

**Step 6: Commit**

```bash
git add ui_xml/settings_panel.xml src/ui/ui_panel_settings.cpp include/ui_panel_settings.h
git commit -m "feat(multi-printer): add Printers row to settings panel"
```

---

## Task 2: "Manage Printers" Button in PrinterManagerOverlay

Add a button at the bottom of the PrinterManagerOverlay that will push the PrinterListOverlay.

**Files:**
- Modify: `ui_xml/printer_manager_overlay.xml` — add button before bottom padding
- Modify: `src/ui/ui_printer_manager_overlay.cpp` — add callback
- Modify: `include/ui_printer_manager_overlay.h` — declare callback + handler

**Step 1: Add button to XML**

In `ui_xml/printer_manager_overlay.xml`, before the bottom padding `<lv_obj>` (line 293), add:

```xml
      <!-- ================================================================== -->
      <!-- SECTION 4: MANAGE PRINTERS -->
      <!-- ================================================================== -->
      <lv_obj width="100%" height="content" style_pad_left="#space_lg" style_pad_right="#space_lg"
              style_pad_top="#space_sm" style_pad_bottom="0" scrollable="false"
              style_bg_opa="0" style_border_width="0">
        <ui_button name="pm_manage_printers_btn"
                   text="Manage Printers" translation_tag="Manage Printers"
                   icon="printer_3d" variant="ghost" width="100%">
          <event_cb trigger="clicked" callback="pm_manage_printers_clicked"/>
        </ui_button>
      </lv_obj>
```

**Step 2: Register callback**

In `src/ui/ui_printer_manager_overlay.cpp`, inside `register_callbacks()`, add to the `register_xml_callbacks({...})` list:

```cpp
{"pm_manage_printers_clicked", pm_manage_printers_clicked_cb},
```

**Step 3: Add static callback**

```cpp
static void pm_manage_printers_clicked_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterManagerOverlay] manage_printers_clicked");
    get_printer_manager_overlay().handle_manage_printers_clicked();
    LVGL_SAFE_EVENT_CB_END();
}
```

**Step 4: Add handler (stub for now)**

```cpp
void PrinterManagerOverlay::handle_manage_printers_clicked() {
    spdlog::info("[{}] Manage Printers clicked", get_name());
    // TODO: Push PrinterListOverlay (Task 4)
}
```

Declare in `include/ui_printer_manager_overlay.h`:
- `void handle_manage_printers_clicked();` as a public method
- `static void pm_manage_printers_clicked_cb(lv_event_t* e);` as a static method (or declare it as a file-static in the .cpp)

**Step 5: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 6: Commit**

```bash
git add ui_xml/printer_manager_overlay.xml src/ui/ui_printer_manager_overlay.cpp include/ui_printer_manager_overlay.h
git commit -m "feat(multi-printer): add Manage Printers button to PrinterManagerOverlay"
```

---

## Task 3: PrinterListOverlay — XML Layout

Create the XML layout for the new printer list overlay.

**Files:**
- Create: `ui_xml/printer_list_overlay.xml`
- Modify: `src/xml_registration.cpp` — register component

**Step 1: Create the XML component**

`ui_xml/printer_list_overlay.xml`:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<component>
  <view name="printer_list_overlay" extends="overlay_panel"
        title="Manage Printers" title_tag="Manage Printers">
    <lv_obj name="overlay_content"
            width="100%" flex_grow="1" style_pad_all="0" style_pad_gap="0"
            flex_flow="column" scrollable="true"
            style_bg_color="#card_bg" style_bg_opa="255">
      <!-- Dynamic printer rows populated by C++ -->
      <lv_obj name="printer_list_container" width="100%" height="content"
              flex_flow="column" style_pad_all="0" style_pad_gap="0"
              scrollable="false"/>
      <!-- Add Printer button -->
      <lv_obj width="100%" height="content"
              style_pad_left="#space_lg" style_pad_right="#space_lg"
              style_pad_top="#space_md" style_pad_bottom="#space_lg"
              scrollable="false" style_bg_opa="0" style_border_width="0">
        <ui_button name="btn_add_printer"
                   text="+ Add Printer" translation_tag="+ Add Printer"
                   variant="ghost" width="100%">
          <event_cb trigger="clicked" callback="printer_list_add_cb"/>
        </ui_button>
      </lv_obj>
    </lv_obj>
  </view>
</component>
```

**Step 2: Register the component**

In `src/xml_registration.cpp`, add near other overlay registrations:

```cpp
register_xml("printer_list_overlay.xml");
```

**Step 3: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 4: Commit**

```bash
git add ui_xml/printer_list_overlay.xml src/xml_registration.cpp
git commit -m "feat(multi-printer): add PrinterListOverlay XML layout"
```

---

## Task 4: PrinterListOverlay — C++ Implementation

Create the C++ class that populates and manages the printer list.

**Files:**
- Create: `include/ui_printer_list_overlay.h`
- Create: `src/ui/ui_printer_list_overlay.cpp`
- Modify: `src/xml_registration.cpp` — register callbacks
- Modify: `src/ui/ui_printer_manager_overlay.cpp` — wire up "Manage Printers" button

**Step 1: Create header**

`include/ui_printer_list_overlay.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_overlay_base.h"

#include <string>

namespace helix::ui {

class PrinterListOverlay : public OverlayBase {
  public:
    PrinterListOverlay();
    ~PrinterListOverlay() override = default;

    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void on_activate() override;
    void on_deactivate() override;

    const char* get_name() const override { return "Printer List"; }

    void show(lv_obj_t* parent_screen);

    void handle_add_printer();
    void handle_switch_printer(const std::string& printer_id);
    void handle_delete_printer(const std::string& printer_id);

  private:
    void populate_printer_list();
    void cleanup_row_user_data();

    lv_obj_t* overlay_root_{nullptr};

    static bool s_callbacks_registered_;

    static void on_add_printer_cb(lv_event_t* e);
    static void on_printer_row_cb(lv_event_t* e);
    static void on_delete_printer_cb(lv_event_t* e);
};

PrinterListOverlay& get_printer_list_overlay();

}  // namespace helix::ui
```

**Step 2: Create implementation**

`src/ui/ui_printer_list_overlay.cpp`:

Pattern mirrors `ui_printer_switch_menu.cpp` row creation but in an overlay context. Key differences:
- Uses OverlayBase instead of ContextMenu
- Adds a delete button (trash icon) on each row
- Delete button hidden when only 1 printer configured
- Uses `modal_show_confirmation()` for delete confirmation
- Uses `lazy_create_and_push_overlay` pattern or direct `NavigationManager::push_overlay()`

The implementation should:

1. **Global instance** via `get_printer_list_overlay()` with `StaticPanelRegistry::register_destroy()`

2. **`register_callbacks()`** — register `printer_list_add_cb` XML callback. Row click and delete callbacks are added via `lv_obj_add_event_cb()` during dynamic row creation (same pattern as PrinterSwitchMenu).

3. **`create()`** — `lv_xml_create(parent, "printer_list_overlay", nullptr)`

4. **`show()`** — lazy create + register + push overlay

5. **`on_activate()`** — call `populate_printer_list()` to rebuild list (in case printers changed)

6. **`populate_printer_list()`** — Same font/color/icon pattern as the fixed PrinterSwitchMenu (Task 7 polish from Phase 3):
   - Resolve `font_body` and `icon_font_sm` via `lv_xml_get_const()` / `lv_xml_get_font()`
   - Use `ui_icon::lookup_codepoint("check")` for active printer indicator
   - Use `theme_manager_get_color("text")` for label color
   - Each row: `lv_obj` container with row flex, containing:
     - Check icon (or spacer) for active printer
     - Printer name label (flex_grow=1)
     - Connection status dot (8x8, colored by connection state)
     - Delete icon button (`delete` icon, ghost variant) — hidden when `printer_ids.size() <= 1`
   - Row click → `handle_switch_printer()`
   - Delete button click → `handle_delete_printer()` which shows confirmation modal

7. **`handle_switch_printer()`** — if not active printer, call `Application::instance().switch_printer(id)`. The soft restart will destroy the overlay naturally.

8. **`handle_delete_printer()`** — Show confirmation via `modal_show_confirmation()`:
   - Title: "Remove Printer"
   - Message: "Remove [name]? All settings for this printer will be deleted."
   - Severity: danger
   - On confirm: `Config::remove_printer(id)`, if deleted printer was active → switch to first remaining, update subjects, repopulate list

9. **`handle_add_printer()`** — call `Application::instance().add_printer_via_wizard()`

10. **`cleanup_row_user_data()`** — delete heap-allocated `std::string*` user_data from rows (same pattern as PrinterSwitchMenu)

**Step 3: Wire "Manage Printers" button**

In `src/ui/ui_printer_manager_overlay.cpp`, update `handle_manage_printers_clicked()`:

```cpp
void PrinterManagerOverlay::handle_manage_printers_clicked() {
    spdlog::info("[{}] Manage Printers clicked — opening printer list", get_name());
    auto& overlay = helix::ui::get_printer_list_overlay();
    overlay.show(lv_display_get_screen_active(nullptr));
}
```

Add `#include "ui_printer_list_overlay.h"` to the .cpp.

**Step 4: Register callbacks in xml_registration.cpp**

```cpp
#include "ui_printer_list_overlay.h"
// In the registration function:
helix::ui::get_printer_list_overlay().register_callbacks();
```

Or alternatively, register callbacks lazily in `show()` before first use.

**Step 5: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 6: Commit**

```bash
git add include/ui_printer_list_overlay.h src/ui/ui_printer_list_overlay.cpp src/ui/ui_printer_manager_overlay.cpp src/xml_registration.cpp
git commit -m "feat(multi-printer): implement PrinterListOverlay with switch/delete/add"
```

---

## Task 5: Wizard Cancel Recovery

When the add-printer wizard is cancelled (user backs out from first step), clean up the empty printer entry and restore the previous printer.

**Files:**
- Modify: `src/ui/ui_wizard.cpp` — hook cancel on back-from-first-step
- Modify: `src/application/application.cpp` — add `cancel_add_printer_wizard()` method
- Modify: `include/application.h` — declare method
- Modify: `include/app_globals.h` — add wizard cancel callback
- Modify: `src/app_globals.cpp` — implement wizard cancel callback

**Step 1: Add wizard cancel callback to app_globals**

In `include/app_globals.h`, add (next to the existing wizard completion callback):

```cpp
void set_wizard_cancel_callback(std::function<void()> callback);
std::function<void()> get_wizard_cancel_callback();
```

Implement in `src/app_globals.cpp`:

```cpp
static std::function<void()> g_wizard_cancel_callback;

void set_wizard_cancel_callback(std::function<void()> callback) {
    g_wizard_cancel_callback = std::move(callback);
}

std::function<void()> get_wizard_cancel_callback() {
    return g_wizard_cancel_callback;
}
```

**Step 2: Hook cancel in wizard back button**

In `src/ui/ui_wizard.cpp`, in `on_back_clicked()`, where it currently does nothing when at the first step (returns early around line 1020-1024), replace the early return with a cancel callback invocation:

When the user is at the earliest step and can't go further back:

```cpp
if (prev_step == 0 && touch_cal_step_skipped) {
    // At first step — invoke cancel callback if registered (add-printer mode)
    auto cancel_cb = get_wizard_cancel_callback();
    if (cancel_cb) {
        spdlog::info("[Wizard] Back from first step — invoking cancel callback");
        cancel_cb();
    }
    navigating = false;
    return;
}
```

Also handle the case where step 0 itself is the first step (touch cal NOT skipped) — currently the back button is hidden via `wizard_back_visible` subject when on step 0. We need to make it visible when in add-printer mode so the user CAN cancel. In `ui_wizard_navigate_to_step()`, around line 469:

```cpp
// Show back button on first step when in add-printer mode (allows cancel)
bool has_cancel = (get_wizard_cancel_callback() != nullptr);
lv_subject_set_int(&wizard_back_visible, (step > min_step || has_cancel) ? 1 : 0);
```

**Step 3: Implement cancel_add_printer_wizard() in Application**

In `src/application/application.cpp`:

```cpp
void Application::cancel_add_printer_wizard() {
    if (m_wizard_previous_printer_id.empty()) {
        spdlog::debug("[Application] No add-printer recovery state — ignoring cancel");
        return;
    }

    std::string failed_id = m_config->get_active_printer_id();
    spdlog::info("[Application] Cancelling add-printer wizard — removing '{}', restoring '{}'",
                 failed_id, m_wizard_previous_printer_id);

    m_config->remove_printer(failed_id);
    m_config->set_active_printer(m_wizard_previous_printer_id);
    m_config->save();
    m_wizard_previous_printer_id.clear();

    // Soft restart back to previous printer
    tear_down_printer_state();
    init_printer_state();
    NavigationManager::instance().set_active(PanelId::Home);
}
```

Declare in `include/application.h`:
```cpp
void cancel_add_printer_wizard();
```

**Step 4: Register cancel callback**

In `src/application/application.cpp`, inside `init_printer_state()`, near where the wizard completion callback is registered (around line 451):

```cpp
set_wizard_cancel_callback([this]() {
    cancel_add_printer_wizard();
});
```

Clear it in `tear_down_printer_state()`:

```cpp
set_wizard_cancel_callback(nullptr);
```

**Step 5: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 6: Commit**

```bash
git add src/ui/ui_wizard.cpp src/application/application.cpp include/application.h include/app_globals.h src/app_globals.cpp
git commit -m "feat(multi-printer): add wizard cancel recovery for add-printer flow"
```

---

## Task 6: Manual Testing — Wizard Launch & Cancel

Test both the add-printer wizard launch and the cancel recovery flow.

**Step 1: Launch app in test mode**

```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/phase4-test.log
```

**Step 2: Test settings entry point**

1. Navigate to Settings panel
2. Scroll to PRINTER section
3. Verify "Printers" row appears at the top with printer_3d icon
4. Tap it — PrinterManagerOverlay should open
5. Scroll to bottom — "Manage Printers" button should be visible
6. Tap "Manage Printers" — PrinterListOverlay should push

**Step 3: Test printer list**

In the PrinterListOverlay:
1. Verify the single mock printer is listed with a check icon
2. Verify the delete button is HIDDEN (can't delete last printer)
3. Verify "+ Add Printer" button is visible at the bottom

**Step 4: Test add-printer wizard launch**

1. Tap "+ Add Printer" in the PrinterListOverlay
2. App should soft restart and show the wizard
3. Wizard should start at connection step (or first non-skipped step)
4. Check logs for: `[Application] Adding new printer`

**Step 5: Test wizard cancel recovery**

1. While in the wizard (from add-printer), press the Back button on the first step
2. App should soft restart back to the previous printer
3. Check logs for: `[Application] Cancelling add-printer wizard`
4. Verify the empty printer entry was removed from config
5. Navigate back to PrinterListOverlay — should show only the original printer

**Step 6: Test printer deletion (requires 2+ printers)**

1. Press P key to create a test printer (makes 2 printers available)
2. Open Settings → Printers → Manage Printers
3. Verify both printers shown, delete buttons visible
4. Tap delete on the non-active printer
5. Confirmation dialog should appear: "Remove [name]?"
6. Confirm → printer removed, list refreshes with 1 printer
7. Delete button should now be hidden (last printer)

**Step 7: Read logs and verify**

```bash
# After user confirms testing is complete
cat /tmp/phase4-test.log
```

Check for:
- No crashes or assertions
- Proper log messages for switch/delete/add flows
- No orphaned printer entries in config

**Step 8: Commit any fixes**

```bash
git add -u
git commit -m "fix(multi-printer): Phase 4 testing fixes"
```
