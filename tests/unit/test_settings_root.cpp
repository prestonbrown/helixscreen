// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_nav_manager.h"
#include "ui_panel_settings.h"
#include "ui_printer_manager_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/application_test_access.h"
#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/ethernet_manager_test_access.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/settings_panel_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "display_settings_manager.h"
#include "ethernet_backend_mock.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "platform_info.h"
#include "plugins_overlay.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <array>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {
struct RootFixture : LVGLUITestFixture {
    // Declared first so it is torn down LAST (reverse declaration order):
    // refresh_status_lines() reads get_wifi_manager() (a process-lifetime
    // singleton) and constructs a member EthernetManager, and without
    // test_mode both pick real platform backends — WifiBackend::create()'s
    // start_async() hands the singleton a worker thread with no join site
    // here (prestonbrown/helixscreen#1531). Forcing the mock backends keeps
    // every case in this file hermetic and gives EthernetBackendMock's
    // connected=true a deterministic "Ethernet" status to assert against.
    ScopedRuntimeConfig scoped_config_;
    lv_obj_t* root_ = nullptr;
    RootFixture() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        get_runtime_config()->use_real_ethernet = false;

        // Nothing in this fixture runs PrinterCapabilitiesState::init_subjects(),
        // so printer_has_speaker is otherwise absent and the Sound row's gate
        // resolves to nothing.
        static lv_subject_t speaker_subject;
        if (!lv_xml_get_subject(nullptr, "printer_has_speaker")) {
            lv_subject_init_int(&speaker_subject, 1);
            lv_xml_register_subject(nullptr, "printer_has_speaker", &speaker_subject);
        }
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "settings_panel", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }
    ~RootFixture() override {
        if (root_ && helix::test::widget_in_tree(root_))
            lv_obj_delete(root_);
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }
    lv_obj_t* find(const char* n) const {
        return lv_obj_find_by_name(root_, n);
    }
    void set_int(const char* subject, int v) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, subject);
        REQUIRE(s != nullptr);
        lv_subject_set_int(s, v);
        process_lvgl(5);
    }
};
} // namespace

TEST_CASE_METHOD(RootFixture, "settings root: three groups, thirteen rows, in order",
                 "[settings][settings_root]") {
    const std::vector<std::pair<const char*, std::vector<const char*>>> groups = {
        {"group_screen", {"row_display", "row_appearance", "row_touch_input", "row_sound"}},
        {"group_printer",
         {"row_printer", "row_printing", "row_devices", "row_safety", "row_connection"}},
        {"group_helixscreen", {"row_language_time", "row_system", "row_updates", "row_help"}},
    };
    for (const auto& [group, rows] : groups) {
        lv_obj_t* g = find(group);
        REQUIRE(g != nullptr);
        int last_index = -1;
        for (const char* r : rows) {
            CAPTURE(group, r);
            lv_obj_t* row = lv_obj_find_by_name(g, r);
            REQUIRE(row != nullptr);
            int idx = lv_obj_get_index(row);
            CHECK(idx > last_index);
            last_index = idx;
        }
    }
    CHECK(find("row_display_sound") == nullptr);
    CHECK(find("row_hardware") == nullptr);
}

TEST_CASE_METHOD(RootFixture, "settings root: Sound hides without a speaker",
                 "[settings][settings_root]") {
    set_int("printer_has_speaker", 0);
    CHECK(lv_obj_has_flag(find("row_sound"), LV_OBJ_FLAG_HIDDEN));
    set_int("printer_has_speaker", 1);
    CHECK_FALSE(lv_obj_has_flag(find("row_sound"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(RootFixture, "settings root: Updates hides only when no update row would show",
                 "[settings][settings_root]") {
    set_int("show_update_settings", 0);
    set_int("updates_firmware_managed", 0);
    set_int("updates_unavailable", 0);
    CHECK(lv_obj_has_flag(find("row_updates"), LV_OBJ_FLAG_HIDDEN));
    set_int("updates_firmware_managed", 1);
    CHECK_FALSE(lv_obj_has_flag(find("row_updates"), LV_OBJ_FLAG_HIDDEN));
    set_int("updates_firmware_managed", 0);
    set_int("show_update_settings", 1);
    CHECK_FALSE(lv_obj_has_flag(find("row_updates"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(RootFixture, "settings root: Updates shows on updates_unavailable alone",
                 "[settings][settings_root]") {
    set_int("show_update_settings", 0);
    set_int("updates_firmware_managed", 0);
    set_int("updates_unavailable", 1);
    CHECK_FALSE(lv_obj_has_flag(find("row_updates"), LV_OBJ_FLAG_HIDDEN));
}

#if HELIX_HAS_PLUGINS
TEST_CASE_METHOD(RootFixture, "settings root: Plugins shows only once a plugin exists",
                 "[settings][settings_root]") {
    lv_obj_t* row = find("row_plugins");
    REQUIRE(row);
    // settings_plugins_available defaults to 0, so the row must not show yet.
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    helix::ConfigDirGuard guard("plugins-row");
    // The host boots from the per-printer cache: HELIX_CACHE_DIR decides where
    // that is, and no driver is built (no Moonraker here).
    helix::plugin::test::TempDir cache_root;
    helix::ScopedEnv cache_dir("HELIX_CACHE_DIR", cache_root.path.string().c_str());
    helix::ScopedEnv no_plugin_dir("HELIX_PLUGIN_DIR", nullptr);
    helix::Config config;
    Application app;
    ApplicationTestAccess::neutralize_destructor(app);
    ApplicationTestAccess::set_config(app, &config);

    const std::filesystem::path plugins =
        cache_root.path / "plugins" /
        (config.get_active_printer_id().empty() ? "default" : config.get_active_printer_id());

    // A host over an empty cache exists, but no plugin does: still hidden.
    std::filesystem::create_directories(plugins);
    ApplicationTestAccess::init_plugins(app);
    process_lvgl(5);
    CHECK(lv_obj_has_flag(find("row_plugins"), LV_OBJ_FLAG_HIDDEN));

    // One plugin in the cache: the row shows.
    std::filesystem::copy("tests/fixtures/plugins/hello", plugins / "hello",
                          std::filesystem::copy_options::recursive);
    ApplicationTestAccess::init_plugins(app);
    process_lvgl(5);
    CHECK_FALSE(lv_obj_has_flag(find("row_plugins"), LV_OBJ_FLAG_HIDDEN));

    // Re-running against an empty cache hides it again.
    std::filesystem::remove_all(plugins / "hello");
    ApplicationTestAccess::init_plugins(app);
    process_lvgl(5);
    CHECK(lv_obj_has_flag(find("row_plugins"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(RootFixture, "settings root: crash-loop safe mode loads no plugins",
                 "[settings][settings_root][crash_loop]") {
    helix::ConfigDirGuard guard("plugins-safe-mode");
    helix::plugin::test::TempDir cache_root;
    helix::ScopedEnv cache_dir("HELIX_CACHE_DIR", cache_root.path.string().c_str());
    helix::ScopedEnv no_plugin_dir("HELIX_PLUGIN_DIR", nullptr);
    ScopedRuntimeConfig scoped_config;
    helix::Config config;
    Application app;
    ApplicationTestAccess::neutralize_destructor(app);
    ApplicationTestAccess::set_config(app, &config);

    const std::filesystem::path plugins =
        cache_root.path / "plugins" /
        (config.get_active_printer_id().empty() ? "default" : config.get_active_printer_id());
    std::filesystem::create_directories(plugins);
    std::filesystem::copy("tests/fixtures/plugins/hello", plugins / "hello",
                          std::filesystem::copy_options::recursive);

    get_runtime_config()->crash_loop_safe_mode = true;
    ApplicationTestAccess::init_plugins(app);
    process_lvgl(5);
    CHECK(lv_obj_has_flag(find("row_plugins"), LV_OBJ_FLAG_HIDDEN));

    get_runtime_config()->crash_loop_safe_mode = false;
    ApplicationTestAccess::init_plugins(app);
    process_lvgl(5);
    CHECK_FALSE(lv_obj_has_flag(find("row_plugins"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(RootFixture, "settings root: tapping Plugins opens the plugins overlay",
                 "[settings][settings_root]") {
    DisplaySettingsManager::instance().set_animations_enabled(false);
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
    {
        helix::plugin::test::HostRig rig; // the host whose plugins get listed
        // The click hands the overlay the panel's parent_screen_; the fixture
        // builds the XML directly, so seed it the way DisplayManager does.
        get_global_settings_panel().setup(root_, test_screen());
        lv_obj_t* row = find("row_plugins");
        REQUIRE(row);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(50);
        CHECK(helix::plugin::get_plugins_overlay().root() != nullptr);
        REQUIRE(NavigationManager::instance().overlay_stack_names().size() == 1);
        // The stack records the overlay root's XML component name.
        CHECK(NavigationManager::instance().overlay_stack_names()[0] == "plugins_overlay");

        NavigationManager::instance().go_back();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(100);
    } // the rig dies after the overlay it fed is closed
    helix::plugin::PluginsOverlay& ov = helix::plugin::get_plugins_overlay();
    if (lv_obj_t* r = ov.root()) {
        lv_obj_t* held = r;
        ov.destroy_overlay_ui(held);
    }
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100); // deferred deletes
    DisplaySettingsManager::instance().set_animations_enabled(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(NavigationManager::instance().overlay_stack_names().empty());
}
#endif

namespace {
std::string status_text(lv_obj_t* root, const char* row) {
    lv_obj_t* r = lv_obj_find_by_name(root, row);
    REQUIRE(r != nullptr);
    lv_obj_t* s = lv_obj_find_by_name(r, "status");
    REQUIRE(s != nullptr);
    return lv_label_get_text(s);
}
} // namespace

TEST_CASE_METHOD(RootFixture, "settings root: status lines follow values on return",
                 "[settings][settings_root]") {
    set_int("settings_sounds_enabled", 1);
    set_int("settings_volume", 40);
    get_global_settings_panel().refresh_status_lines();
    process_lvgl(5);
    CHECK(status_text(root_, "row_sound") == "Volume 40%");

    set_int("settings_volume", 0);
    CHECK(status_text(root_, "row_sound") == "Volume 40%"); // no observer: stale until return
    get_global_settings_panel().on_activate();
    process_lvgl(5);
    CHECK(status_text(root_, "row_sound") == "Muted");
}

TEST_CASE_METHOD(RootFixture, "settings root: rows without state show no status",
                 "[settings][settings_root]") {
    for (const char* row :
         {"row_touch_input", "row_printing", "row_safety", "row_system", "row_help"}) {
        CAPTURE(row);
        lv_obj_t* r = find(row);
        REQUIRE(r != nullptr);
        lv_obj_t* wrap = lv_obj_find_by_name(r, "status_wrap");
        REQUIRE(wrap != nullptr);
        CHECK(lv_obj_has_flag(wrap, LV_OBJ_FLAG_HIDDEN));
    }
}

TEST_CASE_METHOD(RootFixture,
                 "settings root: connection status resolves via the async Ethernet probe",
                 "[settings][settings_root]") {
    // Wi-Fi is mocked disconnected (WifiBackendMock starts with no SSID) and
    // EthernetBackendMock defaults to connected=true, so the resolved status
    // is deterministically "Ethernet". get_info_async() hands the result back
    // on an HttpExecutor worker thread, not synchronously, so this can't be a
    // plain process_lvgl(5) check.
    get_global_settings_panel().refresh_status_lines();
    REQUIRE(wait_until([&]() { return status_text(root_, "row_connection") == "Ethernet"; }));
}

TEST_CASE_METHOD(RootFixture,
                 "settings root: a later refresh's provisional status remembers Ethernet",
                 "[settings][settings_root]") {
    // First refresh resolves via the async probe, latching last_ethernet_up_.
    get_global_settings_panel().refresh_status_lines();
    REQUIRE(wait_until([&]() { return status_text(root_, "row_connection") == "Ethernet"; }));

    // A second refresh's synchronous, pre-probe write must already read
    // "Ethernet" from the remembered state rather than guessing "Not
    // connected" until its own probe lands.
    get_global_settings_panel().refresh_status_lines();
    CHECK(status_text(root_, "row_connection") == "Ethernet");
}

TEST_CASE_METHOD(RootFixture, "settings root: tapping Printer opens the Printer Manager",
                 "[settings][settings_root]") {
    DisplaySettingsManager::instance().set_animations_enabled(false);
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
    get_global_settings_panel().setup(root_, test_screen());
    lv_obj_t* row = find("row_printer");
    REQUIRE(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    REQUIRE(NavigationManager::instance().overlay_stack_names().size() == 1);
    CHECK(NavigationManager::instance().overlay_stack_names()[0] == "printer_manager_overlay");

    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);
    destroy_printer_manager_overlay();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);
    DisplaySettingsManager::instance().set_animations_enabled(true);
    CHECK(NavigationManager::instance().overlay_stack_names().empty());
}

namespace {

struct LinkMocks {
    EthernetBackendMock* eth;
    WifiBackendMock* wifi;
};

/// Settles a first refresh (ethernet_manager_ is created lazily) and returns
/// both mocks, with Ethernet down so the row reports the Wi-Fi state.
LinkMocks settle_with_ethernet_down(RootFixture& f) {
    auto wifi_mgr = helix::get_wifi_manager();
    auto* wifi = dynamic_cast<WifiBackendMock*>(helix::WiFiManagerTestAccess::backend(*wifi_mgr));
    REQUIRE(wifi != nullptr);
    wifi->clear_status_callers();
    get_global_settings_panel().refresh_status_lines();
    REQUIRE(f.wait_until([&]() {
        return status_text(f.root_, "row_connection") == "Ethernet" &&
               !wifi->status_callers().empty();
    }));

    auto* eth_mgr = SettingsPanelTestAccess::ethernet_manager(get_global_settings_panel());
    REQUIRE(eth_mgr != nullptr);
    auto* eth = dynamic_cast<EthernetBackendMock*>(EthernetManagerTestAccess::backend(*eth_mgr));
    REQUIRE(eth != nullptr);
    eth->set_connected_state(false);
    return {eth, wifi};
}

/// Restores the shared mock state for later tests in this process, and never
/// leaves an HttpExecutor worker parked in a held status read.
struct RestoreLinks {
    RootFixture& f;
    LinkMocks m;
    ~RestoreLinks() {
        m.wifi->release_held_status();
        m.eth->set_connected_state(true);
        m.wifi->set_connected_state(false);
        get_global_settings_panel().refresh_status_lines();
        f.wait_until([&]() { return status_text(f.root_, "row_connection") == "Ethernet"; });
    }
};

} // namespace

TEST_CASE_METHOD(RootFixture, "settings root: the Wi-Fi status is read off the UI thread",
                 "[settings][settings_root]") {
    // On wpa_supplicant a status read is a control-socket round trip that can
    // wait 10s, so the panel's own thread must never make one.
    const LinkMocks m = settle_with_ethernet_down(*this);
    RestoreLinks restore{*this, m};
    m.wifi->set_connected_state(true, "TestSSID", "192.168.1.100", 75);
    m.wifi->clear_status_callers();

    get_global_settings_panel().refresh_status_lines();
    REQUIRE(wait_until([&]() { return status_text(root_, "row_connection") == "Wi-Fi TestSSID"; }));

    const auto callers = m.wifi->status_callers();
    REQUIRE_FALSE(callers.empty());
    for (const auto& id : callers) {
        CHECK(id != std::this_thread::get_id());
    }
}

TEST_CASE_METHOD(RootFixture, "settings root: a superseded refresh's Wi-Fi probe is dropped",
                 "[settings][settings_root]") {
    const LinkMocks m = settle_with_ethernet_down(*this);
    RestoreLinks restore{*this, m};

    // Every value the row takes, so a stale answer that is overwritten a
    // moment later still shows up.
    lv_subject_t* row = lv_xml_get_subject(nullptr, "settings_status_connection");
    REQUIRE(row != nullptr);
    std::vector<std::string> shown;
    lv_observer_t* recorder = lv_subject_add_observer(
        row,
        [](lv_observer_t* o, lv_subject_t* s) {
            static_cast<std::vector<std::string>*>(lv_observer_get_user_data(o))
                ->push_back(lv_subject_get_string(s));
        },
        &shown);

    // The first refresh's read answers "Stale", then parks until released.
    m.wifi->set_connected_state(true, "Stale", "192.168.1.100", 75);
    m.wifi->clear_status_callers();
    m.wifi->hold_next_status();
    get_global_settings_panel().refresh_status_lines();
    REQUIRE(wait_until([&]() { return !m.wifi->status_callers().empty(); }));

    m.wifi->set_connected_state(true, "Fresh", "192.168.1.100", 75);
    get_global_settings_panel().refresh_status_lines();
    m.wifi->release_held_status();
    REQUIRE(wait_until([&]() { return status_text(root_, "row_connection") == "Wi-Fi Fresh"; }));
    process_lvgl(50);

    lv_observer_remove(recorder);
    for (const auto& text : shown) {
        CHECK(text != "Wi-Fi Stale");
    }
}

TEST_CASE_METHOD(RootFixture, "settings root: Android shows the printer host as its connection",
                 "[settings][settings_root]") {
    // Neither backend exists on Android (wifi_backend.cpp, ethernet_backend.cpp
    // both compile to nullptr under __ANDROID__), so the row must show the
    // printer host instead of probing hardware this build has no access to.
    helix::Config* config = helix::Config::get_instance();
    const std::string host_key = config->df() + "moonraker_host";
    const std::string port_key = config->df() + "moonraker_port";
    const std::string saved_host = config->get<std::string>(host_key, "");
    const int saved_port = config->get<int>(port_key, 7125);
    config->set<std::string>(host_key, "192.168.1.42");
    config->set<int>(port_key, 7125);

    EthernetManager* before =
        SettingsPanelTestAccess::ethernet_manager(get_global_settings_panel());

    helix::set_platform_override(1); // force Android
    get_global_settings_panel().refresh_status_lines();
    helix::set_platform_override(-1);

    CHECK(status_text(root_, "row_connection") == "192.168.1.42:7125");

    // The Android branch must not construct a new EthernetManager (it would
    // otherwise log an error creating a backend the platform has no use for).
    EthernetManager* after = SettingsPanelTestAccess::ethernet_manager(get_global_settings_panel());
    CHECK(before == after);

    // No async probe was issued either: give any stray one a moment to land,
    // then confirm the status still reads the host rather than having been
    // overwritten by a Wi-Fi/Ethernet result.
    process_lvgl(50);
    CHECK(status_text(root_, "row_connection") == "192.168.1.42:7125");

    config->set<std::string>(host_key, saved_host);
    config->set<int>(port_key, saved_port);
}

TEST_CASE_METHOD(RootFixture, "settings root: Updates status reads firmware-managed",
                 "[settings][settings_root]") {
    set_int("updates_firmware_managed", 1);
    get_global_settings_panel().refresh_status_lines();
    process_lvgl(5);
    CHECK(status_text(root_, "row_updates") == "Managed by firmware");
}

TEST_CASE_METHOD(RootFixture, "settings root: refresh reads every stateful row's source",
                 "[settings][settings_root]") {
    lv_subject_t* brightness = lv_xml_get_subject(nullptr, "settings_brightness");
    lv_subject_t* sleep = lv_xml_get_subject(nullptr, "settings_display_sleep");
    lv_subject_t* has_dimming = lv_xml_get_subject(nullptr, "settings_has_dimming");
    lv_subject_t* dark_mode = lv_xml_get_subject(nullptr, "settings_dark_mode");
    lv_subject_t* time_format = lv_xml_get_subject(nullptr, "settings_time_format");
    REQUIRE(brightness != nullptr);
    REQUIRE(sleep != nullptr);
    REQUIRE(has_dimming != nullptr);
    REQUIRE(dark_mode != nullptr);
    REQUIRE(time_format != nullptr);
    const int saved_brightness = lv_subject_get_int(brightness);
    const int saved_sleep = lv_subject_get_int(sleep);
    const int saved_has_dimming = lv_subject_get_int(has_dimming);
    const int saved_dark_mode = lv_subject_get_int(dark_mode);
    const int saved_time_format = lv_subject_get_int(time_format);

    set_int("settings_brightness", 65);
    set_int("settings_display_sleep", 600);
    set_int("settings_has_dimming", 1);
    set_int("settings_dark_mode", 1);
    set_int("settings_time_format", 1);

    lv_subject_t* hw_level =
        get_printer_state().hardware_validation_state().get_hardware_status_level_subject();
    const int saved_hw_level = lv_subject_get_int(hw_level);
    lv_subject_set_int(hw_level, 1);

    get_global_settings_panel().refresh_status_lines();
    process_lvgl(5);

    CHECK(status_text(root_, "row_display") == "65% · sleep 10 min");
    CHECK(status_text(root_, "row_appearance") ==
          "Dark Mode · " + theme_manager_get_active_theme().name);
    CHECK(status_text(root_, "row_devices") == "Needs attention");
    CHECK(status_text(root_, "row_language_time") == "English · 24-hour");

    lv_subject_set_int(hw_level, saved_hw_level);
    lv_subject_set_int(brightness, saved_brightness);
    lv_subject_set_int(sleep, saved_sleep);
    lv_subject_set_int(has_dimming, saved_has_dimming);
    lv_subject_set_int(dark_mode, saved_dark_mode);
    lv_subject_set_int(time_format, saved_time_format);
}

TEST_CASE_METHOD(RootFixture, "settings root: Appearance status shows the theme's display name",
                 "[settings][settings_root]") {
    // "onedark" (filename) vs "One Dark" (name) makes the two forms
    // unambiguous — a regression back to the filename cannot pass by accident.
    const helix::ThemeData saved_theme = theme_manager_get_active_theme();
    const bool saved_dark = theme_manager_is_dark_mode();

    helix::ThemeData onedark = helix::load_theme_from_file("onedark");
    REQUIRE(onedark.is_valid());
    REQUIRE(onedark.filename == "onedark");
    REQUIRE(onedark.name == "One Dark");

    theme_manager_apply_theme(onedark, saved_dark);
    get_global_settings_panel().refresh_status_lines();
    process_lvgl(5);

    std::string status = status_text(root_, "row_appearance");
    CHECK(status.find("One Dark") != std::string::npos);
    CHECK(status.find("onedark") == std::string::npos);

    theme_manager_apply_theme(saved_theme, saved_dark);
    get_global_settings_panel().refresh_status_lines();
    process_lvgl(5);
}

TEST_CASE("SystemSettingsManager names the current language natively",
          "[settings][settings_root]") {
    CHECK_FALSE(helix::SystemSettingsManager::instance().get_language_display_name().empty());
}
