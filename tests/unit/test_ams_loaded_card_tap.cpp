// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_loaded_card_tap.cpp
 * @brief A tap on the AMS sidebar's loaded card opens the loaded lane's spool:
 *        the editor when the lane links a Spoolman spool, the spool picker when
 *        it links none, and nothing while no lane is loaded (#2094).
 */

#include "ui_ams_edit_overlay.h"
#include "ui_nav_manager.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class LoadedCardTapFixture : public LVGLUITestFixture {
  public:
    LoadedCardTapFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        helix::ui::destroy_static_panels();
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
        NavigationManager::instance().set_panels(panels);

        auto mock = std::make_unique<AmsBackendMock>(4);
        mock->set_operation_delay(0);
        REQUIRE(mock->start().success());
        backend_ = mock.get();
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        spoolman_ = lv_xml_get_subject(nullptr, "printer_has_spoolman");
        REQUIRE(spoolman_ != nullptr);
        spoolman_was_ = lv_subject_get_int(spoolman_);
        lv_subject_set_int(spoolman_, 1);
    }

    ~LoadedCardTapFixture() override {
        lv_subject_set_int(spoolman_, spoolman_was_);
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().set_backend(nullptr);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    void settle() {
        backend_->wait_for_operation_thread();
        AmsState::instance().sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(50);
    }

    AmsBackendMock* backend_ = nullptr;
    lv_subject_t* spoolman_ = nullptr;
    int32_t spoolman_was_ = 0;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(LoadedCardTapFixture, "a tap on the loaded card opens the loaded lane's spool",
                 "[ams][loaded_card][ui_integration]") {
    const int linked_id = GENERATE(0, 42);
    CAPTURE(linked_id);
    SlotInfo info = backend_->get_slot_info(1);
    info.spoolman_id = linked_id;
    REQUIRE(helix::test::apply_edit(*backend_, 1, info).success());
    if (backend_->is_filament_loaded()) {
        REQUIRE(backend_->unload_filament(backend_->get_current_slot()).success());
    }
    settle();

    navigate_to_ams_panel();
    settle();
    AmsPanel* panel = get_existing_ams_panel();
    REQUIRE(panel != nullptr);
    lv_obj_t* card = lv_obj_find_by_name(panel->get_panel(), "current_loaded_card");
    REQUIRE(card != nullptr);
    auto& overlay = helix::ui::get_ams_edit_overlay();

    // Nothing loaded: the card takes no taps, and a tap that lands anyway opens nothing.
    CHECK_FALSE(lv_obj_has_flag(card, LV_OBJ_FLAG_CLICKABLE));
    lv_obj_send_event(card, LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK_FALSE(overlay.is_visible());

    REQUIRE(backend_->load_filament(1).success());
    settle();
    REQUIRE(backend_->get_current_slot() == 1);
    REQUIRE(lv_obj_has_flag(card, LV_OBJ_FLAG_CLICKABLE));

    // A press anywhere on the card lands on the card, not on a child that eats it.
    lv_obj_update_layout(card);
    std::vector<lv_obj_t*> pending{card};
    while (!pending.empty()) {
        lv_obj_t* obj = pending.back();
        pending.pop_back();
        for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
            lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            pending.push_back(child);
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            lv_point_t centre{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
            INFO((lv_obj_get_name(child) ? lv_obj_get_name(child) : "(unnamed)"));
            CHECK(lv_indev_search_obj(card, &centre) == card);
        }
    }

    lv_obj_send_event(card, LV_EVENT_CLICKED, nullptr);
    settle();
    REQUIRE(overlay.is_visible());
    lv_subject_t* view = lv_xml_get_subject(nullptr, "ams_edit_view");
    REQUIRE(view != nullptr);
    CHECK(lv_subject_get_int(view) == (linked_id ? helix::ui::AmsEditOverlay::kViewEditor
                                                 : helix::ui::AmsEditOverlay::kViewSpoolPicker));

    NavigationManager::instance().go_back();
    settle();
}
