// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_width_push.cpp
 * @brief Overlay width applied at push time (prestonbrown/helixscreen#1178)
 *
 * The pure classification rule is covered by test_overlay_width_class.cpp.
 * These tests cover the wiring: NavigationManager::push_overlay() resolving a
 * class against the live stack and writing the width onto the widget.
 *
 * The behaviour that matters and that a static XML attribute cannot provide:
 * the same overlay widget pushed from two different places gets two different
 * widths.
 */

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"
#include "overlay_class.h"
#include "panel_lifecycle.h"
#include "theme_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Minimal lifecycle so push_overlay() sees a registered overlay. `destination`
/// mirrors what AmsPanel / PrintStatusPanel override to true.
class FakeOverlay : public IPanelLifecycle {
  public:
    explicit FakeOverlay(const char* name, bool destination = false)
        : name_(name), destination_(destination) {}

    void on_activate() override {}
    void on_deactivate() override {}
    const char* get_name() const override {
        return name_;
    }
    bool is_destination() const override {
        return destination_;
    }

  private:
    const char* name_;
    bool destination_;
};

class OverlayWidthFixture : public LVGLTestFixture {
  public:
    // LVGLTestFixture owns LVGL init, the display, and — critically — the
    // UpdateQueue lifecycle. push_overlay() defers its work through that queue,
    // so a fixture that does not initialise it silently never pushes anything.
    OverlayWidthFixture() {
        theme_manager_register_responsive_spacing(lv_display_get_default());
        NavigationManager::instance().init();

        // Production always has the active root panel at panel_stack_[0], and
        // push_overlay reads panel_stack_.back() to inherit from. Without
        // seeding it, the SECOND push still looks like the first and inherits
        // from the nav root instead of the overlay beneath it.
        for (int i = 0; i < UI_PANEL_COUNT; i++) {
            root_panels_[i] = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(root_panels_.data());
    }

    ~OverlayWidthFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        NavigationManager::instance().deinit_subjects();
    }

    /// Create an overlay widget, register it, push it, and drain the queue —
    /// push_overlay() defers its work through UpdateQueue.
    lv_obj_t* push(FakeOverlay& lifecycle) {
        lv_obj_t* w = lv_obj_create(lv_screen_active());
        push_existing(w, lifecycle);
        return w;
    }

    void push_existing(lv_obj_t* w, FakeOverlay& lifecycle) {
        auto& nav = NavigationManager::instance();
        nav.register_overlay_instance(w, &lifecycle);
        nav.push_overlay(w);
        helix::ui::UpdateQueue::instance().drain();
        // lv_obj_set_width() writes the style; lv_obj_get_width() reads the
        // COMPUTED coord, which only refreshes on a layout pass. Without this a
        // freshly created widget still reports LVGL's default 160.
        lv_obj_update_layout(w);
    }

    /// Pop one overlay and let the deferred teardown run.
    static void pop() {
        NavigationManager::instance().go_back();
        helix::ui::UpdateQueue::instance().drain();
    }

    std::array<lv_obj_t*, UI_PANEL_COUNT> root_panels_{};

    static int32_t screen_width() {
        return lv_display_get_horizontal_resolution(lv_display_get_default());
    }

    static int32_t expected_transient() {
        return read_const("overlay_width_transient");
    }
    static int32_t expected_destination() {
        return read_const("overlay_width_destination");
    }

    static int32_t read_const(const char* name) {
        const char* s = lv_xml_get_const(nullptr, name);
        REQUIRE(s != nullptr);
        return std::atoi(s);
    }
};

} // namespace

// ============================================================================
// The two widths are distinct and derived as documented
// ============================================================================

TEST_CASE_METHOD(OverlayWidthFixture, "Destination width is wider than transient by the gap",
                 "[overlay][width][1178]") {
    const int32_t transient = expected_transient();
    const int32_t destination = expected_destination();

    const int32_t nav_width = read_const("nav_width");
    const int32_t gap = read_const("space_lg");

    CHECK(destination == screen_width() - nav_width);
    CHECK(transient == screen_width() - nav_width - gap);
    CHECK(destination > transient);
}

// ============================================================================
// Inheritance through the live stack
// ============================================================================

TEST_CASE_METHOD(OverlayWidthFixture, "Overlay from a non-Settings root is transient",
                 "[overlay][width][1178]") {
    // Advanced > Console
    NavigationManager::instance().set_active(PanelId::Advanced);

    FakeOverlay console("console_panel");
    lv_obj_t* w = push(console);

    CHECK(lv_obj_get_width(w) == expected_transient());
}

TEST_CASE_METHOD(OverlayWidthFixture, "Overlay from the Settings root is a destination",
                 "[overlay][width][1178]") {
    // Settings > Network
    NavigationManager::instance().set_active(PanelId::Settings);

    FakeOverlay network("network_settings_overlay");
    lv_obj_t* w = push(network);

    CHECK(lv_obj_get_width(w) == expected_destination());
}

TEST_CASE_METHOD(OverlayWidthFixture, "A drill-down never renders wider than its parent",
                 "[overlay][width][1178]") {
    // The reported defect: Console Settings (930) was pushed from Console (914)
    // and stuck out 16px toward the nav dock.
    NavigationManager::instance().set_active(PanelId::Advanced);

    FakeOverlay console("console_panel");
    lv_obj_t* parent = push(console);

    FakeOverlay console_settings("console_settings_overlay");
    lv_obj_t* child = push(console_settings);

    CHECK(lv_obj_get_width(child) == expected_transient());
    CHECK(lv_obj_get_width(child) <= lv_obj_get_width(parent));
}

TEST_CASE_METHOD(OverlayWidthFixture, "Drill-downs inside Settings stay destinations",
                 "[overlay][width][1178]") {
    // Settings > Display & Sound > Theme Editor
    NavigationManager::instance().set_active(PanelId::Settings);

    FakeOverlay display_sound("settings_display_sound_overlay");
    push(display_sound);

    FakeOverlay theme_editor("theme_editor_overlay");
    lv_obj_t* child = push(theme_editor);

    CHECK(lv_obj_get_width(child) == expected_destination());
}

// ============================================================================
// Promotion travels with the panel, not the push site
// ============================================================================

TEST_CASE_METHOD(OverlayWidthFixture, "A promoted panel is a destination from any root",
                 "[overlay][width][1178]") {
    // AmsPanel is reachable from Home, the Printer Manager overlay, and the AMS
    // Overview. It must be full width from all of them, so the promotion lives
    // on the panel class rather than being repeated at each push site.
    for (PanelId root : {PanelId::Home, PanelId::Filament, PanelId::Advanced}) {
        NavigationManager::instance().set_active(root);

        FakeOverlay ams("ams_panel", /*destination=*/true);
        lv_obj_t* w = push(ams);

        INFO("root=" << static_cast<int>(root));
        CHECK(lv_obj_get_width(w) == expected_destination());

        pop();
    }
}

TEST_CASE_METHOD(OverlayWidthFixture, "Drill-downs of a promoted panel inherit destination",
                 "[overlay][width][1178]") {
    // AMS > Edit Spool. ams_edit_overlay carries no promotion of its own.
    NavigationManager::instance().set_active(PanelId::Filament);

    FakeOverlay ams("ams_panel", /*destination=*/true);
    push(ams);

    FakeOverlay edit("ams_edit_overlay");
    lv_obj_t* child = push(edit);

    CHECK(lv_obj_get_width(child) == expected_destination());
}

// ============================================================================
// The multi-entry case — the reason width is not an XML attribute
// ============================================================================

TEST_CASE_METHOD(OverlayWidthFixture, "The same cached widget gets both widths",
                 "[overlay][width][1178]") {
    // fan_control_overlay is opened from Controls (transient) and from
    // Settings > Fans (destination). OverlayBase caches its root widget across
    // show/hide, so the SAME widget must be re-measured on every push — not
    // just when it is first created.
    FakeOverlay fan_control("fan_control_overlay");
    lv_obj_t* w = lv_obj_create(lv_screen_active());

    NavigationManager::instance().set_active(PanelId::Controls);
    push_existing(w, fan_control);
    const int32_t from_controls = lv_obj_get_width(w);

    pop();

    NavigationManager::instance().set_active(PanelId::Settings);
    FakeOverlay fan_settings("fan_settings_overlay");
    push(fan_settings);
    push_existing(w, fan_control);
    const int32_t from_settings = lv_obj_get_width(w);

    CHECK(from_controls == expected_transient());
    CHECK(from_settings == expected_destination());
    CHECK(from_controls != from_settings);
}
