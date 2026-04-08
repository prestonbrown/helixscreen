// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_discovery_sequence.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "helix_version.h"
#include "hv/requests.h"
#include "led/led_controller.h"
#include "macro_fan_analyzer.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "power_device_state.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "sensor_state.h"

#include <algorithm>
#include <thread>

namespace helix {

MoonrakerDiscoverySequence::MoonrakerDiscoverySequence(MoonrakerClient& client) : client_(client) {}

void MoonrakerDiscoverySequence::clear_cache() {
    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();
    steppers_.clear();
    afc_objects_.clear();
    filament_sensors_.clear();
    hardware_ = PrinterDiscovery{};
    MacroParamCache::instance().clear();
}

bool MoonrakerDiscoverySequence::is_stale() const {
    return client_.connection_generation() != discovery_generation_;
}

void MoonrakerDiscoverySequence::start(std::function<void()> on_complete,
                                       std::function<void(const std::string& reason)> on_error) {
    spdlog::debug("[Moonraker Client] Starting printer auto-discovery");

    // Store callbacks and snapshot the connection generation for stale detection
    on_complete_discovery_ = std::move(on_complete);
    on_error_discovery_ = std::move(on_error);
    discovery_generation_ = client_.connection_generation();

    // Step 0: Identify ourselves to Moonraker to enable receiving notifications
    // Skip if we've already identified on this connection (e.g., wizard tested, then completed)
    if (identified_.load()) {
        spdlog::debug("[Moonraker Client] Already identified, skipping identify step");
        continue_discovery();
        return;
    }

    json identify_params = {{"client_name", "HelixScreen"},
                            {"version", HELIX_VERSION},
                            {"type", "display"},
                            {"url", "https://github.com/helixscreen/helixscreen"}};

    client_.send_jsonrpc(
        "server.connection.identify", identify_params,
        [this](json identify_response) {
            if (is_stale())
                return;

            if (identify_response.contains("result")) {
                auto conn_id = identify_response["result"].value("connection_id", 0);
                spdlog::info("[Moonraker Client] Identified to Moonraker (connection_id: {})",
                             conn_id);
                identified_.store(true);
            } else if (identify_response.contains("error")) {
                // Log but continue - older Moonraker versions may not support this
                spdlog::warn("[Moonraker Client] Failed to identify: {}",
                             identify_response["error"].dump());
            }

            // Continue with discovery regardless of identify result
            continue_discovery();
        },
        [this](const MoonrakerError& err) {
            if (is_stale())
                return;

            // Log but continue - identify is not strictly required
            spdlog::warn("[Moonraker Client] Identify request failed: {}", err.message);
            continue_discovery();
        });
}

void MoonrakerDiscoverySequence::discover_power_devices() {
    // Fire-and-forget power device detection (silent — not all printers
    // have the power component, and "Method not found" is expected).
    // Called both during full discovery and as partial discovery when
    // Klippy is not ready (power devices only need Moonraker, not Klipper).
    client_.send_jsonrpc(
        "machine.device_power.devices", json::object(),
        [](json response) {
            std::vector<PowerDevice> devices;
            if (response.contains("result") && response["result"].contains("devices")) {
                for (const auto& dev : response["result"]["devices"]) {
                    if (!dev.is_object())
                        continue;
                    PowerDevice pd;
                    pd.device = dev.value("device", "");
                    pd.type = dev.value("type", "");
                    pd.status = dev.value("status", "off");
                    pd.locked_while_printing = dev.value("locked_while_printing", false);
                    if (!pd.device.empty()) {
                        devices.push_back(std::move(pd));
                    }
                }
            }
            spdlog::info("[Moonraker Client] Power device detection: {} devices", devices.size());
            get_printer_state().set_power_device_count(static_cast<int>(devices.size()));
            // Marshal to UI thread — set_devices creates LVGL subjects
            auto devices_copy = std::make_shared<std::vector<PowerDevice>>(std::move(devices));
            helix::ui::queue_update("PowerDeviceState::set_devices", [devices_copy]() {
                helix::PowerDeviceState::instance().set_devices(*devices_copy);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[Moonraker Client] Power device detection failed: {}", err.message);
            get_printer_state().set_power_device_count(0);
        },
        0,     // default timeout
        true); // silent — suppress error toast
}

void MoonrakerDiscoverySequence::discover_sensors() {
    // Fire-and-forget sensor detection via MoonrakerAPI (reuses its parsing logic).
    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::debug("[Moonraker Client] Sensor detection skipped — no API instance");
        return;
    }

    api->get_sensors(
        [](const std::vector<helix::SensorInfo>& sensors, const nlohmann::json& initial_values) {
            spdlog::info("[Moonraker Client] Sensor detection: {} sensors", sensors.size());
            get_printer_state().set_sensor_count(static_cast<int>(sensors.size()));
            auto sensors_copy = std::make_shared<std::vector<helix::SensorInfo>>(sensors);
            auto values_copy = std::make_shared<nlohmann::json>(initial_values);
            helix::ui::queue_update("SensorState::set_sensors", [sensors_copy, values_copy]() {
                helix::SensorState::instance().set_sensors(*sensors_copy, *values_copy);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[Moonraker Client] Sensor detection failed: {}", err.message);
            get_printer_state().set_sensor_count(0);
        });
}

void MoonrakerDiscoverySequence::continue_discovery() {
    // Step 1: Check Klippy readiness via server.info before querying printer objects.
    // When Klippy is in STARTUP state, printer.objects.list returns JSON-RPC error
    // -32601 "Method not found", causing confusing error toasts. Gate here instead.
    client_.send_jsonrpc(
        "server.info", json(),
        [this](json server_info_response) {
            if (is_stale())
                return;

            // Extract klippy_state from response
            std::string klippy_state = "unknown";
            if (server_info_response.contains("result") &&
                server_info_response["result"].contains("klippy_state") &&
                server_info_response["result"]["klippy_state"].is_string()) {
                klippy_state = server_info_response["result"]["klippy_state"].get<std::string>();
            }

            spdlog::debug("[Moonraker Client] Klippy state gate check: {}", klippy_state);

            // Allow "ready" and "shutdown" — both have valid Klipper objects.
            // Abort on "startup", "error", or unknown states.
            if (klippy_state != "ready" && klippy_state != "shutdown") {
                std::string reason = fmt::format("Klippy not ready (state: {})", klippy_state);
                spdlog::warn("[Moonraker Client] {}", reason);

                // Partial discovery: power devices are Moonraker-managed, not Klipper-dependent.
                // Query them even when Klippy isn't ready so users can power on their printer.
                discover_power_devices();
                discover_sensors();

                client_.emit_event(MoonrakerEventType::DISCOVERY_FAILED, reason, true);
                if (on_error_discovery_) {
                    auto cb = std::move(on_error_discovery_);
                    on_complete_discovery_ = nullptr;
                    cb(reason);
                }
                return;
            }

            // Klippy is ready/shutdown — proceed to query printer objects
            continue_discovery_objects();
        },
        [this](const MoonrakerError& err) {
            if (is_stale())
                return;
            // server.info failed — cannot determine Klippy state, abort discovery
            spdlog::error("[Moonraker Client] server.info request failed: {}", err.message);
            client_.emit_event(MoonrakerEventType::DISCOVERY_FAILED, err.message, true);
            if (on_error_discovery_) {
                auto cb = std::move(on_error_discovery_);
                on_complete_discovery_ = nullptr;
                cb(err.message);
            }
        });
}

void MoonrakerDiscoverySequence::continue_discovery_objects() {
    // Step 2: Query available printer objects (no params required)
    // Silent=true to suppress error toast if Klippy goes away between gate and this call
    client_.send_jsonrpc(
        "printer.objects.list", json(),
        [this](json response) {
            if (is_stale())
                return;
            // Debug: Log raw response
            spdlog::debug("[Moonraker Client] printer.objects.list response: {}", response.dump());

            // Validate response
            if (!response.contains("result") || !response["result"].contains("objects")) {
                // Extract error message from response if available
                std::string error_reason = "Failed to query printer objects from Moonraker";
                if (response.contains("error") && response["error"].contains("message") &&
                    response["error"]["message"].is_string()) {
                    error_reason = response["error"]["message"].get<std::string>();
                    spdlog::error("[Moonraker Client] printer.objects.list failed: {}",
                                  error_reason);
                } else {
                    spdlog::error(
                        "[Moonraker Client] printer.objects.list failed: invalid response");
                    if (response.contains("error")) {
                        spdlog::error("[Moonraker Client]   Error details: {}",
                                      response["error"].dump());
                    }
                }

                // Emit discovery failed event
                client_.emit_event(MoonrakerEventType::DISCOVERY_FAILED, error_reason, true);

                // Invoke error callback if provided
                spdlog::debug(
                    "[Moonraker Client] Invoking discovery on_error callback, on_error={}",
                    on_error_discovery_ ? "valid" : "null");
                if (on_error_discovery_) {
                    auto cb = std::move(on_error_discovery_);
                    on_complete_discovery_ = nullptr;
                    cb(error_reason);
                }
                return;
            }

            // Parse discovered objects into typed arrays
            const json& objects = response["result"]["objects"];
            parse_objects(objects);

            // Early hardware discovery callback - allows AMS/MMU backends to initialize
            // BEFORE the subscription response arrives, so they can receive initial state
            // naturally. Copy hardware_ to prevent data races if callback defers work (#562).
            if (on_hardware_discovered_) {
                spdlog::debug("[Moonraker Client] Invoking early hardware discovery callback");
                auto hw_snapshot = hardware_;
                on_hardware_discovered_(hw_snapshot);
            }

            // Step 2: Get server information
            client_.send_jsonrpc("server.info", {}, [this](json info_response) {
                if (is_stale())
                    return;
                if (info_response.contains("result")) {
                    const json& result = info_response["result"];
                    std::string klippy_version = result.value("klippy_version", "unknown");
                    auto moonraker_version = result.value("moonraker_version", "unknown");
                    hardware_.set_moonraker_version(moonraker_version);

                    spdlog::debug("[Moonraker Client] Moonraker version: {}", moonraker_version);
                    spdlog::debug("[Moonraker Client] Klippy version: {}", klippy_version);

                    if (result.contains("components") && result["components"].is_array()) {
                        std::vector<std::string> components;
                        for (const auto& comp : result["components"]) {
                            if (comp.is_string()) {
                                components.push_back(comp.get<std::string>());
                            }
                        }
                        spdlog::debug("[Moonraker Client] Server components: {}",
                                      json(components).dump());

                        // Check for Spoolman component and verify connection
                        bool has_spoolman_component =
                            std::find(components.begin(), components.end(), "spoolman") !=
                            components.end();
                        if (has_spoolman_component) {
                            spdlog::info("[Moonraker Client] Spoolman component detected, "
                                         "checking status...");
                            // Fire-and-forget status check - updates PrinterState async
                            client_.send_jsonrpc(
                                "server.spoolman.status", json::object(),
                                [](json response) {
                                    bool connected = false;
                                    if (response.contains("result")) {
                                        connected =
                                            response["result"].value("spoolman_connected", false);
                                    }
                                    spdlog::info("[Moonraker Client] Spoolman status: connected={}",
                                                 connected);
                                    get_printer_state().set_spoolman_available(connected);
                                },
                                [](const MoonrakerError& err) {
                                    spdlog::debug(
                                        "[Moonraker Client] Spoolman status check failed: {}",
                                        err.message);
                                    get_printer_state().set_spoolman_available(false);
                                },
                                0,     // default timeout
                                true); // silent — Spoolman not always configured
                        }
                    }
                }

                // Fire-and-forget webcam detection - independent of components list
                client_.send_jsonrpc(
                    "server.webcams.list", json::object(),
                    [](json response) {
                        bool has_webcam = false;
                        std::string stream_url;
                        std::string snapshot_url;
                        bool flip_h = false;
                        bool flip_v = false;
                        if (response.contains("result") && response["result"].contains("webcams")) {
                            for (const auto& cam : response["result"]["webcams"]) {
                                if (cam.value("enabled", true)) {
                                    has_webcam = true;
                                    stream_url = cam.value("stream_url", "");
                                    snapshot_url = cam.value("snapshot_url", "");
                                    flip_h = cam.value("flip_horizontal", false);
                                    flip_v = cam.value("flip_vertical", false);
                                    break;
                                }
                            }
                        }
                        if (has_webcam) {
                            spdlog::info("[Discovery] Webcam found via Moonraker: stream={}",
                                         stream_url);
                            get_printer_state().set_webcam_available(true, stream_url, snapshot_url,
                                                                     flip_h, flip_v);
                        } else {
                            // No Moonraker webcam config — probe local camera endpoints
                            // Run synchronously on the WS callback instead of spawning a
                            // detached std::thread. Thread creation crashes on resource-
                            // constrained ARM devices (AD5M #724) — std::terminate is
                            // called even with try/catch, likely a GCC 10.3/ARM TLS bug.
                            // Synchronous probing blocks the WS thread for up to 6s (3
                            // URLs × 2s timeout) but only runs once during discovery.
                            spdlog::info(
                                "[Discovery] No Moonraker webcam, probing local camera...");
                            bool found = false;
                            static const char* probe_urls[] = {
                                "http://127.0.0.1:8080/?action=snapshot",
                                "http://127.0.0.1:8081/?action=snapshot",
                                "http://127.0.0.1:4408/webcam/?action=snapshot",
                            };
                            for (const char* url : probe_urls) {
                                spdlog::info("[Discovery] Probing camera at {}", url);
                                auto req = std::make_shared<HttpRequest>();
                                req->method = HTTP_GET;
                                req->url = url;
                                req->timeout = 2;
                                auto resp = requests::request(req);
                                if (resp && resp->status_code == 200) {
                                    spdlog::info("[Discovery] Local camera found at {}", url);
                                    get_printer_state().set_webcam_available(true, "", url, false,
                                                                             false);
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                spdlog::info("[Discovery] No local camera found");
                                get_printer_state().set_webcam_available(false);
                            }
                        }
                    },
                    [](const MoonrakerError& err) {
                        spdlog::debug("[Discovery] Webcam detection failed: {}", err.message);
                        get_printer_state().set_webcam_available(false);
                    },
                    0,     // default timeout
                    true); // silent — webcams not always configured

                // Fire-and-forget power device detection
                discover_power_devices();
                discover_sensors();

                // Step 3: Get printer information
                client_.send_jsonrpc("printer.info", {}, [this](json printer_response) {
                    if (is_stale())
                        return;
                    if (printer_response.contains("result")) {
                        const json& result = printer_response["result"];
                        auto hostname = result.value("hostname", "unknown");
                        auto software_version = result.value("software_version", "unknown");
                        hardware_.set_hostname(hostname);
                        hardware_.set_software_version(software_version);
                        std::string state = result.value("state", "");
                        std::string state_message = result.value("state_message", "");

                        // Detect Kalico (Klipper fork with MPC support)
                        auto app = result.value("app", "");
                        if (app == "Kalico") {
                            hardware_.set_is_kalico(true);
                            spdlog::info("[Moonraker Client] Kalico firmware detected");
                        }

                        spdlog::debug("[Moonraker Client] Printer hostname: {}", hostname);
                        spdlog::debug("[Moonraker Client] Klipper software version: {}",
                                      software_version);
                        if (!state_message.empty()) {
                            spdlog::info("[Moonraker Client] Printer state: {}", state_message);
                        }

                        // Set klippy state based on printer.info response
                        // This ensures we recognize shutdown/error states at startup
                        if (state == "shutdown" || state == "disconnected") {
                            spdlog::warn("[Moonraker Client] Printer is in {} state at startup",
                                         state);
                            get_printer_state().set_klippy_state(KlippyState::SHUTDOWN);
                        } else if (state == "error") {
                            spdlog::warn("[Moonraker Client] Printer is in ERROR state at startup");
                            get_printer_state().set_klippy_state(KlippyState::ERROR);
                        } else if (state == "startup") {
                            spdlog::info("[Moonraker Client] Printer is starting up");
                            get_printer_state().set_klippy_state(KlippyState::STARTUP);
                        } else if (state == "ready") {
                            get_printer_state().set_klippy_state(KlippyState::READY);
                        }
                    }

                    // Step 4: Query configfile for accelerometer detection and macro fan analysis.
                    // Klipper's objects/list only returns objects with get_status() methods.
                    // Accelerometers (adxl345, lis2dw, mpu9250, resonance_tester) don't have
                    // get_status() since they're on-demand calibration tools.
                    // Must check configfile.config keys instead.
                    // Also query configfile.settings for macro fan analysis (M106/M107/M141).
                    client_.send_jsonrpc(
                        "printer.objects.query",
                        {{"objects",
                          json::object({{"configfile", json::array({"config", "settings"})}})}},
                        [this](json config_response) {
                            if (config_response.contains("result") &&
                                config_response["result"].contains("status") &&
                                config_response["result"]["status"].contains("configfile") &&
                                config_response["result"]["status"]["configfile"].contains(
                                    "config")) {
                                const auto& cfg =
                                    config_response["result"]["status"]["configfile"]["config"];
                                hardware_.parse_config_keys(cfg);
                                MacroParamCache::instance().populate_from_configfile(
                                    cfg, hardware_.macros());

                                // Seed probe sensor z_offset from configfile (some probe
                                // modules like flashforge_loadcell return null in status).
                                // Must run on main thread — update_subjects() sets LVGL subjects.
                                nlohmann::json cfg_for_probe = cfg;
                                helix::ui::queue_update([cfg_for_probe]() {
                                    helix::sensors::ProbeSensorManager::instance()
                                        .discover_from_config(cfg_for_probe);
                                });

                                // Update LED controller with configfile data (effect targets +
                                // output_pin PWM + generic LED pin config)
                                nlohmann::json cfg_copy = cfg;
                                helix::ui::queue_update([cfg_copy]() {
                                    auto& led_ctrl = helix::led::LedController::instance();
                                    if (led_ctrl.is_initialized()) {
                                        led_ctrl.update_effect_targets(cfg_copy);
                                        led_ctrl.update_output_pin_config(cfg_copy);
                                        led_ctrl.update_led_pin_config(cfg_copy);
                                    }
                                });
                            }

                            // Analyze M106/M107/M141 macros from configfile.settings to detect
                            // output_pin fan roles. Write role hints as default display names
                            // (only when no custom name already exists).
                            if (config_response.contains("result") &&
                                config_response["result"].contains("status") &&
                                config_response["result"]["status"].contains("configfile") &&
                                config_response["result"]["status"]["configfile"].contains(
                                    "settings")) {
                                const auto& settings =
                                    config_response["result"]["status"]["configfile"]["settings"];
                                helix::MacroFanAnalyzer analyzer;
                                auto macro_result = analyzer.analyze(settings);

                                auto* config = Config::get_instance();
                                if (config && !macro_result.role_hints.empty()) {
                                    for (const auto& [obj_name, role] : macro_result.role_hints) {
                                        std::string key = config->df() + "fans/names/" + obj_name;
                                        if (config->get<std::string>(key, "").empty()) {
                                            config->set(key, role);
                                            spdlog::debug(
                                                "[Discovery] Wrote macro fan role hint: {} -> {}",
                                                obj_name, role);
                                        }
                                    }
                                }
                            }
                        },
                        [](const MoonrakerError& err) {
                            // Configfile query failed - not critical, continue with discovery
                            spdlog::debug(
                                "[Moonraker Client] Configfile query failed, continuing: {}",
                                err.message);
                        });

                    // Step 4b: Query OS version from machine.system_info (parallel)
                    client_.send_jsonrpc(
                        "machine.system_info", json::object(),
                        [this](json sys_response) {
                            // Extract distribution name: result.system_info.distribution.name
                            if (sys_response.contains("result") &&
                                sys_response["result"].contains("system_info") &&
                                sys_response["result"]["system_info"].contains("distribution") &&
                                sys_response["result"]["system_info"]["distribution"].contains(
                                    "name") &&
                                sys_response["result"]["system_info"]["distribution"]["name"]
                                    .is_string()) {
                                std::string os_name =
                                    sys_response["result"]["system_info"]["distribution"]["name"]
                                        .get<std::string>();
                                hardware_.set_os_version(os_name);
                                spdlog::debug("[Moonraker Client] OS version: {}", os_name);
                            }

                            // Extract CPU architecture from cpu_info.processor
                            if (sys_response.contains("result") &&
                                sys_response["result"].contains("system_info") &&
                                sys_response["result"]["system_info"].contains("cpu_info") &&
                                sys_response["result"]["system_info"]["cpu_info"].contains(
                                    "processor") &&
                                sys_response["result"]["system_info"]["cpu_info"]["processor"]
                                    .is_string()) {
                                std::string cpu_arch =
                                    sys_response["result"]["system_info"]["cpu_info"]["processor"]
                                        .get<std::string>();
                                hardware_.set_cpu_arch(cpu_arch);
                                spdlog::debug("[Moonraker Client] CPU architecture: {}", cpu_arch);
                            }
                        },
                        [](const MoonrakerError& err) {
                            spdlog::debug("[Moonraker Client] machine.system_info query "
                                          "failed, continuing: "
                                          "{}",
                                          err.message);
                        });

                    // Step 5: Query MCU information for printer detection
                    // Find all MCU objects (e.g., "mcu", "mcu EBBCan", "mcu rpi")
                    std::vector<std::string> mcu_objects;
                    for (const auto& obj : hardware_.printer_objects()) {
                        // Match "mcu" or "mcu <name>" pattern
                        if (obj == "mcu" || obj.rfind("mcu ", 0) == 0) {
                            mcu_objects.push_back(obj);
                        }
                    }

                    if (mcu_objects.empty()) {
                        spdlog::debug(
                            "[Moonraker Client] No MCU objects found, skipping MCU query");
                        // Continue to subscription step
                        complete_discovery_subscription();
                        return;
                    }

                    // Query all MCU objects in parallel using a shared counter
                    auto pending_mcu_queries =
                        std::make_shared<std::atomic<size_t>>(mcu_objects.size());
                    auto mcu_results =
                        std::make_shared<std::vector<std::pair<std::string, std::string>>>();
                    auto mcu_version_results =
                        std::make_shared<std::vector<std::pair<std::string, std::string>>>();
                    auto mcu_results_mutex = std::make_shared<std::mutex>();

                    for (const auto& mcu_obj : mcu_objects) {
                        json mcu_query = {{mcu_obj, nullptr}};
                        client_.send_jsonrpc(
                            "printer.objects.query", {{"objects", mcu_query}},
                            [this, mcu_obj, pending_mcu_queries, mcu_results, mcu_version_results,
                             mcu_results_mutex](json mcu_response) {
                                if (is_stale())
                                    return;
                                std::string chip_type;
                                std::string mcu_version;

                                // Extract MCU chip type and version from response
                                if (mcu_response.contains("result") &&
                                    mcu_response["result"].contains("status") &&
                                    mcu_response["result"]["status"].contains(mcu_obj)) {
                                    const json& mcu_data =
                                        mcu_response["result"]["status"][mcu_obj];

                                    if (mcu_data.contains("mcu_constants") &&
                                        mcu_data["mcu_constants"].is_object() &&
                                        mcu_data["mcu_constants"].contains("MCU") &&
                                        mcu_data["mcu_constants"]["MCU"].is_string()) {
                                        chip_type =
                                            mcu_data["mcu_constants"]["MCU"].get<std::string>();
                                        spdlog::debug("[Moonraker Client] Detected MCU '{}': {}",
                                                      mcu_obj, chip_type);
                                    }

                                    // Extract mcu_version for About section
                                    if (mcu_data.contains("mcu_version") &&
                                        mcu_data["mcu_version"].is_string()) {
                                        mcu_version = mcu_data["mcu_version"].get<std::string>();
                                        spdlog::debug("[Moonraker Client] MCU '{}' version: {}",
                                                      mcu_obj, mcu_version);
                                    }
                                }

                                // Store results thread-safely
                                {
                                    std::lock_guard<std::mutex> lock(*mcu_results_mutex);
                                    if (!chip_type.empty()) {
                                        mcu_results->push_back({mcu_obj, chip_type});
                                    }
                                    if (!mcu_version.empty()) {
                                        mcu_version_results->push_back({mcu_obj, mcu_version});
                                    }
                                }

                                // Check if all queries complete
                                if (pending_mcu_queries->fetch_sub(1) == 1) {
                                    // All MCU queries complete - populate mcu and mcu_list
                                    std::vector<std::string> mcu_list;
                                    std::string primary_mcu;

                                    // Sort results to ensure consistent ordering (primary "mcu"
                                    // first)
                                    std::lock_guard<std::mutex> lock(*mcu_results_mutex);
                                    auto sort_mcu_first = [](const auto& a, const auto& b) {
                                        // "mcu" comes first, then alphabetical
                                        if (a.first == "mcu")
                                            return true;
                                        if (b.first == "mcu")
                                            return false;
                                        return a.first < b.first;
                                    };
                                    std::sort(mcu_results->begin(), mcu_results->end(),
                                              sort_mcu_first);
                                    std::sort(mcu_version_results->begin(),
                                              mcu_version_results->end(), sort_mcu_first);

                                    for (const auto& [obj_name, chip] : *mcu_results) {
                                        mcu_list.push_back(chip);
                                        if (obj_name == "mcu" && primary_mcu.empty()) {
                                            primary_mcu = chip;
                                        }
                                    }

                                    // Update hardware discovery with MCU info
                                    hardware_.set_mcu(primary_mcu);
                                    hardware_.set_mcu_list(mcu_list);
                                    hardware_.set_mcu_versions(*mcu_version_results);

                                    if (!primary_mcu.empty()) {
                                        spdlog::info("[Moonraker Client] Primary MCU: {}",
                                                     primary_mcu);
                                    }
                                    if (mcu_list.size() > 1) {
                                        spdlog::info("[Moonraker Client] All MCUs: {}",
                                                     json(mcu_list).dump());
                                    }

                                    // Continue to subscription step
                                    complete_discovery_subscription();
                                }
                            },
                            [this, mcu_obj, pending_mcu_queries](const MoonrakerError& err) {
                                if (is_stale())
                                    return;

                                spdlog::warn("[Moonraker Client] MCU query for '{}' failed: {}",
                                             mcu_obj, err.message);

                                // Check if all queries complete (even on error)
                                if (pending_mcu_queries->fetch_sub(1) == 1) {
                                    // Continue to subscription step even if some MCU queries
                                    // failed
                                    complete_discovery_subscription();
                                }
                            });
                    }
                });
            });
        },
        [this](const MoonrakerError& err) {
            if (is_stale())
                return;

            spdlog::error("[Moonraker Client] printer.objects.list request failed: {}",
                          err.message);
            client_.emit_event(MoonrakerEventType::DISCOVERY_FAILED, err.message, true);
            spdlog::debug("[Moonraker Client] Invoking discovery on_error callback, on_error={}",
                          on_error_discovery_ ? "valid" : "null");
            if (on_error_discovery_) {
                auto cb = std::move(on_error_discovery_);
                on_complete_discovery_ = nullptr;
                cb(err.message);
            }
        },
        0,   // default timeout
        true // silent — suppress error toast (Klippy gate already checked)
    );
}

void MoonrakerDiscoverySequence::complete_discovery_subscription() {
    // Step 5: Subscribe to all discovered objects + core objects
    json subscription_objects;

    // Core non-optional objects
    subscription_objects["print_stats"] = nullptr;
    subscription_objects["virtual_sdcard"] = nullptr;
    subscription_objects["toolhead"] = nullptr;
    subscription_objects["gcode_move"] = nullptr;
    subscription_objects["motion_report"] = nullptr;
    subscription_objects["system_stats"] = nullptr;
    subscription_objects["display_status"] = nullptr;

    // Klipper firmware state (shutdown/error detection + state_message)
    subscription_objects["webhooks"] = nullptr;

    // All discovered heaters (extruders, beds, generic heaters)
    for (const auto& heater : heaters_) {
        subscription_objects[heater] = nullptr;
    }

    // All discovered sensors
    for (const auto& sensor : sensors_) {
        subscription_objects[sensor] = nullptr;
    }

    // All discovered fans
    spdlog::info("[Moonraker Client] Subscribing to {} fans: {}", fans_.size(), json(fans_).dump());
    for (const auto& fan : fans_) {
        subscription_objects[fan] = nullptr;
    }

    // Subscribe to fan_feedback if available (Creality tachometer module)
    if (hardware_.has_fan_feedback()) {
        subscription_objects["fan_feedback"] = nullptr;
        spdlog::debug("[MoonrakerDiscoverySequence] Subscribing to fan_feedback for RPM data");
    }

    // All discovered LEDs
    for (const auto& led : leds_) {
        subscription_objects[led] = nullptr;
    }

    // All discovered LED effects (for tracking active/enabled state)
    for (const auto& effect : hardware_.led_effects()) {
        subscription_objects[effect] = nullptr;
    }

    // Bed mesh (for 3D visualization)
    subscription_objects["bed_mesh"] = nullptr;

    // Exclude object (for mid-print object exclusion)
    subscription_objects["exclude_object"] = nullptr;

    // Manual probe (for Z-offset calibration - PROBE_CALIBRATE, Z_ENDSTOP_CALIBRATE)
    subscription_objects["manual_probe"] = nullptr;

    // Stepper enable state (for motor enabled/disabled detection - updates immediately on M84)
    subscription_objects["stepper_enable"] = nullptr;

    // Idle timeout (for printer activity state - Ready/Printing/Idle)
    subscription_objects["idle_timeout"] = nullptr;

    // Happy Hare MMU object (gate status, colors, materials, filament info)
    // Subscribe to specific fields only — nullptr means ALL fields, which causes
    // excessive notifications and Klipper-side serialization cost (#388)
    if (hardware_.has_mmu()) {
        subscription_objects["mmu"] = json::array({"gate",
                                                   "tool",
                                                   "filament",
                                                   "action",
                                                   "reason_for_pause",
                                                   "filament_pos",
                                                   "gate_status",
                                                   "gate_color_rgb",
                                                   "gate_color",
                                                   "gate_material",
                                                   "gate_name",
                                                   "gate_filament_name",
                                                   "gate_spool_id",
                                                   "gate_temperature",
                                                   "has_bypass",
                                                   "num_units",
                                                   "num_gates",
                                                   "unit_gate_counts",
                                                   "unit",
                                                   "ttg_map",
                                                   "endless_spool_groups",
                                                   "sensors",
                                                   "bowden_progress",
                                                   "clog_detection_enabled",
                                                   "encoder",
                                                   "flowguard",
                                                   "drying_state",
                                                   "sync_feedback_state",
                                                   "sync_feedback_bias_modelled",
                                                   "sync_feedback_bias_raw",
                                                   "sync_feedback_flow_rate",
                                                   "sync_drive",
                                                   "spoolman_support",
                                                   "pending_spool_id",
                                                   "espooler_active",
                                                   "num_toolchanges",
                                                   "slicer_tool_map",
                                                   "toolchange_purge_volume",
                                                   "leds"});
        spdlog::info("[Moonraker Client] Subscribing to MMU object (Happy Hare)");
    }

    // All discovered AFC objects (AFC, AFC_stepper, AFC_hub, AFC_extruder)
    // These provide lane status, sensor states, and filament info for MMU support
    for (const auto& afc_obj : afc_objects_) {
        subscription_objects[afc_obj] = nullptr;
    }

    // AD5X IFS requires save_variables for filament state (colors, types, tool mapping)
    if (hardware_.mmu_type() == AmsType::AD5X_IFS) {
        subscription_objects["save_variables"] = nullptr;
        spdlog::info("[Moonraker Client] Subscribing to save_variables (AD5X IFS)");
    }

    // ACE (Anycubic ACE Pro — ValgACE/BunnyACE/DuckACE Klipper drivers)
    // The ace object provides slot colors, materials, status, dryer state via get_status()
    if (hardware_.mmu_type() == AmsType::ACE) {
        subscription_objects["ace"] = nullptr;
        spdlog::info("[Moonraker Client] Subscribing to ace object (Anycubic ACE)");
    }

    // CFS (Creality Filament System) — K2 series with RS-485 CFS units
    if (hardware_.mmu_type() == AmsType::CFS) {
        subscription_objects["box"] = nullptr;
        subscription_objects["motor_control"] = nullptr;
        spdlog::info("[Moonraker Client] Subscribing to box + motor_control (CFS)");
    }

    // Snapmaker U1 SnapSwap — RFID filament, feed modules, task config, extruder states
    if (hardware_.mmu_type() == AmsType::SNAPMAKER) {
        subscription_objects["filament_detect"] = nullptr;
        subscription_objects["filament_feed left"] = nullptr;
        subscription_objects["filament_feed right"] = nullptr;
        subscription_objects["print_task_config"] = nullptr;
        subscription_objects["machine_state_manager"] = nullptr;
        for (int i = 0; i < 4; ++i) {
            subscription_objects[fmt::format("filament_motion_sensor e{}_filament", i)] = nullptr;
        }
        spdlog::info("[Moonraker Client] Subscribing to Snapmaker filament + feed objects");
    }

    // All discovered filament sensors (filament_switch_sensor, filament_motion_sensor)
    // These provide runout detection and encoder motion data
    for (const auto& sensor : filament_sensors_) {
        subscription_objects[sensor] = nullptr;
    }

    // All discovered width sensors (hall_filament_width_sensor, tsl1401cl_filament_width_sensor)
    // These provide filament diameter measurement for flow compensation
    if (hardware_.has_width_sensors()) {
        for (const auto& sensor : hardware_.width_sensor_objects()) {
            subscription_objects[sensor] = nullptr;
        }
        spdlog::info("[Moonraker Client] Subscribing to {} width sensors",
                     hardware_.width_sensor_objects().size());
    }

    // All discovered tool objects (for toolchanger support)
    if (hardware_.has_tool_changer()) {
        subscription_objects["toolchanger"] = nullptr;
        for (const auto& tool_name : hardware_.tool_names()) {
            subscription_objects["tool " + tool_name] = nullptr;
        }
        spdlog::info("[Moonraker Client] Subscribing to toolchanger + {} tool objects",
                     hardware_.tool_names().size());
    }

    // Firmware retraction settings (if printer has firmware_retraction module)
    if (hardware_.has_firmware_retraction()) {
        subscription_objects["firmware_retraction"] = nullptr;
    }

    // Print start macros (for detecting when prep phase completes)
    // These are optional - printers without these macros will silently not receive updates
    // AD5M/KAMP macros:
    subscription_objects["gcode_macro _START_PRINT"] = nullptr;
    subscription_objects["gcode_macro START_PRINT"] = nullptr;
    // HelixScreen custom macro:
    subscription_objects["gcode_macro _HELIX_STATE"] = nullptr;

    json subscribe_params = {{"objects", subscription_objects}};
    size_t num_subscribed = subscription_objects.size();

    client_.send_jsonrpc(
        "printer.objects.subscribe", subscribe_params, [this, num_subscribed](json sub_response) {
            if (is_stale())
                return;
            if (sub_response.contains("result")) {
                spdlog::info("[Moonraker Client] Subscription complete: {} objects subscribed",
                             num_subscribed);

                // Process initial state from subscription response
                // Moonraker returns current values in result.status
                if (sub_response["result"].contains("status")) {
                    const auto& status = sub_response["result"]["status"];
                    spdlog::info(
                        "[Moonraker Client] Processing initial printer state from subscription");

                    // DEBUG: Log print_stats specifically to diagnose startup sync issues
                    if (status.contains("print_stats")) {
                        spdlog::info("[Moonraker Client] INITIAL print_stats: {}",
                                     status["print_stats"].dump());
                    } else {
                        spdlog::warn("[Moonraker Client] INITIAL status has NO print_stats!");
                    }
                }
            } else if (sub_response.contains("error")) {
                spdlog::error("[Moonraker Client] Subscription failed: {}",
                              sub_response["error"].dump());

                // Emit discovery failed event (subscription is part of discovery)
                std::string error_msg = sub_response["error"].dump();
                client_.emit_event(
                    MoonrakerEventType::DISCOVERY_FAILED,
                    fmt::format("Failed to subscribe to printer updates: {}", error_msg),
                    false); // Warning, not error - discovery still completes
            }

            // Discovery complete - notify observers.
            // on_discovery_complete_ fires first so fans/sensors are initialized
            // before dispatch_status_update processes the initial state.
            // Copy hardware_ before invoking callback to prevent data races if
            // the callback defers work while this thread continues mutating (#562).
            if (on_discovery_complete_) {
                auto hw_snapshot = hardware_;
                on_discovery_complete_(hw_snapshot);
            }
            if (sub_response.contains("result")) {
                const auto& status = sub_response["result"]["status"];
                client_.dispatch_status_update(status);
            }
            if (on_complete_discovery_) {
                auto cb = std::move(on_complete_discovery_);
                on_error_discovery_ = nullptr;
                cb();
            }
        });
}

void MoonrakerDiscoverySequence::parse_objects(const json& objects) {
    // Populate unified hardware discovery (Phase 2)
    hardware_.parse_objects(objects);

    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();
    steppers_.clear();
    afc_objects_.clear();
    filament_sensors_.clear();

    // Collect printer_objects for hardware_ as we iterate
    std::vector<std::string> all_objects;
    all_objects.reserve(objects.size());

    for (const auto& obj : objects) {
        if (!obj.is_string())
            continue;
        std::string name = obj.template get<std::string>();

        // Store all objects for detection heuristics (object_exists, macro_match)
        all_objects.push_back(name);

        // Steppers (stepper_x, stepper_y, stepper_z, stepper_z1, etc.)
        if (name.rfind("stepper_", 0) == 0) {
            steppers_.push_back(name);
        }
        // Extruders (controllable heaters)
        // Match "extruder", "extruder1", etc., but NOT "extruder_stepper"
        else if (name.rfind("extruder", 0) == 0 && name.rfind("extruder_stepper", 0) != 0) {
            heaters_.push_back(name);
        }
        // Heated bed
        else if (name == "heater_bed") {
            heaters_.push_back(name);
        }
        // Generic heaters (e.g., "heater_generic chamber")
        else if (name.rfind("heater_generic ", 0) == 0) {
            heaters_.push_back(name);
        }
        // Read-only temperature sensors
        else if (name.rfind("temperature_sensor ", 0) == 0) {
            sensors_.push_back(name);
        }
        // Temperature-controlled fans (also act as sensors)
        else if (name.rfind("temperature_fan ", 0) == 0) {
            sensors_.push_back(name);
            fans_.push_back(name); // Also add to fans for control
        }
        // TMC stepper drivers with built-in temperature (tmc2240, tmc5160)
        else if (name.rfind("tmc2240 ", 0) == 0 || name.rfind("tmc5160 ", 0) == 0) {
            sensors_.push_back(name);
        }
        // Part cooling fan
        else if (name == "fan") {
            fans_.push_back(name);
        }
        // Heater fans (e.g., "heater_fan hotend_fan")
        else if (name.rfind("heater_fan ", 0) == 0) {
            fans_.push_back(name);
        }
        // Generic fans
        else if (name.rfind("fan_generic ", 0) == 0) {
            fans_.push_back(name);
        }
        // Controller fans
        else if (name.rfind("controller_fan ", 0) == 0) {
            fans_.push_back(name);
        }
        // Output pins - classify as fan or LED based on name keywords
        else if (name.rfind("output_pin ", 0) == 0) {
            std::string lower_name = name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            if (lower_name.find("fan") != std::string::npos) {
                fans_.push_back(name);
            } else if (lower_name.find("light") != std::string::npos ||
                       lower_name.find("led") != std::string::npos ||
                       lower_name.find("lamp") != std::string::npos) {
                leds_.push_back(name);
            }
        }
        // LED outputs
        else if (name.rfind("led ", 0) == 0 || name.rfind("neopixel ", 0) == 0 ||
                 name.rfind("dotstar ", 0) == 0) {
            leds_.push_back(name);
        }
        // AFC MMU objects — all AFC objects share the "AFC_" namespace prefix in Klipper.
        // Subscribe to all of them for lane state, sensor data, filament info, and
        // unit-level data (BoxTurtle, OpenAMS, ViViD, NightOwl, etc.)
        else if (name == "AFC" || name.rfind("AFC_", 0) == 0) {
            afc_objects_.push_back(name);
        }
        // Filament sensors (switch or motion type)
        // These provide runout detection and encoder motion data
        else if (name.rfind("filament_switch_sensor ", 0) == 0 ||
                 name.rfind("filament_motion_sensor ", 0) == 0) {
            filament_sensors_.push_back(name);
        }
    }

    spdlog::debug("[Moonraker Client] Discovered: {} heaters, {} sensors, {} fans, {} LEDs, {} "
                  "steppers, {} AFC objects, {} filament sensors",
                  heaters_.size(), sensors_.size(), fans_.size(), leds_.size(), steppers_.size(),
                  afc_objects_.size(), filament_sensors_.size());

    // Debug output of discovered objects
    if (!heaters_.empty()) {
        spdlog::debug("[Moonraker Client] Heaters: {}", json(heaters_).dump());
    }
    if (!sensors_.empty()) {
        spdlog::debug("[Moonraker Client] Sensors: {}", json(sensors_).dump());
    }
    if (!fans_.empty()) {
        spdlog::debug("[Moonraker Client] Fans: {}", json(fans_).dump());
    }
    if (!leds_.empty()) {
        spdlog::debug("[Moonraker Client] LEDs: {}", json(leds_).dump());
    }
    if (!steppers_.empty()) {
        spdlog::debug("[Moonraker Client] Steppers: {}", json(steppers_).dump());
    }
    if (!afc_objects_.empty()) {
        spdlog::info("[Moonraker Client] AFC objects: {}", json(afc_objects_).dump());
    }
    if (!filament_sensors_.empty()) {
        spdlog::info("[Moonraker Client] Filament sensors: {}", json(filament_sensors_).dump());
    }

    // Store printer objects in hardware discovery (handles all capability parsing)
    hardware_.set_printer_objects(all_objects);
}

void MoonrakerDiscoverySequence::parse_bed_mesh(const json& bed_mesh) {
    // Invoke bed mesh callback for API layer
    // The API layer (MoonrakerAPI) owns the bed mesh data; Client is just the transport
    std::function<void(const json&)> callback_copy;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callback_copy = bed_mesh_callback_;
    }
    if (callback_copy) {
        try {
            callback_copy(bed_mesh);
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] Bed mesh callback threw exception: {}", e.what());
        }
    }
}

} // namespace helix
