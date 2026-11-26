// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolhead_settings_overlay.cpp
 * @brief The Toolhead settings overlay: a card per physical head, its size dropdown on the
 *        size the head reports, and a pick that asks before it changes anything.
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_settings_toolhead.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "app_globals.h"
#include "core/http_executor.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "nozzle_size.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

struct ToolheadFixture : LVGLUITestFixture {
    ToolheadFixture() {
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels.data());
    }
    ~ToolheadFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        get_printer_state().temperature_state().init_extruders({"extruder"});
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
        helix::nozzle_size::set_size_command_state(helix::nozzle_size::CommandProbe::Unknown);
    }

    /// Opens the overlay on a printer with @p extruders whose config names the nozzle
    /// sizes in @p sizes ({"extruder": {"nozzle_diameter": 0.4}, ...}); the printer reports
    /// no live diameter, as stock Klipper does.
    void open(const std::vector<std::string>& extruders, const nlohmann::json& sizes) {
        auto& state = get_printer_state();
        // A printer whose command probe has not been run is a plain one: no size command.
        if (helix::nozzle_size::size_command_state() == helix::nozzle_size::CommandProbe::Unknown) {
            helix::nozzle_size::set_size_command_state(helix::nozzle_size::CommandProbe::Absent);
        }
        nlohmann::json settings = nlohmann::json::object();
        for (const auto& [name, section] : sizes.items()) {
            settings[name] = {{"max_temp", 300.0}, {"nozzle_diameter", section["nozzle_diameter"]}};
        }
        helix::PrinterDiscovery hw;
        hw.parse_fit_limits(settings);
        state.set_hardware(hw);
        state.temperature_state().init_extruders(extruders);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(helix::settings::get_toolhead_settings_overlay().show(lv_screen_active()));
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    static lv_obj_t* size_dropdown(const char* head) {
        lv_obj_t* card = lv_obj_find_by_name(lv_screen_active(), head);
        return card ? lv_obj_find_by_name(card, "dropdown") : nullptr;
    }
};

} // namespace

TEST_CASE_METHOD(ToolheadFixture, "toolhead settings: each head's dropdown shows its own size",
                 "[toolhead_settings][nozzle_size]") {
    open({"extruder", "extruder1"},
         {{"extruder", {{"nozzle_diameter", 0.4}}}, {"extruder1", {{"nozzle_diameter", 0.6}}}});
    lv_obj_t* head0 = size_dropdown("head_0");
    lv_obj_t* head1 = size_dropdown("head_1");
    REQUIRE((head0 && head1));
    char buf[32];
    lv_dropdown_get_selected_str(head0, buf, sizeof buf);
    CHECK(std::string(buf) == "0.4 mm");
    lv_dropdown_get_selected_str(head1, buf, sizeof buf);
    CHECK(std::string(buf) == "0.6 mm");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead settings: a pick asks about the head it was made on",
                 "[toolhead_settings][nozzle_size]") {
    open({"extruder", "extruder1"},
         {{"extruder", {{"nozzle_diameter", 0.4}}}, {"extruder1", {{"nozzle_diameter", 0.4}}}});

    const auto asked_about = [](const char* head) {
        lv_obj_t* dd = size_dropdown(head);
        REQUIRE(dd != nullptr);
        lv_dropdown_set_selected(dd, 5); // 0.6 mm
        lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(ModalStack::instance().top_dialog() != nullptr);
        const std::string text = helix::test::top_modal_text();
        helix::test::answer_top_modal("btn_secondary");
        helix::ui::UpdateQueue::instance().drain();
        return text;
    };
    const std::string first = asked_about("head_0");
    const std::string second = asked_about("head_1");
    // The question names the head: two heads, two different questions.
    CHECK(first != second);
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead settings: a flow type on a plain printer applies at once",
                 "[toolhead_settings][nozzle_size]") {
    open({"extruder"}, {{"extruder", {{"nozzle_diameter", 0.4}}}});
    lv_obj_t* card = lv_obj_find_by_name(lv_screen_active(), "head_0");
    REQUIRE(card != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(card, "flow_row");
    lv_obj_t* dd = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
    REQUIRE(dd != nullptr);
    // Nobody has said: the first entry is "Not set", which goes once a type is picked.
    REQUIRE(lv_dropdown_get_option_count(dd) == 3);
    REQUIRE(lv_dropdown_get_selected(dd) == 0);

    SettingsManager& settings = SettingsManager::instance();
    settings.clear_nozzle_flow(0);
    REQUIRE_FALSE(settings.get_nozzle_flow_set(0));
    lv_dropdown_set_selected(dd, 2); // High Flow
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK(settings.get_nozzle_high_flow(0));

    // Rebuilt after the pick: the "Not set" entry is gone and High Flow is selected.
    lv_obj_t* after =
        lv_obj_find_by_name(lv_obj_find_by_name(lv_screen_active(), "flow_row"), "dropdown");
    REQUIRE(after != nullptr);
    CHECK(lv_dropdown_get_option_count(after) == 2);
    CHECK(lv_dropdown_get_selected(after) == 1);
    settings.clear_nozzle_flow(0);
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead settings: every head's card is titled in one style",
                 "[toolhead_settings][nozzle_size]") {
    // The tool table names only head 0, as a two-extruder printer without a tool for
    // each extruder does: the cards must not mix a named head with a bare extruder.
    auto& tools = ToolState::instance();
    tools.deinit_subjects();
    tools.init_subjects(false);
    ToolTopology topo;
    topo.tool_count = 1;
    topo.active_tool = 0;
    topo.tool_to_slot = {0};
    topo.tool_extruder = {"extruder"};
    tools.set_ams_topology(topo);
    helix::ui::UpdateQueue::instance().drain();

    open({"extruder", "extruder1"},
         {{"extruder", {{"nozzle_diameter", 0.4}}}, {"extruder1", {{"nozzle_diameter", 0.4}}}});
    const auto title = [](const char* head) {
        lv_obj_t* card = lv_obj_find_by_name(lv_screen_active(), head);
        lv_obj_t* header = card ? lv_obj_find_by_name(card, "setting_group_header") : nullptr;
        std::string text = header ? helix::test::subtree_text(header) : std::string();
        text.erase(std::remove_if(text.begin(), text.end(),
                                  [](unsigned char c) { return std::isdigit(c) != 0; }),
                   text.end());
        return text;
    };
    const std::string first = title("head_0");
    REQUIRE_FALSE(first.empty());
    CHECK(first == title("head_1"));
    tools.deinit_subjects();
}

namespace {

/// Picks 0.6 mm in head_0's size dropdown; its confirmation is up when this returns.
void ask_06() {
    lv_obj_t* card = lv_obj_find_by_name(lv_screen_active(), "head_0");
    lv_obj_t* row = card ? lv_obj_find_by_name(card, "size_row") : nullptr;
    lv_obj_t* dd = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
    REQUIRE(dd != nullptr);
    lv_dropdown_set_selected(dd, 5); // 0.6 mm
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
}

void answer_change() {
    helix::test::answer_top_modal("btn_primary");
    helix::ui::UpdateQueue::instance().drain();
}

void pick_and_confirm_06() {
    ask_06();
    answer_change();
}

/// The printer starts (or pauses) a job while a confirmation is open.
void job_starts(const char* state) {
    get_printer_state().update_from_status(
        {{"print_stats", {{"state", state}, {"filename", "part.gcode"}}}});
    helix::ui::UpdateQueue::instance().drain();
}

void job_ends() {
    get_printer_state().update_from_status(
        {{"print_stats", {{"state", "standby"}, {"filename", ""}}}});
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead settings: a confirmed config change is written to printer.cfg",
                 "[toolhead_settings][nozzle_size]") {
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, get_printer_state());
    api.set_config_files(
        {{"printer.cfg", "[printer]\nkinematics: corexy\n[extruder]\nnozzle_diameter: 0.4\n"}});
    helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                             helix::ConnectionState::CONNECTED);
    helix::http::HttpExecutor::fast().start();
    set_moonraker_api(&api);

    open({"extruder"}, {{"extruder", {{"nozzle_diameter", 0.4}}}});
    pick_and_confirm_06();

    const auto written = [&api]() {
        const auto cfg = api.get_uploaded_config("printer.cfg");
        return cfg && cfg->find("nozzle_diameter: 0.6") != std::string::npos;
    };
    CHECK(wait_until(written, 5000));

    set_moonraker_api(nullptr);
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead settings: a confirmed change on a printer with the command sends it",
                 "[toolhead_settings][nozzle_size]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
    auto& state = get_printer_state();
    state.init_subjects(false);
    const auto klippy_before = static_cast<KlippyState>(
        lv_subject_get_int(state.network_state().get_klippy_state_subject()));
    state.set_klippy_state_sync(KlippyState::READY);
    MoonrakerAPI api(client, state);
    client.connect("ws://mock/websocket", []() {}, []() {});
    helix::nozzle_size::probe_size_command(client);
    REQUIRE(helix::nozzle_size::size_command_state() == helix::nozzle_size::CommandProbe::Present);
    set_moonraker_api(&api);
    REQUIRE(client.nozzle_properties(0).first == Catch::Approx(0.4));

    open({"extruder"}, {{"extruder", {{"nozzle_diameter", 0.4}}}});
    pick_and_confirm_06();

    CHECK(wait_until([&client]() { return client.nozzle_properties(0).first == 0.6; }, 5000));

    set_moonraker_api(nullptr);
    helix::nozzle_size::set_size_command_state(helix::nozzle_size::CommandProbe::Unknown);
    client.disconnect();
    state.set_klippy_state_sync(klippy_before);
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead settings: a job that starts while the dialog is open stops the change",
                 "[toolhead_settings][nozzle_size]") {
    const char* job = GENERATE("printing", "paused");
    CAPTURE(job);
    SECTION("a config change") {
        MoonrakerClientMock client;
        MoonrakerAPIMock api(client, get_printer_state());
        api.set_config_files(
            {{"printer.cfg", "[printer]\nkinematics: corexy\n[extruder]\nnozzle_diameter: 0.4\n"}});
        helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                                 helix::ConnectionState::CONNECTED);
        helix::http::HttpExecutor::fast().start();
        set_moonraker_api(&api);

        open({"extruder"}, {{"extruder", {{"nozzle_diameter", 0.4}}}});
        ask_06();
        job_starts(job);
        answer_change();

        const auto written = [&api]() {
            const auto cfg = api.get_uploaded_config("printer.cfg");
            return cfg && cfg->find("nozzle_diameter: 0.6") != std::string::npos;
        };
        CHECK_FALSE(wait_until(written, 600));

        job_ends();
        set_moonraker_api(nullptr);
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();
    }
    SECTION("the printer's own command") {
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
        auto& state = get_printer_state();
        state.init_subjects(false);
        const auto klippy_before = static_cast<KlippyState>(
            lv_subject_get_int(state.network_state().get_klippy_state_subject()));
        state.set_klippy_state_sync(KlippyState::READY);
        MoonrakerAPI api(client, state);
        client.connect("ws://mock/websocket", []() {}, []() {});
        helix::nozzle_size::probe_size_command(client);
        set_moonraker_api(&api);

        open({"extruder"}, {{"extruder", {{"nozzle_diameter", 0.4}}}});
        ask_06();
        job_starts(job);
        answer_change();

        CHECK_FALSE(
            wait_until([&client]() { return client.nozzle_properties(0).first == 0.6; }, 600));

        job_ends();
        set_moonraker_api(nullptr);
        client.disconnect();
        state.set_klippy_state_sync(klippy_before);
    }
}
