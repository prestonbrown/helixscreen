// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_sidebar.h"
#include "ui_bed_drying_modal.h"
#include "ui_filament_runout_handler.h"
#include "ui_filament_slot_picker.h"
#include "ui_unload_lane_choice.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/widget_in_tree.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_state.h"
#include "bed_drying_controller.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/ams_sidebar_xml.h"
#include "test_helpers/filament_runout_handler_test_access.h"
#include "test_helpers/filament_sensor_widget_test_access.h"
#include "test_helpers/modal_answer.h"
#include "theme_manager.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsState;

namespace {

helix::AvailableSlot slot(int index, int backend = 0) {
    helix::AvailableSlot s{};
    s.slot_index = index;
    s.backend_index = backend;
    s.material = "PLA";
    s.is_empty = false;
    s.current_tool_mapping = -1;
    return s;
}

std::vector<int> indices(const std::vector<helix::AvailableSlot>& slots) {
    std::vector<int> out;
    for (const auto& s : slots) {
        out.push_back(s.slot_index);
    }
    return out;
}

} // namespace

TEST_CASE("unload_lane_choices offers exactly the candidate lanes", "[ams][1324]") {
    const std::vector<helix::AvailableSlot> slots = {slot(0), slot(1), slot(2), slot(3)};

    SECTION("candidates pick their slots in candidate order") {
        CHECK(indices(helix::ui::unload_lane_choices({2, 0}, slots)) == std::vector<int>{2, 0});
    }
    SECTION("no candidates offers nothing") {
        CHECK(helix::ui::unload_lane_choices({}, slots).empty());
    }
    SECTION("a candidate with no published slot is dropped") {
        CHECK(indices(helix::ui::unload_lane_choices({1, 7}, slots)) == std::vector<int>{1});
    }
    SECTION("a secondary backend's slot with the same index is not the primary's lane") {
        const std::vector<helix::AvailableSlot> mixed = {slot(1, 1), slot(1, 0)};
        const auto choices = helix::ui::unload_lane_choices({1}, mixed);
        REQUIRE(choices.size() == 1);
        CHECK(choices[0].backend_index == 0);
    }
}

namespace {

/// The standalone IFS module with filament in the head that no lane claims,
/// recording what it would send.
class UnaccountedModuleBackend : public helix::AmsBackendAd5xIfs {
  public:
    UnaccountedModuleBackend() : AmsBackendAd5xIfs(nullptr, nullptr) {
        helix::Ad5xIfsTestAccess::set_running(*this, true);
        helix::Ad5xIfsTestAccess::handle_status(
            *this, nlohmann::json{{"ifs",
                                   {{"connected", true},
                                    {"error", nullptr},
                                    {"channel_count", 4},
                                    {"state", 5},
                                    {"activity", "ready"},
                                    {"activity_channel", 0},
                                    {"active_channel", 0},
                                    {"loaded_channels", {1, 2, 3, 4}},
                                    {"moving_channels", nlohmann::json::array()},
                                    {"pending_insert_channels", nlohmann::json::array()},
                                    {"params", nlohmann::json::object()}}}});
        helix::Ad5xIfsTestAccess::set_head_switch(*this, true, true);
        helix::Ad5xIfsTestAccess::set_head_filament(*this, true);
        helix::Ad5xIfsTestAccess::set_zcolor_supported(*this, false);
    }

    std::vector<std::string> sent;

    helix::AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()>) override {
        sent.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                                  helix::SharedCallback<void(const MoonrakerError&)>,
                                  bool) override {
        sent.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return true;
    }
};

struct UnloadLaneSidebarFixture : public XMLTestFixture {
    UnloadLaneSidebarFixture() {
        auto owned = std::make_unique<UnaccountedModuleBackend>();
        backend_ = owned.get();
        AmsState::instance().set_backend(std::move(owned));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();

        helix::test::register_ams_sidebar_xml();
        panel_ = lv_obj_create(test_screen());
        const char* attrs[] = {"name", "ams_operation_sidebar", nullptr};
        REQUIRE(lv_xml_create(panel_, "ams_sidebar", attrs) != nullptr);
        sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(state());
        REQUIRE(sidebar_->setup(panel_));
    }

    ~UnloadLaneSidebarFixture() override {
        sidebar_.reset();
        if (panel_ && helix::test::widget_in_tree(panel_)) {
            lv_obj_delete(panel_);
        }
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().set_backend(nullptr);
    }

    UnaccountedModuleBackend* backend_ = nullptr;
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;
    lv_obj_t* panel_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(UnloadLaneSidebarFixture,
                 "Sidebar unloads the lane the user named as the one in the toolhead",
                 "[ams][1324]") {
    REQUIRE(backend_->get_current_slot() < 0);
    REQUIRE(backend_->unload_lane_question().lanes == std::vector<int>{0, 1, 2, 3});

    SECTION("a named lane counts as loaded and is unloaded by number") {
        sidebar_->handle_unload(2, /*lane_in_toolhead=*/true);
        CHECK(backend_->sent == std::vector<std::string>{"IFS_UNLOAD SLOT=3"});
    }
    SECTION("the same lane from the context menu is still nothing loaded") {
        sidebar_->handle_unload(2);
        CHECK(backend_->sent.empty());
    }
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture, "Sidebar sends the extruder-only clear when chosen",
                 "[ams][1324]") {
    sidebar_->handle_unload(helix::kClearToolheadSlot, /*lane_in_toolhead=*/true);
    CHECK(backend_->sent ==
          std::vector<std::string>{"_IFS_SENSOR_RESUME\n_IFS_CLEAR_EXTRUDER TEMP=220"});
}

namespace {

lv_obj_t* find_on_screen(const char* name) {
    return lv_obj_find_by_name(lv_screen_active(), name);
}

void tap(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
}

} // namespace

TEST_CASE_METHOD(UnloadLaneSidebarFixture,
                 "The lane picker offers the lanes, an extruder-only clear and Cancel",
                 "[ams][1324]") {
    std::vector<int> chosen;
    REQUIRE(helix::ui::choose_unload_lane(backend_, lv_obj_find_by_name(panel_, "btn_unload"),
                                          [&chosen](int slot) { chosen.push_back(slot); }));
    CHECK(ModalStack::instance().stack_empty());

    SECTION("Clear extruder only asks for the toolhead clear") {
        tap(find_on_screen("extra_choice"));
        CHECK(chosen == std::vector<int>{helix::kClearToolheadSlot});
    }
    SECTION("Cancel answers nothing and closes the picker") {
        tap(find_on_screen("cancel_choice"));
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
        CHECK(chosen.empty());
        CHECK(find_on_screen("extra_choice") == nullptr);
        CHECK(backend_->sent.empty());
    }
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture,
                 "One lane holding filament still gets the three-way picker", "[ams][1324]") {
    helix::Ad5xIfsTestAccess::handle_status(
        *backend_, nlohmann::json{{"ifs", {{"loaded_channels", {3}}, {"active_channel", 0}}}});
    AmsState::instance().sync_from_backend();
    REQUIRE(backend_->unload_lane_question().lanes == std::vector<int>{2});

    std::vector<int> chosen;
    REQUIRE(helix::ui::choose_unload_lane(backend_, lv_obj_find_by_name(panel_, "btn_unload"),
                                          [&chosen](int slot) { chosen.push_back(slot); }));
    // A picker, not a dialog whose cancel position would heat and cut.
    CHECK(ModalStack::instance().stack_empty());
    REQUIRE(find_on_screen("extra_choice") != nullptr);
    tap(find_on_screen("cancel_choice"));
    CHECK(chosen.empty());
    CHECK(backend_->sent.empty());
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture,
                 "No lane holding filament asks to confirm the extruder clear", "[ams][1324]") {
    helix::Ad5xIfsTestAccess::handle_status(
        *backend_,
        nlohmann::json{
            {"ifs", {{"loaded_channels", nlohmann::json::array()}, {"active_channel", 0}}}});
    AmsState::instance().sync_from_backend();
    REQUIRE(backend_->unload_lane_question().confirm_clear_toolhead);

    std::vector<int> chosen;
    REQUIRE(helix::ui::choose_unload_lane(backend_, lv_obj_find_by_name(panel_, "btn_unload"),
                                          [&chosen](int slot) { chosen.push_back(slot); }));
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    SECTION("Clear asks for the toolhead clear") {
        helix::test::answer_top_modal("btn_primary");
        CHECK(chosen == std::vector<int>{helix::kClearToolheadSlot});
    }
    SECTION("Cancel answers nothing") {
        helix::test::answer_top_modal("btn_secondary");
        CHECK(chosen.empty());
    }
    SECTION("dismissing answers nothing") {
        helix::test::dismiss_top_modal();
        CHECK(chosen.empty());
    }
    CHECK(backend_->sent.empty());
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture, "The runout dialog's Unload asks the same question",
                 "[ams][runout][1324]") {
    helix::ui::FilamentRunoutHandler handler(nullptr);

    SECTION("lanes holding filament open the picker; Cancel sends nothing") {
        helix::ui::FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
        REQUIRE(find_on_screen("extra_choice") != nullptr);
        tap(find_on_screen("cancel_choice"));
        CHECK(backend_->sent.empty());
    }
    SECTION("no lane holding filament asks to confirm the clear, which then runs") {
        helix::Ad5xIfsTestAccess::handle_status(
            *backend_,
            nlohmann::json{
                {"ifs", {{"loaded_channels", nlohmann::json::array()}, {"active_channel", 0}}}});
        AmsState::instance().sync_from_backend();
        helix::ui::FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
        REQUIRE(ModalStack::instance().top_dialog() != nullptr);
        CHECK(backend_->sent.empty());
        helix::test::answer_top_modal("btn_primary");
        CHECK(backend_->sent ==
              std::vector<std::string>{"_IFS_SENSOR_RESUME\n_IFS_CLEAR_EXTRUDER TEMP=220"});
    }
}

namespace {

void empty_every_lane(UnaccountedModuleBackend& backend) {
    helix::Ad5xIfsTestAccess::handle_status(
        backend,
        nlohmann::json{
            {"ifs", {{"loaded_channels", nlohmann::json::array()}, {"active_channel", 0}}}});
    AmsState::instance().sync_from_backend();
}

} // namespace

TEST_CASE_METHOD(UnloadLaneSidebarFixture,
                 "The filament sensor tile's Unload asks the same question", "[ams][tile][1324]") {
    helix::FilamentSensorWidget tile;

    SECTION("lanes holding filament open the picker; Cancel sends nothing") {
        helix::FilamentSensorWidgetTestAccess::dispatch_unload(tile);
        REQUIRE(find_on_screen("extra_choice") != nullptr);
        tap(find_on_screen("cancel_choice"));
        CHECK(backend_->sent.empty());
    }
    SECTION("a picked lane is unloaded by number") {
        helix::FilamentSensorWidgetTestAccess::dispatch_unload(tile);
        REQUIRE(find_on_screen("extra_choice") != nullptr);
        // The lane rows come in slot order; the first is slot 1.
        tap(find_on_screen("filament_picker_row_0"));
        CHECK(backend_->sent == std::vector<std::string>{"IFS_UNLOAD SLOT=1"});
    }
    SECTION("no lane holding filament asks to confirm the clear, which then runs") {
        empty_every_lane(*backend_);
        helix::FilamentSensorWidgetTestAccess::dispatch_unload(tile);
        REQUIRE(ModalStack::instance().top_dialog() != nullptr);
        CHECK(backend_->sent.empty());
        helix::test::answer_top_modal("btn_primary");
        CHECK(backend_->sent ==
              std::vector<std::string>{"_IFS_SENSOR_RESUME\n_IFS_CLEAR_EXTRUDER TEMP=220"});
    }
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture, "Bed drying's unload asks the same question",
                 "[ams][bed_drying][1324]") {
    helix::BedDryingController ctrl(state(), &api(), nullptr);
    bool moved_on = false;
    helix::ui::unload_before_drying(ctrl, [&] { moved_on = true; });
    REQUIRE(find_on_screen("extra_choice") != nullptr);

    SECTION("Cancel sends nothing and the drying does not move on") {
        tap(find_on_screen("cancel_choice"));
        CHECK(backend_->sent.empty());
        CHECK_FALSE(moved_on);
    }
    SECTION("Clear extruder only sends the clear") {
        tap(find_on_screen("extra_choice"));
        CHECK(backend_->sent ==
              std::vector<std::string>{"_IFS_SENSOR_RESUME\n_IFS_CLEAR_EXTRUDER TEMP=220"});
    }
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture,
                 "The lane picker shows every row, Cancel included, without scrolling",
                 "[ams][1324][picker]") {
    // Four lanes plus the clear and Cancel rows: the tallest the question gets.
    REQUIRE(backend_->unload_lane_question().lanes == std::vector<int>{0, 1, 2, 3});
    REQUIRE(helix::ui::choose_unload_lane(backend_, lv_obj_find_by_name(panel_, "btn_unload"),
                                          [](int) {}));
    lv_obj_t* cancel = find_on_screen("cancel_choice");
    REQUIRE(cancel != nullptr);
    lv_obj_t* card = lv_obj_get_parent(cancel);
    lv_obj_update_layout(card);

    CHECK(lv_obj_get_scroll_bottom(card) <= 0);
    CHECK(lv_obj_get_scroll_top(card) <= 0);
    lv_area_t card_area;
    lv_obj_get_coords(card, &card_area);
    CHECK(card_area.y1 >= 0);
    CHECK(card_area.y2 < lv_display_get_vertical_resolution(lv_display_get_default()));
    lv_area_t cancel_area;
    lv_obj_get_coords(cancel, &cancel_area);
    CHECK(cancel_area.y2 <= card_area.y2);
    tap(cancel);
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture, "The lane picker is as wide as its widest row",
                 "[ams][1324][picker]") {
    REQUIRE(helix::ui::choose_unload_lane(backend_, lv_obj_find_by_name(panel_, "btn_unload"),
                                          [](int) {}));
    lv_obj_t* cancel = find_on_screen("cancel_choice");
    REQUIRE(cancel != nullptr);
    lv_obj_t* card = lv_obj_get_parent(cancel);
    lv_obj_update_layout(card);

    // Every label shows its whole text, and the widest one leaves no slack:
    // the card fits its content rather than a fixed width.
    int32_t least_slack = INT32_MAX;
    const uint32_t rows = lv_obj_get_child_count(card);
    for (uint32_t i = 0; i < rows; ++i) {
        lv_obj_t* row = lv_obj_get_child(card, static_cast<int32_t>(i));
        lv_obj_t* label =
            lv_obj_check_type(row, &lv_label_class) ? row : lv_obj_find_by_name(row, "slot_label");
        if (!label) {
            continue;
        }
        lv_point_t text{};
        lv_text_get_size(&text, lv_label_get_text(label),
                         lv_obj_get_style_text_font(label, LV_PART_MAIN),
                         lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        const int32_t slack = lv_obj_get_content_width(label) - text.x;
        CHECK(slack >= 0);
        least_slack = std::min(least_slack, slack);
    }
    CHECK(least_slack <= 2);
    CHECK(lv_obj_get_width(card) < lv_display_get_horizontal_resolution(lv_display_get_default()));
    tap(cancel);
}

TEST_CASE_METHOD(UnloadLaneSidebarFixture,
                 "A picker row wider than the screen caps the card and ellipsizes its label",
                 "[ams][1324][picker]") {
    helix::ui::FilamentSlotPicker picker;
    helix::ui::FilamentSlotPicker::Options options;
    options.highlight_trigger = false;
    options.extra_choice = std::string(100, 'W');
    options.cancel_choice = "Cancel";
    picker.show(
        lv_screen_active(), lv_obj_find_by_name(panel_, "btn_unload"), {slot(0)},
        helix::ui::FilamentSlotPicker::Selection{},
        [](const helix::ui::FilamentSlotPicker::Selection&) {}, options);

    lv_obj_t* extra = find_on_screen("extra_choice");
    REQUIRE(extra != nullptr);
    lv_obj_t* card = lv_obj_get_parent(extra);
    lv_obj_update_layout(card);

    const int32_t screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    CHECK(lv_obj_get_width(card) <= screen_w - 2 * theme_manager_get_spacing("space_md"));

    lv_obj_t* label = lv_obj_find_by_name(extra, "slot_label");
    REQUIRE(label != nullptr);
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS);
    // The label's own text is the shortened one once dots apply; measure the
    // full string the row was given.
    lv_point_t text{};
    lv_text_get_size(&text, options.extra_choice.c_str(),
                     lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    CHECK(lv_obj_get_content_width(label) < text.x);
    // The row's label grows to the cap, not to whatever the title leaves it.
    CHECK(lv_obj_get_width(card) == screen_w - 2 * theme_manager_get_spacing("space_md"));
    lv_area_t card_area;
    lv_area_t label_area;
    lv_obj_get_coords(card, &card_area);
    lv_obj_get_coords(label, &label_area);
    CHECK(label_area.x2 <= card_area.x2);
    picker.hide();
}
