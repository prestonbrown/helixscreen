// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_environment_overlay_live_refresh.cpp
 * @brief The environment overlay's unit art glows while its unit dries.
 *
 * Live readings following the printer are covered with the zone scope in
 * test_ams_environment_overlay_zones.cpp.
 */

#include "ui_ams_environment_overlay.h"
#include "ui_unit_art.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"

#include <lvgl/lvgl.h>

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLUITestFixture, "The dryer page glows only while its unit is drying",
                 "[ams][unit-art][glow]") {
    using helix::ui::get_ams_environment_overlay;
    auto& display = DisplaySettingsManager::instance();
    const bool prev_animations = display.get_animations_enabled();
    display.set_animations_enabled(true);

    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_ace_identity();
    mock->set_dryer_enabled(true);
    mock->set_environment_mode("dryer");
    REQUIRE(mock->start().success());
    auto* backend = mock.get();
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    auto& overlay = get_ams_environment_overlay();
    overlay.init_subjects();
    overlay.register_callbacks();
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* art = lv_obj_find_by_name(root, "unit_art_picture");
    REQUIRE(art != nullptr);

    const auto drain = [this]() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    };
    const auto glow_is_on = [art]() {
        lv_obj_t* glow = helix::ui::unit_art_glow_obj(art);
        return glow && !lv_obj_is_hidden(glow);
    };

    overlay.on_activate();
    drain();
    REQUIRE(helix::ui::unit_art_is_flattened(art));
    CHECK_FALSE(glow_is_on());

    REQUIRE(backend->start_drying(55.0f, 240, -1, 0).success());
    overlay.refresh();
    drain();
    REQUIRE(glow_is_on());
    lv_obj_t* glow = helix::ui::unit_art_glow_obj(art);
    CHECK(lv_anim_get(glow, nullptr) != nullptr);

    display.set_animations_enabled(false);
    overlay.refresh(); // the setting applies at the next publish
    drain();
    CHECK(glow_is_on());
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
    display.set_animations_enabled(true);
    overlay.refresh(); // the setting applies at the next publish
    drain();
    CHECK(lv_anim_get(glow, nullptr) != nullptr);

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    drain();
    CHECK_FALSE(glow_is_on());
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
    overlay.refresh(); // select_zone() and the keypad still publish on a closed overlay
    CHECK_FALSE(glow_is_on());

    overlay.on_activate();
    drain();
    REQUIRE(glow_is_on());
    REQUIRE(backend->stop_drying(0).success());
    overlay.refresh();
    drain();
    CHECK_FALSE(glow_is_on());

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    drain();
    AmsState::instance().set_backend(nullptr);
    display.set_animations_enabled(prev_animations);
}
