// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plan 4 Task 6 — the real HelixScreen shell booting on the K-Touch.
//
// app_boot_ui() mirrors the desktop Application startup phases (see
// src/application/application.cpp Application::run() / init_ui()) with the real
// registration/init calls, minus SDL/CLI/desktop-only seams. The canonical
// desktop order is reproduced: asset root → Config → fonts → globals.xml →
// theme → widgets → translations → XML components → core subjects →
// MoonrakerManager (ESP factory arm) → panel subjects → app shell. It runs
// once on the UI pthread (created in app_main before any network task) and
// leaves the navbar + six resident panels live on the active screen.
//
// Divergences from desktop (documented in esp32p4-task-6-report.md):
//   * Config storage is injected explicitly (set_storage) to the writable
//     /config LittleFS partition — the /assets container is read-only frogfs.
//   * fonts are registered via helix_fonts_register() (the medium-tier ESP
//     face set, link-anchored in main/CMakeLists.txt) in addition to
//     AssetManager::register_all(), which registers the full token set backed
//     by the aliases in font_aliases.cpp.
//   * Real (non-mock) builds bring WiFi up through the shared WifiBackend
//     (Task 13, wifi_backend_esp.cpp) rather than raw esp_wifi calls; the
//     shell still comes up in the not-ready/connecting UI first and the
//     Moonraker connect fires once the backend reports an IP.
//   * CONFIG_HELIX_MOCK_PRINTER drives a firmware-local synthetic PrinterState
//     driver (below), NOT the app-layer MoonrakerClientMock — that mock
//     inherits the libhv-based concrete MoonrakerClient, whose transport base
//     is intentionally undefined on ESP32 (see helixnet/helixapp shims), so it
//     cannot be constructed on-device. The synthetic driver feeds the same
//     production PrinterState::update_from_status() path the real notify stream
//     would, with zero network.

#include "app_boot.h"

#include "ui_ams_mini_status.h"
#include "ui_bed_mesh.h"
#include "ui_card.h"
#include "ui_component_header_bar.h"
#include "ui_dialog.h"
#include "ui_emergency_stop.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_keyboard_manager.h"
#include "ui_nav_manager.h"
#include "ui_notification_manager.h"
#include "ui_panel_home.h"
#include "ui_severity_card.h"
#include "ui_status_pill.h"
#include "ui_switch.h"
#include "ui_temp_display.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "asset_manager.h"
#include "config.h"
#include "config_storage.h"
#include "connection_state.h"
#include "data_root_resolver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "filament_sensor_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "helix_sparkline.h"
#include "i_moonraker_client.h"
#include "job_queue_state.h"
#include "led/led_controller.h"
#include "moonraker_api.h" // complete MoonrakerAPI : IMoonrakerAPI for the init_panels upcast
#include "moonraker_manager.h"
#include "moonraker_types.h" // FileInfo/FileMetadata/ThumbnailInfo/resolve_thumbnail_path — HTTP HIL probe
#include "panel_factory.h"
#include "pending_startup_warnings.h"
#include "printer_discovery.h" // helix::PrinterDiscovery + init_subsystems (discovery callback args)
#include "printer_fan_state.h" // helix::FanRoleConfig for the non-mock fan-role resolve
#include "printer_state.h"
#include "runtime_config.h"
#include "safety_settings_manager.h"
#include "sdkconfig.h"
#include "setting_group.h"
#include "src/xml/lv_xml.h"
#include "subject_initializer.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"
#include "tips_manager.h"
#include "tool_state.h"
#include "translation_loader.h"
#include "wizard_config_paths.h"
#include "xml_registration.h"

#include <spdlog/spdlog.h>

#include <string>

#if !CONFIG_HELIX_MOCK_PRINTER
// Non-mock (real connect) path only — WiFi bring-up (Task 13, over the shared
// WifiBackend) + Moonraker connect thread.
#include "async_lifetime_guard.h"
#include "provisioning_esp.h"
#include "wifi_backend_esp.h"
#include "wifi_manager.h"

#include <atomic>
#include <pthread.h>
#endif // !CONFIG_HELIX_MOCK_PRINTER

#if CONFIG_HELIX_MOCK_PRINTER
// Task 15 R3: AmsBackendMock has no dependency on the concrete libhv
// MoonrakerClient (unlike moonraker_client_mock.cpp etc. — see the MOCK_SRCS
// comment in CMakeLists.txt), so it links cleanly here. Included only in mock
// builds (CMakeLists.txt's MOCK_SRCS).
#include "ams_backend_mock.h"
#endif // CONFIG_HELIX_MOCK_PRINTER

// Defined in the main component (main/font_registration.c). Declared locally
// rather than via its header: that header lives under main/, which cannot be
// on helixapp's include path (main REQUIRES helixapp — the reverse include
// would be circular). The symbol resolves at the final whole-image link, and
// is kept alive by -Wl,--undefined=helix_fonts_register in main/CMakeLists.txt.
extern "C" void helix_fonts_register(void);
extern "C" void helix_fonts_log_summary(void);

static const char* TAG = "app_boot";

namespace {

// The MoonrakerManager owns the client + API for the process lifetime. Held at
// file scope so app_boot_tick() can pump its notification/timeout queues from
// the render loop. Set once app_boot_ui() completes; null before then.
MoonrakerManager* g_manager = nullptr;

// One-shot boot heap milestone. heap_caps_get_largest_free_block() walks the
// heap in a critical section, so this is called only at discrete boot
// milestones — never from the steady-state render loop (see the audit's
// log_heap vs log_heap_fast note).
void log_heap_milestone(const char* stage) {
    ESP_LOGI(TAG, "[heap:%s] internal free=%u largest=%u | psram free=%u largest=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

// Task 12 R2: Config (settings.json, the `cfg` partition) is the source of
// truth for the Moonraker host/port — same schema desktop uses
// (df()+"moonraker_host" / df()+"moonraker_port", see src/system/config.cpp
// and src/ui/ui_change_host_modal.cpp). CONFIG_HELIX_HIL_MOONRAKER_URL is only
// the FIRST-BOOT seed for that schema: a full "ws://host:port/path" string
// (Kconfig's format), parsed once when Config has no value yet. Split out as
// its own struct/function (rather than reusing ws_to_http_base, which is
// scheme+path only) because Config's two keys need host and port separated.
struct HostPort {
    std::string host;
    int port;
};

HostPort parse_moonraker_kconfig_url(const std::string& url) {
    std::string working = url;
    size_t scheme_end = working.find("://");
    if (scheme_end != std::string::npos) {
        working = working.substr(scheme_end + 3);
    }
    size_t path_start = working.find('/');
    if (path_start != std::string::npos) {
        working = working.substr(0, path_start);
    }
    size_t colon_pos = working.rfind(':');
    if (colon_pos == std::string::npos) {
        return {working, 7125};
    }
    std::string host = working.substr(0, colon_pos);
    int port = 7125;
    try {
        port = std::stoi(working.substr(colon_pos + 1));
    } catch (const std::exception&) {
        port = 7125;
    }
    return {host, port};
}

// Register the custom C++ widgets the XML components extend. Same set as the
// desktop register_widgets() phase (application.cpp:1517) minus the subsystems
// gated off for v1 (camera/gcode-3D/etc.). ui_gcode_viewer_register() resolves
// to the no-op #else branch in ui_gcode_viewer.cpp on ESP (HELIX_HAS_GCODE_
// VIEWER=0) — it still registers a stub <gcode_viewer> widget so XML that names
// it parses.
void register_widgets() {
    ui_icon_register_widget();
    ui_status_pill_register_widget();
    ui_switch_register();
    ui_card_register();
    setting_group_register();
    ui_temp_display_init();
    ui_ams_mini_status_init();
    ui_severity_card_register();
    ui_dialog_register();
    ui_bed_mesh_register();
    ui_gcode_viewer_register();
    ui_gradient_canvas_register();
    helix::ui::register_helix_sparkline_widget();
    ui_component_header_bar_init();
}

// Build the app shell: app_layout.xml instantiates the navbar and all six
// panels resident-and-hidden (the desktop memory model), then PanelFactory
// finds + wires them. Mirrors Application::init_ui() (application.cpp:1721).
// Returns false on any structural failure (logged).
bool build_shell() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* app_layout = static_cast<lv_obj_t*>(lv_xml_create(screen, "app_layout", nullptr));
    if (!app_layout) {
        spdlog::error("app_boot: app_layout XML create FAILED");
        return false;
    }
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_update_layout(screen);
    NavigationManager::instance().set_app_layout(app_layout);

    lv_obj_t* navbar = lv_obj_find_by_name(app_layout, "navbar");
    lv_obj_t* content_area = lv_obj_find_by_name(app_layout, "content_area");
    if (!navbar || !content_area) {
        spdlog::error("app_boot: navbar/content_area not found in app_layout");
        return false;
    }
    NavigationManager::instance().wire_events(navbar);

    lv_obj_t* panel_container = lv_obj_find_by_name(content_area, "panel_container");
    if (!panel_container) {
        spdlog::error("app_boot: panel_container not found");
        return false;
    }
    static helix::PanelFactory panels;
    if (!panels.find_panels(panel_container)) {
        spdlog::error("app_boot: find_panels FAILED");
        return false;
    }
    panels.setup_panels(screen);
    get_global_home_panel().finalize_setup();
    return true;
}

#if CONFIG_HELIX_MOCK_PRINTER
// Firmware-local synthetic printer, gated behind CONFIG_HELIX_MOCK_PRINTER.
// Feeds oscillating temperatures through the same production status-apply path
// (PrinterState::update_from_status) the real Moonraker notify stream uses, so
// the home panel shows live-looking data with no network and no printer. This
// is the ESP substitute for the desktop MoonrakerClientMock (unconstructible
// here — see file header).
void mock_seed_ready() {
    helix::PrinterState& ps = get_printer_state();
    ps.set_klippy_state_sync(helix::KlippyState::READY);
    ps.set_printer_connection_state(static_cast<int>(helix::ConnectionState::CONNECTED),
                                    "Mock printer");
    // Seed a printer identity so the home printer-image widget resolves to a
    // bundled image. The widget reads config PRINTER_TYPE at attach() (during
    // build_shell); without an identity get_best_printer_image("") returns the
    // generic fallback. "Voron 2.4" maps to the staged voron-v2.png. This is why
    // the seed must run BEFORE the shell builds (see call site).
    helix::Config* config = helix::Config::get_instance();
    if (config) {
        config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "Voron 2.4");
    }
    spdlog::info("app_boot: mock printer seeded READY/CONNECTED + type Voron 2.4");
}

// Task 15 R3: mirrors desktop's HELIX_MOCK_AMS=multi (src/printer/ams_backend.cpp
// try_create_mock()) — but that path is gated on RuntimeConfig::should_mock_ams(),
// which requires test_mode, deliberately never set on ESP (see the NOTE in
// app_boot_ui() below: test_mode would route MoonrakerManager through the
// unlinkable app-layer mock arms). So this constructs AmsBackendMock directly,
// bypassing the factory, and installs it as AmsState's active backend the same
// way a real backend would install via AmsState::init_backends_from_hardware().
// set_multi_unit_mode(true) alone (no HELIX_MOCK_AMS_STATE scenario) never
// spawns AmsBackendMock's internal scenario_thread_ — only "loading"/"bypass"
// initial-state scenarios do that — so this stays thread-free, matching R1's
// "no new BG surfaces" constraint.
void mock_seed_ams() {
    auto backend = std::make_unique<AmsBackendMock>(4);
    backend->set_multi_unit_mode(true);
    backend->start();
    AmsState::instance().set_backend(std::move(backend));
    AmsState::instance().sync_from_backend();
    spdlog::info("app_boot: mock AMS seeded (multi-unit)");
}

void mock_push_temps() {
    static int t = 0;
    ++t;
    // Jitter both heaters WITHIN TEMP_TOLERANCE (2.0C) of their targets so the
    // heating-icon animator stays AT_TARGET and never starts its infinite pulse.
    // A wider sweep that crosses (target - tolerance) toggles HEATING<->AT_TARGET
    // and repaints the icon at frame rate for half of every cycle — millions of
    // idle repaints that tear on the unpaced ESP32 blit. Nozzle 214-216 (target
    // 215, threshold 213), bed 58-62 (target 60, threshold 58): both stay above
    // threshold, readout still visibly jitters, animator stays quiet.
    double nozzle = 214.0 + (t % 3);
    double bed = 58.0 + (t % 5);
    nlohmann::json status = {
        {"extruder", {{"temperature", nozzle}, {"target", 215.0}}},
        {"heater_bed", {{"temperature", bed}, {"target", 60.0}}},
    };
    get_printer_state().update_from_status(status);
}
#endif // CONFIG_HELIX_MOCK_PRINTER

#if !CONFIG_HELIX_MOCK_PRINTER
// ---------------------------------------------------------------------------
// Real Moonraker connect path (Task 8) — non-mock builds.
//
// Two halves: (1) setup_discovery_callbacks_esp() registers the ESP consumer of
// the Task 7 discovery chain on the manager's EspMoonrakerClient; (2)
// app_net_start() brings WiFi up and kicks MoonrakerManager::connect(). Both are
// the on-device stand-in for Application (excluded on ESP): without (1) the
// discovery callbacks fire into a void and no subject updates; without (2) the
// client never connects (WiFi was deferred to "Task 13" in Stage A).
// ---------------------------------------------------------------------------

#if CONFIG_HELIX_HTTP_HIL
// One-shot Task 10 HTTP-lane self-test (R5). Lists the gcodes root, fetches
// metadata for the first file found, and pulls its best thumbnail through the
// HTTP lane (ITransfersAPI::download_file_partial — MoonrakerFileTransferAPI's
// real ESP32 implementation, esp_rest_api.cpp), logging the resolved path,
// dimensions, byte count, and heap so the controller can flash-and-verify on
// real hardware without writing any code. Fire-and-forget: nothing here
// blocks boot or the shell, and a failure at any step just logs and stops
// (no retry — this is a manual verification aid, not production behavior).
void run_http_hil_probe(MoonrakerManager* mgr) {
    IMoonrakerAPI* api = mgr->api();
    if (!api) {
        ESP_LOGW(TAG, "[http_hil] no API available — skipping probe");
        return;
    }

    api->files().get_directory(
        "gcodes", "",
        [api](const std::vector<FileInfo>& entries) {
            const FileInfo* first_file = nullptr;
            for (const auto& e : entries) {
                if (!e.is_dir) {
                    first_file = &e;
                    break;
                }
            }
            if (!first_file) {
                ESP_LOGW(TAG, "[http_hil] no gcode files in root — nothing to probe");
                return;
            }

            std::string filename = first_file->filename;
            ESP_LOGI(TAG, "[http_hil] fetching metadata for %s", filename.c_str());

            api->files().get_file_metadata(
                filename,
                [api, filename](const FileMetadata& meta) {
                    const ThumbnailInfo* thumb = meta.get_best_thumbnail(160, 160);
                    if (!thumb) {
                        ESP_LOGW(TAG, "[http_hil] %s has no thumbnails — nothing to fetch",
                                 filename.c_str());
                        return;
                    }
                    std::string thumb_path = resolve_thumbnail_path(thumb->relative_path, "");
                    ESP_LOGI(TAG, "[http_hil] fetching thumbnail %s (%dx%d, metadata-reported)",
                             thumb_path.c_str(), thumb->width, thumb->height);

                    api->transfers().download_file_partial(
                        "gcodes", thumb_path, 512 * 1024,
                        [thumb_path](const std::string& content) {
                            ESP_LOGI(TAG,
                                     "[http_hil] OK: %s -> %u bytes | heap internal=%u psram=%u",
                                     thumb_path.c_str(), (unsigned)content.size(),
                                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                        },
                        [thumb_path](const MoonrakerError& err) {
                            ESP_LOGE(TAG, "[http_hil] FAILED: %s -> %s", thumb_path.c_str(),
                                     err.message.c_str());
                        });
                },
                [filename](const MoonrakerError& err) {
                    ESP_LOGE(TAG, "[http_hil] get_file_metadata(%s) failed: %s", filename.c_str(),
                             err.message.c_str());
                });
        },
        [](const MoonrakerError& err) {
            ESP_LOGE(TAG, "[http_hil] get_directory failed: %s", err.message.c_str());
        });
}
#endif // CONFIG_HELIX_HTTP_HIL

// Mirror Application::setup_discovery_callbacks() (src/application/application.cpp:2478),
// TRIMMED to the v1 Core+AMS cut. Both callbacks fire on the WebSocket task, so
// every subject write is marshalled to the UI thread via ui_queue_update().
//
// Trimmed vs the desktop handler:
//   * on_hardware_discovered does NOT call init_subsystems_from_hardware()
//     (src/printer/printer_discovery.cpp is excluded from the ESP image — see
//     app_srcs.txt), which on desktop wires AMS backends, LED/probe/width/tool-
//     changer state, Spoolman, printer-name sync and standard macros. None of
//     that is in the Task 8 cut. We init only the temperature-sensor subjects so
//     sensor cards populate.
//   * on_discovery_complete drops: splash exit, self-restart sentinel cleanup,
//     temperature-store history seed, LED-chip population, print-hours /
//     timelapse / external-update method callbacks, PrinterDetector auto-detect,
//     heater-role autoheal, HardwareValidator, power/sensor REST subscribe.
//     Kept: hardware into PrinterState, fan + extruder subject init, klipper /
//     moonraker version, and the initial-status dispatch — the load-bearing
//     "live temps on the home panel" path.
void setup_discovery_callbacks_esp(MoonrakerManager& manager) {
    helix::IMoonrakerClient* client = manager.client();
    if (!client) {
        spdlog::error("app_boot: no Moonraker client — discovery callbacks not registered");
        return;
    }

    client->set_on_hardware_discovered([](const helix::PrinterDiscovery& hardware) {
        // Copy on the BG thread so the queued main-thread callback owns a stable,
        // non-aliased snapshot (desktop #761/#789 lesson).
        auto snapshot = std::make_shared<helix::PrinterDiscovery>(hardware);
        helix::ui::queue_update("app_boot::on_hardware_discovered", [snapshot]() {
            helix::sensors::TemperatureSensorManager::instance().discover(snapshot->sensors());
        });
    });

    MoonrakerManager* mgr = &manager;
    client->set_on_discovery_complete(
        [mgr](const helix::PrinterDiscovery& hardware, const nlohmann::json& initial_status) {
            spdlog::debug("[app_boot] on_discovery_complete BG entry (status keys: {})",
                          initial_status.is_object() ? initial_status.size() : 0);
            auto snapshot = std::make_shared<helix::PrinterDiscovery>(hardware);
            auto status_snapshot = std::make_shared<const nlohmann::json>(initial_status);
            helix::ui::queue_update("app_boot::on_discovery_complete", [mgr, snapshot,
                                                                        status_snapshot]() {
                helix::PrinterState& ps = get_printer_state();

                // Hardware into PrinterState first — init_fans / init_extruders
                // build their subjects from it, and set_hardware seeds the
                // capability flags the home/motion panels read.
                ps.set_hardware(*snapshot);

                const auto& fans = snapshot->fans();
                ps.init_fans(fans,
                             helix::FanRoleConfig::from_config(helix::Config::get_instance(), fans),
                             snapshot->fan_max_power());
                ps.init_extruders(snapshot->heaters());

                ps.set_klipper_version(snapshot->software_version());
                ps.set_moonraker_version(snapshot->moonraker_version());

                IMoonrakerAPI* api = mgr->api();
                helix::IMoonrakerClient* c = mgr->client();

                // Task 15 R1: AMS-relevant subset of desktop's
                // init_subsystems_from_hardware() (src/printer/printer_discovery.cpp,
                // excluded from the ESP image) — backend construction, filament
                // sensors, tool state. Runs here (after the fan/extruder subjects
                // above, before the dispatch below) to keep the same "subjects
                // before dispatch" invariant Task 8 established. LED, standard
                // macros, probe/humidity/width sensors, and camera-adjacent
                // subsystems stay deferred (Task 8 review's enumeration).
                AmsState::instance().init_backend_from_hardware(*snapshot, api, c);
                if (snapshot->has_filament_sensors()) {
                    auto& fsm = helix::FilamentSensorManager::instance();
                    fsm.discover_sensors(snapshot->filament_sensor_names());
                    fsm.load_config_from_file();
                }
                helix::ToolState::instance().init_tools(*snapshot);
                helix::ToolState::instance().load_spool_assignments(api);

                // Dispatch the initial subscription status LAST, after the
                // fan/sensor/extruder/AMS subjects exist. dispatch_status_update
                // wraps it in a notify_status_update envelope and fans out to
                // MoonrakerManager's notify handler → notification queue →
                // process_notifications() (pumped from app_boot_tick) →
                // update_from_status() + ToolState — exactly the path an
                // inbound live notification takes. Same call the desktop
                // handler makes (application.cpp:2593).
                // Flagged as a cached snapshot: it was captured when the subscribe
                // response landed, and live WebSocket frames have been flowing ever
                // since, so it must not regress a liveness signal it predates.
                if (c && status_snapshot->is_object() && !status_snapshot->empty()) {
                    c->dispatch_status_update(*status_snapshot, /*from_cached_snapshot=*/true);
                }

                spdlog::info("[app_boot] discovery applied: {} heaters, {} fans, {} sensors, "
                             "{} initial-status keys",
                             snapshot->heaters().size(), snapshot->fans().size(),
                             snapshot->sensors().size(),
                             status_snapshot->is_object() ? status_snapshot->size() : 0);

#if CONFIG_HELIX_HTTP_HIL
                run_http_hil_probe(mgr);
#endif
            });
        });

    spdlog::info("[app_boot] discovery callbacks registered (real connect path)");
}

// ws://host:port/path -> http://host:port  (best-effort HTTP base for the API;
// Task 10's HTTP lane exercises this for print-select thumbnail/gcode-header
// fetches via download_file_partial — jog and macros still round-trip over
// the WebSocket JSON-RPC channel and never touch this base URL).
std::string ws_to_http_base(const std::string& ws_url) {
    std::string url = ws_url;
    if (url.rfind("ws://", 0) == 0) {
        url = "http://" + url.substr(5);
    } else if (url.rfind("wss://", 0) == 0) {
        url = "https://" + url.substr(6);
    }
    // Strip a trailing "/websocket" (or any path) — the HTTP base is scheme+host.
    size_t scheme_end = url.find("://");
    size_t path = url.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
    if (path != std::string::npos) {
        url = url.substr(0, path);
    }
    return url;
}

// Task 13: process-lifetime guard for the state-observer callback below.
// app_net_start()'s pthread is the only thread that registers against it; the
// observer itself fires from WiFiManager::notify_state_observers(), called
// from whatever context the esp_wifi backend reports CONNECTED/DISCONNECTED
// on (see wifi_backend_esp.cpp), and defers via the token to the UpdateQueue
// — drained by app_boot_tick() on the render loop, never back onto the net
// thread. Never invalidated (this boot flow has no owning object to tear
// down), matching net_hil.cpp's "process-lifetime singleton, leaked on
// purpose" precedent for the same reason.
helix::AsyncLifetimeGuard s_net_lifetime;
std::atomic<bool> s_moonraker_connect_kicked{false};

// One-shot handoff to the Moonraker connect, callable from either the bounded
// wait below or the state observer that resolves a later connection. The
// atomic exchange guarantees mgr->connect() fires exactly once regardless of
// which caller wins the race.
void kick_moonraker_connect_once() {
    bool expected = false;
    if (!s_moonraker_connect_kicked.compare_exchange_strong(expected, true)) {
        return;
    }
    MoonrakerManager* mgr = g_manager;
    if (!mgr) {
        ESP_LOGE(TAG, "app_net: no MoonrakerManager — cannot connect");
        return;
    }
    // Task 12 R2: read the effective host/port from Config, not Kconfig
    // directly — app_boot_ui()'s Phase 1 seed guarantees a value is present
    // (either the user's saved Host or the first-boot Kconfig default) by the
    // time this runs (app_net_start() is called last, after Phase 1).
    helix::Config* config = helix::Config::get_instance();
    std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
    int port = config->get<int>(config->df() + "moonraker_port", 7125);
    // No host yet (empty Kconfig seed, nothing saved in Settings): connecting to
    // "ws://:7125/websocket" would hand the websocket client an unresolvable URL
    // and spin the auto-reconnect loop forever. Leave the not-ready UI up
    // instead; ChangeHostModal connects directly once a host is entered, so no
    // reboot is needed. Logged once — the one-shot latch above is already taken.
    if (host.empty()) {
        ESP_LOGI(TAG, "app_net: no Moonraker host configured — set it in Settings");
        return;
    }
    std::string ws_url = "ws://" + host + ":" + std::to_string(port) + "/websocket";
    std::string http_base = ws_to_http_base(ws_url);
    ESP_LOGI(TAG, "app: connecting Moonraker (%s)", ws_url.c_str());
    // Async: connect() starts the WebSocket client task and returns. on_connected
    // → MoonrakerManager::connect()'s discover_printer() → the callbacks
    // registered in setup_discovery_callbacks_esp(), all on the WS task.
    mgr->connect(ws_url, http_base);
}

// R4: bounded wait for the FIRST post-boot association, replacing the old
// portMAX_DELAY park (which held this thread's 32KB stack forever against a
// never-associating network — the Task 9 backlog item now due). 20s covers
// the historical successful-assoc case with margin; the backend's own
// assoc-timeout + bounded backoff retry (wifi_backend_esp.cpp) keep trying
// underneath regardless of whether this wait succeeds.
constexpr int BOOT_BOUNDED_WAIT_MS = 20000;
constexpr int BOOT_POLL_INTERVAL_MS = 200;

void* app_net_thread_main(void*) {
    // Opens the esp_wifi hardware bring-up gate — see wifi_backend_esp.h for
    // why this must happen from THIS pthread specifically (THE PATTERN:
    // >=32KB internal alloc claimed only here, after app_boot_ui()'s
    // internal-DRAM gates), not from whatever thread happens to call
    // get_wifi_manager() first (e.g. NetworkWidget's ctor during the earlier,
    // heavy build_shell() phase).
    helix::wifi_backend_esp_allow_hardware_bringup();

    auto wifi = helix::get_wifi_manager();

    // Register the handoff BEFORE waiting: a CONNECTED that lands after the
    // bounded wait below (weak signal, slow AP) still triggers the Moonraker
    // connect exactly once, with no thread parked waiting for it.
    wifi->add_state_observer(s_net_lifetime.token(), [wifi]() {
        if (wifi->is_connected()) {
            kick_moonraker_connect_once();
        }
    });

    // The singleton may already exist — constructed earlier (gate closed) by
    // some other get_wifi_manager() caller, in which case its start_async()
    // was a harmless no-op. Kick a real bring-up attempt now that the gate is
    // open; idempotent if this call IS the first construction (the ctor
    // already invoked start_async() once).
    wifi->retry_async();

    // Task 14: out-of-box case. retry_async() -> WifiBackendEsp::start() just
    // ran load_or_seed_credentials(), so provisioning_needed() below sees the
    // post-seed state (a dev sdkconfig.local Kconfig SSID, if any, has already
    // been written to NVS) rather than a never-started snapshot. If NVS still
    // has no stored SSID, stand up the SoftAP captive portal instead of
    // silently idling for BOOT_BOUNDED_WAIT_MS below — the shell is already up
    // (this thread starts after build_shell()), so the not-ready UI is on
    // screen throughout and the portal's own instructions modal explains what
    // to do. provisioning_run_portal() blocks until either a join succeeds
    // (through this same WiFiManager) or the user dismisses back to Settings >
    // Network; either way the bounded wait below resolves instantly
    // afterward (is_connected() is already true on the join path).
    if (helix::provisioning_needed()) {
        helix::provisioning_run_portal();
    }

    for (int waited_ms = 0; waited_ms < BOOT_BOUNDED_WAIT_MS; waited_ms += BOOT_POLL_INTERVAL_MS) {
        if (wifi->is_connected()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
    }

    if (wifi->is_connected()) {
        kick_moonraker_connect_once();
    } else {
        ESP_LOGW(TAG,
                 "app_net: no association after %dms — UI stays not-ready; backend keeps "
                 "retrying in the background",
                 BOOT_BOUNDED_WAIT_MS);
    }
    // Handoff is either done above or deferred to the state observer — exit
    // either way, freeing this thread's 32KB stack (R4: no permanent park).
    return nullptr;
}

// Spawn the connect thread. Called at the END of app_boot_ui() (home panel up),
// so the pthread stack — the only >=32KB internal allocation on this path — is
// claimed AFTER the boot's internal-DRAM gates and BEFORE esp_wifi_start()
// (which runs inside the thread). pthread-created + detached, mirroring net_hil
// (which documented an ENOMEM near-miss when a net thread spawned after WiFi).
void app_net_start() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t thread;
    int rc = pthread_create(&thread, &attr, app_net_thread_main, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        ESP_LOGE(TAG, "app_net: pthread_create failed: %d — no live connection", rc);
    }
}
#endif // !CONFIG_HELIX_MOCK_PRINTER

} // namespace

// Cooperative yield for the long synchronous boot build. The UI pthread runs at
// a higher priority than the idle task; a multi-second uninterrupted stretch of
// XML registration / panel layout starves idle, and the Task WDT (which watches
// idle) fires (TG1WDT_SYS_RST). Shared boot code (register_xml_components,
// PanelFactory::setup_panels) calls this between units of work — one tick of
// vTaskDelay lets idle run and feed the WDT. Cheap (~1 tick each) and correct
// regardless of how long the boot build takes. Declared extern "C" so the
// main-tree callers can reference it under their HELIX_PLATFORM_ESP32 gate
// without pulling in FreeRTOS headers.
extern "C" void helix_boot_yield(void) {
    vTaskDelay(1);
}

// Set by main/ during display bring-up, which probes the GT911 before this
// runs. Defaults to true so any build that never reports stays silent rather
// than warning about touch it never checked.
static bool s_touch_available = true;

extern "C" void app_boot_set_touch_available(bool available) {
    s_touch_available = available;
}

extern "C" void app_boot_ui(void) {
    log_heap_milestone("boot-ui-start");

    // Phase 1: asset root + writable config storage. The packed /assets frogfs
    // container is read-only; settings live on the /config LittleFS partition.
    helix::set_asset_root("/assets");
    // get_data_dir() honors $HELIX_DATA_DIR; without this it returns the relative
    // "." — meaningless on a CWD-less VFS. The read-only seed reads that go
    // through get_data_dir() (find_readable's "<data>/assets/config/" seed used
    // by e.g. PrinterDetector's printer_database.json) then resolve under the
    // /assets mount (→ /assets/assets/config/...). Matches set_asset_root above.
    setenv("HELIX_DATA_DIR", "/assets", 1);
    // get_user_config_dir() honors $HELIX_CONFIG_DIR; without this it returns
    // the relative "config", which is meaningless on a CWD-less VFS (theme
    // loader, env backups). Must point at the writable partition.
    setenv("HELIX_CONFIG_DIR", "/config", 1);
    helix::Config* config = helix::Config::get_instance();
    config->set_storage(helix::make_file_config_storage("/config/settings.json"));
    config->init("/config/settings.json");

    // Task 12 R2: first-boot-only Moonraker host/port seed. If settings.json
    // already has a value (any boot after the user has edited Host in Settings,
    // or a prior first-boot seed), leave it untouched — the Kconfig value must
    // never override a user-set value. Seeding here (before any UI/subject
    // reads Config) means the Settings > System > Host row and the real
    // connect path (app_net_start(), below) both see a consistent value from
    // their very first read.
    // The Kconfig URL is empty in the committed tree (a bench address is
    // site-local, supplied through sdkconfig.local), so skip the seed entirely
    // rather than writing a blank host every boot — a blank host would build
    // "ws://:7125/websocket" and feed the reconnect churn loop forever.
    if (config->get<std::string>(config->df() + "moonraker_host", "").empty()) {
        HostPort seed = parse_moonraker_kconfig_url(CONFIG_HELIX_HIL_MOONRAKER_URL);
        if (seed.host.empty()) {
            ESP_LOGI(TAG, "app_boot: no Moonraker host configured — set it in Settings");
        } else {
            config->set(config->df() + "moonraker_host", seed.host);
            config->set(config->df() + "moonraker_port", seed.port);
            config->save();
            ESP_LOGI(TAG, "app_boot: seeded first-boot Moonraker host from Kconfig default (%s:%d)",
                     seed.host.c_str(), seed.port);
        }
    }

    // Phase 2: RuntimeConfig from build config (no CLI). g_runtime_config is a
    // process-global; MoonrakerManager + SubjectInitializer read it back.
    RuntimeConfig& rc = *get_runtime_config();
#if CONFIG_HELIX_MOCK_PRINTER
    // NOTE: we deliberately do NOT set rc.test_mode here. test_mode would route
    // MoonrakerManager through the app-layer mock arms, which construct the
    // libhv-based MoonrakerClientMock/MoonrakerAPIMock — unlinkable on ESP32.
    // The synthetic driver above provides mock data instead.
#endif

    // Phase 3: UI update queue (registers its high-priority drain timer), then
    // fonts BEFORE theme init. helix_fonts_register() is the link-anchored
    // medium-tier face set; AssetManager::register_all() adds the full token
    // set (aliased) + images. Both must precede globals.xml/theme — the
    // responsive font registrar hard-aborts on an unresolved font token.
    helix::ui::update_queue_init();
    helix_fonts_register();
    AssetManager::register_all();

    // Phase 4: globals scope (theme consts, font tokens) then theme init.
    std::string globals = "A:" + helix::asset_path("ui_xml/globals.xml");
    if (lv_xml_register_component_from_file(globals.c_str()) != LV_RESULT_OK) {
        spdlog::error("app_boot: globals.xml register FAILED ({})", globals);
    }
    theme_manager_init(lv_display_get_default(), true);
    log_heap_milestone("theme-up");

    // Phase 5: custom widgets.
    register_widgets();

    // Phase 6: translations for the active locale (before XML create — layout
    // + bindings resolve lv_tr() strings).
    std::string lang = config->get_language();
    helix::ui::ensure_translation_loaded(lang);
    lv_translation_set_language(lang.c_str());
    log_heap_milestone("translations-up");

    // Phase 7: all XML components from the /assets container. This is the
    // heaviest boot phase (~300 component templates: frogfs read + decompress +
    // expat parse each) — the milestone brackets it so the HIL profile can
    // separate registration cost from panel-build cost (subjects-up → home).
    helix::register_xml_components();
    log_heap_milestone("xml-registered");

    // Notification badge click: the real handler opens the NotificationHistory
    // panel, which is excluded from the v1 ESP cut (its accessor isn't linked —
    // notification_register_callbacks() would drag in the excluded panel). The
    // badge still exists on the home widget, so register a no-op for its event
    // (BEFORE app_layout XML is created in build_shell) to silence the
    // "callback not found" warning; opening history is a later stage.
    lv_xml_register_event_cb(nullptr, "status_notification_history_clicked", [](lv_event_t*) {});

    // Phase 8: core subjects (PrinterState / AmsState).
    static SubjectInitializer subjects;
    subjects.init_core_and_state();

    // Bring LedController up with no API yet so its `led_controllable` and
    // `led_command_in_flight` subjects are registered for XML before the
    // home/print-status panels instantiate in build_shell() — print_status_panel
    // and panel_widget_led bind both. Registration is scope-sensitive, so this
    // has to sit exactly here, matching desktop (application.cpp, same call and
    // same phase). This is the only LedController::init() the ESP image ever
    // makes: the re-init that binds a real API lives in printer_discovery.cpp,
    // which is excluded from the image, and setup_discovery_callbacks_esp()
    // below does not wire LED. api_/client_ therefore stay null for the life of
    // the process — the call registers subjects, it does not enable LED control.
    helix::led::LedController::instance().init(nullptr, nullptr);

    // Phase 9: MoonrakerManager — ESP factory arm builds EspMoonrakerClient +
    // the real MoonrakerAPI over it. init() creates client + API; it does NOT
    // connect (WiFi is Task 13). In mock builds the client stays idle and the
    // synthetic driver supplies data.
    static MoonrakerManager manager;
    manager.init(rc, config);
    set_moonraker_manager(&manager);
    g_manager = &manager;
#if !CONFIG_HELIX_MOCK_PRINTER
    // Register the ESP consumer of the Task 7 discovery chain before any connect
    // can fire it. Harmless when app_net_start() below is gated off (NET_HIL
    // owns the network): the callbacks just never run because manager never
    // connects.
    setup_discovery_callbacks_esp(manager);
#endif

    // Phase 10: panel subjects, now that the API pointer exists.
    subjects.init_panels(manager.api(), rc);
    subjects.init_post(rc);

    // E-STOP and smart print cancellation. Mirrors desktop's
    // Application::init_panel_subjects() (application.cpp, same order). Both
    // singletons had their subjects registered by init_panels() above but their
    // API/PrinterState pointers left null, because the desktop-only
    // application.cpp is the tree's sole init() call site — so every
    // emergency_stop() bailed out at the `!api_` guard and the estop_visible
    // subject, which nine XML files bind as a visibility flag, never left 0.
    // create() installs observers on print/klippy state and early-returns unless
    // init() has run and subjects exist, so this ordering is required, and all of
    // it must precede build_shell() below.
    EmergencyStopOverlay::instance().init(get_printer_state(), manager.api());
    EmergencyStopOverlay::instance().create();
    EmergencyStopOverlay::instance().set_require_confirmation(
        helix::SafetySettingsManager::instance().get_estop_require_confirmation());

    helix::AbortManager::instance().init(manager.api(), &get_printer_state());

    // Job queue state — owns the `job_queue_count` subject the home panel's
    // queue widget binds, so it must construct before build_shell(). Mirrors
    // desktop's Application::init_moonraker() (construct, init_subjects, publish
    // through the global accessor). Function-static like the manager above: it
    // lives for the process and is never destroyed on this platform.
    static JobQueueState job_queue(manager.api(), manager.client());
    job_queue.init_subjects();
    set_job_queue_state(&job_queue);
    log_heap_milestone("subjects-up");

    // Global software keyboard — one shared lv_keyboard, hidden until a
    // registered textarea gains focus. Mirrors desktop's
    // Application::init_moonraker() call (application.cpp:1875). Without this,
    // KeyboardManager::register_textarea() is a silent no-op (keyboard_ ==
    // nullptr guard) and no textarea on the device ever raises the keyboard —
    // Change Printer Host, WiFi join password, and provisioning fallback all
    // depend on it. show() move-foregrounds itself, so creating it before
    // build_shell() below is z-order safe.
    KeyboardManager::instance().init(lv_screen_active());

    // Notification + toast systems. Mirrors desktop's Application::init_ui()
    // (application.cpp: notification_manager_init(), ToastManager::init(), then
    // the startup-warning drain). Neither needs a parent widget — the toast
    // stack is created lazily on first show() — but both must run before any
    // panel can raise a message, i.e. before build_shell() below. Without them
    // every ToastManager::show() on the device is silently dropped, which is
    // how error feedback (failed gcode, connection loss, E-STOP) went missing.
    helix::ui::notification_manager_init();
    ToastManager::instance().init();

    // A touch controller that failed to probe no longer aborts boot, so the
    // only thing telling the user why the panel is unresponsive is this
    // warning. Enqueued immediately before the drain below so it goes out
    // through the same toast path as the pre-UI backend warnings.
    if (!s_touch_available) {
        helix::PendingStartupWarnings::instance().enqueue(
            helix::PendingStartupWarnings::Severity::ERROR,
            "Touchscreen not detected - display only");
    }

    // init() does NOT drain the queue: warnings enqueued during pre-UI boot
    // (display/asset backends) stay stranded unless drained explicitly.
    helix::PendingStartupWarnings::instance().drain(
        [](helix::PendingStartupWarnings::Severity sev, const std::string& msg) {
            ToastSeverity toast_sev = ToastSeverity::INFO;
            switch (sev) {
            case helix::PendingStartupWarnings::Severity::INFO:
                toast_sev = ToastSeverity::INFO;
                break;
            case helix::PendingStartupWarnings::Severity::SUCCESS:
                toast_sev = ToastSeverity::SUCCESS;
                break;
            case helix::PendingStartupWarnings::Severity::WARNING:
                toast_sev = ToastSeverity::WARNING;
                break;
            case helix::PendingStartupWarnings::Severity::ERROR:
                toast_sev = ToastSeverity::ERROR;
                break;
            }
            ToastManager::instance().show(toast_sev, msg.c_str(), 8000);
        });

#if CONFIG_HELIX_MOCK_PRINTER
    // Before the shell builds: seed READY/CONNECTED + the printer identity so the
    // home printer-image widget reads a resolvable PRINTER_TYPE at attach().
    mock_seed_ready();
    // Task 15 R3: seed a multi-unit AMS backend so the AMS panels have slots/
    // colors to render in mock builds (AmsState::init_subjects(), which already
    // ran in Phase 8 above, left backends_ empty on ESP — see mock_seed_ams()).
    mock_seed_ams();
#endif

    // Task 12 R3: tips database. Must run before build_shell() below — the home
    // panel's TipsWidget calls TipsManager::get_random_unique_tip() from
    // attach(), which fires as soon as app_layout's XML is created. Mirrors
    // desktop's Application::init_ui() call (application.cpp). ESP32 boot never
    // called this (root cause of "No tips available for unique selection" on
    // every boot — NOT a staging/path bug: printing_tips.json is staged and
    // find_readable() resolves it correctly under /assets/assets/config/, the
    // init() call itself was simply missing).
    if (!helix::TipsManager::get_instance()->init(helix::find_readable("printing_tips.json"))) {
        spdlog::warn("app_boot: Failed to initialize tips manager");
    }

    // Phase 11: the app shell (navbar + six resident panels).
    if (!build_shell()) {
        spdlog::error("app_boot: shell build failed — UI incomplete");
        return;
    }

    ESP_LOGI(TAG, "helix: home panel up");
    log_heap_milestone("home-panel-up");
    // The .bin font loads at ~2s print into the WiFi RF-cal serial dead window
    // (CH340 drops off USB); re-log them here where serial is reliable.
    helix_fonts_log_summary();

    // Settle-heal: one full-screen repaint ~600ms after the shell is up. The
    // synchronous boot build is the heaviest load window on the unpaced blit,
    // and any tear it leaves on STATIC content (the navbar never repaints on
    // its own) sticks on screen forever. Network is already quiet by now
    // (websocket connects ~4s, home-up ~11s), so this fires into calm and
    // forces a clean present that heals the boot-window artifacts. The heal
    // present could itself tear, but at idle load the blit wins the beam race;
    // it is one-shot and re-arms nothing. Stage B's pointer-swap makes tears
    // impossible — this is the Stage A mitigation.
    lv_timer_t* heal = lv_timer_create(
        [](lv_timer_t*) {
            lv_obj_invalidate(lv_screen_active());
            ESP_LOGI(TAG, "helix: boot settle-heal repaint");
        },
        600, nullptr);
    lv_timer_set_repeat_count(heal, 1);

#if !CONFIG_HELIX_MOCK_PRINTER && !CONFIG_HELIX_NET_HIL
    // Bring up WiFi + connect to Moonraker, LAST — the shell is already up, so
    // the connect thread's stack (the only >=32KB internal alloc on this path)
    // is claimed after every boot internal-DRAM gate, and the not-ready UI is
    // already on screen while the network converges. Gated off for the NET_HIL
    // test build (net_hil.cpp owns WiFi + its own client there) and for mock
    // builds (synthetic driver, no network).
    app_net_start();
#endif
}

extern "C" void app_boot_tick(void) {
    if (g_manager) {
        // Drain queued Moonraker notifications + request timeouts on the UI
        // thread — the work Application does each main-loop iteration.
        g_manager->process_notifications();
        g_manager->process_timeouts();
    }
    // One-shot steady-state budget sample at t=60s: by then WiFi, WebSocket,
    // discovery, and live status streaming are all up, so this reads budgets
    // under real load. Uses only the O(1) free-size counters — the
    // largest-block walk takes a heap-wide critical section and is banned
    // from the steady-state loop (see log_heap_milestone).
    static bool steady_logged = false;
    if (!steady_logged && esp_timer_get_time() > 60000000LL) {
        steady_logged = true;
        ESP_LOGI(TAG, "[heap:steady-60s] internal free=%u | psram free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        // Stack headroom for the two IDF-owned tasks that run our code but that
        // we never sized deliberately: "sys_evt" carries wifi_event_handler
        // (-> on_got_ip / on_scan_done) and "esp_timer" carries
        // housekeeping_trampoline (-> process_timeouts -> execute_reconnect).
        // uxTaskGetStackHighWaterMark reports the MINIMUM free bytes each has
        // ever had since boot, so sampling once at steady state covers the
        // whole boot + connect + discovery burst. Without this the only
        // overflow signal is the canary, which reboots without saying which
        // task died. Same one-shot, non-loop discipline as the heap numbers
        // above; the sizes are set in sdkconfig.defaults.
        struct {
            const char* name;
            unsigned configured;
        } const watched[] = {
            {"sys_evt", (unsigned)CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE},
            {"esp_timer", (unsigned)CONFIG_ESP_TIMER_TASK_STACK_SIZE},
        };
        for (const auto& t : watched) {
            TaskHandle_t h = xTaskGetHandle(t.name);
            if (h) {
                ESP_LOGI(TAG, "[stack:steady-60s] %s free=%u of %u", t.name,
                         (unsigned)uxTaskGetStackHighWaterMark(h), t.configured);
            } else {
                // Not fatal: the name is an IDF implementation detail and a
                // rename would only cost us the telemetry, not the boot.
                ESP_LOGW(TAG, "[stack:steady-60s] task '%s' not found", t.name);
            }
        }
    }
#if CONFIG_HELIX_MOCK_PRINTER
    // ~1 Hz synthetic temperature push. tick fires every render iteration
    // (5-50ms); gate to roughly once a second.
    static int64_t last = 0;
    int64_t now = esp_timer_get_time();
    if (now - last > 1000000) {
        last = now;
        mock_push_temps();
    }
#endif
}
