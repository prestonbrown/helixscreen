// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_navbar_estop.cpp
 * @brief The navbar carries the one E-stop while a job holds the machine.
 *
 * The navbar keeps a slot; the button itself lives on the screen over that slot,
 * so it stays bright and tappable above overlay and keypad backdrops, which dim
 * the rest of the navbar. An open modal covers it, like the rest of the navbar. Every panel and
 * overlay sits beside the navbar. Screens that cover the navbar (lock screen, fullscreen camera)
 * carry their own.
 *
 * Run with: ./build/bin/helix-tests "[estop_navbar]"
 */

#include "ui_component_keypad.h"
#include "ui_effects.h"
#include "ui_keyboard_manager.h"
#include "ui_lock_screen.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "theme_manager.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

std::string read_xml(const std::string& path) {
    std::ifstream file(path);
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/// Whether @p xml has an element bound to estop_visible that fires the E-stop.
bool has_estop_button(const std::string& xml) {
    return xml.find("subject=\"estop_visible\"") != std::string::npos &&
           (xml.find("callback=\"emergency_stop_clicked\"") != std::string::npos ||
            xml.find("callback=\"lock_screen_estop_clicked\"") != std::string::npos);
}

/// The keyboard manager initialised on @p parent for one test, reset after.
struct ScopedKeyboard {
    explicit ScopedKeyboard(lv_obj_t* parent) {
        release();
        KeyboardManager::instance().init(parent);
    }
    ~ScopedKeyboard() {
        release();
    }
    ScopedKeyboard(const ScopedKeyboard&) = delete;
    ScopedKeyboard& operator=(const ScopedKeyboard&) = delete;
    static void release() {
        KeyboardManager::instance().reset();
        helix::ui::UpdateQueue::instance().drain();
    }
};

struct NavbarFixture : public LVGLUITestFixture {
    NavbarFixture() {
        estop_visible_ = lv_xml_get_subject(nullptr, "estop_visible");
        REQUIRE(estop_visible_ != nullptr);
        saved_ = lv_subject_get_int(estop_visible_);
        navbar_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
        REQUIRE(navbar_ != nullptr);
        slot_ = lv_obj_find_by_name(navbar_, "nav_estop_slot");
        REQUIRE(slot_ != nullptr);
        NavigationManager::instance().wire_events(navbar_);
        estop_ = NavigationManager::instance().navbar_estop();
        REQUIRE(estop_ != nullptr);
    }
    ~NavbarFixture() override {
        lv_subject_set_int(estop_visible_, saved_);
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_visible(int v) {
        lv_subject_set_int(estop_visible_, v);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(test_screen());
    }

    static int32_t index_of(lv_obj_t* obj) {
        return static_cast<int32_t>(lv_obj_get_index(obj));
    }

    lv_subject_t* estop_visible_ = nullptr;
    int saved_ = 0;
    lv_obj_t* navbar_ = nullptr;
    lv_obj_t* slot_ = nullptr;
    lv_obj_t* estop_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(NavbarFixture, "navbar E-stop is shown exactly while estop_visible is set",
                 "[estop_navbar][navigation]") {
    set_visible(0);
    CHECK(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(slot_, LV_OBJ_FLAG_HIDDEN));
    set_visible(1);
    CHECK_FALSE(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(slot_, LV_OBJ_FLAG_HIDDEN));
    set_visible(0);
    CHECK(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(NavbarFixture, "navbar E-stop fires the emergency stop callback",
                 "[estop_navbar][navigation]") {
    lv_event_cb_t estop_cb = lv_xml_get_event_cb(nullptr, "emergency_stop_clicked");
    REQUIRE(estop_cb != nullptr);
    bool wired = false;
    for (uint32_t i = 0; i < lv_obj_get_event_count(estop_); ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(estop_, i);
        if (lv_event_dsc_get_cb(dsc) == estop_cb) {
            wired = true;
        }
    }
    CHECK(wired);
}

TEST_CASE_METHOD(NavbarFixture, "navbar E-stop sits over its slot in the navbar",
                 "[estop_navbar][navigation]") {
    set_visible(1);
    lv_area_t slot, button;
    lv_obj_get_coords(slot_, &slot);
    lv_obj_get_coords(estop_, &button);
    CHECK(button.x1 == slot.x1);
    CHECK(button.y1 == slot.y1);
    CHECK(lv_area_get_width(&button) == lv_area_get_width(&slot));
    CHECK(lv_area_get_height(&button) == lv_area_get_height(&slot));
}

TEST_CASE_METHOD(NavbarFixture, "navbar E-stop survives a panel switch",
                 "[estop_navbar][navigation]") {
    // Switching panels hides every stray screen child as a stale overlay; the
    // E-stop is not one and must stay up.
    auto& nav = NavigationManager::instance();
    lv_obj_t* panels[kUiPanelCount] = {nullptr};
    panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
    panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
    nav.set_panels(panels);
    set_visible(1);
    REQUIRE_FALSE(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));

    NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Controls);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));

    // Closing an overlay sweeps stray screen children the same way.
    lv_obj_t* overlay = lv_obj_create(test_screen());
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav.register_overlay_instance(overlay, nullptr);
    nav.push_overlay(overlay);
    helix::ui::UpdateQueue::instance().drain();
    nav.go_back();
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));
    nav.unregister_overlay_instance(overlay);

    NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Home);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(NavbarFixture, "navbar E-stop stays bright above an overlay backdrop",
                 "[estop_navbar][navigation]") {
    auto& nav = NavigationManager::instance();
    set_visible(1);
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, test_screen());
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    // Above the dimming backdrop, not part of it: still its own, fully opaque
    // button, and still the thing a tap there lands on.
    CHECK(index_of(estop_) > index_of(backdrop));
    CHECK_FALSE(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_opa(estop_, LV_PART_MAIN) == LV_OPA_COVER);

    // A re-taken snapshot slots in above the old one; the E-stop stays above it.
    NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
    lv_obj_t* fresh = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(fresh != nullptr);
    CHECK(index_of(estop_) > index_of(fresh));
}

TEST_CASE_METHOD(NavbarFixture, "an open modal covers the navbar E-stop",
                 "[estop_navbar][navigation][modal_covers_navbar]") {
    set_visible(1);
    lv_obj_update_layout(test_screen());
    lv_area_t button;
    lv_obj_get_coords(estop_, &button);
    lv_point_t tap{(button.x1 + button.x2) / 2, (button.y1 + button.y2) / 2};
    REQUIRE(lv_indev_search_obj(test_screen(), &tap) == estop_);

    bool dismissed = false;
    helix::ui::ConfirmOptions opts;
    opts.on_dismiss = [&dismissed] { dismissed = true; };
    lv_obj_t* dialog =
        helix::ui::modal_confirm("Title", "Message", ModalSeverity::Info, "OK", [] {}, opts);
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = lv_obj_get_parent(dialog);
    REQUIRE(backdrop != nullptr);
    REQUIRE(lv_obj_get_parent(backdrop) == lv_obj_get_parent(estop_));
    CHECK(index_of(backdrop) > index_of(estop_));

    // A tap on the E-stop's spot reaches the backdrop, never the button. The plain
    // backdrop a tripped blur breaker falls back to has no size until a layout pass.
    lv_obj_update_layout(test_screen());
    lv_obj_t* hit = lv_indev_search_obj(test_screen(), &tap);
    CHECK(hit == backdrop);

    // A keypad or overlay raised while the modal is open leaves it covered.
    lv_obj_t* other = lv_obj_create(test_screen());
    helix::ui::bring_to_front(other);
    CHECK(index_of(estop_) < index_of(backdrop));
    helix::ui::safe_delete(other);

    // The backdrop takes the tap and, for a dismissible modal, closes it.
    REQUIRE(hit != nullptr);
    lv_obj_send_event(hit, LV_EVENT_CLICKED, nullptr);
    process_lvgl(400);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(dismissed);
    CHECK_FALSE(Modal::any_visible());

    // Closed, the navbar is live again.
    CHECK(lv_indev_search_obj(test_screen(), &tap) == estop_);
}

TEST_CASE_METHOD(NavbarFixture, "navbar E-stop stays reachable above the keyboard",
                 "[estop_navbar][navigation]") {
    // The keyboard spans the bottom of the screen, navbar included, which is
    // where the E-stop sits.
    ScopedKeyboard keyboard(test_screen());

    set_visible(1);
    lv_obj_t* textarea = lv_textarea_create(test_screen());
    KeyboardManager::instance().show(textarea);
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(kb != nullptr);
    REQUIRE(lv_obj_get_parent(kb) == lv_obj_get_parent(estop_));
    REQUIRE_FALSE(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    lv_obj_update_layout(test_screen());

    // Visible, clear of every key, and above the keyboard in z.
    lv_area_t kb_area, button, slot;
    lv_obj_get_coords(kb, &kb_area);
    lv_obj_get_coords(estop_, &button);
    lv_obj_get_coords(slot_, &slot);
    REQUIRE(slot.y2 >= kb_area.y1); // the premise: the keyboard covers the slot
    CHECK_FALSE(lv_obj_has_flag(estop_, LV_OBJ_FLAG_HIDDEN));
    CHECK(button.y2 < kb_area.y1);
    CHECK(button.y1 >= 0);
    CHECK(button.x1 == slot.x1);
    CHECK(index_of(estop_) > index_of(kb));

    // Closed, the keyboard hands the E-stop back to its slot.
    KeyboardManager::instance().hide();
    lv_obj_update_layout(test_screen());
    lv_obj_get_coords(estop_, &button);
    CHECK(button.y1 == slot.y1);
}

TEST_CASE_METHOD(NavbarFixture, "a keyboard opened over a modal does not lift the E-stop over it",
                 "[estop_navbar][navigation][modal_covers_navbar]") {
    ScopedKeyboard keyboard(test_screen());
    set_visible(1);
    lv_obj_t* dialog =
        helix::ui::modal_confirm("Title", "Message", ModalSeverity::Info, "OK", [] {});
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = lv_obj_get_parent(dialog);

    lv_obj_t* textarea = lv_textarea_create(dialog);
    KeyboardManager::instance().show(textarea);
    REQUIRE(KeyboardManager::instance().is_visible());

    CHECK(index_of(estop_) < index_of(backdrop));
    KeyboardManager::instance().hide();
}

TEST_CASE_METHOD(NavbarFixture,
                 "a portrait keyboard covers the navbar E-stop instead of lifting it",
                 "[estop_navbar][navigation]") {
    // A bottom bar: wider than tall, along the bottom of the screen.
    lv_obj_set_size(navbar_, lv_obj_get_width(test_screen()), 70);
    lv_obj_align(navbar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    ScopedKeyboard keyboard(test_screen());

    set_visible(1);
    REQUIRE(lv_obj_get_width(navbar_) > lv_obj_get_height(navbar_));
    lv_obj_t* textarea = lv_textarea_create(test_screen());
    KeyboardManager::instance().show(textarea);
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(kb != nullptr);
    REQUIRE(lv_obj_get_parent(kb) == lv_obj_get_parent(estop_));

    // Under the keyboard, never on top of its keys.
    CHECK(index_of(estop_) < index_of(kb));
    KeyboardManager::instance().hide();
}

TEST_CASE_METHOD(NavbarFixture,
                 "a modal closing over a portrait keyboard leaves the E-stop under it",
                 "[estop_navbar][navigation][modal_covers_navbar]") {
    lv_obj_set_size(navbar_, lv_obj_get_width(test_screen()), 70);
    lv_obj_align(navbar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    ScopedKeyboard keyboard(test_screen());

    set_visible(1);
    lv_obj_t* textarea = lv_textarea_create(test_screen());
    KeyboardManager::instance().show(textarea);
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(kb != nullptr);
    REQUIRE(index_of(estop_) < index_of(kb));

    lv_obj_t* dialog =
        helix::ui::modal_confirm("Title", "Message", ModalSeverity::Info, "OK", [] {});
    REQUIRE(dialog != nullptr);
    Modal::hide(dialog);
    process_lvgl(400);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(Modal::any_visible());

    CHECK(index_of(estop_) < index_of(kb));
    KeyboardManager::instance().hide();
}

TEST_CASE_METHOD(NavbarFixture, "a navbar E-stop built while a modal is open sits under it",
                 "[estop_navbar][navigation][modal_covers_navbar]") {
    set_visible(1);
    lv_obj_t* dialog =
        helix::ui::modal_confirm("Title", "Message", ModalSeverity::Info, "OK", [] {});
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = lv_obj_get_parent(dialog);

    // A navbar rebuild with the modal still up builds a fresh E-stop.
    lv_obj_t* rebuilt =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
    REQUIRE(rebuilt != nullptr);
    NavigationManager::instance().wire_events(rebuilt);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* fresh = NavigationManager::instance().navbar_estop();
    REQUIRE(fresh != nullptr);
    REQUIRE(lv_obj_get_parent(fresh) == lv_obj_get_parent(backdrop));
    CHECK(index_of(fresh) < index_of(backdrop));

    Modal::hide(dialog);
    process_lvgl(400);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(test_screen());
    lv_area_t button;
    lv_obj_get_coords(fresh, &button);
    lv_point_t tap{(button.x1 + button.x2) / 2, (button.y1 + button.y2) / 2};
    CHECK(lv_indev_search_obj(test_screen(), &tap) == fresh);
}

TEST_CASE_METHOD(NavbarFixture, "a keypad over an overlay keeps the navbar E-stop on top",
                 "[estop_navbar][navigation][keypad]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& panel : panels) {
        panel = lv_obj_create(test_screen());
    }
    NavigationManager::instance().set_panels(panels.data());
    set_visible(1);

    ui_keypad_init(test_screen());
    ui_keypad_config_t config = {};
    config.initial_value = 100;
    config.min_value = 0;
    config.max_value = 300;
    config.title_label = "Nozzle";
    config.unit_label = "C";
    ui_keypad_show(&config);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ui_keypad_is_visible());

    // The keypad brings its own backdrop, above the first overlay's; only other
    // navbar chrome may sit above the E-stop.
    const uint32_t count = lv_obj_get_child_count(test_screen());
    for (uint32_t i = static_cast<uint32_t>(index_of(estop_)) + 1; i < count; ++i) {
        CHECK(
            helix::ui::is_screen_chrome(lv_obj_get_child(test_screen(), static_cast<int32_t>(i))));
    }

    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(NavbarFixture, "a printer switch leaves exactly one navbar E-stop",
                 "[estop_navbar][navigation]") {
    auto& nav = NavigationManager::instance();
    auto count_estops = [&]() {
        int n = 0;
        for (uint32_t i = 0; i < lv_obj_get_child_count(test_screen()); ++i) {
            lv_obj_t* child = lv_obj_get_child(test_screen(), static_cast<int32_t>(i));
            const char* name = lv_obj_get_name(child);
            if (name && std::string(name) == "nav_btn_estop") {
                ++n;
            }
        }
        return n;
    };
    for (int i = 0; i < 2; ++i) {
        nav.deinit_subjects();
        nav.init();
        nav.wire_events(navbar_);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }
    CHECK(count_estops() == 1);
    lv_obj_t* current = nav.navbar_estop();
    REQUIRE(current != nullptr);
    CHECK(helix::ui::is_screen_chrome(current));
    set_visible(1);
    CHECK_FALSE(lv_obj_has_flag(current, LV_OBJ_FLAG_HIDDEN));
    set_visible(0);
    CHECK(lv_obj_has_flag(current, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(NavbarFixture, "the lock screen covers the navbar E-stop",
                 "[estop_navbar][navigation]") {
    set_visible(1);
    // The navbar E-stop lives on the screen; the lock screen is drawn on the top
    // layer, which is always above it and absorbs every touch.
    CHECK(lv_obj_get_screen(estop_) == test_screen());
    helix::ui::LockScreenOverlay::instance().show();
    CHECK(helix::ui::LockScreenOverlay::instance().is_visible());
    bool lock_on_top_layer = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); ++i) {
        if (lv_obj_find_by_name(lv_obj_get_child(lv_layer_top(), static_cast<int32_t>(i)),
                                "estop_fab")) {
            lock_on_top_layer = true;
        }
    }
    CHECK(lock_on_top_layer);
    CHECK(lv_obj_get_parent(estop_) != lv_layer_top());
    helix::ui::LockScreenOverlay::instance().hide();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(NavbarFixture, "navbar E-stop is no smaller than the circles it stands in for",
                 "[estop_navbar][navigation]") {
    set_visible(1);
    const int32_t size = lv_obj_get_width(estop_);
    const int32_t navbar_width = lv_obj_get_width(navbar_);
    const int32_t fab = std::max<int32_t>(44, theme_manager_get_spacing("button_height"));
    // A landscape navbar narrower than the old circle caps the target at the navbar.
    CHECK(size >= std::min(fab, navbar_width));
    CHECK(lv_obj_get_height(estop_) == size);
}

TEST_CASE("every overlay leaves the navbar and its E-stop on screen",
          "[estop_navbar][navigation]") {
    // Panels live beside the navbar in app_layout; overlays are sized by these two
    // classes. Neither may reach into the navbar's strip, or the E-stop under it
    // would be covered.
    struct Geometry {
        int32_t w, h, nav_width, nav_height;
    };
    const Geometry geometries[] = {
        {480, 272, 42, 34},    {480, 320, 54, 40},    {800, 480, 104, 70},
        {1024, 600, 132, 96},  {1280, 720, 148, 112}, {1920, 1080, 176, 128},
        {1920, 480, 176, 112}, {480, 800, 104, 70},   {1080, 2400, 176, 128},
    };
    for (const auto& g : geometries) {
        INFO(g.w << "x" << g.h);
        const auto widths = helix::compute_overlay_widths(g.w, g.h, g.nav_width, 16);
        const auto heights = helix::compute_overlay_heights(g.w, g.h, g.nav_height, 16);
        const bool portrait = g.h > g.w;
        if (portrait) {
            CHECK(heights.transient <= g.h - g.nav_height);
            CHECK(heights.destination <= g.h - g.nav_height);
        } else {
            CHECK(widths.transient <= g.w - g.nav_width);
            CHECK(widths.destination <= g.w - g.nav_width);
        }
    }
}

TEST_CASE("the navbar is the one E-stop where the navbar shows, and screens that cover it keep one",
          "[estop_navbar][navigation]") {
    CHECK(has_estop_button(read_xml("ui_xml/components/navbar_estop.xml")));

    // These cover the navbar, so they carry their own.
    for (const char* path :
         {"ui_xml/components/lock_screen.xml", "ui_xml/components/camera_fullscreen.xml"}) {
        INFO(path);
        CHECK(has_estop_button(read_xml(path)));
    }

    // These sit beside the navbar and drew a second E-stop over their content.
    for (const char* path :
         {"ui_xml/home_panel.xml", "ui_xml/controls_panel.xml", "ui_xml/micro/controls_panel.xml",
          "ui_xml/print_status_panel.xml", "ui_xml/motion_panel.xml"}) {
        INFO(path);
        const std::string xml = read_xml(path);
        CHECK(xml.find("estop_visible") == std::string::npos);
        CHECK(xml.find("emergency_stop_clicked") == std::string::npos);
    }
}

TEST_CASE("every screen-level backdrop goes up through bring_to_front",
          "[estop_navbar][navigation]") {
    // A backdrop raised with a bare lv_obj_move_foreground() buries the navbar
    // E-stop. Backdrops on lv_layer_top are above the screen anyway.
    int checked = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator("src")) {
        if (entry.path().extension() != ".cpp") {
            continue;
        }
        const std::string path = entry.path().generic_string();
        if (path == "src/ui/effects/ui_effects.cpp" || path == "src/ui/effects/backdrop_blur.cpp") {
            continue; // the creators themselves
        }
        if (path == "src/ui/ui_busy_overlay.cpp") {
            continue; // parents its backdrop on lv_layer_top through a local
        }
        const std::string src = read_xml(path);
        size_t pos = 0;
        bool creates_screen_backdrop = false;
        for (const char* call : {"create_fullscreen_backdrop(", "create_darkened_backdrop("}) {
            pos = 0;
            while ((pos = src.find(call, pos)) != std::string::npos) {
                const size_t eol = src.find('\n', pos);
                const std::string line = src.substr(pos, eol - pos);
                if (line.find("lv_layer_top()") == std::string::npos) {
                    creates_screen_backdrop = true;
                }
                pos += 1;
            }
        }
        if (!creates_screen_backdrop) {
            continue;
        }
        ++checked;
        INFO(path << " creates a screen-level backdrop");
        CHECK(src.find("bring_to_front(") != std::string::npos);
    }
    CHECK(checked >= 4);
}

TEST_CASE("no root panel takes text input, so the keyboard never opens over a live navbar",
          "[estop_navbar][navigation]") {
    // The lifted E-stop sits over a navbar button. That is only safe while the
    // navbar is inert under an overlay or modal backdrop.
    for (const char* stem : {"home_panel", "print_select_panel", "controls_panel", "filament_panel",
                             "settings_panel", "advanced_panel"}) {
        for (const char* dir : {"ui_xml/", "ui_xml/micro/", "ui_xml/portrait/"}) {
            const std::string path = std::string(dir) + stem + ".xml";
            if (!std::filesystem::exists(path)) {
                continue;
            }
            INFO(path);
            const std::string xml = read_xml(path);
            CHECK(xml.find("<text_input") == std::string::npos);
            CHECK(xml.find("<lv_textarea") == std::string::npos);
        }
    }
}

// ============================================================================
// The spools-on-the-bed button: the same navbar treatment as the E-stop, so a
// bed-drying latch stays visible beside overlays that cover the banner.
// ============================================================================

namespace {

struct DryingNavbarFixture : public NavbarFixture {
    DryingNavbarFixture() {
        drying_state_ = lv_xml_get_subject(nullptr, "bed_drying_state");
        if (!drying_state_) {
            // The registry keeps the pointer past this fixture, so it is static.
            static lv_subject_t own_state;
            lv_subject_init_int(&own_state, 0);
            lv_xml_register_subject(nullptr, "bed_drying_state", &own_state);
            drying_state_ = &own_state;
            // The navbar binds at creation, so rebuild it against the subject.
            auto& nav = NavigationManager::instance();
            nav.deinit_subjects();
            nav.init();
            navbar_ =
                static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
            nav.wire_events(navbar_);
            helix::ui::UpdateQueue::instance().drain();
            // The rebuild replaced the navbar the base fixture looked up.
            slot_ = lv_obj_find_by_name(navbar_, "nav_estop_slot");
            estop_ = nav.navbar_estop();
            REQUIRE(estop_ != nullptr);
        }
        saved_drying_ = lv_subject_get_int(drying_state_);
        drying_slot_ = lv_obj_find_by_name(navbar_, "nav_drying_slot");
        REQUIRE(drying_slot_ != nullptr);
        drying_ = NavigationManager::instance().navbar_drying();
        REQUIRE(drying_ != nullptr);
    }
    ~DryingNavbarFixture() override {
        lv_subject_set_int(drying_state_, saved_drying_);
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_latched(int state) {
        lv_subject_set_int(drying_state_, state);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(test_screen());
    }

    lv_subject_t* drying_state_ = nullptr;
    int saved_drying_ = 0;
    lv_obj_t* drying_slot_ = nullptr;
    lv_obj_t* drying_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(DryingNavbarFixture,
                 "navbar drying button is shown exactly while spools are latched",
                 "[estop_navbar][bed_drying][navigation]") {
    set_latched(0);
    CHECK(lv_obj_has_flag(drying_, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(drying_slot_, LV_OBJ_FLAG_HIDDEN));
    set_latched(4); // any non-idle run state
    CHECK_FALSE(lv_obj_has_flag(drying_, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(drying_slot_, LV_OBJ_FLAG_HIDDEN));
    set_latched(0);
    CHECK(lv_obj_has_flag(drying_, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(DryingNavbarFixture, "navbar drying button does what the banner does",
                 "[estop_navbar][bed_drying][navigation]") {
    CHECK(read_xml("ui_xml/components/navbar_drying.xml")
              .find("callback=\"on_bed_drying_banner_clicked\"") != std::string::npos);
}

TEST_CASE_METHOD(DryingNavbarFixture, "navbar drying button sits over its slot beside the E-stop",
                 "[estop_navbar][bed_drying][navigation]") {
    set_latched(4);
    set_visible(1);
    lv_area_t slot, button, estop;
    lv_obj_get_coords(drying_slot_, &slot);
    lv_obj_get_coords(drying_, &button);
    lv_obj_get_coords(estop_, &estop);
    CHECK(button.x1 == slot.x1);
    CHECK(button.y1 == slot.y1);
    const bool overlap = button.x1 <= estop.x2 && estop.x1 <= button.x2 && button.y1 <= estop.y2 &&
                         estop.y1 <= button.y2;
    CHECK_FALSE(overlap);
}

TEST_CASE_METHOD(DryingNavbarFixture,
                 "navbar drying button and E-stop both stay above an overlay backdrop",
                 "[estop_navbar][bed_drying][navigation]") {
    auto& nav = NavigationManager::instance();
    set_latched(4);
    set_visible(1);
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, test_screen());
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);
    CHECK(index_of(drying_) > index_of(backdrop));
    CHECK(index_of(estop_) > index_of(backdrop));
    CHECK_FALSE(lv_obj_has_flag(drying_, LV_OBJ_FLAG_HIDDEN));
    CHECK(helix::ui::is_screen_chrome(drying_));
}
