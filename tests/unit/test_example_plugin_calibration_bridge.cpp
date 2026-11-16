// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_modal.h"
#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "locale_formats.h"
#include "lodepng_encode.h"
#include "panel_widget_registry.h"
#include "plugin_host.h"
#include "plugin_menu.h"
#include "plugin_xml_policy.h"
#include "translation_loader.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

json cb_block(const json& settings) {
    json block = enabled("calibration-bridge", {"http", "moonraker_write"});
    // auto_open off by default in tests: most cases assert the state machine,
    // not the overlay push.
    json s{{"auto_open", false}};
    s.merge_patch(settings);
    block["settings"]["calibration-bridge"] = s;
    return block;
}

/// One available event, shaped as the companion posts it.
json available_event(const char* wizard = "temp_tower") {
    return json{{"wizard", wizard},
                {"filament", "Generic PLA - Voron Rapido"},
                {"printer", "Voron 2.4 300"},
                {"nozzle_mm", 0.4},
                {"current", 220},
                {"choices", json::array({190, 195, 200, 205, 210, 215, 220, 225, 230})},
                {"file", "cal_temp_tower.gcode"}};
}

/// Delivers one agent event through every captured notify_agent_event
/// subscription; the binding filters by event name.
void fire_agent_event(HostRig& rig, const char* event, const json& data) {
    for (auto& [name, handler] : rig.fake.notify)
        if (name == "notify_agent_event")
            handler(
                json{{"params",
                      json::array({json{{"event", event}, {"agent", "orca"}, {"data", data}}})}});
    drain();
}

void set_print_state(const char* v) {
    lv_subject_set_string(lv_xml_get_subject(nullptr, "print_state"), v);
    drain();
}

std::string cb_str(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}

int cb_int(const char* name) {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
}

/// Answers every http request since `next` with `status`.
void answer_http(HostRig& rig, size_t& next, int status) {
    while (next < rig.fake.requests.size()) {
        auto& r = rig.fake.requests[next++];
        if (r.kind == "http")
            r.reply(RpcResult{true, json{{"status", status}, {"body", ""}}, {}});
    }
    drain();
}

size_t http_count(HostRig& rig) {
    size_t n = 0;
    for (const auto& r : rig.fake.requests)
        if (r.kind == "http")
            ++n;
    return n;
}

/// Panels seeded the way the app does, so a pushed plugin overlay resolves
/// its base panel (an unregistered push aborts under HELIX_STRICT_OVERLAY_CHECK).
class CalibrationFx : public XMLTestFixture {
  public:
    std::array<lv_obj_t*, kUiPanelCount> panels{};

    CalibrationFx() {
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        // The result dialog is built from the app's dialog chrome.
        REQUIRE(register_component("divider_vertical"));
        REQUIRE(register_component("modal_button_row"));
    }
    ~CalibrationFx() override {
        NavigationManager::instance().go_back();
        drain();
        for (auto& p : panels)
            lv_obj_delete(p);
        drain();
    }
};

/// Available, started, and printed to completion: the asking state.
void run_to_asking(HostRig& rig) {
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    set_print_state("printing");
    set_print_state("complete");
    REQUIRE(cb_int("calibration-bridge__state") == 3);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "calibration-bridge loads as an overlay-only plugin",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.info("calibration-bridge"));
    CHECK(rig.info("calibration-bridge")->status == PluginStatus::Loaded);
    // No widget: the overlay is the whole UI, so nothing lands in the registry.
    CHECK(helix::find_widget_def("calibration-bridge__tile") == nullptr);

    for (const char* stem : {"calibration-bridge__ask", "calibration-bridge__result"}) {
        const std::string path =
            std::string("examples/plugins/calibration-bridge/ui/") + stem + ".xml";
        std::ifstream in(path);
        REQUIRE(in);
        std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(check_plugin_xml("calibration-bridge",
                               {"calibration-bridge__ask", "calibration-bridge__result"},
                               xml) == std::string());
    }
    CHECK(cb_int("calibration-bridge__state") == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "an available event fills the context and arms Start",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    CHECK(cb_int("calibration-bridge__state") == 1);
    CHECK(cb_str("calibration-bridge__state_title") == "Temperature tower");
    CHECK(cb_str("calibration-bridge__ctx_printer") == "Voron 2.4 300 · nozzle 0.4");
    CHECK(cb_str("calibration-bridge__ctx_filament") == "Generic PLA - Voron Rapido");
    CHECK(cb_str("calibration-bridge__ctx_current") == "Profile: 220°C");
    CHECK(cb_int("calibration-bridge__start_on") == 1);
    CHECK(http_count(rig) == 0);

    // Start reaches Moonraker with the event's file; a 2xx reply arms the
    // printing state.
    rig.host->dispatch_event("calibration-bridge__start");
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].a == "printer.print.start");
    CHECK(rig.fake.requests[0].params == json{{"filename", "cal_temp_tower.gcode"}});
    CHECK(cb_int("calibration-bridge__state") == 1); // still available until the reply lands
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    CHECK(cb_int("calibration-bridge__state") == 2);
}

TEST_CASE_METHOD(XMLTestFixture, "print completion hands over to the picker", "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    REQUIRE(cb_int("calibration-bridge__state") == 2);

    set_print_state("printing");
    set_print_state("complete");
    CHECK(cb_int("calibration-bridge__state") == 3);
    CHECK(cb_int("calibration-bridge__pick_on") == 1);
    CHECK(cb_int("calibration-bridge__result_on") == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "a cancelled calibration print returns to available",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();

    set_print_state("printing");
    set_print_state("cancelled");
    CHECK(cb_int("calibration-bridge__state") == 1);
    CHECK(cb_str("calibration-bridge__status_line") == "print canceled");
    CHECK(cb_int("calibration-bridge__start_on") == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "accept posts the chosen value back to the companion",
                 "[plugin][example]") {
    HostRig rig(cb_block(json{{"companion_url", "https://orca-pc:8732/helixcal"}}));
    rig.host->load_from("examples/plugins");
    run_to_asking(rig);
    // The dropdown's bind_value writes LVGL's 0-based selection: 3 is the
    // fourth option, 205 degrees.
    lv_subject_set_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx"), 3);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();

    REQUIRE(http_count(rig) == 1);
    const FakeBackend::Request* post = nullptr;
    for (const auto& q : rig.fake.requests)
        if (q.kind == "http")
            post = &q;
    REQUIRE(post);
    CHECK(post->b == "https://orca-pc:8732/helixcal");
    CHECK(post->params.value("Content-Type", "") == "application/json");
    const json body = json::parse(post->c);
    CHECK(body.value("event", "") == "chosen");
    CHECK(body.value("wizard", "") == "temp_tower");
    CHECK(body.value("value", 0) == 205);
    CHECK(body.value("current", 0) == 220);
    CHECK(cb_int("calibration-bridge__state") == 4);
    CHECK(cb_str("calibration-bridge__result_value") == "205°C");

    size_t next = 0;
    answer_http(rig, next, 200);
    CHECK(cb_str("calibration-bridge__result_sub") == "sent to the companion");
}

TEST_CASE_METHOD(XMLTestFixture, "without a companion URL the result stays local",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    run_to_asking(rig);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx"), 3);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();
    CHECK(http_count(rig) == 0);
    CHECK(cb_int("calibration-bridge__state") == 4);
    CHECK(cb_str("calibration-bridge__result_sub") == "(no companion URL set)");
}

TEST_CASE_METHOD(XMLTestFixture, "a cancel event resets to idle", "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    REQUIRE(cb_int("calibration-bridge__state") == 1);
    fire_agent_event(rig, "helixcal.cancel", json::object());
    CHECK(cb_int("calibration-bridge__state") == 0);
    CHECK(cb_str("calibration-bridge__state_title") == "No calibration running");
    CHECK(cb_str("calibration-bridge__ctx_filament") == "");
}

TEST_CASE_METHOD(XMLTestFixture, "a malformed available event is a status line",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", json{{"wizard", "temp_tower"}});
    CHECK(cb_int("calibration-bridge__state") == 0);
    CHECK(cb_str("calibration-bridge__status_line") == "bad event");
}

TEST_CASE_METHOD(XMLTestFixture, "a pa pattern formats and posts two decimals",
                 "[plugin][example]") {
    json ev = available_event("pa_pattern");
    ev["choices"] = json::array({0.4, 0.45, 0.5, 0.55});
    ev["current"] = 0.6;
    HostRig rig(cb_block(json{{"companion_url", "https://orca-pc:8732/helixcal"}}));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", ev);
    CHECK(cb_str("calibration-bridge__state_title") == "PA pattern");
    CHECK(cb_str("calibration-bridge__ctx_current") == "Profile: 0.60");
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    set_print_state("printing");
    set_print_state("complete");
    lv_subject_set_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx"), 1);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();
    CHECK(cb_str("calibration-bridge__result_value") == "0.45");
    const FakeBackend::Request* post = nullptr;
    for (const auto& q : rig.fake.requests)
        if (q.kind == "http")
            post = &q;
    REQUIRE(post);
    CHECK(json::parse(post->c).value("value", 0.0) == Approx(0.45));
}

TEST_CASE_METHOD(XMLTestFixture, "a pause is not the end of the calibration print",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();

    set_print_state("printing");
    set_print_state("paused");
    CHECK(cb_int("calibration-bridge__state") == 2); // still printing, no failure
    CHECK(cb_str("calibration-bridge__status_line") == "");
    set_print_state("printing"); // resumed
    set_print_state("complete");
    CHECK(cb_int("calibration-bridge__state") == 3);
}

TEST_CASE_METHOD(XMLTestFixture, "a double tap of Start issues one print start",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    rig.host->dispatch_event("calibration-bridge__start"); // before the reply
    size_t starts = 0;
    for (const auto& r : rig.fake.requests)
        if (r.a == "printer.print.start")
            ++starts;
    CHECK(starts == 1);
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    CHECK(cb_int("calibration-bridge__state") == 2);
}

TEST_CASE_METHOD(XMLTestFixture, "a failed POST is a note, not a fault", "[plugin][example]") {
    HostRig rig(cb_block(json{{"companion_url", "https://orca-pc:8732/helixcal"}}));
    rig.host->load_from("examples/plugins");
    run_to_asking(rig);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx"), 3);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();
    size_t next = 0;
    answer_http(rig, next, 500);
    CHECK(cb_str("calibration-bridge__result_sub") == "HTTP 500");
    CHECK(rig.info("calibration-bridge")->status == PluginStatus::Loaded);
}

TEST_CASE_METHOD(XMLTestFixture, "accept without a selection or a print is a no-op",
                 "[plugin][example]") {
    HostRig rig(cb_block(json{{"companion_url", "https://orca-pc:8732/helixcal"}}));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    // The state is available, not asking: accept has nothing to answer.
    lv_subject_set_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx"), 0);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();
    rig.host->dispatch_event("calibration-bridge__accept"); // still no-op
    drain();
    CHECK(http_count(rig) == 0);
    CHECK(cb_int("calibration-bridge__state") == 1);

    // Out-of-range selection (a choice list that shrank): still nothing posted.
    run_to_asking(rig);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx"), 99);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();
    CHECK(http_count(rig) == 0);
    CHECK(cb_int("calibration-bridge__state") == 3);
}

namespace {
/// The picker dropdown of the result dialog on screen, or nullptr.
lv_obj_t* live_picker() {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    if (!dialog || ModalStack::instance().top_visible_tag() != "calibration-bridge")
        return nullptr;
    return lv_obj_find_by_name(dialog, "calibration-bridge__pick");
}
} // namespace

TEST_CASE_METHOD(CalibrationFx, "the result dialog carries the choices and the pick",
                 "[plugin][example]") {
    HostRig rig(
        cb_block(json{{"auto_open", true}, {"companion_url", "https://orca-pc:8732/helixcal"}}));
    REQUIRE(register_component("header_bar"));
    rig.host->load_from("examples/plugins");
    run_to_asking(rig);
    process_lvgl(600);
    drain();

    lv_obj_t* dd = live_picker();
    REQUIRE(dd);
    CHECK(std::string(lv_dropdown_get_options(dd)).rfind("190\u00B0C\n195\u00B0C", 0) == 0);
    CHECK(lv_dropdown_get_selected(dd) == 0); // first option pre-selected

    // The real widget path: selecting drives bind_value onto the subject, and
    // accept posts the value behind the label the user read.
    lv_dropdown_set_selected(dd, 3); // 0-based: the fourth option, 205
    // A tap on the list selects and then sends VALUE_CHANGED; the library
    // call alone does not, and it is the event bind_value listens on.
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
    drain();
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx")) == 3);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();
    REQUIRE(http_count(rig) == 1); // requests[0] is the print start
    for (const auto& q : rig.fake.requests)
        if (q.kind == "http")
            CHECK(json::parse(q.c).value("value", 0) == 205);
}

TEST_CASE_METHOD(CalibrationFx, "auto_ask at print completion leaves the picker open",
                 "[plugin][example]") {
    HostRig rig(cb_block(json{{"auto_open", true}}));
    REQUIRE(register_component("header_bar"));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    process_lvgl(600);
    CHECK_FALSE(live_picker()); // nothing to answer yet

    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    set_print_state("printing");
    set_print_state("complete");
    process_lvgl(600);
    drain();
    REQUIRE(cb_int("calibration-bridge__state") == 3);
    lv_obj_t* dd = live_picker();
    REQUIRE(dd);
    CHECK(std::string(lv_dropdown_get_options(dd)).rfind("190\u00B0C\n195\u00B0C", 0) == 0);
}

TEST_CASE_METHOD(CalibrationFx,
                 "completion opens the result dialog behind the stock completion modal",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    lv_obj_t* app = Modal::show("print_cancel_confirm_modal"); // stands in for print completion
    REQUIRE(app != nullptr);
    run_to_asking(rig);
    process_lvgl(600);
    CHECK_FALSE(live_picker());
    CHECK(ModalStack::instance().top_dialog() == app);

    Modal::hide(app);
    drain();
    process_lvgl(600);
    CHECK(live_picker());
}

TEST_CASE_METHOD(CalibrationFx, "accept posts the choice and closes the result dialog",
                 "[plugin][example]") {
    HostRig rig(cb_block(json{{"companion_url", "https://orca-pc:8732/helixcal"}}));
    rig.host->load_from("examples/plugins");
    run_to_asking(rig);
    process_lvgl(600);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(live_picker());
    lv_obj_t* use = lv_obj_find_by_name(dialog, "calibration-bridge__use");
    REQUIRE(use);
    lv_obj_send_event(use, LV_EVENT_CLICKED, nullptr);
    drain();
    process_lvgl(600);
    CHECK(cb_int("calibration-bridge__state") == 4);
    CHECK(http_count(rig) == 1);
    CHECK_FALSE(live_picker());
}

TEST_CASE_METHOD(CalibrationFx, "dismissing the result dialog keeps the result askable",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    run_to_asking(rig);
    process_lvgl(600);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(live_picker());
    lv_obj_send_event(ModalStack::instance().backdrop_for(dialog), LV_EVENT_CLICKED, nullptr);
    drain();
    process_lvgl(600);
    CHECK_FALSE(live_picker());
    CHECK(cb_int("calibration-bridge__state") == 3);

    rig.host->dispatch_event("calibration-bridge__open_result");
    process_lvgl(600);
    CHECK(live_picker());
}

TEST_CASE_METHOD(CalibrationFx, "a reset calibration withdraws a queued result dialog",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    lv_obj_t* app = Modal::show("print_cancel_confirm_modal");
    run_to_asking(rig); // the result dialog is queued behind the app modal
    fire_agent_event(rig, "helixcal.cancel", json::object());
    REQUIRE(cb_int("calibration-bridge__state") == 0);

    Modal::hide(app);
    drain();
    process_lvgl(600);
    CHECK_FALSE(live_picker());
}

namespace {
/// The calibration is started and its print is running (state 2).
void run_to_printing(HostRig& rig) {
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    REQUIRE(cb_int("calibration-bridge__state") == 2);
}

size_t call_count(HostRig& rig, const char* method) {
    size_t n = 0;
    for (const auto& r : rig.fake.requests)
        if (r.a == method)
            ++n;
    return n;
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "the print file is validated and shown on the card",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    for (const char* bad :
         {"../etc/x.gcode", "/abs/x.gcode", "x.txt", "a\nb.gcode", "dir\\x.gcode", "x.gcode.sh"}) {
        CAPTURE(bad);
        json ev = available_event();
        ev["file"] = bad;
        fire_agent_event(rig, "helixcal.available", ev);
        CHECK(cb_int("calibration-bridge__state") == 0);
        CHECK(cb_str("calibration-bridge__status_line") == "bad event");
    }
    json ev = available_event();
    ev["file"] = "calibration/Tower v2.GCODE";
    fire_agent_event(rig, "helixcal.available", ev);
    CHECK(cb_int("calibration-bridge__state") == 1);
    CHECK(cb_str("calibration-bridge__ctx_file") == "calibration/Tower v2.GCODE");
}

TEST_CASE_METHOD(XMLTestFixture, "choices that cannot be printed or told apart are refused",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    auto refused = [&](const char* wizard, json choices) {
        json ev = available_event(wizard);
        ev["choices"] = std::move(choices);
        ev["current"] = 1;
        fire_agent_event(rig, "helixcal.available", ev);
        CHECK(cb_int("calibration-bridge__state") == 0);
    };
    refused("temp_tower", json::array({1e300, 2})); // %d has no integer for it
    refused("flow_rate", json::array({95, 1e19}));
    refused("temp_tower", json::array({200, 200, 210}));     // two identical labels
    refused("pa_pattern", json::array({0.1234567891, 0.5})); // no format keeps it exact
    refused("pa_pattern", json::array({0.5, 0.5}));
}

TEST_CASE_METHOD(XMLTestFixture, "pa pattern labels use the decimals the choices need",
                 "[plugin][example]") {
    json ev = available_event("pa_pattern");
    ev["choices"] = json::array({0.0040, 0.0041, 0.0042});
    ev["current"] = 0.004;
    HostRig rig(cb_block(json{{"companion_url", "https://orca-pc:8732/helixcal"}}));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", ev);
    REQUIRE(cb_int("calibration-bridge__state") == 1);
    CHECK(cb_str("calibration-bridge__ctx_current") == "Profile: 0.0040");
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    set_print_state("printing");
    set_print_state("complete");
    lv_subject_set_int(lv_xml_get_subject(nullptr, "calibration-bridge__choice_idx"), 1);
    rig.host->dispatch_event("calibration-bridge__accept");
    drain();
    CHECK(cb_str("calibration-bridge__result_value") == "0.0041");
}

TEST_CASE_METHOD(XMLTestFixture, "a running calibration ignores a replacing available event",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    run_to_printing(rig);
    json ev = available_event("flow_rate");
    ev["choices"] = json::array({90, 95, 100});
    fire_agent_event(rig, "helixcal.available", ev);
    CHECK(cb_int("calibration-bridge__state") == 2);
    CHECK(cb_str("calibration-bridge__state_title") == "Temperature tower");

    set_print_state("printing");
    set_print_state("complete");
    REQUIRE(cb_int("calibration-bridge__state") == 3);
    fire_agent_event(rig, "helixcal.available", ev);
    CHECK(cb_int("calibration-bridge__state") == 3);
    CHECK(cb_str("calibration-bridge__state_title") == "Temperature tower");
}

TEST_CASE_METHOD(XMLTestFixture, "events arriving while the start is in flight change nothing",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    REQUIRE(rig.fake.requests.size() == 1);

    json ev = available_event("flow_rate");
    ev["choices"] = json::array({90, 95, 100});
    fire_agent_event(rig, "helixcal.available", ev);
    fire_agent_event(rig, "helixcal.cancel", json::object());
    rig.fake.requests[0].reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    CHECK(cb_int("calibration-bridge__state") == 2);
    CHECK(cb_str("calibration-bridge__state_title") == "Temperature tower");
}

TEST_CASE_METHOD(XMLTestFixture, "the cancel event never abandons a running print",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    run_to_printing(rig);
    fire_agent_event(rig, "helixcal.cancel", json::object());
    CHECK(cb_int("calibration-bridge__state") == 2);
    CHECK(call_count(rig, "printer.print.cancel") == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "a refused print start shows the reason", "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__start");
    rig.fake.requests[0].reply(RpcResult{false, json(), "Printer is not ready"});
    drain();
    CHECK(cb_int("calibration-bridge__state") == 1);
    CHECK(cb_str("calibration-bridge__status_line") == "print failed: Printer is not ready");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Cancel during a print confirms, then resets only on success",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    run_to_printing(rig);

    auto answer = [&](const char* button) {
        process_lvgl(50);
        drain();
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog);
        lv_obj_t* b = lv_obj_find_by_name(dialog, button);
        REQUIRE(b);
        lv_obj_send_event(b, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        drain();
    };

    // Declining the dialog leaves everything alone.
    rig.host->dispatch_event("calibration-bridge__cancel");
    CHECK(call_count(rig, "printer.print.cancel") == 0);
    answer("btn_secondary");
    CHECK(call_count(rig, "printer.print.cancel") == 0);
    CHECK(cb_int("calibration-bridge__state") == 2);

    // Confirming sends the cancel; a failure keeps the calibration and says so.
    rig.host->dispatch_event("calibration-bridge__cancel");
    answer("btn_primary");
    REQUIRE(call_count(rig, "printer.print.cancel") == 1);
    rig.fake.requests.back().reply(RpcResult{false, json(), "Klippy not ready"});
    drain();
    CHECK(cb_int("calibration-bridge__state") == 2);
    CHECK(cb_str("calibration-bridge__status_line") ==
          "could not cancel the print: Klippy not ready");

    // A cancel Moonraker accepts resets the calibration.
    rig.host->dispatch_event("calibration-bridge__cancel");
    answer("btn_primary");
    REQUIRE(call_count(rig, "printer.print.cancel") == 2);
    rig.fake.requests.back().reply(RpcResult{true, json{{"ok", true}}, {}});
    drain();
    CHECK(cb_int("calibration-bridge__state") == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "Cancel without a running print resets without asking",
                 "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    fire_agent_event(rig, "helixcal.available", available_event());
    rig.host->dispatch_event("calibration-bridge__cancel");
    CHECK(cb_int("calibration-bridge__state") == 0);
    CHECK(call_count(rig, "printer.print.cancel") == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "the state machine speaks German", "[plugin][example]") {
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    CHECK(cb_str("calibration-bridge__state_title") == "No calibration running");

    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");
    helix::ui::locale_set_language("de");
    CHECK(cb_str("calibration-bridge__state_title") == "Keine Kalibrierung aktiv");
    fire_agent_event(rig, "helixcal.available", json{{"wizard", "temp_tower"}});
    CHECK(cb_str("calibration-bridge__status_line") == "fehlerhaftes Ereignis");

    lv_translation_set_language("en");
    helix::ui::locale_set_language("en");
}

// The result dialog over the overlay, for eyes rather than assertions. Hidden;
// run on demand:
//   HELIX_SHOT_DIR=/tmp/out ./build/bin/helix-tests '[calibration_bridge_shot]'
TEST_CASE_METHOD(CalibrationFx, "calibration-bridge result dialog screenshot",
                 "[.][calibration_bridge_shot]") {
    const char* dir = std::getenv("HELIX_SHOT_DIR");
    // A screenshot for eyes: with nowhere to write, there is nothing to run.
    if (dir == nullptr) {
        SKIP("HELIX_SHOT_DIR is unset");
    }
    HostRig rig(cb_block(json{{"auto_open", true}}));
    REQUIRE(register_component("header_bar"));
    rig.host->load_from("examples/plugins");
    run_to_asking(rig);
    process_lvgl(800);
    drain();
    REQUIRE(ModalStack::instance().top_visible_tag() == "calibration-bridge");
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    const int w = static_cast<int>(snap->header.w);
    const int h = static_cast<int>(snap->header.h);
    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const auto* row =
            reinterpret_cast<const lv_color32_t*>(snap->data + y * snap->header.stride);
        for (int x = 0; x < w; ++x) {
            unsigned char* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            d[0] = row[x].red;
            d[1] = row[x].green;
            d[2] = row[x].blue;
            d[3] = 255;
        }
    }
    unsigned char* out = nullptr;
    size_t out_size = 0;
    REQUIRE(lodepng_encode32(&out, &out_size, rgba.data(), w, h) == 0);
    std::ofstream(std::string(dir) + "/calibration-bridge-result.png", std::ios::binary)
        .write(reinterpret_cast<char*>(out), static_cast<std::streamsize>(out_size));
    free(out);
    lv_draw_buf_destroy(snap);
}

TEST_CASE_METHOD(XMLTestFixture, "Calibrate with this file feeds the waiting calibration",
                 "[plugin][example]") {
    namespace pm = helix::plugin_menu;
    HostRig rig(cb_block(json::object()));
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.info("calibration-bridge")->status == PluginStatus::Loaded);
    const json printer_file{
        {"source", "printer"}, {"path", "cal/my_tower.gcode"}, {"name", "my_tower.gcode"}};
    const json usb_file{
        {"source", "usb"}, {"local_path", "/media/usb0/x.gcode"}, {"name", "x.gcode"}};

    // Nothing waiting: shown greyed with why.
    auto idle = pm::resolve(pm::MenuKind::PrintFile, printer_file);
    REQUIRE(idle.size() == 1);
    CHECK(idle[0].label == "Calibrate with this file");
    CHECK_FALSE(idle[0].enabled);
    CHECK(idle[0].reason == "Start a calibration in the slicer first");

    fire_agent_event(rig, "helixcal.available", available_event());
    auto usb = pm::resolve(pm::MenuKind::PrintFile, usb_file);
    REQUIRE(usb.size() == 1);
    CHECK_FALSE(usb[0].enabled);
    CHECK(usb[0].reason == "Copy the file to the printer first");

    auto ready = pm::resolve(pm::MenuKind::PrintFile, printer_file);
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].enabled);
    pm::activate(ready[0], printer_file);
    CHECK(cb_str("calibration-bridge__ctx_file") == "cal/my_tower.gcode");
    CHECK_FALSE(rig.host->runtime("calibration-bridge")->faulted());

    // Start prints the chosen file.
    rig.host->dispatch_event("calibration-bridge__start");
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].params == json{{"filename", "cal/my_tower.gcode"}});
}

#endif // HELIX_HAS_PLUGINS
