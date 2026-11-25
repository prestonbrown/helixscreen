// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ipv4_settings_apply.cpp
 * @brief IPv4 settings' Apply: the header's labelled primary, inside the
 *        screen at 480x272 and 800x480, and its tap reaches the form check.
 *        Use DHCP stays in the content.
 */

#include "ui_button.h"
#include "ui_nav_manager.h"
#include "ui_overlay_ipv4_settings.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_geometry.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../ui_test_utils.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "runtime_config.h"

#include <array>
#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct ScreenSize {
    const char* what;
    int32_t w;
    int32_t h;
};

constexpr ScreenSize kSizes[] = {
    {"480x272", 480, 272},
    {"800x480", 800, 480},
};

bool shown(lv_obj_t* obj) {
    for (lv_obj_t* p = obj; p != nullptr; p = lv_obj_get_parent(p)) {
        if (lv_obj_is_hidden(p)) {
            return false;
        }
    }
    return true;
}

bool inside_screen(lv_obj_t* obj, const ScreenSize& s) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.x1 >= 0 && a.y1 >= 0 && a.x2 < s.w && a.y2 < s.h;
}

bool is_descendant(lv_obj_t* obj, lv_obj_t* ancestor) {
    for (lv_obj_t* p = lv_obj_get_parent(obj); p != nullptr; p = lv_obj_get_parent(p)) {
        if (p == ancestor) {
            return true;
        }
    }
    return false;
}

class Ipv4ApplyFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config_;

    Ipv4ApplyFixture() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        get_runtime_config()->use_real_ethernet = false;
        for (auto& p : panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels_.data());
    }

    ~Ipv4ApplyFixture() override {
        UITest::cleanup();
        close();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    void close() {
        for (lv_obj_t* orphan : StaticPanelRegistry::instance().destroy_all()) {
            lv_obj_delete(orphan);
        }
        settle();
    }

    lv_obj_t* find(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(lv_screen_active(), name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    lv_subject_t* subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return s;
    }

    /// Opens the Ethernet form and waits for the mock's settings to load.
    void open_form() {
        settings::get_ipv4_settings_overlay().open(lv_screen_active(), net::Ipv4Link::Ethernet,
                                                   "192.168.1.50");
        settle();
        REQUIRE(wait_until([this] { return lv_subject_get_int(subject("ipv4_busy")) == 0; }));
        settle();
        lv_obj_update_layout(lv_screen_active());
        UITest::init(lv_screen_active());
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(Ipv4ApplyFixture,
                 "IPv4 Apply is the header's labelled action and its tap checks the form",
                 "[ipv4][save_header]") {
    for (const ScreenSize& s : kSizes) {
        INFO(s.what);
        helix::test::ScopedGeometry screen(s.w, s.h);
        open_form();

        lv_obj_t* apply = find("ipv4_apply_btn");
        CHECK(shown(apply));
        CHECK(inside_screen(apply, s));
        CHECK(is_descendant(apply, find("header_actions")));
        lv_obj_t* label = ui_button_get_label(apply);
        REQUIRE(label != nullptr);
        CHECK(shown(label));

        // Use DHCP stays in the content, inside the screen.
        lv_obj_t* dhcp = find("ipv4_dhcp_btn");
        CHECK(is_descendant(dhcp, find("ipv4_buttons")));
        CHECK(shown(dhcp));
        CHECK(inside_screen(dhcp, s));

        // A malformed address: the tap reaches the form check, which names it.
        lv_subject_set_string(subject("ipv4_address"), "999.1.1.1");
        REQUIRE(std::string(lv_subject_get_string(subject("ipv4_error"))).empty());
        REQUIRE(UITest::click(apply));
        settle();
        CHECK_FALSE(std::string(lv_subject_get_string(subject("ipv4_error"))).empty());

        close();
    }
}
