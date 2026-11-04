// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_layout_sync.cpp
 * @brief HomeLayoutSync publish / fetch through the mock Moonraker database,
 *        the connection locality verdict it is gated on, and the seed a
 *        remote client's home layout takes from the printer's own screen.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/scoped_int_subject.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "connection_state.h"
#include "filament_sensor_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "home_layout_share.h"
#include "home_layout_sync.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_manager.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "printer_network_state.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "spoolman_manager.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::home_layout::kKey;
using helix::home_layout::kNamespace;
using json = nlohmann::json;

namespace {

json widget(const char* id, int col, int row, int colspan, int rowspan) {
    return json{{"id", id},   {"enabled", true},    {"col", col},
                {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
}

/// A home layout saved on a 12x8 grid.
json saved_home_12x8(const char* second_widget = "fan") {
    return json{
        {"pages",
         json::array({json{{"id", "main"},
                           {"widgets", json::array({widget("printer_image", 0, 0, 4, 4),
                                                    widget("temperature", 4, 0, 2, 2),
                                                    widget(second_widget, 6, 0, 2, 2)})}}})},
        {"main_page_index", 0},
        {"next_page_id", 1},
        {"grid", "12x8"}};
}

class HomeLayoutSyncFixture : public LVGLUITestFixture {
  public:
    HomeLayoutSyncFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        api_ = std::make_unique<MoonrakerAPIMock>(client_, get_printer_state());
        global_config_ = Config::get_instance();
        REQUIRE(global_config_ != nullptr);
        saved_active_id_ = ConfigTestAccess::active_printer_id(*global_config_);
        ConfigTestAccess::active_printer_id(*global_config_) = "printer_a";
        get_printer_state().clear_moonraker_locality();
        drain();
        HomeLayoutSync::instance().init(*api_);
    }

    ~HomeLayoutSyncFixture() override {
        HomeLayoutSync::instance().set_seed_listener(nullptr);
        HomeLayoutSync::instance().shutdown();
        get_printer_state().clear_moonraker_locality();
        drain();
        ConfigTestAccess::active_printer_id(*global_config_) = saved_active_id_;
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_locality(bool remote) {
        get_printer_state().set_moonraker_is_remote(remote);
        drain();
    }

    /// A loaded home PanelWidgetConfig over a private Config holding @p saved.
    PanelWidgetConfig& home_config(const json& saved) {
        ConfigTestAccess::data(local_config_) = json::object();
        ConfigTestAccess::data(local_config_)["printers"]["default"]["panel_widgets"]["home"] =
            saved;
        ConfigTestAccess::active_printer_id(local_config_) = "default";
        configs_.push_back(std::make_unique<PanelWidgetConfig>("home", local_config_));
        configs_.back()->load();
        return *configs_.back();
    }

    bool published() {
        return client_.mock_db().contains(kNamespace) &&
               client_.mock_db()[kNamespace].contains(kKey);
    }

    json published_value() {
        return client_.mock_db_get(kNamespace, kKey);
    }

    void forget_published() {
        if (client_.mock_db().contains(kNamespace)) {
            client_.mock_db()[kNamespace].erase(kKey);
        }
    }

    void publish_remote_layout(const json& value) {
        client_.mock_db_set(kNamespace, kKey, value);
    }

    json valid_published_payload() {
        PanelWidgetConfig& source = home_config(saved_home_12x8());
        return home_layout::build_payload(source.published_pages(), "12x8", "1.0.0", 1700000000);
    }

    MoonrakerClientMock client_;
    std::unique_ptr<MoonrakerAPIMock> api_;
    Config* global_config_ = nullptr;
    std::string saved_active_id_;
    Config local_config_;
    std::vector<std::unique_ptr<PanelWidgetConfig>> configs_;
};

} // namespace

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: publishes nothing while locality is unknown",
                 "[home_layout_sync]") {
    REQUIRE(get_printer_state().moonraker_locality() == MoonrakerLocality::Unknown);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    wc.save();
    drain();
    CHECK_FALSE(published());

    HomeLayoutSync::instance().on_connected();
    drain();
    CHECK_FALSE(published());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: publishes on save when Local, payload parses back",
                 "[home_layout_sync]") {
    set_locality(false);
    REQUIRE(get_printer_state().moonraker_locality() == MoonrakerLocality::Local);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    REQUIRE(wc.grid_signature() == "12x8");

    wc.save();
    drain();
    REQUIRE(published());

    const json value = published_value();
    CHECK(value["schema"] == home_layout::kSchema);
    CHECK(value["grid"] == "12x8");
    CHECK_FALSE(value.contains("next_page_id"));
    const auto parsed = home_layout::parse_payload(value);
    REQUIRE(parsed.has_value());
    CHECK(parsed->cols == 12);
    CHECK(parsed->rows == 8);
    const json& widgets = parsed->pages_payload["pages"][0]["widgets"];
    std::vector<std::string> ids;
    for (const auto& w : widgets) {
        // load() appends the registry's other widgets disabled and unplaced.
        if (w.value("enabled", false) && w.value("col", -1) >= 0) {
            ids.push_back(w["id"].get<std::string>());
        }
    }
    CHECK(ids == std::vector<std::string>{"printer_image", "temperature", "fan"});
    CHECK(widgets[2]["col"] == 6);
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a panel other than home is not published",
                 "[home_layout_sync]") {
    set_locality(false);
    ConfigTestAccess::data(local_config_) = json::object();
    ConfigTestAccess::data(local_config_)["printers"]["default"]["panel_widgets"]["controls"] =
        saved_home_12x8();
    ConfigTestAccess::active_printer_id(local_config_) = "default";
    PanelWidgetConfig other("controls", local_config_);
    other.load();
    other.save();
    drain();
    CHECK_FALSE(published());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture, "home_layout_sync: remote client never publishes",
                 "[home_layout_sync]") {
    set_locality(true);
    REQUIRE(get_printer_state().moonraker_locality() == MoonrakerLocality::Remote);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    wc.save();
    drain();
    CHECK_FALSE(published());

    HomeLayoutSync::instance().on_connected();
    drain();
    CHECK_FALSE(published());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture, "home_layout_sync: identical consecutive saves post once",
                 "[home_layout_sync]") {
    set_locality(false);
    PanelWidgetConfig& wc = home_config(saved_home_12x8());
    wc.save();
    drain();
    REQUIRE(published());

    // An unchanged layout is not posted again, so the key stays gone.
    forget_published();
    wc.save();
    drain();
    CHECK_FALSE(published());

    SECTION("a changed layout posts again") {
        PanelWidgetConfig& changed = home_config(saved_home_12x8("led"));
        changed.save();
        drain();
        CHECK(published());
    }

    SECTION("a new connection posts again") {
        HomeLayoutSync::instance().on_disconnected();
        wc.save();
        drain();
        CHECK(published());
    }
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: on_connected as Remote fetches and take_seed returns it once",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);

    std::vector<std::string> told;
    HomeLayoutSync::instance().set_seed_listener(
        [&told](const std::string& printer_id) { told.push_back(printer_id); });

    HomeLayoutSync::instance().on_connected();
    // Nothing is handed over until the reply reaches the main thread.
    CHECK(told.empty());
    drain();
    CHECK(told == std::vector<std::string>{"printer_a"});

    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_b").has_value());
    auto seed = HomeLayoutSync::instance().take_seed("printer_a");
    REQUIRE(seed.has_value());
    CHECK(seed->cols == 12);
    CHECK(seed->rows == 8);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a reply is parsed on the main thread, not in the callback",
                 "[home_layout_sync]") {
    publish_remote_layout(json{{"schema", "no-such-schema"}});
    set_locality(true);

    LogCapture capture;
    HomeLayoutSync::instance().on_connected();
    // The mock delivers on this thread: a parse in the callback would already have logged.
    CHECK(capture.count_containing("Published home layout is unreadable") == 0);
    drain();
    CHECK(capture.count_containing("Published home layout is unreadable") == 1);
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a reply for a printer no longer active is dropped",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);
    bool told = false;
    HomeLayoutSync::instance().set_seed_listener([&told](const std::string&) { told = true; });

    HomeLayoutSync::instance().on_connected();
    // The mock answers at once; the reply is queued for the main thread.
    ConfigTestAccess::active_printer_id(*global_config_) = "printer_b";
    drain();

    CHECK_FALSE(told);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_b").has_value());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a reply that lands after a disconnect is dropped",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);
    HomeLayoutSync::instance().on_connected();
    HomeLayoutSync::instance().on_disconnected();
    drain();
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: missing key or unparseable value yields no seed and no error "
                 "toast",
                 "[home_layout_sync]") {
    set_locality(true);
    SECTION("missing key") {}
    SECTION("unparseable value") {
        publish_remote_layout(json{{"schema", 99}, {"grid", "nonsense"}});
    }
    SECTION("not an object") {
        publish_remote_layout("garbage");
    }

    LogCapture capture;
    bool told = false;
    HomeLayoutSync::instance().set_seed_listener([&told](const std::string&) { told = true; });
    HomeLayoutSync::instance().on_connected();
    drain();

    CHECK_FALSE(told);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
    for (auto level : capture.levels_for("[HomeLayoutSync]")) {
        CHECK(level < spdlog::level::warn);
    }
    for (auto level : capture.levels_for("database")) {
        CHECK(level < spdlog::level::warn);
    }
}

TEST_CASE_METHOD(HomeLayoutSyncFixture, "home_layout_sync: fetch_now hands the layout to done",
                 "[home_layout_sync]") {
    set_locality(true);
    std::vector<bool> results;
    auto record = [&results](std::optional<home_layout::ParsedLayout> layout) {
        results.push_back(layout.has_value());
    };

    HomeLayoutSync::instance().fetch_now(record);
    drain();
    publish_remote_layout(valid_published_payload());
    HomeLayoutSync::instance().fetch_now(record);
    drain();
    // The manual fetch does not leave a seed behind.
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
    // A printer switch in flight still answers, with nothing.
    HomeLayoutSync::instance().fetch_now(record);
    ConfigTestAccess::active_printer_id(*global_config_) = "printer_b";
    drain();

    CHECK(results == std::vector<bool>{false, true, false});
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: locality: Unknown after clear, Local/Remote after set, cleared "
                 "on disconnect edge",
                 "[home_layout_sync]") {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "moonraker_locality");
    REQUIRE(subject != nullptr);
    PrinterState& ps = get_printer_state();

    CHECK(ps.moonraker_locality() == MoonrakerLocality::Unknown);
    CHECK(lv_subject_get_int(subject) == 0);

    ps.set_moonraker_is_remote(false);
    // The verdict is readable as soon as it is made; the subject follows the queue.
    CHECK(ps.moonraker_locality() == MoonrakerLocality::Local);
    drain();
    CHECK(lv_subject_get_int(subject) == 1);
    CHECK_FALSE(ps.is_moonraker_remote());

    ps.set_moonraker_is_remote(true);
    drain();
    CHECK(ps.moonraker_locality() == MoonrakerLocality::Remote);
    CHECK(lv_subject_get_int(subject) == 2);
    CHECK(ps.is_moonraker_remote());

    ps.clear_moonraker_locality();
    CHECK(ps.moonraker_locality() == MoonrakerLocality::Unknown);
    drain();
    CHECK(lv_subject_get_int(subject) == 0);
    // is_moonraker_remote() keeps the last verdict, as its callers expect.
    CHECK(ps.is_moonraker_remote());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: a non-CONNECTED connection edge clears the verdict and the "
                 "held seed",
                 "[home_layout_sync]") {
    publish_remote_layout(valid_published_payload());
    set_locality(true);
    HomeLayoutSync::instance().on_connected();
    drain();

    const int edge = GENERATE(static_cast<int>(ConnectionState::DISCONNECTED),
                              static_cast<int>(ConnectionState::RECONNECTING),
                              static_cast<int>(ConnectionState::FAILED));
    INFO("edge " << edge);
    MoonrakerManager manager;
    MoonrakerManagerTestAccess::queue_connection_state(
        manager, static_cast<int>(ConnectionState::CONNECTED), edge);
    manager.process_notifications();
    drain();

    CHECK(get_printer_state().moonraker_locality() == MoonrakerLocality::Unknown);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "moonraker_locality")) == 0);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: an unedited default is not published until someone edits it",
                 "[home_layout_sync]") {
    set_locality(false);
    ConfigTestAccess::data(local_config_) = json::object();
    ConfigTestAccess::active_printer_id(local_config_) = "default";
    PanelWidgetConfig wc("home", local_config_);
    wc.load();
    REQUIRE(wc.has_pending_anchors());

    // Resolving the default for a known grid saves it.
    wc.apply_pending_anchors(12, 8);
    drain();
    REQUIRE(wc.grid_signature() == "12x8");
    REQUIRE(wc.is_unedited_default());
    CHECK_FALSE(published());
    wc.save();
    drain();
    CHECK_FALSE(published());

    const PanelWidgetEntry& first = wc.entries().front();
    REQUIRE(wc.set_enabled_by_id(first.id, !first.enabled));
    REQUIRE_FALSE(wc.is_unedited_default());
    wc.save();
    drain();
    CHECK(published());
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: on_connected as Local publishes the saved home layout",
                 "[home_layout_sync]") {
    json& data = ConfigTestAccess::data(*global_config_);
    const json saved_data = data;
    data["printers"]["printer_a"]["panel_widgets"]["home"] = saved_home_12x8();
    PanelWidgetManager::instance().clear_all_panel_configs();

    set_locality(false);
    HomeLayoutSync::instance().on_connected();
    drain();

    const bool posted = published();
    const json value = posted ? published_value() : json();
    PanelWidgetManager::instance().clear_all_panel_configs();
    data = saved_data;

    REQUIRE(posted);
    CHECK(value["grid"] == "12x8");
    CHECK(home_layout::parse_payload(value).has_value());
}

namespace {

constexpr const char* kSeededToast = "Home layout copied from the printer's screen";

/// A home screen built by PanelWidgetManager::populate_widgets() on the
/// 800x480 content box (a 12x8 grid), against the global Config's home node
/// for the active printer, with the printer CONNECTED and READY.
class SeedPopulateFixture : public HomeLayoutSyncFixture {
  public:
    SeedPopulateFixture() : prev_test_mode_(get_runtime_config()->test_mode) {
        // NetworkWidget's attach() otherwise builds the platform WiFi backend.
        get_runtime_config()->test_mode = true;
        PanelWidgetManager::instance().init_widget_subjects();
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(false);
        ToolTopology topo;
        topo.tool_count = 1;
        topo.active_tool = 0;
        ToolState::instance().set_ams_topology(topo);
        get_printer_state().temperature_state().init_extruders({"extruder"});
        if (lv_xml_get_subject(nullptr, "ams_slot_count") == nullptr) {
            AmsState::instance().init_subjects(true);
        }
        pin("ams_slot_count", 0);
        pin("home_dryer_available", 0);
        pin("printer_has_chamber", 0);
        pin("printer_is_enclosed", 0);
        pin("led_controllable", 1);
        pin("filament_sensor_count", 1);
        pin("printer_has_spoolman", 0);
        pin("printer_connection_state", static_cast<int>(ConnectionState::CONNECTED));
        pin("klippy_state", static_cast<int>(KlippyState::READY));

        lv_display_t* disp = lv_display_get_default();
        REQUIRE(disp != nullptr);
        res_ = std::make_unique<ScopedResolution>(disp, 800, 480);
        theme_manager_refresh_layout_constants(disp);
        container_ = lv_obj_create(test_screen());
        lv_obj_set_size(container_, 710, 466);
        lv_obj_set_style_pad_all(container_, 0, 0);
        lv_obj_set_style_border_width(container_, 0, 0);
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_SCROLLABLE);

        saved_home_ = global_config_->get<json>(home_path(), json());
        fresh_home();
        helix::ui::set_test_toast_hook([this](ToastSeverity, const std::string& message, uint32_t) {
            toasts.push_back(message);
        });
        helix::ui::UpdateQueue::instance().drain();
    }

    ~SeedPopulateFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        auto& mgr = PanelWidgetManager::instance();
        PanelWidgetManager::clear_gate_observers("home");
        global_config_->set<json>(home_path(), saved_home_);
        mgr.clear_all_panel_configs();
        mgr.clear_panel_config("home");
        PrintStatusWidget::destroy_formatter_for_test();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain_until_empty();
        res_.reset();
        if (lv_display_t* d = lv_display_get_default()) {
            theme_manager_refresh_layout_constants(d);
        }
        get_runtime_config()->test_mode = prev_test_mode_;
    }

    std::string home_path() const {
        return global_config_->df() + "panel_widgets/home";
    }

    /// No saved home layout: the next load builds the pending default.
    void fresh_home() {
        global_config_->set<json>(home_path(), json());
        auto& mgr = PanelWidgetManager::instance();
        mgr.clear_all_panel_configs();
        mgr.clear_panel_config("home");
    }

    void populate() {
        PanelWidgetManager::instance().populate_widgets("home", container_, 0);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void clear_container() {
        lv_obj_clean(container_);
    }

    PanelWidgetConfig& home() {
        return PanelWidgetManager::instance().get_widget_config("home");
    }

    /// Every seated, enabled widget on the main page, as "id@col,row colspan x rowspan".
    std::vector<std::string> seats() {
        std::vector<std::string> out;
        for (const auto& e : home().page_entries(0)) {
            if (e.enabled && e.is_placed()) {
                out.push_back(e.id + "@" + std::to_string(e.col) + "," + std::to_string(e.row) +
                              " " + std::to_string(e.colspan) + "x" + std::to_string(e.rowspan));
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    static bool has_seat(const std::vector<std::string>& seats, const std::string& seat) {
        return std::find(seats.begin(), seats.end(), seat) != seats.end();
    }

    /// The printer's screen published @p value; this client connects as Remote.
    void connect_remote_with(const json& value) {
        publish_remote_layout(value);
        set_locality(true);
        HomeLayoutSync::instance().on_connected();
        drain();
    }

    /// The default home layout this grid gets with no seed, resolved and saved.
    std::vector<std::string> default_seats() {
        populate();
        REQUIRE_FALSE(home().has_pending_anchors());
        REQUIRE(home().is_unedited_default());
        return seats();
    }

    void check_seeded_seats() {
        const auto now = seats();
        INFO("seats: " << Catch::Detail::stringify(now));
        CHECK(has_seat(now, "printer_image@0,0 4x4"));
        CHECK(has_seat(now, "temperature@4,0 2x2"));
        CHECK(has_seat(now, "fan@6,0 2x2"));
        CHECK_FALSE(home().is_unedited_default());
        CHECK_FALSE(home().has_pending_anchors());
    }

    void pin(const char* name, int value) {
        pins_.push_back(std::make_unique<helix::test::ScopedIntSubject>(name, value));
    }

    std::vector<std::string> toasts;

  private:
    bool prev_test_mode_;
    std::vector<std::unique_ptr<helix::test::ScopedIntSubject>> pins_;
    std::unique_ptr<ScopedResolution> res_;
    lv_obj_t* container_ = nullptr;
    json saved_home_;
};

/// A payload that parses but whose only widget is one no screen knows.
json unknown_widget_payload() {
    return json{
        {"schema", home_layout::kSchema},
        {"grid", "12x8"},
        {"main_page_index", 0},
        {"pages",
         json::array({json{{"id", "main"},
                           {"widgets", json::array({widget("no_such_widget", 0, 0, 2, 2)})}}})}};
}

} // namespace

TEST_CASE_METHOD(SeedPopulateFixture, "seed present at the READY populate replaces the default",
                 "[home_layout_sync][seed]") {
    LogCapture capture;
    connect_remote_with(valid_published_payload());
    REQUIRE(capture.count_containing("Holding the printer's home layout") == 1);
    // The reply landed before any default was applied, so it waits for the populate.
    REQUIRE(home().has_pending_anchors());
    CHECK(toasts.empty());

    populate();

    check_seeded_seats();
    CHECK(home().grid_signature() == "12x8");
    CHECK(toasts == std::vector<std::string>{kSeededToast});
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(SeedPopulateFixture, "seed held while Klipper is not READY waits for READY",
                 "[home_layout_sync][seed]") {
    connect_remote_with(valid_published_payload());
    {
        helix::test::ScopedIntSubject starting("klippy_state",
                                               static_cast<int>(KlippyState::STARTUP));
        populate();
        CHECK(home().has_pending_anchors());
        CHECK(toasts.empty());
    }
    // An unchanged widget list skips the rebuild; this checks only what a
    // READY populate does with a seed the transient one left alone.
    clear_container();
    populate();
    check_seeded_seats();
    CHECK(toasts == std::vector<std::string>{kSeededToast});
}

TEST_CASE_METHOD(SeedPopulateFixture, "seed used when home first showed while offline",
                 "[home_layout_sync][seed]") {
    const bool held_first = GENERATE(true, false);
    INFO((held_first ? "seed held before" : "seed held after") << " the offline populate");
    if (held_first) {
        connect_remote_with(valid_published_payload());
    }
    {
        helix::test::ScopedIntSubject offline("printer_connection_state",
                                              static_cast<int>(ConnectionState::DISCONNECTED));
        populate();
        REQUIRE(home().has_pending_anchors());
        REQUIRE(home().grid_signature() == "12x8");
    }
    if (!held_first) {
        connect_remote_with(valid_published_payload());
    }
    clear_container();
    populate();
    check_seeded_seats();
    CHECK(toasts == std::vector<std::string>{kSeededToast});
}

TEST_CASE_METHOD(SeedPopulateFixture, "no seed: default anchors applied as before",
                 "[home_layout_sync][seed]") {
    HomeLayoutSync::instance().shutdown();
    const auto without_sync = default_seats();
    REQUIRE_FALSE(without_sync.empty());

    fresh_home();
    HomeLayoutSync::instance().init(*api_);
    set_locality(true);
    HomeLayoutSync::instance().on_connected(); // nothing is published
    drain();

    CHECK(default_seats() == without_sync);
    CHECK(toasts.empty());
}

TEST_CASE_METHOD(SeedPopulateFixture, "invalid seed falls back to defaults",
                 "[home_layout_sync][seed]") {
    const auto defaults = default_seats();
    fresh_home();

    LogCapture capture;
    connect_remote_with(unknown_widget_payload());
    REQUIRE(capture.count_containing("Holding the printer's home layout") == 1);

    CHECK(default_seats() == defaults);
    CHECK(toasts.empty());
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(SeedPopulateFixture, "late reply replaces an unedited default",
                 "[home_layout_sync][seed]") {
    default_seats();
    REQUIRE(home().grid_signature() == "12x8");

    connect_remote_with(valid_published_payload());

    check_seeded_seats();
    CHECK(toasts == std::vector<std::string>{kSeededToast});
    // Saved, not only held in memory: a reload from Config reads the same.
    home().mark_dirty();
    check_seeded_seats();
}

TEST_CASE_METHOD(SeedPopulateFixture, "late reply never reseeds a layout that was reset",
                 "[home_layout_sync][seed]") {
    default_seats();
    home().reset_to_defaults();
    home().save();
    REQUIRE(home().is_unedited_default());
    REQUIRE(home().had_saved_layout());
    const auto reset = seats();

    connect_remote_with(valid_published_payload());

    populate();

    CHECK(seats() == reset);
    CHECK(home().is_unedited_default());
    CHECK(toasts.empty());
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(SeedPopulateFixture, "a restart never reseeds a layout that was saved",
                 "[home_layout_sync][seed]") {
    default_seats();
    home().reset_to_defaults();
    home().save();
    const auto reset = seats();
    PanelWidgetManager::instance().clear_all_panel_configs();
    PanelWidgetManager::instance().clear_panel_config("home");
    REQUIRE(home().had_saved_layout());

    connect_remote_with(valid_published_payload());
    populate();

    CHECK(toasts.empty());
    CHECK(home().is_unedited_default());
    CHECK(seats() == reset);
}

TEST_CASE_METHOD(SeedPopulateFixture, "a brand-new printer has no saved layout",
                 "[home_layout_sync][seed]") {
    CHECK_FALSE(home().had_saved_layout());
    default_seats();
    CHECK_FALSE(home().had_saved_layout());
}

TEST_CASE_METHOD(SeedPopulateFixture, "late reply leaves an edited layout alone",
                 "[home_layout_sync][seed]") {
    default_seats();
    const PanelWidgetEntry first = home().entries().front();
    REQUIRE(home().set_enabled_by_id(first.id, !first.enabled));
    home().save();
    REQUIRE_FALSE(home().is_unedited_default());
    const auto edited = seats();

    connect_remote_with(valid_published_payload());

    CHECK(seats() == edited);
    CHECK_FALSE(home().is_unedited_default());
    CHECK(toasts.empty());
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(SeedPopulateFixture, "late reply skips the import while home is being edited",
                 "[home_layout_sync][seed]") {
    const auto defaults = default_seats();
    home().set_edit_session_open(true);

    LogCapture capture;
    connect_remote_with(valid_published_payload());
    home().set_edit_session_open(false);

    CHECK(seats() == defaults);
    CHECK(home().is_unedited_default());
    CHECK(toasts.empty());
    CHECK(capture.count_containing("Home is being edited") == 1);
}

TEST_CASE_METHOD(SeedPopulateFixture, "late reply whose import is refused leaves the layout alone",
                 "[home_layout_sync][seed]") {
    const auto defaults = default_seats();

    LogCapture capture;
    connect_remote_with(unknown_widget_payload());

    CHECK(seats() == defaults);
    CHECK(home().is_unedited_default());
    CHECK(toasts.empty());
    CHECK(capture.count_containing("did not import") == 1);
    CHECK_FALSE(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(SeedPopulateFixture,
                 "late reply with no grid and no pending anchors keeps the seed",
                 "[home_layout_sync][seed]") {
    global_config_->set<json>(
        home_path(),
        json{{"main_page_index", 0},
             {"pages", json::array({json{{"id", "main"},
                                         {"widgets", json::array({json{{"id", "temperature"},
                                                                       {"enabled", true}}})}}})}});
    auto& mgr = PanelWidgetManager::instance();
    home().mark_dirty();
    mgr.clear_panel_config("home");
    REQUIRE_FALSE(home().has_pending_anchors());
    REQUIRE(home().stamped_grid() == std::pair<int, int>(0, 0));

    connect_remote_with(valid_published_payload());

    CHECK(toasts.empty());
    CHECK(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

TEST_CASE_METHOD(SeedPopulateFixture, "populate while home is being edited does not take the seed",
                 "[home_layout_sync][seed]") {
    connect_remote_with(valid_published_payload());
    REQUIRE(home().has_pending_anchors());
    home().set_edit_session_open(true);

    populate();
    home().set_edit_session_open(false);

    CHECK(toasts.empty());
    CHECK(home().is_unedited_default());
    CHECK(HomeLayoutSync::instance().take_seed("printer_a").has_value());
}

namespace {

/// A file holding @p content, removed with the guard.
class TempJsonFile {
  public:
    explicit TempJsonFile(const std::string& content)
        : path_(std::filesystem::temp_directory_path() /
                ("helix_mock_home_layout_" + std::to_string(::getpid()) + ".json")) {
        std::ofstream(path_) << content;
    }
    ~TempJsonFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    std::string path() const {
        return path_.string();
    }

  private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: HELIX_MOCK_HOME_LAYOUT pre-seeds the published record",
                 "[home_layout_sync]") {
    const json payload = valid_published_payload();
    TempJsonFile file(payload.dump());
    ScopedEnv env("HELIX_MOCK_HOME_LAYOUT", file.path().c_str());

    MoonrakerClientMock seeded(MoonrakerClientMock::PrinterType::VORON_24);

    CHECK(seeded.mock_db_get(kNamespace, kKey) == payload);
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: HELIX_MOCK_HOME_LAYOUT naming no JSON object seeds nothing",
                 "[home_layout_sync]") {
    TempJsonFile file("[1, 2]");
    ScopedEnv env("HELIX_MOCK_HOME_LAYOUT", file.path().c_str());

    MoonrakerClientMock seeded(MoonrakerClientMock::PrinterType::VORON_24);

    CHECK_FALSE(seeded.mock_db().contains(kNamespace));
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: the manager wires the sync to its API and unwires it on "
                 "shutdown",
                 "[home_layout_sync]") {
    TempJsonFile file(valid_published_payload().dump());
    ScopedEnv env("HELIX_MOCK_HOME_LAYOUT", file.path().c_str());
    RuntimeConfig runtime_config;
    runtime_config.test_mode = true;
    set_locality(true);

    std::vector<bool> results;
    auto record = [&results](std::optional<home_layout::ParsedLayout> layout) {
        results.push_back(layout.has_value());
    };

    // init() hands its API to these globals and the app's session takes it back;
    // here nothing would, and the next test to reach one would use a freed API.
    struct ApiGlobalsRestore {
        helix::IMoonrakerAPI* saved = get_moonraker_api();
        ~ApiGlobalsRestore() {
            set_moonraker_api(saved);
            helix::AmsState::instance().set_moonraker_api(nullptr);
            helix::FilamentSensorManager::instance().set_moonraker_api(nullptr);
            SpoolmanManager::instance().set_api(nullptr);
        }
    } restore_api_globals;
    MoonrakerManager manager;
    REQUIRE(manager.init(runtime_config, nullptr));
    REQUIRE(get_moonraker_api() != nullptr);
    HomeLayoutSync::instance().fetch_now(record);
    drain();
    manager.shutdown();
    // The API it installed is gone, so nothing may still reach it through the global.
    CHECK(get_moonraker_api() == nullptr);
    HomeLayoutSync::instance().fetch_now(record);
    drain();

    CHECK(results == std::vector<bool>{true, false});
}

TEST_CASE_METHOD(HomeLayoutSyncFixture,
                 "home_layout_sync: init_subjects starts the locality verdict Unknown and the "
                 "subject follows a verdict",
                 "[home_layout_sync]") {
    set_locality(true);
    REQUIRE(get_printer_state().moonraker_locality() == MoonrakerLocality::Remote);
    lv_subject_t* subject = get_printer_state().network_state().get_moonraker_locality_subject();
    CHECK(lv_subject_get_int(subject) == static_cast<int>(MoonrakerLocality::Remote));

    get_printer_state().deinit_subjects();
    get_printer_state().init_subjects(true);

    CHECK(get_printer_state().moonraker_locality() == MoonrakerLocality::Unknown);
}
