// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_manager_overlay.cpp
 * @brief Unit tests for PrinterManagerOverlay
 *
 * Tests subject initialization, lifecycle guards, and global accessor pattern.
 * Uses LVGLTestFixture for LVGL-dependent subject operations.
 *
 * @see ui_printer_manager_overlay.h
 */

#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_nav_manager.h"
#include "ui_overlay_retraction_settings.h"
#include "ui_panel_calibration_tool_offset.h"
#include "ui_printer_manager_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/printer_manager_overlay_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "accel_sensor_manager.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "printer_capabilities_state.h"
#include "printer_manager_chips.h"
#include "printer_state.h"
#include "subject_debug_registry.h"
#include "tool_state.h"
#include "width_sensor_manager.h"

#include <array>
#include <memory>
#include <string_view>

#include "../catch_amalgamated.hpp"

namespace {

/// Observer body is irrelevant - the test only cares that attaching the
/// observer to a widget installs an LV_EVENT_DELETE hook on that widget, and
/// that lv_subject_deinit() takes it back off again.
void sentinel_observer_cb(lv_observer_t*, lv_subject_t*) {}

/// One of the four subjects PrinterManagerOverlay::init_subjects() publishes.
constexpr const char* kOwnedSubjectName = "pm_name_editing";

} // namespace

// =============================================================================
// Basic Properties
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: get_name returns expected value",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE(std::string(overlay.get_name()) == "Printer Manager");
}

// =============================================================================
// Subject Initialization
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: init_subjects sets initialized flag",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;

    REQUIRE_FALSE(overlay.are_subjects_initialized());

    overlay.init_subjects();

    REQUIRE(overlay.are_subjects_initialized());
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: double init_subjects does not crash",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;

    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());

    // Second call should be a no-op (guarded)
    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());
}

// =============================================================================
// Global Accessor Pattern
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: global accessor returns valid reference",
                 "[printer_manager]") {
    PrinterManagerOverlay& overlay = get_printer_manager_overlay();
    REQUIRE(std::string(overlay.get_name()) == "Printer Manager");

    // Cleanup for other tests
    destroy_printer_manager_overlay();
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: global accessor returns same instance",
                 "[printer_manager]") {
    PrinterManagerOverlay& first = get_printer_manager_overlay();
    PrinterManagerOverlay& second = get_printer_manager_overlay();

    REQUIRE(&first == &second);

    destroy_printer_manager_overlay();
}

// =============================================================================
// Destructor / Cleanup
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterManagerOverlay: destructor cleans up initialized subjects",
                 "[printer_manager]") {
    // ~PrinterManagerOverlay() -> deinit_subjects_base() -> lv_subject_deinit()
    // on every subject the overlay published. lv_subject_deinit() walks the
    // subject's observer list and calls lv_observer_remove(), which for an
    // obj-bound observer strips the LV_EVENT_DELETE hook that
    // lv_subject_add_observer_obj() installed on the target widget.
    //
    // So a sentinel widget's event count is direct, checkable evidence: it goes
    // up by one when we observe the overlay's subject, and only comes back down
    // if the destructor actually deinitialized that subject. A destructor that
    // skips cleanup leaves the hook - and a dangling observer pointing at freed
    // subject memory - in place, with no crash to give it away.
    lv_obj_t* sentinel = lv_obj_create(test_screen());
    const uint32_t baseline_events = lv_obj_get_event_count(sentinel);
    lv_observer_t* observer = nullptr;

    {
        PrinterManagerOverlay overlay;
        overlay.init_subjects();
        REQUIRE(overlay.are_subjects_initialized());

        lv_subject_t* owned = SubjectDebugRegistry::instance().lookup_by_name(kOwnedSubjectName);
        REQUIRE(owned != nullptr);

        observer = lv_subject_add_observer_obj(owned, sentinel_observer_cb, sentinel, nullptr);
        REQUIRE(observer != nullptr);
        REQUIRE(lv_obj_get_event_count(sentinel) == baseline_events + 1);
        // Destructor runs here.
    }

    const uint32_t after_events = lv_obj_get_event_count(sentinel);

    if (after_events != baseline_events) {
        // Broken build: the hook still points at a freed observer/subject, so
        // deleting the sentinel would fault before Catch2 could report. Strip it
        // by user_data (a null cb means "any callback") and let the check below
        // do the reporting.
        lv_obj_remove_event_cb_with_user_data(sentinel, nullptr, observer);
    }
    lv_obj_delete(sentinel);

    REQUIRE(after_events == baseline_events);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: destructor safe without init_subjects",
                 "[printer_manager]") {
    // An overlay that never called init_subjects() owns nothing, so neither its
    // construction nor its destruction may disturb the subject another instance
    // published under the same XML name. Pin that with a live owner: the registry
    // entry must still resolve to the owner's subject afterwards, and the owner's
    // observer hook must still be attached.
    PrinterManagerOverlay owner;
    owner.init_subjects();
    lv_subject_t* owned = SubjectDebugRegistry::instance().lookup_by_name(kOwnedSubjectName);
    REQUIRE(owned != nullptr);

    lv_obj_t* sentinel = lv_obj_create(test_screen());
    const uint32_t baseline_events = lv_obj_get_event_count(sentinel);
    lv_observer_t* observer =
        lv_subject_add_observer_obj(owned, sentinel_observer_cb, sentinel, nullptr);
    REQUIRE(observer != nullptr);
    REQUIRE(lv_obj_get_event_count(sentinel) == baseline_events + 1);

    {
        PrinterManagerOverlay uninitialized;
        REQUIRE_FALSE(uninitialized.are_subjects_initialized());
        // Destructor runs here - it must be a no-op.
    }

    REQUIRE(SubjectDebugRegistry::instance().lookup_by_name(kOwnedSubjectName) == owned);
    REQUIRE(lv_obj_get_event_count(sentinel) == baseline_events + 1);

    // Owner is still alive; drop the observer before the sentinel so teardown
    // order cannot matter.
    lv_observer_remove(observer);
    lv_obj_delete(sentinel);
}

// =============================================================================
// Visibility / Lifecycle State
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: initially not visible",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE_FALSE(overlay.is_visible());
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: cleanup sets flag", "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE_FALSE(overlay.cleanup_called());

    overlay.cleanup();

    REQUIRE(overlay.cleanup_called());
}

// =============================================================================
// Overlay Root State
// =============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterManagerOverlay: root is null before create",
                 "[printer_manager]") {
    PrinterManagerOverlay overlay;
    REQUIRE(overlay.get_root() == nullptr);
}

// The edit box holds the printer's own name only: a host or type shown in its place is not
// a name, and confirming it would save it as one and push it to Mainsail/Fluidd.
TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterManagerOverlay: renaming starts from the saved name, never the fallback",
                 "[printer_manager][multi-printer]") {
    helix::Config* cfg = helix::Config::get_instance();
    const nlohmann::json saved = helix::ConfigTestAccess::data(*cfg);
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);
    nlohmann::json data = saved;
    data["printers"]["printer-2"] = {{"moonraker_host", "10.0.0.9"}};
    helix::ConfigTestAccess::data(*cfg) = data;
    helix::ConfigTestAccess::active_printer_id(*cfg) = "printer-2";
    const std::string name_key = "/printers/printer-2/printer_name";
    {
        PrinterManagerOverlay overlay;
        overlay.init_subjects();
        lv_obj_t* input = lv_textarea_create(test_screen());
        PrinterManagerOverlayTestAccess::set_name_input(overlay, input);
        PrinterManagerOverlayTestAccess::refresh(overlay);

        PrinterManagerOverlayTestAccess::start_name_edit(overlay);
        CHECK(std::string(lv_textarea_get_text(input)).empty());

        // Confirming an empty box writes nothing.
        PrinterManagerOverlayTestAccess::finish_name_edit(overlay);
        CHECK(cfg->get<std::string>(name_key, "") == "");

        PrinterManagerOverlayTestAccess::start_name_edit(overlay);
        lv_textarea_set_text(input, "Voron");
        PrinterManagerOverlayTestAccess::finish_name_edit(overlay);
        CHECK(cfg->get<std::string>(name_key, "") == "Voron");

        PrinterManagerOverlayTestAccess::start_name_edit(overlay);
        CHECK(std::string(lv_textarea_get_text(input)) == "Voron");
        PrinterManagerOverlayTestAccess::finish_name_edit(overlay);
        lv_obj_delete(input);
    }
    helix::ConfigTestAccess::data(*cfg) = saved;
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;
}

// =============================================================================
// Name editing: a tap anywhere outside the input commits
// =============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrinterManagerOverlay: a tap anywhere outside the name input ends the edit",
                 "[printer_manager]") {
    PrinterManagerOverlay& pm = get_printer_manager_overlay();
    pm.show(test_screen());
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(30);
    lv_obj_t* root = pm.get_root();
    REQUIRE(root != nullptr);
    lv_subject_t* editing = lv_xml_get_subject(nullptr, "pm_name_editing");
    REQUIRE(editing != nullptr);

    // The details column scrolls, so it is clickable itself and a tap on it, or
    // on anything inside it, only reaches the body if it bubbles.
    for (const char* target : {"overlay_content", "pm_details", "group_software", "pm_row_klipper",
                               "group_caps_motion", "pm_identity_card"}) {
        INFO("tap on " << target);
        PrinterManagerOverlayTestAccess::start_name_edit(pm);
        REQUIRE(lv_subject_get_int(editing) == 1);
        lv_obj_t* obj = lv_obj_find_by_name(root, target);
        REQUIRE(obj != nullptr);
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
        CHECK(lv_subject_get_int(editing) == 0);
    }

    // A tap on the input itself keeps editing.
    PrinterManagerOverlayTestAccess::start_name_edit(pm);
    lv_obj_send_event(lv_obj_find_by_name(root, "pm_printer_name_input"), LV_EVENT_CLICKED,
                      nullptr);
    CHECK(lv_subject_get_int(editing) == 1);

    destroy_printer_manager_overlay();
}

// =============================================================================
// Capability chips
// =============================================================================

namespace {

/// The overlay shown on the fixture's screen, with the fixture's API published
/// as the app's, so the chips read the discovery the test writes. The managers
/// whose counts the chips show are started for real, so the overlay's
/// observers attach to the subjects production publishes.
class CapabilityChipsFixture : public LVGLUITestFixture {
  public:
    CapabilityChipsFixture() : previous_api_(get_moonraker_api()) {
        set_moonraker_api(api());
        helix::AmsState::instance().init_subjects(true);
        // init_subjects() keeps the tools a live ToolState already holds.
        helix::ToolState::instance().deinit_subjects();
        helix::ToolState::instance().init_subjects(true);
        helix::FilamentSensorManager::instance().init_subjects();
        helix::sensors::WidthSensorManager::instance().init_subjects();
        helix::sensors::AccelSensorManager::instance().init_subjects();
    }
    ~CapabilityChipsFixture() override {
        ModalStack::instance().clear();
        destroy_printer_manager_overlay();
        helix::ui::UpdateQueue::instance().drain();
        helix::AmsState::instance().clear_backends();
        helix::sensors::AccelSensorManager::instance().deinit_subjects();
        helix::sensors::WidthSensorManager::instance().deinit_subjects();
        helix::FilamentSensorManager::instance().deinit_subjects();
        helix::ToolState::instance().deinit_subjects();
        helix::AmsState::instance().deinit_subjects();
        set_moonraker_api(previous_api_);
    }

    lv_obj_t* show() {
        PrinterManagerOverlay& pm = get_printer_manager_overlay();
        pm.show(test_screen());
        settle();
        return pm.get_root();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(30);
        if (lv_obj_t* root = get_printer_manager_overlay().get_root()) {
            lv_obj_update_layout(root);
        }
        process_lvgl(30);
    }

    void click(const char* chip) {
        lv_obj_t* obj = lv_obj_find_by_name(get_printer_manager_overlay().get_root(), chip);
        REQUIRE(obj != nullptr);
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
        settle();
    }

    /// Discovery as the app hands it over: the API's copy, PrinterState and ToolState.
    void discover(const nlohmann::json& objects) {
        api()->hardware().parse_objects(objects);
        state().set_hardware(api()->hardware());
        helix::ToolState::instance().init_tools(api()->hardware());
        helix::ui::UpdateQueue::instance().drain();
    }

    static std::string text_of(const char* subject_name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, subject_name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }

    static int int_of(const char* subject_name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, subject_name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }

  private:
    helix::IMoonrakerAPI* previous_api_;
};

void count_chips(lv_obj_t* obj, int& count) {
    const char* name = lv_obj_get_name(obj);
    if (name && std::string_view(name).rfind("pm_chip_", 0) == 0) {
        ++count;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        count_chips(lv_obj_get_child(obj, i), count);
    }
}

bool shown(lv_obj_t* obj) {
    return obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// The chips a group shows, named, so a failure says which one leaked.
std::string shown_chips(lv_obj_t* obj) {
    std::string out;
    const char* name = lv_obj_get_name(obj);
    if (shown(obj) && name && std::string_view(name).rfind("pm_chip_", 0) == 0) {
        out += std::string(name) + " ";
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        out += shown_chips(lv_obj_get_child(obj, i));
    }
    return out;
}

constexpr const char* kCapGroups[] = {"group_caps_motion",      "group_caps_thermal",
                                      "group_caps_filament",    "group_caps_printing",
                                      "group_caps_electronics", "group_caps_addons"};

/// Beta features pinned for one test, restored after.
struct ScopedBeta {
    explicit ScopedBeta(bool on)
        : previous_(helix::Config::get_instance()->is_beta_features_enabled()) {
        helix::Config::get_instance()->set<bool>("/beta_features", on);
    }
    ~ScopedBeta() {
        helix::Config::get_instance()->set<bool>("/beta_features", previous_);
    }
    bool previous_;
};

} // namespace

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: every chip in the layout has a spec and every spec a chip",
                 "[printer_manager]") {
    lv_obj_t* root = show();
    REQUIRE(root != nullptr);
    for (const auto& spec : helix::cap_chip_specs()) {
        INFO(spec.name);
        CHECK(lv_obj_find_by_name(root, spec.name) != nullptr);
        CHECK(helix::find_cap_chip_spec(spec.name) == &spec);
        CHECK(lv_xml_get_subject(nullptr, spec.text_subject) != nullptr);
    }
    int chips = 0;
    count_chips(root, chips);
    CHECK(chips == static_cast<int>(helix::kCapChipCount));
}

TEST_CASE_METHOD(
    CapabilityChipsFixture,
    "PrinterManagerOverlay: a group with no detected capability collapses, header and all",
    "[printer_manager]") {
    lv_obj_t* root = show();
    REQUIRE(root != nullptr);
    for (const char* group_name : kCapGroups) {
        INFO(group_name);
        lv_obj_t* group = lv_obj_find_by_name(root, group_name);
        REQUIRE(group != nullptr);
        INFO("shown: " << shown_chips(group));
        CHECK_FALSE(shown(lv_obj_find_by_name(group, "setting_group_header")));
        CHECK(lv_obj_get_height(group) == 0);
    }

    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_probe"), 1);
    settle();
    lv_obj_t* motion = lv_obj_find_by_name(root, "group_caps_motion");
    CHECK(shown(lv_obj_find_by_name(root, "pm_chip_probe")));
    CHECK(shown(lv_obj_find_by_name(motion, "setting_group_header")));
    CHECK(lv_obj_get_height(motion) > 0);
    lv_obj_t* thermal = lv_obj_find_by_name(root, "group_caps_thermal");
    CHECK(lv_obj_get_height(thermal) == 0);

    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_probe"), 0);
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: chip text follows the printer on every activation",
                 "[printer_manager]") {
    api()->hardware().set_kinematics("corexy");
    REQUIRE(show() != nullptr);
    CHECK(text_of("pm_chip_text_kinematics") == "CoreXY");
    CHECK(int_of("pm_has_kinematics") == 1);

    api()->hardware().set_kinematics("cartesian");
    get_printer_manager_overlay().on_activate();
    settle();
    CHECK(text_of("pm_chip_text_kinematics") == "Cartesian");

    api()->hardware().set_kinematics("");
    get_printer_manager_overlay().on_activate();
    settle();
    CHECK(int_of("pm_has_kinematics") == 0);
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: new discovery while open re-texts the chips",
                 "[printer_manager]") {
    api()->hardware().set_kinematics("corexy");
    REQUIRE(show() != nullptr);
    REQUIRE(text_of("pm_chip_text_kinematics") == "CoreXY");

    api()->hardware().set_kinematics("delta");
    state().capabilities_state().bump_hardware_generation();
    settle();
    CHECK(text_of("pm_chip_text_kinematics") == "Delta");
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: one activation computes the chips once",
                 "[printer_manager]") {
    REQUIRE(show() != nullptr);
    CHECK(PrinterManagerOverlayTestAccess::chip_refresh_count(get_printer_manager_overlay()) == 1);
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: destroying the UI stops watching the chip inputs",
                 "[printer_manager]") {
    REQUIRE(show() != nullptr);
    PrinterManagerOverlay& pm = get_printer_manager_overlay();
    const int before = PrinterManagerOverlayTestAccess::chip_refresh_count(pm);

    PrinterManagerOverlayTestAccess::destroy_ui(pm);
    state().capabilities_state().set_power_device_count(5);
    settle();

    CHECK(PrinterManagerOverlayTestAccess::chip_refresh_count(pm) == before);
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: a count that changes while open updates its chip",
                 "[printer_manager]") {
    auto& caps = state().capabilities_state();
    caps.set_power_device_count(2);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(show() != nullptr);
    CHECK(text_of("pm_chip_text_power") == "Power · 2");

    caps.set_power_device_count(3);
    settle();
    CHECK(text_of("pm_chip_text_power") == "Power · 3");
}

TEST_CASE_METHOD(
    CapabilityChipsFixture,
    "PrinterManagerOverlay: an override that adds QGL says so; one that removes it hides it",
    "[printer_manager]") {
    SECTION("ENABLE on an undetected QGL") {
        PrinterStateTestAccess::set_capability_override(state(), capability::QGL,
                                                        OverrideState::ENABLE);
        discover(nlohmann::json{"fan"});
        lv_obj_t* root = show();
        REQUIRE(root != nullptr);
        CHECK(shown(lv_obj_find_by_name(root, "pm_chip_qgl")));
        CHECK(text_of("pm_chip_text_qgl") == "QGL · set manually");
    }
    SECTION("DISABLE on a detected QGL") {
        PrinterStateTestAccess::set_capability_override(state(), capability::QGL,
                                                        OverrideState::DISABLE);
        discover(nlohmann::json{"quad_gantry_level"});
        lv_obj_t* root = show();
        REQUIRE(root != nullptr);
        CHECK_FALSE(shown(lv_obj_find_by_name(root, "pm_chip_qgl")));
    }
    PrinterStateTestAccess::set_capability_override(state(), capability::QGL, OverrideState::AUTO);
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: a chip with no screen opens its info popup",
                 "[printer_manager]") {
    REQUIRE(show() != nullptr);
    const helix::CapChipSpec* spec = helix::find_cap_chip_spec("pm_chip_qgl");
    REQUIRE(spec != nullptr);
    REQUIRE(spec->open == nullptr);
    REQUIRE(ModalStack::instance().empty());

    click("pm_chip_qgl");
    CHECK_FALSE(ModalStack::instance().empty());
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: the Tool Offsets chip opens its screen with beta off",
                 "[printer_manager]") {
    ScopedBeta beta(false);
    REQUIRE(show() != nullptr);
    const helix::CapChipSpec* spec = helix::find_cap_chip_spec("pm_chip_tool_offsets");
    REQUIRE(spec != nullptr);
    REQUIRE(spec->open != nullptr);

    click("pm_chip_tool_offsets");
    CHECK(ModalStack::instance().empty());
    lv_obj_t* root = helix::ui::get_global_tool_offset_cal_panel().get_root();
    REQUIRE(root != nullptr);
    CHECK(helix::nav::is_in_stack(root));
}

TEST_CASE_METHOD(CapabilityChipsFixture, "PrinterManagerOverlay: a chip pushes its screen",
                 "[printer_manager]") {
    REQUIRE(show() != nullptr);
    click("pm_chip_retraction");
    lv_obj_t* retraction = get_global_retraction_settings().get_root();
    REQUIRE(retraction != nullptr);
    CHECK(helix::nav::is_in_stack(retraction));
    CHECK(ModalStack::instance().empty());
}

TEST_CASE_METHOD(CapabilityChipsFixture, "PrinterManagerOverlay: a chip opens its modal",
                 "[printer_manager]") {
    REQUIRE(show() != nullptr);
    click("pm_chip_job_queue");
    CHECK(ModalStack::instance().top_component_name() == "job_queue_modal");
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: a chip whose screen is a main panel switches to it",
                 "[printer_manager]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(test_screen());
    }
    auto& nav = NavigationManager::instance();
    nav.set_panels(panels.data());
    nav.set_active(helix::PanelId::Home);
    REQUIRE(show() != nullptr);

    click("pm_chip_helix_macros");
    CHECK(nav.get_active() == helix::PanelId::Advanced);
    nav.set_active(helix::PanelId::Home);
}

TEST_CASE_METHOD(CapabilityChipsFixture,
                 "PrinterManagerOverlay: tool changer and multiple extruders",
                 "[printer_manager]") {
    auto& ams = helix::AmsState::instance();

    SECTION("a klipper-toolchanger beside a filament system shows both, with the head count") {
        discover(nlohmann::json{"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
                                "extruder", "extruder1", "extruder2", "extruder3"});
        auto mock = std::make_unique<helix::AmsBackendMock>(4);
        mock->set_operation_delay(0);
        ams.set_backend(std::move(mock));
        REQUIRE(show() != nullptr);
        CHECK(int_of("pm_has_tool_changer") == 1);
        CHECK(text_of("pm_chip_text_tool_changer") == "Tool Changer · 4 Tools");
        CHECK(int_of("pm_has_filament_system") == 1);
        CHECK_FALSE(text_of("pm_chip_text_filament_system").empty());
    }
    SECTION("two extruders and no tool changer are extruders") {
        discover(nlohmann::json{"extruder", "extruder1"});
        REQUIRE(show() != nullptr);
        CHECK(int_of("pm_has_tool_changer") == 1);
        CHECK(text_of("pm_chip_text_tool_changer") == "2 Extruders");
    }
    SECTION("a single extruder has no chip") {
        discover(nlohmann::json{"extruder"});
        REQUIRE(show() != nullptr);
        CHECK(int_of("pm_has_tool_changer") == 0);
    }
    SECTION("a tool changer backend alone is no multi-filament system") {
        auto mock = std::make_unique<helix::AmsBackendMock>(4);
        mock->set_operation_delay(0);
        mock->set_tool_changer_mode(true);
        ams.set_backend(std::move(mock));
        REQUIRE(show() != nullptr);
        CHECK(int_of("pm_has_tool_changer") == 1);
        CHECK(text_of("pm_chip_text_filament_system").empty());
        CHECK(int_of("pm_has_filament_system") == 0);
    }
}
