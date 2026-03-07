# Multi-Printer Phase 3: UI Integration — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a navbar printer badge and context menu for switching between configured printers, plus wizard integration for adding new printers.

**Architecture:** Navbar gets a printer badge (name + connection dot) at the bottom, hidden when only one printer configured. Tapping opens a `PrinterSwitchMenu` (subclass of `ContextMenu`) with a list of printers and "+ Add Printer". Switching calls existing `Application::switch_printer()`. Adding launches the wizard targeting a new config entry.

**Tech Stack:** LVGL 9.5, XML declarative UI, ContextMenu base class, Config v3 CRUD API, soft restart machinery.

**Reference docs:** `docs/devel/UI_CONTRIBUTOR_GUIDE.md`, `docs/devel/LVGL9_XML_GUIDE.md`

---

## Task 1: Printer Switch Subjects

Add subjects to PrinterState so the navbar badge and context menu can bind to them.

**Files:**
- Modify: `include/printer_state.h` — add subject declarations and accessors
- Modify: `src/printer/printer_state.cpp` — init subjects, register cleanup, add setters
- Modify: `src/application/application.cpp` — set subjects during `init_printer_state()`
- Test: `tests/unit/test_config.cpp` — add test for subject initialization

**Step 1: Add subject declarations to PrinterState**

In `include/printer_state.h`, add to private members (near other subject declarations):

```cpp
lv_subject_t active_printer_name_subject_;
lv_subject_t multi_printer_enabled_subject_;  // int: 0=single, 1=multi
```

Add public accessors:

```cpp
lv_subject_t* get_active_printer_name_subject() { return &active_printer_name_subject_; }
lv_subject_t* get_multi_printer_enabled_subject() { return &multi_printer_enabled_subject_; }
void set_active_printer_name(const std::string& name);
void set_multi_printer_enabled(bool enabled);
```

**Step 2: Init subjects in PrinterState::init_subjects()**

In `src/printer/printer_state.cpp`, inside `init_subjects()`:

```cpp
static char printer_name_buf[128] = "Printer";
lv_subject_init_string(&active_printer_name_subject_, printer_name_buf, nullptr, sizeof(printer_name_buf), sizeof(printer_name_buf));
lv_subject_init_int(&multi_printer_enabled_subject_, 0);
```

Add to `deinit_subjects()`:
```cpp
lv_subject_deinit(&active_printer_name_subject_);
lv_subject_deinit(&multi_printer_enabled_subject_);
```

Register as XML subjects (global scope) so XML bindings work:
```cpp
lv_xml_register_subject(nullptr, "active_printer_name", &active_printer_name_subject_);
lv_xml_register_subject(nullptr, "multi_printer_enabled", &multi_printer_enabled_subject_);
```

**Step 3: Implement setters**

```cpp
void PrinterState::set_active_printer_name(const std::string& name) {
    lv_subject_copy_string(&active_printer_name_subject_, name.c_str());
}

void PrinterState::set_multi_printer_enabled(bool enabled) {
    lv_subject_set_int(&multi_printer_enabled_subject_, enabled ? 1 : 0);
}
```

**Step 4: Set subjects in Application::init_printer_state()**

In `src/application/application.cpp`, inside `init_printer_state()`, after subjects are initialized and config is available:

```cpp
// Set multi-printer subjects
auto printer_ids = m_config->get_printer_ids();
auto active_id = m_config->get_active_printer_id();
std::string printer_name = m_config->get<std::string>(m_config->df() + "printer_name", active_id);
get_printer_state().set_active_printer_name(printer_name);
get_printer_state().set_multi_printer_enabled(printer_ids.size() > 1);
```

**Step 5: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 6: Commit**

```bash
git add include/printer_state.h src/printer/printer_state.cpp src/application/application.cpp
git commit -m "feat(multi-printer): add active_printer_name and multi_printer_enabled subjects"
```

---

## Task 2: Navbar Printer Badge XML

Add the printer badge element to the navbar XML layout.

**Files:**
- Modify: `ui_xml/navigation_bar.xml` — add badge at bottom

**Step 1: Add printer badge to navigation_bar.xml**

After `nav_btn_advanced` (the last navigation button, around line 130), add:

```xml
    <!-- Printer switcher badge — visible only when multiple printers configured -->
    <lv_obj name="nav_printer_badge"
            width="100%" height="content"
            style_pad_all="#space_xs" style_pad_gap="#space_xs"
            style_bg_color="#elevated_bg" style_bg_opa="255"
            style_radius="#border_radius"
            style_border_width="1" style_border_color="#card_border"
            flex_flow="row" style_flex_cross_place="center"
            clickable="true">
      <bind_flag_if_eq subject="multi_printer_enabled" flag="hidden" ref_value="0"/>
      <lv_obj name="nav_printer_dot" width="8" height="8"
              style_radius="4" style_bg_opa="255"
              style_bg_color="#success"
              clickable="false" event_bubble="true"/>
      <text_tiny name="nav_printer_name" bind_text="active_printer_name"
                 style_text_color="#text_secondary"
                 flex_grow="1" long_mode="dot"
                 clickable="false" event_bubble="true"/>
    </lv_obj>
```

Note: All children have `clickable="false" event_bubble="true"` per lesson L071 so clicks reach the parent badge.

**Step 2: Test visually**

No rebuild needed (L031 — XML loaded at runtime). Launch app with 2+ printers configured:
```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

The badge should appear at the bottom of the navbar.

**Step 3: Commit**

```bash
git add ui_xml/navigation_bar.xml
git commit -m "feat(multi-printer): add printer badge to navbar XML"
```

---

## Task 3: Printer Switch Context Menu — XML Component

Create the XML layout for the printer switch context menu.

**Files:**
- Create: `ui_xml/printer_switch_menu.xml`

**Step 1: Create the XML component**

Based on the `favorite_macro_picker.xml` pattern:

```xml
<component>
  <view name="printer_switch_menu" extends="lv_obj"
        width="100%" height="100%" style_bg_opa="128" style_bg_color="0x000000"
        style_border_width="0" style_pad_all="0" style_radius="0" clickable="true">
    <event_cb trigger="clicked" callback="printer_switch_backdrop_cb"/>
    <lv_obj name="context_menu"
            width="content" height="content" style_bg_color="#elevated_bg" style_bg_opa="255"
            style_radius="#border_radius" style_pad_all="#space_sm" style_pad_gap="#space_xs"
            style_min_width="160"
            flex_flow="column" scrollable="false" clickable="true">
      <text_small text="Switch Printer" translation_tag="Switch Printer"
                  style_text_color="#text_muted"/>
      <lv_obj width="100%" height="1" style_bg_color="#text_muted" style_bg_opa="38"/>
      <lv_obj name="printer_list" width="100%" height="content"
              flex_flow="column" style_pad_all="0" style_pad_gap="0"
              scrollable="true"/>
      <lv_obj width="100%" height="1" style_bg_color="#text_muted" style_bg_opa="38"/>
      <ui_button name="btn_add_printer" text="+ Add Printer"
                 translation_tag="+ Add Printer"
                 variant="ghost" width="100%">
        <event_cb trigger="clicked" callback="printer_switch_add_cb"/>
      </ui_button>
    </lv_obj>
  </view>
</component>
```

**Step 2: Register the component**

In `src/xml_registration.cpp`, add near other context menu registrations:

```cpp
register_xml("printer_switch_menu.xml");
```

**Step 3: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 4: Commit**

```bash
git add ui_xml/printer_switch_menu.xml src/xml_registration.cpp
git commit -m "feat(multi-printer): add printer switch context menu XML component"
```

---

## Task 4: PrinterSwitchMenu Class — Implementation

Create the `PrinterSwitchMenu` class that subclasses `ContextMenu`.

**Files:**
- Create: `include/ui_printer_switch_menu.h`
- Create: `src/ui/ui_printer_switch_menu.cpp`
- Modify: `src/xml_registration.cpp` — register event callbacks

**Step 1: Create header**

`include/ui_printer_switch_menu.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include <functional>
#include <string>

namespace helix::ui {

class PrinterSwitchMenu : public ContextMenu {
  public:
    enum class MenuAction {
        SWITCH,
        ADD_PRINTER,
        CANCELLED,
    };

    using SwitchCallback = std::function<void(MenuAction action, const std::string& printer_id)>;

    PrinterSwitchMenu() = default;

    void show(lv_obj_t* parent, lv_obj_t* near_widget);

    void set_switch_callback(SwitchCallback callback) { switch_callback_ = std::move(callback); }

    static void register_callbacks();

  protected:
    const char* xml_component_name() const override { return "printer_switch_menu"; }
    void on_created(lv_obj_t* menu) override;
    void on_backdrop_clicked() override;

  private:
    void populate_printer_list();
    void handle_printer_selected(const std::string& printer_id);
    void handle_add_printer();
    void dispatch_switch_action(MenuAction action, const std::string& printer_id = "");

    SwitchCallback switch_callback_;

    static PrinterSwitchMenu* s_active_instance_;
    static bool s_callbacks_registered_;

    static PrinterSwitchMenu* get_active_instance();
    static void on_backdrop_cb(lv_event_t* e);
    static void on_add_printer_cb(lv_event_t* e);
    static void on_printer_row_cb(lv_event_t* e);
};

}  // namespace helix::ui
```

**Step 2: Create implementation**

`src/ui/ui_printer_switch_menu.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_switch_menu.h"

#include "config.h"
#include "ui_safe_delete.h"
#include "ui_theme.h"

#include "spdlog/spdlog.h"

namespace helix::ui {

PrinterSwitchMenu* PrinterSwitchMenu::s_active_instance_ = nullptr;
bool PrinterSwitchMenu::s_callbacks_registered_ = false;

void PrinterSwitchMenu::register_callbacks() {
    if (s_callbacks_registered_) return;
    lv_xml_register_event_cb(nullptr, "printer_switch_backdrop_cb", on_backdrop_cb);
    lv_xml_register_event_cb(nullptr, "printer_switch_add_cb", on_add_printer_cb);
    s_callbacks_registered_ = true;
}

PrinterSwitchMenu* PrinterSwitchMenu::get_active_instance() {
    return s_active_instance_;
}

void PrinterSwitchMenu::show(lv_obj_t* parent, lv_obj_t* near_widget) {
    s_active_instance_ = this;
    lv_point_t pt;
    lv_area_t area;
    lv_obj_get_coords(near_widget, &area);
    pt.x = area.x2;
    pt.y = (area.y1 + area.y2) / 2;
    set_click_point(pt);
    show_near_widget(parent, 0, near_widget);
}

void PrinterSwitchMenu::on_created(lv_obj_t* menu_widget) {
    populate_printer_list();
}

void PrinterSwitchMenu::populate_printer_list() {
    lv_obj_t* list = lv_obj_find_by_name(menu(), "printer_list");
    if (!list) return;

    Config* cfg = Config::get_instance();
    if (!cfg) return;

    auto printer_ids = cfg->get_printer_ids();
    auto active_id = cfg->get_active_printer_id();

    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_set_style_max_height(list, screen_h * 2 / 3, 0);

    for (const auto& id : printer_ids) {
        std::string name = cfg->get<std::string>("/printers/" + id + "/printer_name", id);

        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_pad_gap(row, 8, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        bool is_active = (id == active_id);

        // Checkmark for active printer
        if (is_active) {
            lv_obj_t* check = lv_label_create(row);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(check, ui_theme_get_color("accent"), 0);
            lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(check, LV_OBJ_FLAG_EVENT_BUBBLE);
        } else {
            // Spacer for alignment
            lv_obj_t* spacer = lv_obj_create(row);
            lv_obj_set_size(spacer, 16, 16);
            lv_obj_set_style_bg_opa(spacer, 0, 0);
            lv_obj_set_style_border_width(spacer, 0, 0);
            lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(spacer, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, name.c_str());
        lv_obj_set_style_text_color(label, is_active ? ui_theme_get_color("text_primary")
                                                     : ui_theme_get_color("text_secondary"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Highlight on press
        lv_obj_set_style_bg_opa(row, 30, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, ui_theme_get_color("accent"), LV_STATE_PRESSED);

        // Store printer ID for click handler
        auto* id_copy = new std::string(id);
        lv_obj_set_user_data(row, id_copy);
        lv_obj_add_event_cb(row, on_printer_row_cb, LV_EVENT_CLICKED, nullptr);
    }
}

void PrinterSwitchMenu::on_backdrop_clicked() {
    dispatch_switch_action(MenuAction::CANCELLED);
}

void PrinterSwitchMenu::handle_printer_selected(const std::string& printer_id) {
    Config* cfg = Config::get_instance();
    if (cfg && printer_id == cfg->get_active_printer_id()) {
        // Already active — just dismiss
        dispatch_switch_action(MenuAction::CANCELLED);
        return;
    }
    dispatch_switch_action(MenuAction::SWITCH, printer_id);
}

void PrinterSwitchMenu::handle_add_printer() {
    dispatch_switch_action(MenuAction::ADD_PRINTER);
}

void PrinterSwitchMenu::dispatch_switch_action(MenuAction action, const std::string& printer_id) {
    auto callback = switch_callback_;
    // Clean up heap-allocated printer ID strings before hide() deletes the menu
    lv_obj_t* list = lv_obj_find_by_name(menu(), "printer_list");
    if (list) {
        uint32_t count = lv_obj_get_child_count(list);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* row = lv_obj_get_child(list, i);
            auto* id_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
            delete id_ptr;
            lv_obj_set_user_data(row, nullptr);
        }
    }
    s_active_instance_ = nullptr;
    hide();
    if (callback) callback(action, printer_id);
}

// Static callbacks
void PrinterSwitchMenu::on_backdrop_cb(lv_event_t*) {
    auto* self = get_active_instance();
    if (self) self->on_backdrop_clicked();
}

void PrinterSwitchMenu::on_add_printer_cb(lv_event_t*) {
    auto* self = get_active_instance();
    if (self) self->handle_add_printer();
}

void PrinterSwitchMenu::on_printer_row_cb(lv_event_t* e) {
    auto* self = get_active_instance();
    if (!self) return;
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    // Walk up to the row with user_data (click may hit child)
    while (target && !lv_obj_get_user_data(target)) {
        target = lv_obj_get_parent(target);
    }
    if (!target) return;
    auto* id_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
    if (id_ptr) self->handle_printer_selected(*id_ptr);
}

}  // namespace helix::ui
```

**Step 3: Register callbacks in xml_registration.cpp**

Add near other callback registrations:

```cpp
helix::ui::PrinterSwitchMenu::register_callbacks();
```

And add include:
```cpp
#include "ui_printer_switch_menu.h"
```

**Step 4: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 5: Commit**

```bash
git add include/ui_printer_switch_menu.h src/ui/ui_printer_switch_menu.cpp src/xml_registration.cpp
git commit -m "feat(multi-printer): implement PrinterSwitchMenu context menu"
```

---

## Task 5: Wire Navbar Badge to Context Menu

Connect the navbar badge click to opening the PrinterSwitchMenu, and handle switch/add actions.

**Files:**
- Modify: `src/ui/ui_nav_manager.cpp` — add badge click handler and menu instance
- Modify: `include/ui_nav_manager.h` — add menu member and method declarations
- Modify: `src/application/application.cpp` — add `add_printer_via_wizard()` method
- Modify: `include/application.h` — declare new method

**Step 1: Add PrinterSwitchMenu to NavigationManager**

In `include/ui_nav_manager.h`, add:
```cpp
#include "ui_printer_switch_menu.h"
```

Add private members:
```cpp
helix::ui::PrinterSwitchMenu printer_switch_menu_;
```

Add private method:
```cpp
void on_printer_badge_clicked();
```

**Step 2: Wire badge click in NavigationManager::wire_events()**

In `src/ui/ui_nav_manager.cpp`, inside `wire_events()`, after the nav button loop:

```cpp
// Printer badge click handler
lv_obj_t* printer_badge = lv_obj_find_by_name(navbar, "nav_printer_badge");
if (printer_badge) {
    lv_obj_add_event_cb(printer_badge, [](lv_event_t* e) {
        NavigationManager::instance().on_printer_badge_clicked();
    }, LV_EVENT_CLICKED, nullptr);
}
```

**Step 3: Implement on_printer_badge_clicked()**

```cpp
void NavigationManager::on_printer_badge_clicked() {
    if (printer_switch_menu_.is_visible()) {
        printer_switch_menu_.hide();
        return;
    }

    lv_obj_t* badge = lv_obj_find_by_name(navbar_widget_, "nav_printer_badge");
    if (!badge) return;

    lv_obj_t* screen = lv_obj_get_screen(navbar_widget_);

    printer_switch_menu_.set_switch_callback(
        [](helix::ui::PrinterSwitchMenu::MenuAction action, const std::string& printer_id) {
            switch (action) {
                case helix::ui::PrinterSwitchMenu::MenuAction::SWITCH:
                    spdlog::info("[Nav] Switching to printer '{}'", printer_id);
                    Application::instance().switch_printer(printer_id);
                    break;
                case helix::ui::PrinterSwitchMenu::MenuAction::ADD_PRINTER:
                    spdlog::info("[Nav] Adding new printer via wizard");
                    Application::instance().add_printer_via_wizard();
                    break;
                case helix::ui::PrinterSwitchMenu::MenuAction::CANCELLED:
                    break;
            }
        });

    printer_switch_menu_.show(screen, badge);
}
```

**Step 4: Implement Application::add_printer_via_wizard()**

In `src/application/application.cpp`:

```cpp
void Application::add_printer_via_wizard() {
    // Generate a temporary ID for the new printer
    std::string new_id = "printer-" + std::to_string(m_config->get_printer_ids().size() + 1);
    std::string previous_id = m_config->get_active_printer_id();

    // Create empty printer entry and switch to it
    m_config->add_printer(new_id, nlohmann::json::object());
    m_config->set_active_printer(new_id);
    m_config->save();

    // Store previous ID for cancel recovery
    m_wizard_previous_printer_id = previous_id;

    // Soft restart into wizard (new printer has no config → wizard_required)
    tear_down_printer_state();
    init_printer_state();
    NavigationManager::instance().set_active(PanelId::Home);
}
```

Add to `include/application.h`:
```cpp
void add_printer_via_wizard();
```

Add private member:
```cpp
std::string m_wizard_previous_printer_id;
```

**Step 5: Handle wizard cancellation**

In the wizard cancel/back handler (wherever the wizard exits without completing), add recovery logic:

```cpp
// If we were adding a new printer, clean up the empty entry
if (!m_wizard_previous_printer_id.empty()) {
    std::string failed_id = m_config->get_active_printer_id();
    m_config->remove_printer(failed_id);
    m_config->set_active_printer(m_wizard_previous_printer_id);
    m_config->save();
    m_wizard_previous_printer_id.clear();

    tear_down_printer_state();
    init_printer_state();
    NavigationManager::instance().set_active(PanelId::Home);
}
```

Note: The exact location for wizard cancel handling needs investigation during implementation — search for wizard exit/cancel handlers in `ui_wizard.cpp`.

**Step 6: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 7: Commit**

```bash
git add include/ui_nav_manager.h src/ui/ui_nav_manager.cpp include/application.h src/application/application.cpp
git commit -m "feat(multi-printer): wire navbar badge to printer switch menu and add-printer wizard"
```

---

## Task 6: Connection Status Dot

Update the navbar badge connection dot color based on WebSocket state.

**Files:**
- Modify: `src/ui/ui_nav_manager.cpp` — observe connection state, update dot color
- Modify: `include/ui_nav_manager.h` — add observer guard

**Step 1: Add observer guard for connection state dot**

In `include/ui_nav_manager.h`, add private member:
```cpp
ObserverGuard printer_dot_observer_;
lv_obj_t* printer_dot_widget_ = nullptr;
```

**Step 2: Wire observer in wire_events()**

In `src/ui/ui_nav_manager.cpp`, inside `wire_events()`, after the badge click handler:

```cpp
// Connection status dot
printer_dot_widget_ = lv_obj_find_by_name(navbar, "nav_printer_dot");
if (printer_dot_widget_) {
    printer_dot_observer_ = observe_int_sync<NavigationManager>(
        get_printer_state().get_printer_connection_state_subject(), this,
        [](NavigationManager* mgr, int state) {
            if (!mgr->printer_dot_widget_) return;
            lv_color_t color;
            switch (state) {
                case 2:  // connected
                    color = ui_theme_get_color("success");
                    break;
                case 1:  // connecting
                case 3:  // reconnecting
                    color = ui_theme_get_color("warning");
                    break;
                default:  // disconnected, failed
                    color = ui_theme_get_color("danger");
                    break;
            }
            lv_obj_set_style_bg_color(mgr->printer_dot_widget_, color, 0);
        });
}
```

**Step 3: Reset in shutdown**

In `NavigationManager::shutdown()`:
```cpp
printer_dot_observer_.reset();
printer_dot_widget_ = nullptr;
```

**Step 4: Build and verify**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

**Step 5: Commit**

```bash
git add include/ui_nav_manager.h src/ui/ui_nav_manager.cpp
git commit -m "feat(multi-printer): add connection status dot to navbar printer badge"
```

---

## Task 7: Integration Test & Polish

End-to-end manual testing and polish.

**Step 1: Test with mock printer**

```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

Verify:
- Badge hidden when single printer (default state)
- Press 'P' to cycle printers (adds test printer) → badge appears
- Badge shows printer name + green dot
- Click badge → context menu appears near badge
- Menu shows printer list with checkmark on active
- Click different printer → soft restart → new printer active
- Click "+ Add Printer" → wizard launches
- Click backdrop → menu dismisses

**Step 2: Run full test suite**

```bash
make test-run 2>&1 | tail -5
```

Expected: All tests pass.

**Step 3: Visual polish as needed**

Adjust spacing, colors, text truncation based on manual testing. XML-only changes don't require rebuild (L031).

**Step 4: Final commit**

```bash
git add -A
git commit -m "feat(multi-printer): Phase 3 UI integration complete"
```
