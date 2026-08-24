// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_discovery_sequence.h"

#include "ui_update_queue.h"

#include "accel_sensor_manager.h"
#include "app_globals.h"
#include "config.h"
#include "helix_version.h"
#include "humidity_sensor_types.h"
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
#include "unit_conversions.h"
#include "z_offset_persistence.h"

#include <algorithm>
#include <cctype>
#include <thread>
#include <unordered_set>

namespace helix {

namespace {
// Lowercase a copy of a string (ASCII) for case-insensitive suffix/substring checks.
std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The path component of a URL (everything before '?' or '#').
std::string url_path(const std::string& url) {
    auto cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}
} // namespace

bool is_usable_snapshot_url(const std::string& snapshot_url) {
    if (snapshot_url.empty())
        return false;
    std::string lower = to_lower_ascii(snapshot_url);
    // Reject HTML pages (e.g. /snapshot.html, /camera.html) — never a JPEG.
    if (ends_with(url_path(lower), ".html"))
        return false;
    // Accept obvious image/snapshot endpoints.
    if (lower.find("action=snapshot") != std::string::npos)
        return true;
    std::string path = url_path(lower);
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg") || ends_with(path, ".png"))
        return true;
    // No HTML marker and no explicit image marker: treat conservatively as usable
    // so atypical-but-real endpoints (bare host, query-only) are not dropped.
    return true;
}

bool probe_snapshot_reachable(const std::string& snapshot_url) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = snapshot_url;
    req->connect_timeout = SNAPSHOT_PROBE_CONNECT_TIMEOUT_SEC;
    req->timeout = SNAPSHOT_PROBE_TOTAL_TIMEOUT_SEC;
    auto resp = requests::request(req);
    if (resp && resp->status_code == 200) {
        return true;
    }
    // Distinguish the two failure shapes in the log: a status code means something
    // answered and said no; "no response" means connect or the response budget ran
    // out. The second is the one worth re-reading if a working camera is dropped.
    if (resp) {
        spdlog::debug("[Discovery] Snapshot probe of {} answered {}", snapshot_url,
                      static_cast<int>(resp->status_code));
    } else {
        spdlog::debug("[Discovery] Snapshot probe of {} got no response within {}s "
                      "(connect budget {}s)",
                      snapshot_url, SNAPSHOT_PROBE_TOTAL_TIMEOUT_SEC,
                      SNAPSHOT_PROBE_CONNECT_TIMEOUT_SEC);
    }
    return false;
}

MoonrakerDiscoverySequence::MoonrakerDiscoverySequence(MoonrakerClient& client) : client_(client) {}

void MoonrakerDiscoverySequence::clear_cache() {
    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();
    steppers_.clear();
    afc_objects_.clear();
    filament_sensors_.clear();
    mcus_.clear();
    {
        std::lock_guard<std::mutex> lock(hardware_mutex_);
        hardware_ = PrinterDiscovery{};
    }
    discovery_completed_.store(false);
    MacroParamCache::instance().clear();
}

bool MoonrakerDiscoverySequence::is_stale() const {
    return client_.connection_generation() != discovery_generation_;
}

bool MoonrakerDiscoverySequence::is_current_sequence(uint64_t seq) const {
    return seq == discovery_sequence_;
}

void MoonrakerDiscoverySequence::start(std::function<void()> on_complete,
                                       std::function<void(const std::string& reason)> on_error) {
    spdlog::debug("[Moonraker Client] Starting printer auto-discovery");

    // Store callbacks and snapshot the connection generation for stale detection.
    // Bump discovery_sequence_ so any in-flight callbacks from a prior start() are stale.
    on_complete_discovery_ = std::move(on_complete);
    on_error_discovery_ = std::move(on_error);
    discovery_generation_ = client_.connection_generation();
    uint64_t seq = ++discovery_sequence_;

    // Step 0: Identify ourselves to Moonraker to enable receiving notifications
    // Skip if we've already identified on this connection (e.g., wizard tested, then completed)
    if (identified_.load()) {
        spdlog::debug("[Moonraker Client] Already identified, skipping identify step");
        continue_discovery(seq);
        return;
    }

    json identify_params = {{"client_name", "HelixScreen"},
                            {"version", HELIX_VERSION},
                            {"type", "display"},
                            {"url", "https://github.com/helixscreen/helixscreen"}};

    client_.send_jsonrpc(
        "server.connection.identify", identify_params,
        [this, seq](json identify_response) {
            if (is_stale() || !is_current_sequence(seq))
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
            continue_discovery(seq);
        },
        [this, seq](const MoonrakerError& err) {
            if (is_stale() || !is_current_sequence(seq))
                return;

            // Log but continue - identify is not strictly required
            spdlog::warn("[Moonraker Client] Identify request failed: {}", err.message);
            continue_discovery(seq);
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

void MoonrakerDiscoverySequence::continue_discovery(uint64_t seq) {
    // Step 1: Check Klippy readiness via server.info before querying printer objects.
    // When Klippy is in STARTUP state, printer.objects.list returns JSON-RPC error
    // -32601 "Method not found", causing confusing error toasts. Gate here instead.
    client_.send_jsonrpc(
        "server.info", json(),
        [this, seq](json server_info_response) {
            if (is_stale() || !is_current_sequence(seq))
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

                // Deferred (retryable) — notify_klippy_ready/shutdown will retry.
                client_.emit_event(MoonrakerEventType::DISCOVERY_DEFERRED, reason, true);
                if (on_error_discovery_) {
                    auto cb = std::move(on_error_discovery_);
                    on_complete_discovery_ = nullptr;
                    cb(reason);
                }
                return;
            }

            // Klippy is ready/shutdown — proceed to query printer objects
            continue_discovery_objects(seq);
        },
        [this, seq](const MoonrakerError& err) {
            if (is_stale() || !is_current_sequence(seq))
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

void MoonrakerDiscoverySequence::continue_discovery_objects(uint64_t seq) {
    // Step 2: Query available printer objects (no params required)
    // Silent=true to suppress error toast if Klippy goes away between gate and this call
    client_.send_jsonrpc(
        "printer.objects.list", json(),
        [this, seq](json response) {
            if (is_stale() || !is_current_sequence(seq))
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
            // naturally. Copy hardware_ under lock to prevent data races (#562, #777).
            if (on_hardware_discovered_) {
                spdlog::debug("[Moonraker Client] Invoking early hardware discovery callback");
                PrinterDiscovery hw_snapshot;
                {
                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                    hw_snapshot = hardware_;
                }
                on_hardware_discovered_(hw_snapshot);
            }

            // Step 2: Get server information
            client_.send_jsonrpc("server.info", {}, [this, seq](json info_response) {
                if (is_stale() || !is_current_sequence(seq))
                    return;
                if (info_response.contains("result")) {
                    const json& result = info_response["result"];
                    std::string klippy_version = result.value("klippy_version", "unknown");
                    auto moonraker_version = result.value("moonraker_version", "unknown");
                    {
                        std::lock_guard<std::mutex> lock(hardware_mutex_);
                        hardware_.set_moonraker_version(moonraker_version);
                    }

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

                        // Clear component-derived capability flags before scanning so
                        // switching to a printer that no longer has Spoolman/Timelapse
                        // installed correctly hides the corresponding UI rows. The flags
                        // get re-set below if the components are detected (and, for
                        // Spoolman, after its status RPC confirms connectivity).
                        get_printer_state().set_spoolman_available(false);
                        get_printer_state().set_timelapse_available(false);

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

                        // moonraker-timelapse is a Moonraker component (configured in
                        // moonraker.conf), not a Klipper object — so it never appears in
                        // printer.objects.list. Detect it here so users with a pre-existing
                        // install (MainsailOS, manual config) see the Settings row, not just
                        // those who ran the in-app install wizard.
                        bool has_timelapse_component =
                            std::find(components.begin(), components.end(), "timelapse") !=
                            components.end();
                        if (has_timelapse_component) {
                            spdlog::info("[Moonraker Client] Timelapse component detected");
                            // Card visibility must not depend on the settings
                            // fetch — surface the row immediately.
                            get_printer_state().set_timelapse_available(true);

                            // Fire-and-forget: seed the pre-print toggle default
                            // from the global moonraker-timelapse `enabled`
                            // setting. The plugin has no per-print concept, so a
                            // hardcoded-false default would silently disable a
                            // user's global timelapse on print start (#1094).
                            client_.send_jsonrpc(
                                "machine.timelapse.get_settings", json::object(),
                                [](json response) {
                                    bool enabled = false;
                                    if (response.contains("result")) {
                                        enabled = response["result"].value("enabled", false);
                                    }
                                    spdlog::info("[Moonraker Client] Timelapse global enabled={}",
                                                 enabled);
                                    get_printer_state().set_timelapse_default_enabled(enabled);
                                },
                                [](const MoonrakerError& err) {
                                    spdlog::debug(
                                        "[Moonraker Client] Timelapse settings fetch failed: {}",
                                        err.message);
                                    // Leave the default false — component exists
                                    // but settings unreadable.
                                },
                                0,     // default timeout
                                true); // silent — timelapse not always configured
                        }
                    }
                }

            // Fire-and-forget webcam detection - independent of components list.
            // Skipped on platforms without a camera widget (HELIX_HAS_CAMERA=0:
            // AD5M/AD5X/CC1/K1/K2/MIPS/SnapmakerU1). On AD5X specifically, the
            // firmware's H.264 codec driver crashes the kernel on v4l2_open
            // (dma_coherent_mem_available NULL deref → process killed by SIGBUS),
            // so even probing for a webcam is unwanted.
#if HELIX_HAS_CAMERA
                client_.send_jsonrpc(
                    "server.webcams.list", json::object(),
                    [](json response) {
                        bool has_webcam = false;
                        std::string chosen_name;
                        std::string chosen_service;
                        std::string stream_url;
                        std::string snapshot_url;
                        bool flip_h = false;
                        bool flip_v = false;
                        int target_fps = 15;
                        if (response.contains("result") && response["result"].contains("webcams")) {
                            const auto& cams = response["result"]["webcams"];

                            // MJPEG-family service substrings CameraStream can consume
                            // directly. Everything else (webrtc-*, ipcamera, hlsstream, …)
                            // returns HTML/SDP/HLS and wastes three failover attempts
                            // before snapshot polling kicks in.
                            auto is_mjpeg_service = [](const std::string& svc) {
                                if (svc.empty())
                                    return false;
                                return svc.find("mjpeg") != std::string::npos ||
                                       svc.find("ustreamer") != std::string::npos;
                            };

                            // First pass: prefer an enabled MJPEG-compatible webcam so we
                            // get the real live stream for QR decoding.
                            for (const auto& cam : cams) {
                                if (!cam.value("enabled", true))
                                    continue;
                                std::string service = cam.value("service", "");
                                if (is_mjpeg_service(service)) {
                                    has_webcam = true;
                                    chosen_name = cam.value("name", "");
                                    chosen_service = service;
                                    stream_url = cam.value("stream_url", "");
                                    snapshot_url = cam.value("snapshot_url", "");
                                    flip_h = cam.value("flip_horizontal", false);
                                    flip_v = cam.value("flip_vertical", false);
                                    target_fps = cam.value("target_fps", 15);
                                    break;
                                } else {
                                    spdlog::debug("[Discovery] Skipping webcam '{}' stream_url: "
                                                  "service '{}' is not MJPEG-compatible",
                                                  cam.value("name", ""),
                                                  service.empty() ? "<unset>" : service);
                                }
                            }

                            // Second pass: no MJPEG entry — fall back to any enabled entry
                            // with a usable snapshot_url. Force snapshot-only mode by
                            // leaving stream_url empty so CameraStream doesn't waste three
                            // failed MJPEG attempts on a WebRTC/HLS endpoint first.
                            if (!has_webcam) {
                                for (const auto& cam : cams) {
                                    if (!cam.value("enabled", true))
                                        continue;
                                    std::string snap = cam.value("snapshot_url", "");
                                    if (snap.empty())
                                        continue;
                                    // Reject HTML viewer pages (e.g. the K2's
                                    // "iframe" service points snapshot_url at
                                    // /snapshot.html). CameraStream would poll it
                                    // forever and never decode a JPEG frame.
                                    if (!is_usable_snapshot_url(snap)) {
                                        spdlog::info(
                                            "[Discovery] Rejecting snapshot fallback for "
                                            "service '{}': snapshot_url '{}' is an HTML page, "
                                            "not an image endpoint",
                                            cam.value("service", "").empty()
                                                ? "<unset>"
                                                : cam.value("service", ""),
                                            snap);
                                        continue;
                                    }
                                    has_webcam = true;
                                    chosen_name = cam.value("name", "");
                                    chosen_service = cam.value("service", "");
                                    stream_url = "";
                                    snapshot_url = snap;
                                    flip_h = cam.value("flip_horizontal", false);
                                    flip_v = cam.value("flip_vertical", false);
                                    target_fps = cam.value("target_fps", 15);
                                    spdlog::info(
                                        "[Discovery] No MJPEG webcam found; using snapshot-only "
                                        "fallback for service '{}'",
                                        chosen_service.empty() ? "<unset>" : chosen_service);
                                    break;
                                }
                            }
                        }
                        // Guard against a stale ABSOLUTE webcam URL — e.g. an
                        // install-time-detected LAN IP that has since changed via
                        // DHCP, or an IOT-subnet address unreachable from here. A
                        // registered-but-dead entry otherwise wins over (and
                        // suppresses) the localhost probe below, leaving the camera
                        // silently broken. Probe the SNAPSHOT url only — it returns
                        // and closes, unlike an MJPEG stream that would hang until
                        // the timeout and read as a false negative. Relative URLs
                        // are skipped: they resolve against the Moonraker base and
                        // are the churn-immune case we don't need to second-guess.
                        if (has_webcam && !snapshot_url.empty()) {
                            auto is_absolute = [](const std::string& u) {
                                return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
                            };
                            if (is_absolute(snapshot_url) &&
                                !probe_snapshot_reachable(snapshot_url)) {
                                spdlog::warn(
                                    "[Discovery] Configured webcam '{}' unreachable at {} — "
                                    "falling back to local camera probe",
                                    chosen_name.empty() ? "<unnamed>" : chosen_name, snapshot_url);
                                has_webcam = false;
                            }
                        }
                        if (has_webcam) {
                            spdlog::info("[Discovery] Webcam selected: name='{}' service='{}' "
                                         "stream={} snapshot={}",
                                         chosen_name,
                                         chosen_service.empty() ? "<unset>" : chosen_service,
                                         stream_url.empty() ? "<none>" : stream_url,
                                         snapshot_url.empty() ? "<none>" : snapshot_url);
                            get_printer_state().set_webcam_available(true, stream_url, snapshot_url,
                                                                     flip_h, flip_v, target_fps);
                        } else {
                            // No Moonraker webcam config — probe local camera endpoints
                            // Run synchronously on the WS callback instead of spawning a
                            // detached std::thread. Thread creation crashes on resource-
                            // constrained ARM devices (AD5M #724) — std::terminate is
                            // called even with try/catch, likely a GCC 10.3/ARM TLS bug.
                            // Synchronous probing blocks the WS thread, but these are all
                            // loopback addresses: an unbound port is refused immediately,
                            // so the common "no local camera" path costs milliseconds, not
                            // the full per-URL budget. Runs once during discovery.
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
                                // Same two-budget probe as the configured-webcam case above.
                                // A local go2rtc is just as capable of needing a keyframe
                                // wait, and an unbound loopback port is refused instantly,
                                // so the longer response budget costs nothing here.
                                if (probe_snapshot_reachable(url)) {
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
#else
                // No camera widget on this platform — explicitly mark unavailable
                // so any consumer that observes the subject sees a definitive
                // "no" instead of the default-constructed initial state.
                get_printer_state().set_webcam_available(false);
#endif // HELIX_HAS_CAMERA

                // Fire-and-forget power device detection
                discover_power_devices();
                discover_sensors();

                // Step 3: Get printer information
                client_.send_jsonrpc("printer.info", {}, [this, seq](json printer_response) {
                    if (is_stale() || !is_current_sequence(seq))
                        return;
                    if (printer_response.contains("result")) {
                        const json& result = printer_response["result"];
                        auto hostname = result.value("hostname", "unknown");
                        auto software_version = result.value("software_version", "unknown");
                        {
                            std::lock_guard<std::mutex> lock(hardware_mutex_);
                            hardware_.set_hostname(hostname);
                            hardware_.set_software_version(software_version);
                        }
                        std::string state = result.value("state", "");
                        std::string state_message = result.value("state_message", "");

                        // Detect Kalico (Klipper fork with MPC support)
                        auto app = result.value("app", "");
                        if (app == "Kalico") {
                            std::lock_guard<std::mutex> lock(hardware_mutex_);
                            hardware_.set_is_kalico(true);
                            spdlog::info("[Moonraker Client] Kalico firmware detected");
                        }

                        spdlog::debug("[Moonraker Client] Printer hostname: {}", hostname);
                        spdlog::debug("[Moonraker Client] Klipper software version: {}",
                                      software_version);
                        if (!state_message.empty()) {
                            spdlog::info("[Moonraker Client] Printer state: {}", state_message);
                        }

                        // Seed klippy state from the printer.info response so
                        // shutdown/error at startup is recognised before any
                        // webhooks frame arrives.
                        //
                        // SEED only, never override: this RPC's response describes
                        // the printer as of when Moonraker answered, and discovery
                        // runs concurrently with live WebSocket traffic. Klipper can
                        // shut down between the request and the response, and the
                        // stale answer would then re-enable everything against a
                        // dead printer.
                        if (state == "shutdown" || state == "disconnected") {
                            spdlog::warn("[Moonraker Client] Printer is in {} state at startup",
                                         state);
                            get_printer_state().set_klippy_state_if_unseeded(KlippyState::SHUTDOWN);
                        } else if (state == "error") {
                            spdlog::warn("[Moonraker Client] Printer is in ERROR state at startup");
                            get_printer_state().set_klippy_state_if_unseeded(KlippyState::ERROR);
                        } else if (state == "startup") {
                            spdlog::info("[Moonraker Client] Printer is starting up");
                            get_printer_state().set_klippy_state_if_unseeded(KlippyState::STARTUP);
                        } else if (state == "ready") {
                            get_printer_state().set_klippy_state_if_unseeded(KlippyState::READY);
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
                                std::unordered_set<std::string> macros_snapshot;
                                {
                                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                                    hardware_.parse_config_keys(cfg);
                                    macros_snapshot = hardware_.macros();
                                }
                                MacroParamCache::instance().populate_from_configfile(
                                    cfg, macros_snapshot);

                                // Seed probe sensor z_offset from configfile (some probe
                                // modules like flashforge_loadcell return null in status).
                                // Accelerometers have no get_status(), so configfile.config
                                // is the ONLY place they appear — this is the sole caller
                                // that fills AccelSensorManager, which Settings > Sensors,
                                // telemetry and detect_belt_hardware() all read.
                                // Both must run on main thread — update_subjects() sets
                                // LVGL subjects. discover_from_config() rebuilds its list
                                // from scratch, so a reconnect re-run cannot duplicate.
                                nlohmann::json cfg_for_sensors = cfg;
                                helix::ui::queue_update([cfg_for_sensors]() {
                                    helix::sensors::ProbeSensorManager::instance()
                                        .discover_from_config(cfg_for_sensors);
                                    helix::sensors::AccelSensorManager::instance()
                                        .discover_from_config(cfg_for_sensors);
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

                                // Build volume from the [stepper_*] travel limits. This is
                                // the only place it can be filled before discovery completes:
                                // the safety-limits fetch that also writes it is kicked off
                                // from the discovery-complete handler, i.e. after
                                // PrinterDetector::auto_detect() has already run and read an
                                // empty volume. Two thirds of the printer database discriminate
                                // on build_volume_range, so detection was declining without its
                                // single most decisive input.
                                //
                                // Pure data write under the same mutex the config parse above
                                // takes — no LVGL and no subject is touched here. The API-side
                                // fetch still runs later and still calls
                                // notify_build_volume_changed(), so any UI that observes the
                                // volume is refreshed from the main thread as before.
                                {
                                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                                    hardware_.parse_build_volume(settings);
                                }

                                helix::MacroFanAnalyzer analyzer;
                                auto macro_result = analyzer.analyze(settings);

                                // Record the chamber cooling fan's configured resting/off
                                // target. M141 S0 resets the cooling fan to this value (35°C
                                // on the K2); recognizing it lets the chamber report Off
                                // instead of misreading the resting fan target as a deliberate
                                // "Maintaining" set. Klipper lowercases section names in
                                // configfile.settings, so lowercase the fan's object name for
                                // the lookup. Stored on hardware_ and read in
                                // PrinterState::set_hardware (same path as the cooling-fan name).
                                {
                                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                                    std::string fan = hardware_.chamber_cooling_fan_name();
                                    if (!fan.empty()) {
                                        std::string key = fan;
                                        std::transform(key.begin(), key.end(), key.begin(),
                                                       ::tolower);
                                        if (settings.contains(key) &&
                                            settings[key].contains("target_temp") &&
                                            settings[key]["target_temp"].is_number()) {
                                            int resting_deci = helix::units::to_decidegrees(
                                                settings[key]["target_temp"].get<double>());
                                            hardware_.set_chamber_fan_resting_deci(resting_deci);
                                            spdlog::debug("[Discovery] Chamber cooling fan '{}' "
                                                          "resting target: {} deci",
                                                          fan, resting_deci);
                                        }
                                    }
                                }

                                // Record each fan's configured max_power. Klipper reports
                                // `speed` as last_fan_value = value * max_power, so the UI
                                // divides it back out to show the logical fraction (a full-on
                                // heater_fan with max_power: 0.5 should read 100%, not 50% —
                                // matching Mainsail). Section names in configfile.settings are
                                // lowercased by Klipper; store keyed the same way so
                                // PrinterFanState::normalize_speed() (lowercased lookup) matches.
                                {
                                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                                    for (const auto& [section, cfg_vals] : settings.items()) {
                                        const bool is_fan_section =
                                            section == "fan" ||
                                            section.rfind("heater_fan ", 0) == 0 ||
                                            section.rfind("fan_generic ", 0) == 0 ||
                                            section.rfind("controller_fan ", 0) == 0 ||
                                            section.rfind("temperature_fan ", 0) == 0;
                                        if (!is_fan_section || !cfg_vals.is_object() ||
                                            !cfg_vals.contains("max_power") ||
                                            !cfg_vals["max_power"].is_number()) {
                                            continue;
                                        }
                                        double mp = cfg_vals["max_power"].get<double>();
                                        if (mp <= 0.0 || mp >= 1.0) {
                                            continue; // 1.0 (or invalid) means no scaling
                                        }
                                        hardware_.set_fan_max_power(section, mp);
                                        spdlog::debug("[Discovery] Fan '{}' max_power: {}", section,
                                                      mp);
                                    }
                                }

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
                                {
                                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                                    hardware_.set_os_version(os_name);
                                }
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
                                {
                                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                                    hardware_.set_cpu_arch(cpu_arch);
                                }
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
                    std::vector<std::string> printer_objects_snapshot;
                    {
                        std::lock_guard<std::mutex> lock(hardware_mutex_);
                        printer_objects_snapshot = hardware_.printer_objects();
                    }
                    std::vector<std::string> mcu_objects;
                    for (const auto& obj : printer_objects_snapshot) {
                        // Match "mcu" or "mcu <name>" pattern
                        if (obj == "mcu" || obj.rfind("mcu ", 0) == 0) {
                            mcu_objects.push_back(obj);
                        }
                    }

                    if (mcu_objects.empty()) {
                        spdlog::debug(
                            "[Moonraker Client] No MCU objects found, skipping MCU query");
                        // Continue to subscription step
                        complete_discovery_subscription(seq);
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
                            [this, seq, mcu_obj, pending_mcu_queries, mcu_results,
                             mcu_version_results, mcu_results_mutex](json mcu_response) {
                                if (is_stale() || !is_current_sequence(seq))
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
                                    {
                                        std::lock_guard<std::mutex> lock(hardware_mutex_);
                                        hardware_.set_mcu(primary_mcu);
                                        hardware_.set_mcu_list(mcu_list);
                                        hardware_.set_mcu_versions(*mcu_version_results);
                                    }

                                    if (!primary_mcu.empty()) {
                                        spdlog::info("[Moonraker Client] Primary MCU: {}",
                                                     primary_mcu);
                                    }
                                    if (mcu_list.size() > 1) {
                                        spdlog::info("[Moonraker Client] All MCUs: {}",
                                                     json(mcu_list).dump());
                                    }

                                    // Continue to subscription step
                                    complete_discovery_subscription(seq);
                                }
                            },
                            [this, seq, mcu_obj, pending_mcu_queries](const MoonrakerError& err) {
                                if (is_stale() || !is_current_sequence(seq))
                                    return;

                                spdlog::warn("[Moonraker Client] MCU query for '{}' failed: {}",
                                             mcu_obj, err.message);

                                // Check if all queries complete (even on error)
                                if (pending_mcu_queries->fetch_sub(1) == 1) {
                                    // Continue to subscription step even if some MCU queries
                                    // failed
                                    complete_discovery_subscription(seq);
                                }
                            });
                    }
                });
            });
        },
        [this, seq](const MoonrakerError& err) {
            if (is_stale() || !is_current_sequence(seq))
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

json MoonrakerDiscoverySequence::build_subscription_objects(
    const PrinterDiscovery& hw, const std::vector<std::string>& heaters,
    const std::vector<std::string>& sensors, const std::vector<std::string>& fans,
    const std::vector<std::string>& leds, const std::vector<std::string>& afc_objects,
    const std::vector<std::string>& filament_sensors, const std::vector<std::string>& mcus) {
    json subscription_objects;

    // Core non-optional objects — narrow each to the fields HelixScreen
    // parsers actually read. Klipper publishes notify_status_update on every
    // subscribed-field change, and several of these (toolhead, gcode_move,
    // motion_report) update on every motion step (~100Hz during a print) —
    // nullptr would have us receiving every internal field per step.
    // power_loss: Creality-Klipper-fork-only Power-Loss-Recovery capability
    // marker. Absent on mainline Klipper — Moonraker answers a subscribed field
    // the firmware never populates with an explicit null, which the parser
    // rejects (presence must mean present AND numeric). See
    // docs/devel/POWER_LOSS_RECOVERY.md.
    subscription_objects["print_stats"] =
        json::array({"state", "filename", "filament_used", "print_duration", "total_duration",
                     "estimated_time", "info", "message", "power_loss"});
    // virtual_sdcard.progress drives the progress bar. layer / layer_count
    // are the FALLBACK source for layer tracking — preferred source is
    // print_stats.info.{current_layer,total_layer} (slicer-supplied via
    // SET_PRINT_STATS_INFO), with virtual_sdcard.layer / layer_count taking
    // over when info isn't populated. PrinterPrintState reads both.
    // is_active distinguishes "paused but resumable" from "paused but
    // SD playback was deactivated" (Snapmaker U1 dirty-bed exception,
    // level-2 aborts, post-SDCARD_RESET_FILE). prepare_for_resume reads
    // it to decide between RESUME and "Restart from beginning?" UX.
    // pl_env_valid / file_path: Snapmaker-fork Power-Loss-Recovery fields.
    // Absent (harmless) on mainline Klipper — Moonraker sends explicit null
    // for a subscribed field the connected firmware never populates.
    subscription_objects["virtual_sdcard"] =
        json::array({"progress", "layer", "layer_count", "is_active", "pl_env_valid", "file_path"});
    subscription_objects["toolhead"] =
        json::array({"position", "homed_axes", "kinematics", "extruder", "max_velocity",
                     "axis_minimum", "axis_maximum"});
    subscription_objects["gcode_move"] =
        json::array({"gcode_position", "speed", "speed_factor", "extrude_factor", "homing_origin"});
    subscription_objects["motion_report"] = json::array({"live_extruder_velocity"});
    subscription_objects["display_status"] = json::array({"message", "progress"});

    // system_stats was previously subscribed with nullptr but no parser ever
    // reads it — drop the subscription entirely. (idle_timeout IS subscribed
    // below with a narrowed ["state"] field; PrinterCalibrationState reads it.)

    // Klipper firmware state (shutdown/error detection + state_message)
    subscription_objects["webhooks"] = json::array({"state", "state_message"});

    // Pause/resume state (PAUSE/RESUME gcode) — PrinterState::is_paused() reads this.
    subscription_objects["pause_resume"] = json::array({"is_paused"});

    // All discovered heaters (extruders, beds, generic heaters).
    // PrinterTemperatureState reads only temperature + target.
    static const json heater_fields = json::array({"temperature", "target"});
    for (const auto& heater : heaters) {
        subscription_objects[heater] = heater_fields;
    }

    // All discovered sensors. temperature_fan also lives in fans and is
    // overwritten in the fans loop below with the union of fields. "humidity" is
    // requested for every sensor — Moonraker simply omits it for sensors that
    // don't report it (temperature-only), and it carries box humidity for the
    // bme280/htu21d/sht3x chips a Happy Hare filament dryer reads.
    static const json temp_sensor_fields = json::array({"temperature", "humidity"});
    for (const auto& sensor : sensors) {
        subscription_objects[sensor] = temp_sensor_fields;
    }

    // All discovered fans. Klipper publishes fan speed updates whenever PWM
    // duty changes — without narrowing, every config field would also stream
    // back per update. Field shape varies by object type:
    //   - "fan", heater_fan, fan_generic, controller_fan: { "speed" }
    //   - temperature_fan: { "temperature", "target", "speed" } (also a sensor)
    //   - output_pin: { "value" } (Creality-style)
    static const json fan_speed_fields = json::array({"speed"});
    static const json temp_fan_fields = json::array({"temperature", "target", "speed"});
    static const json output_pin_value_fields = json::array({"value"});
    for (const auto& fan : fans) {
        if (fan.rfind("temperature_fan ", 0) == 0) {
            subscription_objects[fan] = temp_fan_fields;
        } else if (fan.rfind("output_pin ", 0) == 0) {
            subscription_objects[fan] = output_pin_value_fields;
        } else {
            subscription_objects[fan] = fan_speed_fields;
        }
    }

    // Creality tachometer module — PrinterFanState reads fan0_speed..fan9_speed.
    if (hw.has_fan_feedback()) {
        static const json fan_feedback_fields =
            json::array({"fan0_speed", "fan1_speed", "fan2_speed", "fan3_speed", "fan4_speed",
                         "fan5_speed", "fan6_speed", "fan7_speed", "fan8_speed", "fan9_speed"});
        subscription_objects["fan_feedback"] = fan_feedback_fields;
    }

    // All discovered LEDs. Native LEDs (neopixel/dotstar/led) report color_data;
    // output_pin LEDs report a single value. Other config fields are static
    // topology already fetched at startup.
    static const json led_color_fields = json::array({"color_data"});
    for (const auto& led : leds) {
        if (led.rfind("output_pin ", 0) == 0) {
            subscription_objects[led] = output_pin_value_fields;
        } else {
            subscription_objects[led] = led_color_fields;
        }
    }

    // All discovered LED effects (klipper-led_effect plugin objects).
    // We only read `enabled`. Klipper otherwise publishes per-frame animation
    // state on every effect tick — subscribing to nullptr would have us
    // receiving (and Klipper serialising) hundreds of messages per second on
    // hardware with many led_effects (e.g. AFC BoxTurtle: 2 effects × 8 lanes
    // = 46 effect objects updating in lockstep with the print).
    for (const auto& effect : hw.led_effects()) {
        subscription_objects[effect] = json::array({"enabled"});
    }

    // Bed mesh (for 3D visualization). MoonrakerAdvancedAPI reads only the
    // profile/topology fields; the rest of `bed_mesh` is internal state.
    subscription_objects["bed_mesh"] = json::array(
        {"profile_name", "probed_matrix", "mesh_min", "mesh_max", "mesh_params", "profiles"});

    // Exclude object (for mid-print object exclusion). PrinterState reads
    // excluded_objects + objects (with name/center/polygon) + current_object.
    subscription_objects["exclude_object"] =
        json::array({"objects", "excluded_objects", "current_object"});

    // Manual probe (Z-offset calibration). PrinterCalibrationState reads
    // is_active + z_position.
    subscription_objects["manual_probe"] = json::array({"is_active", "z_position"});

    // Stepper enable state (for motor enabled/disabled detection on M84).
    subscription_objects["stepper_enable"] = json::array({"steppers"});

    // idle_timeout (Klipper's canonical busy flag). state == "Printing" for the
    // full duration of ANY blocking op (G28, BED_MESH_CALIBRATE, QGL,
    // PROBE_ACCURACY, long macro) issued from idle. PrinterCalibrationState reads
    // idle_timeout.state to drive the "blocking non-print operation" signal.
    // Narrowed to the single `state` field to avoid needless notifications.
    subscription_objects["idle_timeout"] = json::array({"state"});

    // Happy Hare MMU object (gate status, colors, materials, filament info)
    // Subscribe to specific fields only — nullptr means ALL fields, which causes
    // excessive notifications and Klipper-side serialization cost (#388)
    if (hw.has_mmu()) {
        // endless_spool_enabled is the ENABLE bit for endless_spool_groups. Happy Hare
        // ignores a GROUPS= write while it is 0, so an edit sent without reading it first
        // fails silently.
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
                                                   "endless_spool_enabled",
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
    }

    // All discovered AFC objects — narrow per object-type to the fields the
    // AmsBackendAfc parsers actually read. nullptr here would cost dearly:
    // an AFC BoxTurtle setup typically has ~9 AFC_* objects, all updated by
    // the firmware on every lane state change. AFC_led is currently subscribed
    // but never parsed by HelixScreen; skip it entirely.
    //
    // Field lists mirror parse_afc_state / parse_afc_stepper / parse_afc_hub /
    // parse_afc_buffer / parse_afc_extruder / parse_afc_unit_object in
    // src/printer/ams_backend_afc.cpp. Keep these in sync when adding parser
    // fields.
    static const json afc_state_fields = json::array({"connected",
                                                      "bypass_state",
                                                      "quiet_mode",
                                                      "current_load",
                                                      "current_lane",
                                                      "current_state",
                                                      "current_tool",
                                                      "current_toolchange",
                                                      "error_state",
                                                      "filament_loaded",
                                                      "lane_loaded",
                                                      "led_state",
                                                      "message",
                                                      "name",
                                                      "number_of_toolchanges",
                                                      "num_extruders",
                                                      "status",
                                                      "system",
                                                      "tool_sensor_after_extruder",
                                                      "tool_stn",
                                                      "tool_stn_unload",
                                                      "type",
                                                      "units",
                                                      "lanes",
                                                      "hubs",
                                                      "extruders",
                                                      "buffers"});
    // "current_map" (AFC virtual tools, #605) names which of a multi-tool lane's
    // T-commands is active. The subscription is a strict allowlist, so a field
    // missing here never reaches parse_afc_stepper at all. Older AFC simply does not
    // publish it and Moonraker omits what an object does not have, so asking for it
    // is safe against every version.
    static const json afc_stepper_fields =
        json::array({"buffer_status", "color", "current_map", "dist_hub", "extruder",
                     "filament_status", "hub", "load", "loaded_to_hub", "map", "material", "prep",
                     "runout_lane", "spool_id", "status", "tool_loaded", "weight"});
    static const json afc_hub_fields = json::array({"state", "afc_bowden_length"});
    static const json afc_buffer_fields = json::array(
        {"state", "distance_to_fault", "error_sensitivity", "fault_detection_enabled", "lanes"});
    static const json afc_extruder_fields =
        json::array({"lane_loaded", "tool_end_status", "tool_start_status"});
    static const json afc_unit_fields = json::array({"lanes", "extruders", "hubs", "buffers"});

    for (const auto& afc_obj : afc_objects) {
        // Top-level "AFC" (no space, no underscore-suffix) — system state
        if (afc_obj == "AFC" || afc_obj == "afc") {
            subscription_objects[afc_obj] = afc_state_fields;
        } else if (afc_obj.rfind("AFC_stepper ", 0) == 0 || afc_obj.rfind("AFC_lane ", 0) == 0) {
            subscription_objects[afc_obj] = afc_stepper_fields;
        } else if (afc_obj.rfind("AFC_hub ", 0) == 0) {
            subscription_objects[afc_obj] = afc_hub_fields;
        } else if (afc_obj.rfind("AFC_buffer ", 0) == 0) {
            subscription_objects[afc_obj] = afc_buffer_fields;
        } else if (afc_obj.rfind("AFC_extruder ", 0) == 0) {
            subscription_objects[afc_obj] = afc_extruder_fields;
        } else if (afc_obj.rfind("AFC_led ", 0) == 0) {
            // Not parsed anywhere in HelixScreen — skip subscription entirely
            continue;
        } else {
            // Unit-level object: AFC_BoxTurtle, AFC_OpenAMS, AFC_vivid,
            // AFC_NightOwl, etc. — parse_afc_unit_object reads only topology
            // arrays.
            subscription_objects[afc_obj] = afc_unit_fields;
        }
    }

    // AD5X IFS requires save_variables for filament state (colors, types, tool mapping)
    if (hw.mmu_type() == AmsType::AD5X_IFS) {
        subscription_objects["save_variables"] = nullptr;
    }

    // Firmware that keeps the authoritative z-offset outside gcode_move needs
    // whatever object stores it; without this the Z-offset row reads 0.000
    // whenever such a printer is idle. See include/z_offset_persistence.h.
    for (const auto& obj : helix::zoffset::required_status_objects(hw)) {
        subscription_objects[obj] = nullptr;
    }

    // ACE (Anycubic ACE Pro — ValgACE/BunnyACE/DuckACE Klipper drivers, native
    // GoKlipper `filament_hub`, or the Kobra S1 mainline-Python fork's
    // `ace_instance_N` objects, #1107). Subscribe the real detected object
    // name(s) so slot colors, materials, status, and dryer state push live via
    // get_status(). Falls back to `ace` if the name list is unexpectedly empty.
    if (hw.mmu_type() == AmsType::ACE) {
        if (hw.ace_object_names().empty()) {
            subscription_objects["ace"] = nullptr;
        } else {
            for (const auto& ace_obj : hw.ace_object_names()) {
                subscription_objects[ace_obj] = nullptr;
            }
        }
    }

    // CFS (Creality Filament System) — K2 series with RS-485 CFS units
    if (hw.mmu_type() == AmsType::CFS) {
        subscription_objects["box"] = nullptr;
        subscription_objects["motor_control"] = nullptr;
    }

    // Snapmaker U1 SnapSwap — RFID filament, feed modules, task config, extruder states
    if (hw.mmu_type() == AmsType::SNAPMAKER) {
        subscription_objects["filament_detect"] = nullptr;
        subscription_objects["filament_feed left"] = nullptr;
        subscription_objects["filament_feed right"] = nullptr;
        subscription_objects["print_task_config"] = nullptr;
        subscription_objects["machine_state_manager"] = nullptr;
        for (int i = 0; i < 4; ++i) {
            subscription_objects[fmt::format("filament_motion_sensor e{}_filament", i)] = nullptr;
        }
    }

    // QIDI Box — box_extras carries box_drying_state.box<N>.{dry_state, end_time}
    // for the dryer countdown (issue #1019); save_variables carries slot/box state.
    // Both are also bootstrap-queried in AmsBackendQidi::on_started(); subscribing
    // keeps drying-state changes pushing live to handle_status_update().
    if (hw.mmu_type() == AmsType::QIDI_BOX) {
        subscription_objects["box_extras"] = nullptr;
        subscription_objects["save_variables"] = nullptr;
    }

    // All discovered filament sensors (filament_switch_sensor, filament_motion_sensor).
    // FilamentSensorManager reads filament_detected + enabled + detection_count.
    static const json filament_sensor_fields =
        json::array({"filament_detected", "enabled", "detection_count"});
    for (const auto& sensor : filament_sensors) {
        subscription_objects[sensor] = filament_sensor_fields;
    }

    // All discovered width sensors. WidthSensorManager reads Diameter + Raw.
    if (hw.has_width_sensors()) {
        static const json width_sensor_fields = json::array({"Diameter", "Raw"});
        for (const auto& sensor : hw.width_sensor_objects()) {
            subscription_objects[sensor] = width_sensor_fields;
        }
    }

    // Toolchanger + per-tool objects. AmsBackendToolchanger reads
    // status/tool_number/tool_numbers; ToolState reads tool active/mounted/
    // detect_state + gcode_*_offset + extruder + fan.
    if (hw.has_tool_changer()) {
        subscription_objects["toolchanger"] =
            json::array({"status", "tool_number", "tool_numbers"});
        static const json tool_fields =
            json::array({"active", "mounted", "detect_state", "gcode_x_offset", "gcode_y_offset",
                         "gcode_z_offset", "extruder", "fan"});
        for (const auto& tool_name : hw.tool_names()) {
            subscription_objects["tool " + tool_name] = tool_fields;
        }
    }

    // Firmware retraction. PrinterCalibrationState reads the four tunable
    // fields; the rest of the firmware_retraction object is static.
    if (hw.has_firmware_retraction()) {
        subscription_objects["firmware_retraction"] = json::array(
            {"retract_length", "retract_speed", "unretract_extra_length", "unretract_speed"});
    }

    // Print start macros — only the boolean flags PrintStartCollector reads.
    // Other variables on these macros (slicer args, profile names, etc.) are
    // irrelevant to the start-detection logic.
    subscription_objects["gcode_macro _START_PRINT"] = json::array({"print_started"});
    subscription_objects["gcode_macro START_PRINT"] = json::array({"preparation_done"});
    subscription_objects["gcode_macro _HELIX_STATE"] = json::array({"print_started"});

    // MCUs — PerformanceState (MoonrakerPerformanceSource) reads
    // last_stats.mcu_awake for the load % and last_stats.bytes_retransmit
    // for the link-health counter. Subscribing to last_stats pulls the whole
    // dict (both fields land in one update). Host throttle bits arrive via
    // notify_proc_stat_update (not subscribed here). Must be in this union
    // subscription: printer.objects.subscribe REPLACES the per-connection
    // subscription, so a separate subscribe call from PerformanceSource
    // would wipe everything above.
    static const json mcu_fields = json::array({"last_stats"});
    for (const auto& mcu : mcus) {
        subscription_objects[mcu] = mcu_fields;
    }

    return subscription_objects;
}

void MoonrakerDiscoverySequence::complete_discovery_subscription(uint64_t seq) {
    // Snapshot hardware_ once under lock for all reads in this method (#777)
    PrinterDiscovery hw;
    {
        std::lock_guard<std::mutex> lock(hardware_mutex_);
        hw = hardware_;
    }

    // Step 5: Subscribe to all discovered objects + core objects. The pure
    // helper builds the full objects map; per-section logging stays here.
    json subscription_objects = build_subscription_objects(hw, heaters_, sensors_, fans_, leds_,
                                                           afc_objects_, filament_sensors_, mcus_);

    if (!mcus_.empty()) {
        spdlog::info("[Moonraker Client] Subscribing to {} MCU object(s): {}", mcus_.size(),
                     json(mcus_).dump());
    }

    spdlog::info("[Moonraker Client] Subscribing to {} fans: {}", fans_.size(), json(fans_).dump());
    if (hw.has_fan_feedback()) {
        spdlog::debug("[MoonrakerDiscoverySequence] Subscribing to fan_feedback for RPM data");
    }
    if (hw.has_mmu()) {
        spdlog::info("[Moonraker Client] Subscribing to MMU object (Happy Hare)");
    }
    int afc_led_skipped = 0;
    for (const auto& afc_obj : afc_objects_) {
        if (afc_obj.rfind("AFC_led ", 0) == 0)
            ++afc_led_skipped;
    }
    if (afc_led_skipped > 0) {
        spdlog::debug("[Moonraker Client] Skipped {} unparsed AFC_led object(s) from subscription",
                      afc_led_skipped);
    }
    if (hw.mmu_type() == AmsType::AD5X_IFS) {
        spdlog::info("[Moonraker Client] Subscribing to save_variables (AD5X IFS)");
    }
    if (helix::zoffset::firmware_persists_z_offset(hw)) {
        spdlog::info("[Moonraker Client] Subscribing persisted z-offset objects ({})",
                     helix::zoffset::persistence_provider_name(hw));
    }
    if (hw.mmu_type() == AmsType::ACE) {
        spdlog::info("[Moonraker Client] Subscribing to ace object (Anycubic ACE)");
    }
    if (hw.mmu_type() == AmsType::CFS) {
        spdlog::info("[Moonraker Client] Subscribing to box + motor_control (CFS)");
    }
    if (hw.mmu_type() == AmsType::SNAPMAKER) {
        spdlog::info("[Moonraker Client] Subscribing to Snapmaker filament + feed objects");
    }
    if (hw.has_width_sensors()) {
        spdlog::info("[Moonraker Client] Subscribing to {} width sensors",
                     hw.width_sensor_objects().size());
    }
    if (hw.has_tool_changer()) {
        spdlog::info("[Moonraker Client] Subscribing to toolchanger + {} tool objects",
                     hw.tool_names().size());
    }

    json subscribe_params = {{"objects", subscription_objects}};
    size_t num_subscribed = subscription_objects.size();

    client_.send_jsonrpc(
        "printer.objects.subscribe", subscribe_params,
        [this, seq, num_subscribed](json sub_response) {
            if (is_stale() || !is_current_sequence(seq))
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

            // Discovery complete - pass initial status to the callback so the caller
            // can dispatch it AFTER initializing subsystems (init_fans, etc.).
            // Previously dispatch_status_update was called here separately, but that
            // used a different queue than the init_fans callback, causing a race where
            // initial fan/sensor data was processed before subjects existed.
            // Copy hardware_ under lock before invoking callback (#562, #777).
            json initial_status;
            if (sub_response.contains("result") && sub_response["result"].contains("status")) {
                initial_status = sub_response["result"]["status"];
            }
            if (on_discovery_complete_) {
                PrinterDiscovery hw_snapshot;
                {
                    std::lock_guard<std::mutex> lock(hardware_mutex_);
                    hw_snapshot = hardware_;
                }
                on_discovery_complete_(hw_snapshot, initial_status);
            }
            discovery_completed_.store(true);
            if (on_complete_discovery_) {
                auto cb = std::move(on_complete_discovery_);
                on_error_discovery_ = nullptr;
                cb();
            }
        });
}

void MoonrakerDiscoverySequence::invoke_discovery_complete() {
    if (on_discovery_complete_) {
        PrinterDiscovery snapshot;
        {
            std::lock_guard<std::mutex> lock(hardware_mutex_);
            snapshot = hardware_;
        }
        on_discovery_complete_(snapshot, json::object());
    }
}

void MoonrakerDiscoverySequence::parse_objects(const json& objects) {
    // Populate unified hardware discovery (Phase 2)
    {
        std::lock_guard<std::mutex> lock(hardware_mutex_);
        hardware_.parse_objects(objects);
    }

    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();
    steppers_.clear();
    afc_objects_.clear();
    filament_sensors_.clear();
    mcus_.clear();

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
        // Humidity-capable sensor chips. Happy Hare's filament dryer reads box
        // humidity from these directly (mmu_environment_manager ENV_SENSOR_CHIPS).
        // Classified from objects.list, so only existing objects are subscribed;
        // the humidity field is added to the sensor subscription below.
        else if (helix::sensors::humidity_chip_for_object(name) != nullptr) {
            // aht20_f is QIDI's box humidity/temp chip ("aht20_f heater_box1",
            // publishes {temperature, humidity}); confirmed on stock Q2 (#1022).
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
        // These provide runout detection and encoder motion data.
        // AD5X native Z-Mod publishes its head/motion sensors under custom module
        // namespaces (zmod_ifs_switch_sensor / zmod_ifs_motion_sensor) rather than
        // the stock filament_switch_sensor / filament_motion_sensor sections, so
        // bucket those too — the IFS backend subscribes to them for head-loaded
        // state.
        else if (name.rfind("filament_switch_sensor ", 0) == 0 ||
                 name.rfind("filament_motion_sensor ", 0) == 0 ||
                 name.rfind("zmod_ifs_switch_sensor ", 0) == 0 ||
                 name.rfind("zmod_ifs_motion_sensor ", 0) == 0) {
            filament_sensors_.push_back(name);
        }
        // MCUs: "mcu" (primary) and "mcu <name>" (secondary boards, host MCU,
        // toolhead MCUs, etc.). Subscribed for PerformanceState — last_stats
        // for load %, bytes_retransmit for link health. Must ride the main
        // subscription because Moonraker replaces the per-connection
        // subscription on every printer.objects.subscribe call.
        else if (name == "mcu" || name.rfind("mcu ", 0) == 0) {
            mcus_.push_back(name);
        }
    }

    spdlog::debug("[Moonraker Client] Discovered: {} heaters, {} sensors, {} fans, {} LEDs, {} "
                  "steppers, {} AFC objects, {} filament sensors, {} MCUs",
                  heaters_.size(), sensors_.size(), fans_.size(), leds_.size(), steppers_.size(),
                  afc_objects_.size(), filament_sensors_.size(), mcus_.size());

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
    {
        std::lock_guard<std::mutex> lock(hardware_mutex_);
        hardware_.set_printer_objects(all_objects);
    }
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
