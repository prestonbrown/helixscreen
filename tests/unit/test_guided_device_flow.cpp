// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_guided_device_flow.cpp
 * @brief Device actions that name a guided flow start it; every other action executes.
 *
 * Run with: ./build/bin/helix-tests "[ams][guided-flow]"
 */

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_guided_device_flow.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/device_action_controls.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_device_action_value.h"
#include "ams_error.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml_expr.h"
#include "indx_contract.h"
#include "snapmaker_nozzle_offset.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::printer::ActionType;
using helix::printer::DeviceAction;
using helix::printer::DeviceSection;
using helix::ui::get_ams_device_section_detail_overlay;

namespace helix::ui {
class AmsDeviceSectionDetailOverlayTestAccess {
  public:
    static void open_keypad_for(AmsDeviceSectionDetailOverlay& o, const std::string& id) {
        o.keypad_action_id_ = id;
    }
    static void keypad_value(float value) {
        AmsDeviceSectionDetailOverlay::on_keypad_value(value, nullptr);
    }
};
} // namespace helix::ui

namespace {

class CountingMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    int cutter_calibrations = 0;
    AmsError calibrate_cutter(std::function<void(bool, const std::string&)>) override {
        ++cutter_calibrations;
        return AmsErrorHelper::success();
    }
};

class GuidedFlowFixture : public LVGLUITestFixture {
  public:
    CountingMock* backend = nullptr;

    GuidedFlowFixture() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        auto& ams = AmsState::instance();
        ams.deinit_subjects();

        auto owned = std::make_unique<CountingMock>(4);
        backend = owned.get();
        backend->set_operation_delay(0);
        REQUIRE(backend->start().success());
        ams.set_backend(std::move(owned));
        ams.init_subjects(true);
        ams.sync_from_backend();
    }

    ~GuidedFlowFixture() override {
        for (const char* id : {"t", "cfs_cutter", "cfs_purge_chute"}) {
            helix::ui::unregister_guided_device_flow(id);
        }
        NavigationManager::instance().go_back();
        settle();
        helix::ui::destroy_static_panels();
        settle();
        AmsState::instance().set_backend(nullptr);
    }

    void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        process_lvgl(10);
    }

    /// Build the overlay for a "calibration" section holding @p actions and return the
    /// button named @p id.
    lv_obj_t* open_section_and_find(std::vector<DeviceAction> actions, const char* id) {
        backend->set_device_sections({DeviceSection{"calibration", "Calibration", 0, ""}});
        backend->set_device_actions(std::move(actions));
        auto& overlay = get_ams_device_section_detail_overlay();
        overlay.init_subjects();
        overlay.show(test_screen(), "calibration", "Calibration");
        settle();
        lv_obj_t* btn = lv_obj_find_by_name(overlay.get_root(), id);
        REQUIRE(btn != nullptr);
        return btn;
    }

    /// Build the overlay for a "prefs" section holding @p actions; returns its root.
    lv_obj_t* open_prefs(std::vector<DeviceAction> actions) {
        backend->set_device_sections({DeviceSection{"prefs", "Preferences", 0, ""}});
        backend->set_device_actions(std::move(actions));
        auto& overlay = get_ams_device_section_detail_overlay();
        overlay.init_subjects();
        overlay.show(test_screen(), "prefs", "Preferences");
        settle();
        return overlay.get_root();
    }

    static std::string text_of(lv_obj_t* row, const char* name) {
        lv_obj_t* label = lv_obj_find_by_name(row, name);
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    static DeviceAction button(const char* id, const char* flow) {
        DeviceAction a = DeviceAction::button(id, id, "calibration");
        a.guided_flow = flow;
        return a;
    }

    void click(lv_obj_t* btn) {
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        settle();
    }

    std::string last_executed() const {
        return backend->get_last_executed_action().first;
    }
};

} // namespace

TEST_CASE_METHOD(GuidedFlowFixture,
                 "the section overlay starts a registered flow and executes the rest",
                 "[ams][guided-flow]") {
    int starts = 0;
    std::string started_label;
    helix::ui::register_guided_device_flow("t", {nullptr, [&](const std::string& label) {
                                                     ++starts;
                                                     started_label = label;
                                                 }});

    lv_obj_t* guided = open_section_and_find(
        {button("guided_action", "t"), button("plain_action", "")}, "guided_action");
    click(guided);
    CHECK(starts == 1);
    CHECK(started_label == "guided_action");
    CHECK(last_executed().empty());

    click(lv_obj_find_by_name(get_ams_device_section_detail_overlay().get_root(), "plain_action"));
    CHECK(starts == 1);
    CHECK(last_executed() == "plain_action");
}

TEST_CASE_METHOD(GuidedFlowFixture, "an unregistered guided flow never executes its action",
                 "[ams][guided-flow]") {
    lv_obj_t* btn =
        open_section_and_find({button("calibrate_cutter", "cfs_cutter")}, "calibrate_cutter");

    SECTION("before any registration") {
        click(btn);
        CHECK(last_executed().empty());
        CHECK(backend->cutter_calibrations == 0);
        CHECK(ModalStack::instance().empty());
    }

#if HELIX_HAS_CFS
    SECTION("after registration the confirmation gates the action") {
        helix::ui::register_cfs_device_flows();
        click(btn);
        // A registered CFS flow asks before it moves the printer.
        CHECK(helix::ui::find_guided_device_flow("cfs_cutter") != nullptr);
        CHECK_FALSE(ModalStack::instance().empty());
        CHECK(backend->cutter_calibrations == 0);
        CHECK(last_executed().empty());

        lv_obj_t* confirm = lv_obj_find_by_name(lv_screen_active(), "btn_primary");
        REQUIRE(confirm != nullptr);
        click(confirm);
        CHECK(backend->cutter_calibrations == 1);
    }
#endif
}

TEST_CASE_METHOD(GuidedFlowFixture, "a flow's status attach runs once per section build",
                 "[ams][guided-flow]") {
    int attaches = 0;
    helix::ui::register_guided_device_flow("t", {[&](lv_obj_t* container) {
                                                     REQUIRE(container != nullptr);
                                                     ++attaches;
                                                 },
                                                 [](const std::string&) {}});

    open_section_and_find({button("a", "t"), button("b", "t")}, "a");
    CHECK(attaches == 1);
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "a registered flow with no start is a no-op, not a call into an empty function",
                 "[ams][guided-flow]") {
    helix::ui::register_guided_device_flow("t", {nullptr, nullptr});
    lv_obj_t* btn = open_section_and_find({button("a", "t")}, "a");
    click(btn);
    CHECK(last_executed().empty());
}

TEST_CASE_METHOD(GuidedFlowFixture, "the section header fits a 480x320 screen",
                 "[ams][guided-flow][device-section]") {
    ScopedResolution small(lv_display_get_default(), 480, 320);
    theme_manager_refresh_layout_constants(lv_display_get_default());

    backend->set_device_sections({DeviceSection{"print_prefs", "Print Behavior", 0, ""}});
    backend->set_device_actions({DeviceAction::toggle("t", "t", "print_prefs", false)});
    auto& overlay = get_ams_device_section_detail_overlay();
    overlay.init_subjects();
    overlay.show(test_screen(), "print_prefs", "Print Behavior");
    settle();

    lv_obj_t* title = lv_obj_find_by_name(overlay.get_root(), "header_title");
    REQUIRE(title != nullptr);
    lv_obj_update_layout(title);
    CHECK(std::string(lv_label_get_text(title)) == "PRINT BEHAVIOR");
    // Positive scroll_right is the width of text clipped past the header's edge.
    CHECK(lv_obj_get_scroll_right(title) <= 0);
}

TEST_CASE_METHOD(GuidedFlowFixture, "section action buttons take the standard button height",
                 "[ams][guided-flow][device-section]") {
    ScopedResolution small(lv_display_get_default(), 480, 320);
    theme_manager_refresh_layout_constants(lv_display_get_default());

    lv_obj_t* btn = open_section_and_find({button("a", ""), button("b", "")}, "a");
    lv_obj_update_layout(btn);
    CHECK(lv_obj_get_height(btn) == theme_manager_get_spacing("button_height"));
}

// ============================================================================
// Controls come from the setting_*_row components and carry every field
// ============================================================================

TEST_CASE_METHOD(GuidedFlowFixture, "a disabled toggle is locked and says why",
                 "[ams][device-section]") {
    DeviceAction soft = DeviceAction::toggle("unload_t0", "Unload at end", "prefs", false, "",
                                             "Unload this head when a print ends");
    soft.enabled = false;
    soft.disable_reason = "Soft filament is never unloaded";
    lv_obj_t* root = open_prefs({soft});

    lv_obj_t* row = lv_obj_find_by_name(root, "unload_t0");
    REQUIRE(row != nullptr);
    lv_obj_t* sw = helix::test::find_device_action_control(root, "unload_t0", &lv_switch_class);
    REQUIRE(sw != nullptr);
    CHECK(lv_obj_has_state(sw, LV_STATE_DISABLED));
    CHECK(text_of(row, "description") == "Soft filament is never unloaded");
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "an enabled toggle shows its description, icon and value, and sends a bool",
                 "[ams][device-section]") {
    lv_obj_t* root = open_prefs({DeviceAction::toggle("purge", "Enable Purge", "prefs", true,
                                                      "water", "Purge during tool changes")});
    lv_obj_t* row = lv_obj_find_by_name(root, "purge");
    REQUIRE(row != nullptr);
    CHECK(text_of(row, "description") == "Purge during tool changes");
    lv_obj_t* icon = lv_obj_find_by_name(row, "row_icon");
    REQUIRE(icon != nullptr);
    CHECK_FALSE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* sw = helix::test::find_device_action_control(root, "purge", &lv_switch_class);
    REQUIRE(sw != nullptr);
    CHECK(lv_obj_has_state(sw, LV_STATE_CHECKED));
    CHECK_FALSE(lv_obj_has_state(sw, LV_STATE_DISABLED));

    lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    const auto [id, value] = backend->get_last_executed_action();
    CHECK(id == "purge");
    const bool* sent = std::any_cast<bool>(&value);
    REQUIRE(sent != nullptr);
    CHECK_FALSE(*sent);
}

// A toggle whose value the printer changes on its own (a driver turning feed
// assist on after a load) follows the backend while the section is open.
TEST_CASE_METHOD(GuidedFlowFixture, "an open section follows the backend's action values",
                 "[ams][device-section][1853]") {
    lv_obj_t* root = open_prefs({DeviceAction::toggle("assist", "Feed Assist", "prefs", false)});
    lv_obj_t* sw = helix::test::find_device_action_control(root, "assist", &lv_switch_class);
    REQUIRE(sw != nullptr);
    REQUIRE_FALSE(lv_obj_has_state(sw, LV_STATE_CHECKED));

    SECTION("a tick with nothing changed keeps the controls") {
        AmsState::instance().bump_data_revision();
        settle();
        CHECK(helix::test::find_device_action_control(root, "assist", &lv_switch_class) == sw);
    }
    SECTION("a new value is shown") {
        backend->set_device_actions({DeviceAction::toggle("assist", "Feed Assist", "prefs", true)});
        AmsState::instance().bump_data_revision();
        settle();
        lv_obj_t* now = helix::test::find_device_action_control(root, "assist", &lv_switch_class);
        REQUIRE(now != nullptr);
        CHECK(lv_obj_has_state(now, LV_STATE_CHECKED));
    }
    SECTION("a switch the backend never confirms goes back") {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
        REQUIRE(backend->get_last_executed_action().first == "assist");
        AmsState::instance().bump_data_revision();
        settle();
        lv_obj_t* now = helix::test::find_device_action_control(root, "assist", &lv_switch_class);
        REQUIRE(now != nullptr);
        CHECK_FALSE(lv_obj_has_state(now, LV_STATE_CHECKED));
    }
    SECTION("an action becoming unavailable is locked") {
        DeviceAction off = DeviceAction::toggle("assist", "Feed Assist", "prefs", false);
        off.enabled = false;
        off.disable_reason = "Load filament first";
        backend->set_device_actions({off});
        AmsState::instance().bump_data_revision();
        settle();
        lv_obj_t* now = helix::test::find_device_action_control(root, "assist", &lv_switch_class);
        REQUIRE(now != nullptr);
        CHECK(lv_obj_has_state(now, LV_STATE_DISABLED));
    }
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "a fractional slider moves in its step and sends the real value",
                 "[ams][device-section]") {
    lv_obj_t* root = open_prefs(
        {DeviceAction::slider("speed_fwd", "Forward Multiplier", "prefs", 1.0f, 0.5f, 2.0f, "x")
             .with_step(0.05f)});
    lv_obj_t* slider = helix::test::find_device_action_slider(root, "speed_fwd");
    REQUIRE(slider != nullptr);
    CHECK(lv_slider_get_min_value(slider) == 10);
    CHECK(lv_slider_get_max_value(slider) == 40);
    CHECK(lv_slider_get_value(slider) == 20);
    lv_obj_t* row = lv_obj_find_by_name(root, "speed_fwd");
    CHECK(text_of(row, "value") == "1.00 x");

    lv_slider_set_value(slider, 30, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    settle();
    const auto [id, value] = backend->get_last_executed_action();
    CHECK(id == "speed_fwd");
    const auto sent = helix::printer::device_action_number(value);
    REQUIRE(sent.has_value());
    CHECK(*sent == Catch::Approx(1.5));
    CHECK(text_of(row, "value") == "1.50 x");
}

TEST_CASE_METHOD(GuidedFlowFixture, "a disabled slider and dropdown are locked",
                 "[ams][device-section]") {
    DeviceAction slider = DeviceAction::slider("len", "Length", "prefs", 50.0f, 0.0f, 100.0f, "mm");
    slider.enabled = false;
    DeviceAction dropdown =
        DeviceAction::dropdown("mode", "Mode", "prefs", {"Off", "Auto"}, std::string("Auto"));
    dropdown.enabled = false;
    lv_obj_t* root = open_prefs({slider, dropdown});

    lv_obj_t* s = helix::test::find_device_action_slider(root, "len");
    REQUIRE(s != nullptr);
    CHECK(lv_obj_has_state(s, LV_STATE_DISABLED));
    lv_obj_t* d = helix::test::find_device_action_control(root, "mode", &lv_dropdown_class);
    REQUIRE(d != nullptr);
    CHECK(lv_obj_has_state(d, LV_STATE_DISABLED));
    CHECK(lv_dropdown_get_selected(d) == 1);
}

TEST_CASE_METHOD(GuidedFlowFixture, "a machine-motion action greys while a print holds the printer",
                 "[ams][device-section]") {
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    lv_obj_t* moving = open_section_and_find(
        {DeviceAction::button("calibrate", "Calibrate", "calibration").with_machine_motion(),
         button("refresh", "")},
        "calibrate");
    lv_obj_t* still =
        lv_obj_find_by_name(get_ams_device_section_detail_overlay().get_root(), "refresh");
    REQUIRE(still != nullptr);
    CHECK_FALSE(lv_obj_has_state(moving, LV_STATE_DISABLED));

    helix::test::set_wire_state(state(), PrintJobState::PRINTING);
    settle();
    CHECK(lv_obj_has_state(moving, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(still, LV_STATE_DISABLED));

    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    settle();
}

TEST_CASE_METHOD(GuidedFlowFixture, "a machine-motion toggle greys while a print holds the printer",
                 "[ams][device-section]") {
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    lv_obj_t* root = open_prefs(
        {DeviceAction::toggle("purge", "Enable Purge", "prefs", false).with_machine_motion()});
    lv_obj_t* sw = helix::test::find_device_action_control(root, "purge", &lv_switch_class);
    REQUIRE(sw != nullptr);
    CHECK_FALSE(lv_obj_has_state(sw, LV_STATE_DISABLED));
    helix::test::set_wire_state(state(), PrintJobState::PAUSED);
    settle();
    CHECK(lv_obj_has_state(sw, LV_STATE_DISABLED));
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    settle();
}

TEST_CASE_METHOD(GuidedFlowFixture, "a per-tool action names its tool in the app's tool-label form",
                 "[ams][device-section]") {
    DeviceAction unload = DeviceAction::toggle("unload_t1", "Unload at end", "prefs", false);
    unload.tool_index = 1;
    CHECK(helix::ui::device_action_label(unload) == "Unload at end - Tool 2 (T1)");
    CHECK(helix::ui::device_action_label(DeviceAction::toggle("x", "Plain", "prefs", false)) ==
          "Plain");

    lv_obj_t* root = open_prefs({unload});
    lv_obj_t* row = lv_obj_find_by_name(root, "unload_t1");
    REQUIRE(row != nullptr);
    CHECK(text_of(row, "label") == "Unload at end - Tool 2 (T1)");
}

TEST_CASE_METHOD(GuidedFlowFixture, "an enabled action's row and label read as enabled",
                 "[ams][device-section]") {
    DeviceAction off = DeviceAction::toggle("off", "Off", "prefs", false);
    off.enabled = false;
    lv_obj_t* root =
        open_prefs({DeviceAction::toggle("on", "On", "prefs", false),
                    DeviceAction::slider("len", "Length", "prefs", 5.0f, 0, 10, "mm"), off});
    const lv_color_t text = theme_manager_get_color("text");
    for (const char* id : {"on", "len"}) {
        INFO(id);
        lv_obj_t* row = lv_obj_find_by_name(root, id);
        REQUIRE(row != nullptr);
        // The row itself stays enabled: a disabled root dims every child.
        CHECK_FALSE(lv_obj_has_state(row, LV_STATE_DISABLED));
        lv_obj_t* label = lv_obj_find_by_name(row, "label");
        REQUIRE(label != nullptr);
        CHECK(lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)) ==
              lv_color_to_u32(text));
    }
    lv_obj_t* label = lv_obj_find_by_name(lv_obj_find_by_name(root, "off"), "label");
    CHECK(lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)) !=
          lv_color_to_u32(text));
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "a motion action allowed while paused greys only while printing",
                 "[ams][device-section]") {
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    lv_obj_t* btn =
        open_section_and_find({DeviceAction::button("load_extruder", "Load Extruder", "calibration")
                                   .with_machine_motion(true)},
                              "load_extruder");
    CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));
    helix::test::set_wire_state(state(), PrintJobState::PRINTING);
    settle();
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
    helix::test::set_wire_state(state(), PrintJobState::PAUSED);
    settle();
    CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    settle();
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "an unavailable machine-motion button stays disabled, says why, and does nothing",
                 "[ams][device-section]") {
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    DeviceAction cal = DeviceAction::button("calibrate_encoder", "Calibrate Encoder", "calibration")
                           .with_machine_motion();
    cal.enabled = false;
    cal.disable_reason = "No unit has an encoder";
    lv_obj_t* btn = open_section_and_find({cal}, "calibrate_encoder");
    settle();
    // machine_motion_blocked is 0 here; moves_machine's binding must not clear it.
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));

    lv_obj_t* note = lv_obj_find_by_name(get_ams_device_section_detail_overlay().get_root(),
                                         "calibrate_encoder_reason");
    REQUIRE(note != nullptr);
    CHECK(text_of(note, "note") == "Calibrate Encoder: No unit has an encoder");

    click(btn);
    CHECK(last_executed().empty());
}

TEST_CASE_METHOD(GuidedFlowFixture, "a keypad value for an action disabled meanwhile sends nothing",
                 "[ams][device-section]") {
    using helix::ui::AmsDeviceSectionDetailOverlayTestAccess;
    DeviceAction len = DeviceAction::slider("len", "Length", "prefs", 50.0f, 0.0f, 100.0f, "mm");
    open_prefs({len});
    auto& overlay = get_ams_device_section_detail_overlay();
    AmsDeviceSectionDetailOverlayTestAccess::open_keypad_for(overlay, "len");

    // While the keypad is up the backend disables it, and the section rebuilds.
    len.enabled = false;
    len.disable_reason = "Loading configuration...";
    backend->set_device_actions({len});
    overlay.refresh();
    settle();

    AmsDeviceSectionDetailOverlayTestAccess::keypad_value(42.0f);
    settle();
    CHECK(last_executed().empty());
}

TEST_CASE_METHOD(GuidedFlowFixture, "a keypad value for an available action is sent",
                 "[ams][device-section]") {
    using helix::ui::AmsDeviceSectionDetailOverlayTestAccess;
    open_prefs({DeviceAction::slider("len", "Length", "prefs", 50.0f, 0.0f, 100.0f, "mm")});
    AmsDeviceSectionDetailOverlayTestAccess::open_keypad_for(
        get_ams_device_section_detail_overlay(), "len");
    AmsDeviceSectionDetailOverlayTestAccess::keypad_value(42.0f);
    settle();
    CHECK(last_executed() == "len");
}

TEST_CASE_METHOD(GuidedFlowFixture, "the device button disable expressions parse and evaluate",
                 "[ams][device-section]") {
    auto resolve = [](void*, const char* name) { return lv_xml_get_subject(nullptr, name); };
    auto eval = [&](const std::string& src) {
        lv_xml_expr_t* expr = lv_xml_expr_compile(src.c_str(), resolve, nullptr);
        REQUIRE(expr != nullptr);
        const int32_t v = lv_xml_expr_eval(expr);
        lv_xml_expr_free(expr);
        return v;
    };

    const std::string never =
        helix::ui::device_action_disabled_cond(DeviceAction::button("b", "B", "s"));
    CHECK(never == "0 eq 1");
    const std::string paused_ok = helix::ui::device_action_disabled_cond(
        DeviceAction::button("b", "B", "s").with_machine_motion(true));

    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    settle();
    CHECK(eval(never) == 0);
    CHECK(eval(paused_ok) == 0);
    helix::test::set_wire_state(state(), PrintJobState::PRINTING);
    settle();
    CHECK(eval(never) == 0);
    CHECK(eval(paused_ok) != 0);
    helix::test::set_wire_state(state(), PrintJobState::PAUSED);
    settle();
    CHECK(eval(paused_ok) == 0);
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    settle();
}

TEST_CASE_METHOD(GuidedFlowFixture, "a dropdown with no value it lists shows a placeholder",
                 "[ams][device-section]") {
    DeviceAction unknown =
        DeviceAction::dropdown("speed", "Speed", "prefs", {"Stealth", "Normal"}, std::any{});
    DeviceAction unmatched = DeviceAction::dropdown(
        "other", "Other", "prefs", {"Stealth", "Normal"}, std::string("ludicrous"));
    DeviceAction known = DeviceAction::dropdown("known", "Known", "prefs", {"Stealth", "Normal"},
                                                std::string("Stealth"));
    lv_obj_t* root = open_prefs({unknown, unmatched, known});

    for (const char* id : {"speed", "other"}) {
        INFO(id);
        lv_obj_t* d = helix::test::find_device_action_control(root, id, &lv_dropdown_class);
        REQUIRE(d != nullptr);
        REQUIRE(lv_dropdown_get_text(d) != nullptr);
        CHECK(std::string(lv_dropdown_get_text(d)) == "-");
    }
    lv_obj_t* k = helix::test::find_device_action_control(root, "known", &lv_dropdown_class);
    REQUIRE(k != nullptr);
    CHECK(lv_dropdown_get_text(k) == nullptr);
    CHECK(lv_dropdown_get_selected(k) == 0);

    // Picking the first option, which the placeholder had hidden, sends it and shows it.
    lv_obj_t* d = helix::test::find_device_action_control(root, "speed", &lv_dropdown_class);
    lv_dropdown_set_selected(d, 0);
    lv_obj_send_event(d, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK(lv_dropdown_get_text(d) == nullptr);
    CHECK(last_executed() == "speed");
}

namespace {
/// A dropdown showing "0.4 mm" whose "0.2 mm" pick asks first.
DeviceAction confirmed_dropdown(const char* shown = "0.4 mm") {
    return DeviceAction::dropdown("nozzle", "Nozzle diameter", "prefs",
                                  {"0.2 mm", "0.4 mm", "0.6 mm"}, std::string(shown))
        .with_option_confirm("0.2 mm", "Fine nozzles refuse some filaments.", "Change");
}
void pick_option(lv_obj_t* dropdown, uint32_t index) {
    lv_dropdown_set_selected(dropdown, index);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
}
} // namespace

TEST_CASE_METHOD(GuidedFlowFixture, "a dropdown option that needs a yes is sent only after one",
                 "[ams][device-section][nozzle]") {
    lv_obj_t* root = open_prefs({confirmed_dropdown()});
    lv_obj_t* d = helix::test::find_device_action_control(root, "nozzle", &lv_dropdown_class);
    REQUIRE(d != nullptr);

    pick_option(d, 0);
    settle();
    CHECK(last_executed().empty());
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK(helix::test::top_modal_text().find("Fine nozzles refuse some filaments.") !=
          std::string::npos);

    helix::test::answer_top_modal("btn_primary");
    settle();
    CHECK(last_executed() == "nozzle");
    CHECK(std::any_cast<std::string>(backend->get_last_executed_action().second) == "0.2 mm");
    d = helix::test::find_device_action_control(get_ams_device_section_detail_overlay().get_root(),
                                                "nozzle", &lv_dropdown_class);
    REQUIRE(d != nullptr);
    CHECK(lv_dropdown_get_selected(d) == 0);
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "a cancelled or dismissed confirmation sends nothing and the row goes back",
                 "[ams][device-section][nozzle]") {
    const bool dismiss = GENERATE(false, true);
    CAPTURE(dismiss);
    lv_obj_t* root = open_prefs({confirmed_dropdown()});
    lv_obj_t* d = helix::test::find_device_action_control(root, "nozzle", &lv_dropdown_class);
    REQUIRE(d != nullptr);

    pick_option(d, 0);
    settle();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    if (dismiss) {
        helix::test::dismiss_top_modal();
    } else {
        helix::test::answer_top_modal("btn_secondary");
    }
    settle();
    CHECK(last_executed().empty());
    d = helix::test::find_device_action_control(get_ams_device_section_detail_overlay().get_root(),
                                                "nozzle", &lv_dropdown_class);
    REQUIRE(d != nullptr);
    CHECK(lv_dropdown_get_selected(d) == 1);
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "a confirmed pick for an action disabled meanwhile sends nothing",
                 "[ams][device-section][nozzle]") {
    lv_obj_t* root = open_prefs({confirmed_dropdown()});
    lv_obj_t* d = helix::test::find_device_action_control(root, "nozzle", &lv_dropdown_class);
    REQUIRE(d != nullptr);
    pick_option(d, 0);
    settle();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    // While the question is up a print starts and the backend disables the row.
    DeviceAction locked = confirmed_dropdown();
    locked.enabled = false;
    locked.disable_reason = "Finish or cancel the print first";
    backend->set_device_actions({locked});
    get_ams_device_section_detail_overlay().refresh();
    settle();

    helix::test::answer_top_modal("btn_primary");
    settle();
    CHECK(last_executed().empty());
}

TEST_CASE_METHOD(GuidedFlowFixture,
                 "re-picking the value a row shows, or a plain option, asks nothing",
                 "[ams][device-section][nozzle]") {
    SECTION("the shown value") {
        lv_obj_t* root = open_prefs({confirmed_dropdown("0.2 mm")});
        lv_obj_t* d = helix::test::find_device_action_control(root, "nozzle", &lv_dropdown_class);
        REQUIRE(d != nullptr);
        pick_option(d, 0);
    }
    SECTION("an option with no question") {
        lv_obj_t* root = open_prefs({confirmed_dropdown()});
        lv_obj_t* d = helix::test::find_device_action_control(root, "nozzle", &lv_dropdown_class);
        REQUIRE(d != nullptr);
        pick_option(d, 2);
    }
    settle();
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK(last_executed() == "nozzle");
}

TEST_CASE("the built-in registrar registers every vendor's flows", "[ams][guided-flow]") {
    std::vector<std::string> expected = {helix::indx::kSeatByHandFlowId, "indx_cycle_all",
                                         "indx_heater_calibration", "indx_fan_calibration"};
#if HELIX_HAS_CFS
    expected.insert(expected.end(), {"cfs_purge_chute", "cfs_cutter"});
#endif
#if HELIX_HAS_SNAPMAKER
    expected.push_back(helix::snapmaker::nozzle_offset::kGuidedFlowId);
#endif
    for (const auto& id : expected) {
        helix::ui::unregister_guided_device_flow(id);
    }

    helix::ui::register_builtin_guided_device_flows();
    for (const auto& id : expected) {
        INFO(id);
        CHECK(helix::ui::find_guided_device_flow(id) != nullptr);
    }

    // Other cases start from an empty registry.
    for (const auto& id : expected) {
        helix::ui::unregister_guided_device_flow(id);
    }
}
