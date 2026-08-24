// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Moonraker seam, stubbed per the plan: "hardcoded mock state pushed
// through the real subject pipeline — mirrors what --test mode does, minus
// libhv". Symbols are added here exactly as the linker demands them; each
// group gets a rationale comment. Phase 2 replaces this file with a real
// esp_websocket_client / esp_http_client port behind IMoonrakerClient.

// (grown by the link loop)

#include "moonraker_advanced_api.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_file_api.h"
#include "moonraker_job_api.h"
#include "moonraker_manager.h"
#include "moonraker_motion_api.h"
#include "moonraker_queue_api.h"

#include <string>
#include <vector>

// NOTE: none of these bodies ever invoke the success/error callbacks. The
// slice's global API pointer is nullptr at runtime; these definitions exist
// only to satisfy the linker.

// --- MoonrakerAPI command surface (Task 2 bucket: libhv WebSocket seam) -----
// Real implementations serialize JSON-RPC over the libhv WebSocket client;
// Phase 2 ports them onto esp_websocket_client.
void MoonrakerAPI::emergency_stop(SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::execute_gcode(const std::string&, SuccessCallback, ErrorCallback, uint32_t,
                                 bool) {}
void MoonrakerAPI::query_configfile(JsonCallback, ErrorCallback) {}
void MoonrakerAPI::restart_firmware(SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::restart_klipper(SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::restart_moonraker(SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::set_fan_speed(const std::string&, double, SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::set_temperature(const std::string&, double, SuccessCallback, ErrorCallback) {}

// --- MoonrakerAdvancedAPI bed-mesh readers (Task 2 bucket: libhv WebSocket
// seam) — read state cached from WebSocket subscriptions; no mesh on the
// slice, so report "no mesh".
const BedMeshProfile* MoonrakerAdvancedAPI::get_active_bed_mesh() const {
    return nullptr;
}
const BedMeshProfile* MoonrakerAdvancedAPI::get_bed_mesh_profile(const std::string&) const {
    return nullptr;
}
std::vector<std::string> MoonrakerAdvancedAPI::get_bed_mesh_profiles() const {
    return {};
}
bool MoonrakerAdvancedAPI::has_bed_mesh() const {
    return false;
}

// --- MoonrakerFileAPI (Task 2 bucket: libhv WebSocket/HTTP seam) — file list
// and metadata RPCs plus HTTP-backed scans.
void MoonrakerFileAPI::delete_file(const std::string&, SuccessCallback, ErrorCallback) {}
void MoonrakerFileAPI::get_file_roots(FileRootsCallback, ErrorCallback) {}
void MoonrakerFileAPI::list_files(const std::string&, const std::string&, bool, FileListCallback,
                                  ErrorCallback) {}
void MoonrakerFileAPI::metascan_file(const std::string&, FileMetadataCallback, ErrorCallback,
                                     bool) {}

// --- MoonrakerJobAPI (Task 2 bucket: libhv WebSocket seam) — print job RPCs.
void MoonrakerJobAPI::cancel_print(SuccessCallback, ErrorCallback) {}
void MoonrakerJobAPI::start_print(const std::string&, SuccessCallback, ErrorCallback) {}

// --- MoonrakerMotionAPI (Task 2 bucket: libhv WebSocket seam) — motion gcode
// RPCs.
void MoonrakerMotionAPI::home_axes(const std::string&, SuccessCallback, ErrorCallback) {}
void MoonrakerMotionAPI::move_axis(char, double, double, SuccessCallback, ErrorCallback) {}

// --- MoonrakerManager (Task 2 bucket: libhv WebSocket seam) — owns the
// WebSocket client lifecycle and macro-analysis wiring. connect() reports
// failure (0 = success in the real contract); macro analysis absent.
int MoonrakerManager::connect(const std::string&, const std::string&) {
    return -1;
}
helix::MacroModificationManager* MoonrakerManager::macro_analysis() const {
    return nullptr;
}

void MoonrakerAPI::machine_shutdown(SuccessCallback, ErrorCallback) {}
// Real validator lives in the seam file; nothing in the render slice sends
// user gcode, so the conservative answer is fine for the audit.
bool MoonrakerAPI::is_safe_gcode_param(const std::string&) {
    return false;
}

bool helix::MoonrakerClient::unregister_method_callback(const std::string&, const std::string&) {
    return false;
}

void MoonrakerQueueAPI::get_queue_status(StatusCallback, ErrorCallback) {}
void MoonrakerQueueAPI::start_queue(SuccessCallback, ErrorCallback) {}
void MoonrakerQueueAPI::pause_queue(SuccessCallback, ErrorCallback) {}
void MoonrakerQueueAPI::remove_jobs(const std::vector<std::string>&, SuccessCallback,
                                    ErrorCallback) {}

void MoonrakerAPI::exclude_object(const std::string&, SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::machine_reboot(SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::restart_service(const std::string&, SuccessCallback, ErrorCallback) {}
void MoonrakerAPI::set_led(const std::string&, double, double, double, double, SuccessCallback,
                           ErrorCallback) {}
// Expired weak_ptr = "client already gone", the safe answer for lock() callers.
std::weak_ptr<bool> MoonrakerAPI::client_lifetime_weak() const {
    return {};
}

void MoonrakerFileAPI::copy_file(const std::string&, const std::string&, SuccessCallback,
                                 ErrorCallback) {}

void MoonrakerMotionAPI::move_to_position(char, double, double, SuccessCallback, ErrorCallback) {}

namespace helix {
SubscriptionId MoonrakerClient::register_notify_update(std::function<void(const json&)>) {
    return INVALID_SUBSCRIPTION_ID;
}
bool MoonrakerClient::unsubscribe_notify_update(SubscriptionId) {
    return false;
}
void MoonrakerClient::register_method_callback(const std::string&, const std::string&,
                                               std::function<void(const json&)>) {}
} // namespace helix
