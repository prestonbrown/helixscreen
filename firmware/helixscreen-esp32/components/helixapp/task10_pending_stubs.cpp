// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEMPORARY link stubs for Moonraker sub-APIs NOT covered by Task 10's HTTP
// lane. Each symbol here is referenced by a kept app TU but defined in a
// source file excluded from this component (see app_srcs.txt "EXCLUDED, and
// why"). Bodies log + surface the failure through the API's ErrorCallback:
// a stray user action (e.g. tapping a temp preset) must fail gracefully, NOT
// abort/reset the board in the user's hands.
//
// Task 10 (esp32p4-task-10-report.md) moved MoonrakerRestAPI and
// MoonrakerFileTransferAPI OUT of this file and into helixnet/esp_rest_api.cpp
// — that's their real home now (download_file_partial is genuinely
// implemented there; every other method on those two classes is an asserting
// stub, per Task 10's R1 enumeration: none of them are reachable from the v1
// print-select thumbnail/metadata surface). What's LEFT here is out of Task
// 10's scope entirely:
//   - Group A (MoonrakerAPI facade controls below): the printer command
//     surface (set_temperature, execute_gcode, restart_*, etc.) — a later
//     Stage C/D task (print/settings HTTP, or JSON-RPC if that's how these
//     route on ESP) owns making these real.
//   - MoonrakerTimelapseAPI: timelapse viewer is excluded from the v1
//     Core+AMS cut (HELIX_HAS_TIMELAPSE_VIEWER=0) — no reachable UI calls it.
//
// Symbols are added here exactly as the linker demands them; each carries the
// excluded source file it comes from. The full inventory is mirrored in
// esp32p4-task-5-report.md.

#include "esp_log.h"
#include "moonraker_api.h"
#include "moonraker_spoolman_api.h"
#include "moonraker_timelapse_api.h"

#include <functional>
#include <string>
#include <vector>

namespace {
// Non-fatal: log the call and return. Every Moonraker ErrorCallback in this file
// is std::function<void(const MoonrakerError&)>, so callers that carry one route
// through task10_unimplemented_err() below to surface failure to the UI.
[[maybe_unused]] void task10_unimplemented(const char* sym) {
    ESP_LOGE("helixapp", "task10 stub: %s", sym);
}

// Same log, then invoke the API's error callback so the UI reports "feature
// unavailable" instead of hanging on a response that will never arrive. Does NOT
// call any SuccessCallback — that would signal a false success.
[[maybe_unused]] void
task10_unimplemented_err(const char* sym, const std::function<void(const MoonrakerError&)>& err) {
    task10_unimplemented(sym);
    if (err) {
        MoonrakerError e;
        e.type = MoonrakerErrorType::VALIDATION_ERROR;
        e.method = sym; // sym identifies the unavailable call; message left empty to save flash
        err(e);
    }
}
} // namespace

// ===========================================================================
// GROUP A — Moonraker HTTP/transport surface deferred to Task 10.
// ===========================================================================

// ---------------------------------------------------------------------------
// MoonrakerAPI power-device methods.
// The rest of the control surface (E-STOP, temperature, fan, gcode, restart)
// now compiles for real: src/api/moonraker_api_controls.cpp is in the manifest
// since its two HTTP calls were split into the EXCLUDED
// src/api/moonraker_api_power.cpp (one of the 5 hv/requests.h TUs). Only these
// two remain stubbed — Moonraker's device_power endpoints are REST-only, so
// they need an esp_http_client port rather than a send_jsonrpc call. The
// parameter typedefs (SuccessCallback, ErrorCallback, PowerDevicesCallback)
// resolve in MoonrakerAPI's inherited scope, guaranteeing the definitions
// mangle identically to the header declarations.
// ---------------------------------------------------------------------------

void MoonrakerAPI::get_power_devices(PowerDevicesCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerAPI::get_power_devices", err);
}

void MoonrakerAPI::set_device_power(const std::string&, const std::string&, SuccessCallback,
                                    ErrorCallback err) {
    task10_unimplemented_err("MoonrakerAPI::set_device_power", err);
}

// ---------------------------------------------------------------------------
// MoonrakerTimelapseAPI (timelapse_api_ member).
// moonraker_api.cpp (KEPT) constructs it as a std::unique_ptr member via
// make_unique and destroys it in ~MoonrakerAPI. Because the class OVERRIDES
// its interface's pure virtuals, a bare ctor stub is NOT enough: the ctor emits
// a reference to the class vtable, whose key function (the out-of-line dtor) and
// every virtual slot live in the EXCLUDED src/api/moonraker_timelapse_api.cpp.
// So we must define ctor + dtor + ALL virtuals here to let the compiler emit
// the vtable in this TU with every slot resolved.
//
//   ctor : real no-op — runs during MoonrakerAPI construction on boot. Only
//          initializes the two reference members (client_, http_base_url_).
//   dtor : real no-op — runs during MoonrakerAPI destruction on shutdown.
//   virtuals : log-only if called — the timelapse viewer is excluded from the
//              v1 Core+AMS cut (HELIX_HAS_TIMELAPSE_VIEWER=0), so there is no
//              reachable UI path to any of these on the ESP32 build.
//
// Callback-parameter typedefs resolve in the class's inherited scope.
// ---------------------------------------------------------------------------

// --- MoonrakerTimelapseAPI (src/api/moonraker_timelapse_api.cpp) ---
MoonrakerTimelapseAPI::MoonrakerTimelapseAPI(helix::IMoonrakerClient& client,
                                             const std::string& http_base_url)
    : client_(client), http_base_url_(http_base_url) {}

MoonrakerTimelapseAPI::~MoonrakerTimelapseAPI() {}

void MoonrakerTimelapseAPI::get_timelapse_settings(TimelapseSettingsCallback, ErrorCallback) {
    task10_unimplemented("MoonrakerTimelapseAPI::get_timelapse_settings");
}

void MoonrakerTimelapseAPI::set_timelapse_settings(const TimelapseSettings&, SuccessCallback,
                                                   ErrorCallback) {
    task10_unimplemented("MoonrakerTimelapseAPI::set_timelapse_settings");
}

void MoonrakerTimelapseAPI::set_timelapse_enabled(bool, SuccessCallback, ErrorCallback) {
    task10_unimplemented("MoonrakerTimelapseAPI::set_timelapse_enabled");
}

void MoonrakerTimelapseAPI::render_timelapse(SuccessCallback, ErrorCallback) {
    task10_unimplemented("MoonrakerTimelapseAPI::render_timelapse");
}

void MoonrakerTimelapseAPI::save_timelapse_frames(SuccessCallback, ErrorCallback) {
    task10_unimplemented("MoonrakerTimelapseAPI::save_timelapse_frames");
}

void MoonrakerTimelapseAPI::get_last_frame_info(std::function<void(const LastFrameInfo&)>,
                                                ErrorCallback) {
    task10_unimplemented("MoonrakerTimelapseAPI::get_last_frame_info");
}

void MoonrakerTimelapseAPI::get_webcam_list(WebcamListCallback, ErrorCallback) {
    task10_unimplemented("MoonrakerTimelapseAPI::get_webcam_list");
}

// ---------------------------------------------------------------------------
// MoonrakerSpoolmanAPI (spoolman_api_ member).
//
// Same shape as MoonrakerTimelapseAPI above, and for the same reason:
// moonraker_api.cpp (KEPT) owns it as a unique_ptr member, so the ctor is
// referenced on boot and emits the vtable in THIS TU — which means every
// ISpoolmanAPI slot must be defined here, not just the ctor.
//
// Spoolman itself is out of the v1 Core+AMS cut as of 2026-08-12 (see
// app_srcs_excluded.txt): `printer_has_spoolman` is only ever written by the
// second server.info discovery call, which the ESP client does not make, so
// every UI entry point is permanently hidden. If discovery is restored these
// stubs must go back to the real src/api/moonraker_spoolman_api.cpp.
//
// Bodies route through the ErrorCallback so a call that somehow arrives fails
// visibly instead of hanging on a response that will never come.
// ---------------------------------------------------------------------------

MoonrakerSpoolmanAPI::MoonrakerSpoolmanAPI(helix::IMoonrakerClient& client) : client_(client) {}

void MoonrakerSpoolmanAPI::get_spoolman_status(std::function<void(bool, int)>, ErrorCallback err,
                                               bool) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_status", err);
}

void MoonrakerSpoolmanAPI::get_spoolman_spools(helix::SpoolListCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_spools", err);
}

void MoonrakerSpoolmanAPI::get_spoolman_spool(int, helix::SpoolCallback, ErrorCallback err, bool) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_spool", err);
}

void MoonrakerSpoolmanAPI::set_active_spool(int, SuccessCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::set_active_spool", err);
}

void MoonrakerSpoolmanAPI::get_spool_usage_history(
    int, std::function<void(const std::vector<FilamentUsageRecord>&)>, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spool_usage_history", err);
}

void MoonrakerSpoolmanAPI::update_spoolman_spool_weight(int, double, SuccessCallback,
                                                        ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::update_spoolman_spool_weight", err);
}

void MoonrakerSpoolmanAPI::update_spoolman_spool(int, const nlohmann::json&, SuccessCallback,
                                                 ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::update_spoolman_spool", err);
}

void MoonrakerSpoolmanAPI::update_spoolman_filament(int, const nlohmann::json&, SuccessCallback,
                                                    ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::update_spoolman_filament", err);
}

void MoonrakerSpoolmanAPI::update_spoolman_filament_color(int, const std::string&, SuccessCallback,
                                                          ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::update_spoolman_filament_color", err);
}

void MoonrakerSpoolmanAPI::get_spoolman_vendors(helix::VendorListCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_vendors", err);
}

void MoonrakerSpoolmanAPI::get_spoolman_filaments(helix::FilamentListCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_filaments", err);
}

void MoonrakerSpoolmanAPI::get_spoolman_filaments(int, helix::FilamentListCallback,
                                                  ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_filaments(vendor)", err);
}

void MoonrakerSpoolmanAPI::create_spoolman_vendor(const nlohmann::json&,
                                                  helix::VendorCreateCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::create_spoolman_vendor", err);
}

void MoonrakerSpoolmanAPI::create_spoolman_filament(const nlohmann::json&,
                                                    helix::FilamentCreateCallback,
                                                    ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::create_spoolman_filament", err);
}

void MoonrakerSpoolmanAPI::create_spoolman_spool(const nlohmann::json&, helix::SpoolCreateCallback,
                                                 ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::create_spoolman_spool", err);
}

void MoonrakerSpoolmanAPI::delete_spoolman_spool(int, SuccessCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::delete_spoolman_spool", err);
}

void MoonrakerSpoolmanAPI::delete_spoolman_vendor(int, SuccessCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::delete_spoolman_vendor", err);
}

void MoonrakerSpoolmanAPI::delete_spoolman_filament(int, SuccessCallback, ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::delete_spoolman_filament", err);
}

void MoonrakerSpoolmanAPI::get_spoolman_external_vendors(helix::VendorListCallback,
                                                         ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_external_vendors", err);
}

void MoonrakerSpoolmanAPI::get_spoolman_external_filaments(const std::string&,
                                                           helix::FilamentListCallback,
                                                           ErrorCallback err) {
    task10_unimplemented_err("MoonrakerSpoolmanAPI::get_spoolman_external_filaments", err);
}

// (further symbols appended here as later tasks' link passes demand them)
