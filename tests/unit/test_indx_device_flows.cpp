// SPDX-License-Identifier: GPL-3.0-or-later

// The INDX guided device flows: each asks before it moves the printer, tracks
// the run on the section's status line, and offers the save a calibration
// needs only when the calibration answered success.

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_guided_device_flow.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_recording_api.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_indx.h"
#include "ams_state.h"
#include "app_globals.h"
#include "indx_contract.h"
#include "macro_param_cache.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// An INDX on a recording API, with a mounted tool and a cold nozzle.
class FlowIndx : public AmsBackendIndx {
  public:
    FlowIndx(IMoonrakerAPI* api, helix::IMoonrakerClient* client) : AmsBackendIndx(api, client) {
        running_ = true;
    }
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

  protected:
    [[nodiscard]] double nozzle_target_c() const override {
        return 0.0;
    }
};

PrinterDiscovery indx_discovery(const std::vector<std::string>& extra) {
    json objects =
        json::array({"indx", "save_variables", "extruder", "toolhead", "gcode_macro TOOL_POSITIONS",
                     "gcode_macro _TOOLCHANGE_VARS", "gcode_macro CHANGE_TOOL",
                     "gcode_macro PARK_TOOL", "gcode_macro LOAD_FILAMENT"});
    for (int i = 0; i < 4; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
    for (const auto& m : extra) {
        objects.push_back("gcode_macro " + m);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    const json config = {{"gcode_macro TOOL_POSITIONS", {{"variable_tool_count", "4"}}}};
    hw.parse_config_keys(config);
    MacroParamCache::instance().populate_from_configfile(config, hw.macros());
    return hw;
}

const std::vector<std::string> kFlowMacros = {"MODE_STEALTH",     "MODE_NORMAL",
                                              "MODE_SPORT",       "TC_CYCLE_ALL",
                                              "MANUAL_TOOL_SEAT", "MANUAL_TOOL_REMOVE"};

class IndxFlowFixture : public LVGLUITestFixture {
  public:
    IndxFlowFixture() {
        set_moonraker_api(&api);
        register_indx_flows_once();
        reset_flow_subjects();
        harness->set_discovery(indx_discovery(kFlowMacros));
        harness->feed(json{{"save_variables", {{"variables", {{"active_tool", 1}}}}}});
    }
    ~IndxFlowFixture() override {
        reset_flow_subjects();
        set_moonraker_api(nullptr);
        MacroParamCache::instance().clear();
    }

    /// The flows' status subjects are file statics: each case starts and ends idle.
    static void reset_flow_subjects() {
        if (lv_subject_t* phase = lv_xml_get_subject(nullptr, "indx_flow_phase")) {
            lv_subject_set_int(phase, 0);
        }
        if (lv_subject_t* status = lv_xml_get_subject(nullptr, "indx_flow_status")) {
            lv_subject_set_string(status, "");
        }
    }

    static void register_indx_flows_once() {
        ui::register_indx_device_flows();
    }

    void start(const char* flow_id) {
        const ui::GuidedDeviceFlow* flow = ui::find_guided_device_flow(flow_id);
        REQUIRE(flow != nullptr);
        REQUIRE(flow->start);
        flow->start("label");
        process_lvgl(20);
    }

    static int phase() {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "indx_flow_phase");
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
    static std::string status() {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "indx_flow_status");
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }
    void settle() {
        ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }
    [[nodiscard]] bool sent(const std::string& gcode) const {
        return std::find(api.sent.begin(), api.sent.end(), gcode) != api.sent.end();
    }

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_TRIDENT};
    test::GcodeRecordingApi api{client, get_printer_state()};
    test::RegisteredBackend<FlowIndx> harness{&api, &client};
};

} // namespace

TEST_CASE_METHOD(IndxFlowFixture,
                 "INDX flows: the heater calibration asks, runs, then offers the save",
                 "[indx][flows]") {
    api.hold_acks = true;
    start("indx_heater_calibration");
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK(api.sent.empty());

    test::answer_top_modal("btn_primary");
    settle();
    CHECK(sent("INDX_CALIBRATE"));
    CHECK(phase() == 1);

    api.ack_held();
    settle();
    CHECK(phase() == 2);
    CHECK_THAT(status(), Catch::Matchers::ContainsSubstring("not saved"));
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK_THAT(test::top_modal_text(), Catch::Matchers::ContainsSubstring("printer.cfg"));

    test::answer_top_modal("btn_primary");
    settle();
    CHECK(sent("SAVE_CONFIG"));
    api.hold_acks = false;
    api.ack_held();
    settle();
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: a timed-out calibration offers no save",
                 "[indx][flows]") {
    api.time_out = {"INDX_CALIBRATE"};
    start("indx_heater_calibration");
    test::answer_top_modal("btn_primary");
    settle();
    CHECK(sent("INDX_CALIBRATE"));
    CHECK(phase() == 2);
    CHECK_THAT(status(), Catch::Matchers::ContainsSubstring("may still be"));
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK_FALSE(sent("SAVE_CONFIG"));
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: cancelling the confirm sends nothing",
                 "[indx][flows]") {
    start("indx_cycle_all");
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    test::answer_top_modal("btn_secondary");
    settle();
    CHECK(api.sent.empty());

    start("indx_cycle_all");
    test::answer_top_modal("btn_primary");
    settle();
    CHECK(sent("TC_CYCLE_ALL"));
    CHECK(phase() == 2);
    CHECK_THAT(status(), Catch::Matchers::ContainsSubstring("finished"));
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: registered ids match the backend's actions",
                 "[indx][flows]") {
    int flows = 0;
    for (const auto& action : harness->get_device_actions()) {
        if (action.guided_flow.empty()) {
            continue;
        }
        INFO(action.id << " -> " << action.guided_flow);
        CHECK(ui::find_guided_device_flow(action.guided_flow) != nullptr);
        ++flows;
    }
    CHECK(flows >= 4);
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: seat by hand picks, asks, then seats",
                 "[indx][flows]") {
    auto pick = [this](int head) {
        start(indx::kSeatByHandFlowId);
        lv_obj_t* picker = ModalStack::instance().top_dialog();
        REQUIRE(picker != nullptr);
        lv_obj_t* row = lv_obj_find_by_name(picker, ("item_" + std::to_string(head)).c_str());
        REQUIRE(row != nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        test::answer_top_modal("btn_primary");
        process_lvgl(20);
    };

    pick(2);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK(test::top_modal_text().find(indx::seat_confirm_message(2)) != std::string::npos);
    CHECK(api.sent.empty());
    test::answer_top_modal("btn_secondary");
    settle();
    CHECK(api.sent.empty());

    pick(2);
    test::answer_top_modal("btn_primary");
    settle();
    CHECK(api.sent == std::vector<std::string>{"MANUAL_TOOL_SEAT TOOL=2"});
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: a backend replaced mid-run does not wedge the flows",
                 "[indx][flows]") {
    api.hold_acks = true;
    start("indx_heater_calibration");
    test::answer_top_modal("btn_primary");
    settle();
    REQUIRE(phase() == 1);

    // A printer switch replaces the backend; the old run's answer never comes.
    auto replacement = std::make_unique<FlowIndx>(&api, &client);
    FlowIndx* fresh = replacement.get();
    AmsState::instance().set_backend(std::move(replacement));
    fresh->set_discovery(indx_discovery(kFlowMacros));
    fresh->feed(json{{"save_variables", {{"variables", {{"active_tool", 1}}}}}});
    api.ack_held();
    settle();

    api.hold_acks = false;
    start("indx_cycle_all");
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    test::answer_top_modal("btn_primary");
    settle();
    CHECK(sent("TC_CYCLE_ALL"));
    CHECK(phase() == 2);
    CHECK_THAT(status(), Catch::Matchers::ContainsSubstring("finished"));
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: Save & Restart refuses under a print",
                 "[indx][flows]") {
    start("indx_heater_calibration");
    test::answer_top_modal("btn_primary");
    settle();
    REQUIRE(phase() == 2);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::PRINTING);
    test::answer_top_modal("btn_primary");
    settle();
    CHECK_FALSE(sent("SAVE_CONFIG"));
    CHECK_THAT(status(), Catch::Matchers::ContainsSubstring("not saved"));
    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: the running line survives the section rebuild",
                 "[indx][flows]") {
    // An open section rebuilds on every flow state change, and each rebuild
    // re-attaches the status line, which is where a running phase is judged.
    // Stand in for that rebuild: attach on every phase change.
    const ui::GuidedDeviceFlow* flow = ui::find_guided_device_flow("indx_heater_calibration");
    REQUIRE(flow != nullptr);
    REQUIRE(flow->attach_status);
    lv_obj_t* container = lv_obj_create(test_screen());
    flow->attach_status(container); // also creates the flow's subjects
    struct Rebuild {
        const ui::GuidedDeviceFlow* flow;
        lv_obj_t* container;
        lv_observer_t* observer = nullptr;
        ~Rebuild() {
            if (observer) {
                lv_observer_delete(observer);
            }
            lv_obj_delete(container);
        }
    } rebuild{flow, container};
    lv_subject_t* phase_subject = lv_xml_get_subject(nullptr, "indx_flow_phase");
    REQUIRE(phase_subject != nullptr);
    rebuild.observer = lv_subject_add_observer(
        phase_subject,
        [](lv_observer_t* o, lv_subject_t*) {
            // The line already exists, so this attaches nothing: deleting
            // widgets bound to the subject mid-notification is not safe, and
            // the judgment of the running phase is what is under test.
            auto* r = static_cast<Rebuild*>(lv_observer_get_user_data(o));
            r->flow->attach_status(r->container);
        },
        &rebuild);

    api.hold_acks = true;
    start("indx_heater_calibration");
    test::answer_top_modal("btn_primary");
    settle();
    CHECK(sent("INDX_CALIBRATE"));
    CHECK(phase() == 1);
    CHECK_THAT(status(), Catch::Matchers::ContainsSubstring("Calibrating"));

    api.ack_held();
    settle();
    CHECK(phase() == 2);
    test::answer_top_modal("btn_secondary"); // the save offer: Later
    settle();
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: the running line shows while the section is open",
                 "[indx][flows]") {
    // The real section overlay, rebuilt on every flow state change. AmsState's
    // subjects are initialized in place: deinit_subjects() would clear the
    // backends, freeing the one the fixture registered.
    ui::destroy_static_panels();
    settle();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    ams.sync_from_backend();
    auto& overlay = ui::get_ams_device_section_detail_overlay();
    overlay.init_subjects();
    overlay.show(test_screen(), "calibration", "Calibration");
    settle();

    api.hold_acks = true;
    start("indx_heater_calibration");
    test::answer_top_modal("btn_primary");
    settle();
    CHECK(sent("INDX_CALIBRATE"));
    CHECK(phase() == 1);
    CHECK_THAT(status(), Catch::Matchers::ContainsSubstring("Calibrating"));

    api.ack_held();
    settle();
    CHECK(phase() == 2);
    test::answer_top_modal("btn_secondary");
    settle();

    NavigationManager::instance().go_back();
    settle();
    ui::destroy_static_panels();
    settle();
}

TEST_CASE_METHOD(IndxFlowFixture, "INDX flows: the seat picker holds one tool", "[indx][flows]") {
    start(indx::kSeatByHandFlowId);
    lv_obj_t* picker = ModalStack::instance().top_dialog();
    REQUIRE(picker != nullptr);
    CHECK_THAT(test::top_modal_text(),
               Catch::Matchers::ContainsSubstring("Seat which tool by hand?"));
    lv_obj_t* first = lv_obj_find_by_name(picker, "item_1");
    lv_obj_t* second = lv_obj_find_by_name(picker, "item_2");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    const auto checked = [](lv_obj_t* row) {
        lv_obj_t* box = lv_obj_find_by_name(row, "check");
        REQUIRE(box != nullptr);
        return lv_obj_has_state(box, LV_STATE_CHECKED);
    };
    CHECK_FALSE(checked(first));
    CHECK_FALSE(checked(second));

    lv_obj_send_event(first, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(second, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(checked(first));
    CHECK(checked(second));

    test::answer_top_modal("btn_primary");
    process_lvgl(20);
    CHECK(test::top_modal_text().find(indx::seat_confirm_message(2)) != std::string::npos);
    test::answer_top_modal("btn_secondary");
    settle();
    CHECK(api.sent.empty());
}
