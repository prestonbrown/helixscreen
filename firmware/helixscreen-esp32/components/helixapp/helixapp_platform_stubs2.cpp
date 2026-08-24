// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link stubs, round 2 — platform-bound singletons/utilities demanded by the
// slice link. One row each in the audit categorization table.

#include "ui_panel_spoolman.h"
#include "ui_spoolman_overlay.h"

#include "app_globals.h"
#include "bluetooth_loader.h"
#include "bt_print_utils.h"
#include "camera_stream.h"
#include "display_manager.h"
#include "esp_attr.h"
#include "ethernet_manager.h"
#include "filament_display_name.h"
#include "gcode_data_source.h"
#include "host_identity.h"
#include "hv/WebSocketClient.h"
#include "ipp_printer.h"
#include "logging_init.h"
#include "makeid_bt_printer.h"
#include "mdns_discovery.h"
#include "platform_info.h"
#include "plugin_manager.h"
#include "snapshot_qr_scanner.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "system/crash_handler.h"
#include "system/debug_bundle_collector.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "thumbnail_processor.h"
#include "usb_manager.h"
#include "wifi_manager.h"

#include <cstdio>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

// --- DisplayManager (SDL/DRM/fbdev backends; ESP32 uses esp_lcd) -----------
// The header contract allows instance() to return nullptr when no Application
// has constructed one — exactly the slice's situation, so no static instance
// is needed.
DisplayManager* DisplayManager::instance() {
    return nullptr;
}
void DisplayManager::wake_display() {}
void DisplayManager::register_resize_callback(ResizeCallback) {}
bool DisplayManager::needs_touch_calibration() const {
    return false;
}
// GT911 capacitive touch reports absolute panel coordinates — there is no
// resistive-style calibration to offer, so the settings row stays hidden.
bool DisplayManager::supports_touch_calibration() const {
    return false;
}

// --- crash_handler breadcrumbs (Linux signal-context crash pipeline) --------
namespace crash_handler {
namespace breadcrumb {
void note(const char*, const char*) noexcept {}
void note(const char*, const char*, long) noexcept {}
} // namespace breadcrumb
} // namespace crash_handler

// --- TelemetryManager hooks (libhv HTTP POST pipeline) ----------------------
// instance()/dtor/notify_panel_changed/notify_klippy_state_changed/
// init_print_outcome_observer live in audit_stubs.cpp — only the newly
// demanded hooks here.
void TelemetryManager::record_error(const std::string&, const std::string&, const std::string&) {}
void TelemetryManager::notify_overlay_opened(const std::string&) {}
void TelemetryManager::notify_connection_state_changed(int) {}

// --- UsbManager readers (udev/sysfs USB storage; no USB host on the slice) --
// ctor/dtor/start/stop/set_drive_callback live in audit_stubs.cpp.
std::vector<UsbDrive> UsbManager::get_drives() const {
    return {};
}
bool UsbManager::is_running() const {
    return false;
}

// --- host identity / platform info (gethostname/getifaddrs; Android JNI) ----
namespace helix {
bool is_moonraker_on_same_host(std::string_view) {
    return false;
}
bool is_android_platform() {
    return false;
}
bool platform_host_power_supported() {
    // Mirrors src/system/platform_info.cpp (excluded from this cut): host power
    // is available everywhere except Android. show_shutdown_dialog()'s backstop
    // (e49f8968e) is the first kept TU to reach this — ESP32 drives the printer
    // host's machine.shutdown/reboot over WiFi, so the answer is true here.
    return !is_android_platform();
}
} // namespace helix

// --- app_globals slice seam (Linux app-lifecycle hub) ------------------------
// get_printer_state is already stubbed in audit_stubs.cpp.
bool is_wizard_active() {
    return false;
}
// Real process-global storage for the Moonraker API/client pointers: app_boot
// wires them via set_moonraker_api/client, and consumers read them back here
// (the audit slice returned nullptr because nothing set them). set_moonraker_
// manager lives in helixapp_platform_stubs.cpp next to get_moonraker_manager.
static IMoonrakerAPI* g_moonraker_api = nullptr;
IMoonrakerAPI* get_moonraker_api() {
    return g_moonraker_api;
}
void set_moonraker_api(IMoonrakerAPI* api) {
    g_moonraker_api = api;
}
std::string app_get_runtime_dir() {
    return std::string("/config");
}

// --- SpoolmanOverlay (Moonraker database + HTTP probe UI) --------------------
// The real ctor/dtor and virtual overrides live in src/ui/ui_spoolman_overlay
// .cpp (outside the slice). Constructing the function-local static emits the
// vtable, so every non-inline virtual must be defined here too. Members are
// plain pointers/lv_subject_t — no cascading dtors.
namespace helix {
namespace ui {
SpoolmanOverlay::SpoolmanOverlay() = default;
SpoolmanOverlay::~SpoolmanOverlay() = default;
void SpoolmanOverlay::init_subjects() {}
void SpoolmanOverlay::register_callbacks() {}
lv_obj_t* SpoolmanOverlay::create(lv_obj_t*) {
    return nullptr;
}
void SpoolmanOverlay::on_ui_destroyed() {}
void SpoolmanOverlay::on_activate() {}
void SpoolmanOverlay::on_deactivate() {}
void SpoolmanOverlay::show(lv_obj_t*) {}
SpoolmanOverlay& get_spoolman_overlay() {
    static SpoolmanOverlay overlay;
    return overlay;
}
} // namespace ui
} // namespace helix

// --- Spoolman proper (8 TUs, dropped 2026-08-12) -----------------------------
// SCOPE DECISION, not a cleanup. Spoolman is unreachable on this build only
// because the ESP client skips the second server.info discovery call
// (helixnet/esp_moonraker_client.cpp), so `printer_has_spoolman` is never set
// and every XML entry row stays hidden. The subject itself is still registered
// for real by the KEPT printer_capabilities_state.cpp, so no XML binding is
// orphaned by this removal. If discovery is ever restored, these stubs must be
// replaced by re-adding the eight src/ TUs — see app_srcs_excluded.txt.
//
// SpoolmanManager has no virtual call reaching it from kept code, so raw
// storage is safe there (and fails CLOSED: a new call site becomes a link
// error rather than a null-vtable fault). SpoolmanSlotSaver is constructed by
// value in ui_ams_edit_overlay.cpp, so it needs a real ctor.
//
// SpoolmanPanel CANNOT be raw storage: two kept call sites
// (ui_panel_advanced.cpp handle_spoolman_clicked, ui_printer_manager_overlay
// .cpp on_chip_spoolman_clicked) hand it to lazy_create_and_push_overlay,
// which dispatches init_subjects(), register_callbacks() and create() on it.
// Those are virtual, so raw storage would fault (LoadProhibited) the moment
// the capability gate that hides both rows is lifted. So the accessor
// constructs a real object, which requires defining the ctor and every
// OverlayBase override here so the vtable emits with all slots filled — plus
// the ctors/dtors/virtuals of the three excluded member classes it aggregates
// (list view, context menu, edit modal), which live in dropped TUs.
//
// create() returns nullptr, which lazy_create_and_push_overlay already handles:
// it logs and raises the "Failed to open Spoolman" toast, so a tap that gets
// past the gate produces a visible error instead of a panic. The real
// init_subjects() registers only spoolman_panel_state / spoolman_header_title,
// both bound solely by spoolman_panel.xml — an overlay that is never created
// here — so the no-op body leaves no binding unsatisfied.

// src/ui/ui_panel_spoolman.cpp (DEFINE_GLOBAL_PANEL)
SpoolmanPanel::SpoolmanPanel() = default;
SpoolmanPanel::~SpoolmanPanel() = default;
void SpoolmanPanel::init_subjects() {}
void SpoolmanPanel::register_callbacks() {}
lv_obj_t* SpoolmanPanel::create(lv_obj_t*) {
    return nullptr;
}
void SpoolmanPanel::on_activate() {}
void SpoolmanPanel::on_deactivate() {}

SpoolmanPanel& get_global_spoolman_panel() {
    static SpoolmanPanel panel;
    return panel;
}

// Member subobjects of SpoolmanPanel whose own TUs are dropped. Constructing
// the panel above requires their ctors/dtors, and defining their out-of-line
// virtuals here is what makes their vtables emit complete too.

// src/ui/ui_spoolman_list_view.cpp
namespace helix::ui {
SpoolmanListView::~SpoolmanListView() = default;

// src/ui/ui_spoolman_context_menu.cpp
SpoolmanContextMenu::SpoolmanContextMenu() = default;
SpoolmanContextMenu::~SpoolmanContextMenu() = default;
void SpoolmanContextMenu::on_created(lv_obj_t*) {}
void SpoolmanContextMenu::on_backdrop_clicked() {}

// src/ui/ui_spoolman_edit_modal.cpp
SpoolEditModal::SpoolEditModal() = default;
SpoolEditModal::~SpoolEditModal() = default;
void SpoolEditModal::on_show() {}
void SpoolEditModal::on_hide() {}
} // namespace helix::ui

// src/printer/spoolman_types.cpp — no spools exist, so nothing survives a filter.
std::vector<SpoolInfo> filter_spools(const std::vector<SpoolInfo>&, const std::string&) {
    return {};
}

// src/printer/spoolman_manager.cpp — the real init_subjects() registers only
// observers (no XML subjects), so a no-op leaves no binding unsatisfied.
SpoolmanManager& SpoolmanManager::instance() {
    alignas(SpoolmanManager) EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(SpoolmanManager)];
    return *reinterpret_cast<SpoolmanManager*>(storage);
}
void SpoolmanManager::init_subjects() {}
void SpoolmanManager::set_api(IMoonrakerAPI*) {}
void SpoolmanManager::start_spoolman_polling() {}
void SpoolmanManager::stop_spoolman_polling() {}
// Called by the KEPT ams_state.cpp commit_slot_edit() when an edit drops a
// spool's identity. No cache exists here, so there is nothing to drop.
void SpoolmanManager::invalidate_identity(int) {}
std::optional<helix::SpoolIdentity> SpoolmanManager::find_identity(int) {
    return std::nullopt;
}

// src/spoolman/spoolman_slot_saver.cpp — the AMS edit overlay compiles calls to
// these, but reaches them only from the Spoolman-gated save path. "No change"
// and "not complete" are the answers that make that path a no-op if entered.
namespace helix {
SpoolmanSlotSaver::SpoolmanSlotSaver(IMoonrakerAPI* api) : api_(api) {}

ChangeSet SpoolmanSlotSaver::detect_changes(const SlotInfo&, const SlotInfo&) {
    return ChangeSet{};
}
bool SpoolmanSlotSaver::is_filament_complete(const SlotInfo&) {
    return false;
}
void SpoolmanSlotSaver::build_spool_patches(const SpoolInfo&, const SpoolInfo&, nlohmann::json&,
                                            nlohmann::json&) {}

// Reports failure rather than silently succeeding — a caller that got here
// must not believe it persisted anything to Spoolman.
void SpoolmanSlotSaver::save(const SlotInfo&, const SlotInfo&, LinkIntent,
                             CompletionCallback on_complete) {
    if (on_complete) {
        on_complete(SaveResult{});
    }
}
} // namespace helix

// --- process-spawn stubs newlib lacks ----------------------------------------
// No process model on ESP-IDF; callers see failure.
extern "C" {
FILE* popen(const char*, const char*) {
    return nullptr;
}
int pclose(FILE*) {
    return -1;
}
pid_t waitpid(pid_t, int*, int) {
    return -1;
}
int execvp(const char*, char* const[]) {
    return -1;
}
}

// ============================================================================
// Round 2 — second batch of link-loop demands.
// ============================================================================

// --- DisplayManager touch calibration (evdev/tslib backends) ----------------
bool DisplayManager::apply_touch_calibration(const helix::TouchCalibration&) {
    return false;
}

namespace helix {

// --- MdnsDiscovery (Avahi/Bonjour DNS-SD over BSD sockets) -------------------
// Constructing the object emits the vtable, so all non-inline virtuals are
// defined here. Impl is the pimpl owning the discovery thread; an empty
// definition makes unique_ptr<Impl> destructible in this TU.
class MdnsDiscovery::Impl {};
MdnsDiscovery::MdnsDiscovery(std::string) {}
MdnsDiscovery::~MdnsDiscovery() = default;
void MdnsDiscovery::start_discovery(DiscoveryCallback) {}
void MdnsDiscovery::stop_discovery() {}
bool MdnsDiscovery::is_discovering() const {
    return false;
}
std::vector<DiscoveredPrinter> MdnsDiscovery::get_discovered_printers() const {
    return {};
}

} // namespace helix

// --- ThumbnailProcessor (libhv HThreadPool worker file) ----------------------
// HThreadPool is libhv's thread pool, forward-declared in the header; an
// empty definition here makes unique_ptr<HThreadPool> destructible (the
// pointer stays null — nothing ever constructs a pool).
class HThreadPool {};

namespace helix {

ThumbnailProcessor& ThumbnailProcessor::instance() {
    static ThumbnailProcessor p;
    return p;
}
ThumbnailProcessor::ThumbnailProcessor() = default;
ThumbnailProcessor::~ThumbnailProcessor() = default;
void ThumbnailProcessor::process_async(const std::vector<uint8_t>&, const std::string&,
                                       const ThumbnailTarget&, ProcessSuccessCallback,
                                       ProcessErrorCallback) {}
void ThumbnailProcessor::set_cache_dir(const std::string&) {}
ThumbnailTarget ThumbnailProcessor::get_target_for_display(ThumbnailSize) {
    return {};
}

} // namespace helix

// --- BluetoothLoader (dlopen of libhelix-bluetooth.so; BlueZ D-Bus) ----------
// is_available() == false is the documented "no BT hardware" state; every
// caller checks it before touching the function pointers.
namespace helix {
namespace bluetooth {

BluetoothLoader& BluetoothLoader::instance() {
    static BluetoothLoader loader;
    return loader;
}
BluetoothLoader::BluetoothLoader() = default;
BluetoothLoader::~BluetoothLoader() = default;
bool BluetoothLoader::is_available() const {
    return false;
}
helix_bt_context* BluetoothLoader::get_or_create_context() {
    return nullptr;
}

} // namespace bluetooth
} // namespace helix

namespace helix {

// --- CameraStream (libhv HTTP MJPEG client + turbojpeg/stb decode) -----------
// Gated on HELIX_HAS_CAMERA: with camera off (v1 ESP cut) the class itself is
// compiled out of camera_stream.h, and no kept TU references it, so these
// stubs are neither compilable nor needed.
#if HELIX_HAS_CAMERA
void CameraStream::start(const std::string&, const std::string&, FrameCallback, ErrorCallback) {}
void CameraStream::stop() {}
bool CameraStream::configure_from_printer(std::string&, std::string&) {
    return false;
}
#endif // HELIX_HAS_CAMERA

// --- DebugBundleCollector (libhv HTTPS upload of diagnostics) ----------------
// Reports failure rather than returning silently: the debug-bundle modal shows
// a progress state with no buttons while it waits for this callback, so never
// invoking it strands the user there with no way out but a reboot.
void DebugBundleCollector::upload_async(const BundleOptions&, ResultCallback callback) {
    if (callback) {
        BundleResult result;
        result.success = false;
        result.error_message = "Debug bundle upload is not available on this device";
        callback(result);
    }
}

// --- host identity cache (getifaddrs/gethostname; see round 1) ---------------
void invalidate_host_identity_cache() {}

// --- platform info (uname; see round 1) --------------------------------------
std::string host_arch_string() {
    return std::string("xtensa");
}

// --- IppPrinter (IPP/PWG-Raster over libhv HTTP; detached print thread) ------
// The slice UI constructs one before calling the setters, so ctor/dtor and
// the full ILabelPrinter virtual set are stubbed to complete the vtable.
IppPrinter::IppPrinter() = default;
IppPrinter::~IppPrinter() = default;
std::string IppPrinter::name() const {
    return {};
}
void IppPrinter::print(const LabelBitmap&, const LabelSize&, PrintCallback) {}
std::vector<LabelSize> IppPrinter::supported_sizes() const {
    return {};
}
void IppPrinter::set_target(const std::string&, uint16_t, const std::string&) {}
void IppPrinter::set_sheet_template(int) {}
void IppPrinter::set_label_count(int) {}
void IppPrinter::set_start_position(int) {}
std::vector<LabelSize> IppPrinter::supported_sizes_static() {
    return {};
}

// --- SnapshotQrScanner (libhv HTTP snapshot poll + quirc decode thread) ------
#if HELIX_HAS_CAMERA
void SnapshotQrScanner::start(const std::string&, FrameCallback, QrResultCallback, ErrorCallback) {}
void SnapshotQrScanner::stop() {}
void SnapshotQrScanner::frame_consumed() {}
#endif // HELIX_HAS_CAMERA

} // namespace helix

// --- logging init (spdlog sinks: files/syslog/journald are Linux-bound) ------
// The spdlog named here is the audit shim (helixcore/shim/spdlog_shim.h).
namespace helix {
namespace logging {

spdlog::level::level_enum parse_level(const std::string&, spdlog::level::level_enum default_level) {
    return default_level;
}
void set_runtime_level(spdlog::level::level_enum) {}
std::string effective_destination() {
    return std::string("console");
}

} // namespace logging
} // namespace helix

// --- MakeIdBluetoothPrinter (BLE GATT via BluetoothLoader) --------------------
// Vtable demanded: ctor/dtor are implicit (inline), so the three ILabelPrinter
// overrides plus set_device complete the class.
namespace helix {
namespace label {

void MakeIdBluetoothPrinter::set_device(const std::string&, const std::string&) {}
std::string MakeIdBluetoothPrinter::name() const {
    return {};
}
void MakeIdBluetoothPrinter::print(const LabelBitmap&, const LabelSize&, PrintCallback) {}
std::vector<LabelSize> MakeIdBluetoothPrinter::supported_sizes() const {
    return {};
}

} // namespace label
} // namespace helix

// --- TelemetryManager, second batch (libhv HTTP POST pipeline; see round 1) --
void TelemetryManager::clear_queue() {}
void TelemetryManager::set_enabled(bool) {}
void TelemetryManager::notify_setting_changed(const std::string&, const std::string&,
                                              const std::string&) {}

// --- UpdateChecker (libhv HTTPS to GitHub/R2; worker thread) -----------------
// Private ctor is header-inline (= default); only the declared dtor needs a
// definition for the function-local static.
UpdateChecker& UpdateChecker::instance() {
    static UpdateChecker checker;
    return checker;
}
UpdateChecker::~UpdateChecker() = default;
void UpdateChecker::clear_cache() {}
void UpdateChecker::start_download() {}
void UpdateChecker::cancel_download() {}

// --- hv::WebSocketClient audit stand-in (shim hv_stub/hv/WebSocketClient.h) --
// The shim declares these; Phase 2 replaces the seam with
// esp_websocket_client. Ctor/dtor defined proactively per the link plan.
namespace hv {

WebSocketClient::WebSocketClient(EventLoopPtr) {}
WebSocketClient::~WebSocketClient() = default;
void WebSocketClient::setReconnect(reconn_setting_t*) {}

} // namespace hv

// --- process-spawn stubs, second batch ---------------------------------------
// No process model on ESP-IDF; callers see failure.
extern "C" {
int execl(const char*, const char*, ...) {
    return -1;
}
int execv(const char*, char* const[]) {
    return -1;
}
ssize_t readlink(const char*, char*, size_t) {
    return -1;
}
}

// ============================================================================
// Round 3 — third batch of link-loop demands.
// ============================================================================

// --- DisplayManager screensaver/backlight, third batch (sysfs backlight) -----
void DisplayManager::set_dim_timeout(int) {}
void DisplayManager::preview_screensaver(int) {}
void DisplayManager::set_backlight_brightness(int) {}
bool DisplayManager::has_backlight_control() const {
    return false;
}
helix::TouchCalibration DisplayManager::get_current_calibration() const {
    return {};
}

// --- app_globals path accessors, third batch ---------------------------------
// ESP32 storage root is the LittleFS mount; all path families collapse to it.
// Writable-storage roots on ESP32. The asset container (/assets) is a
// read-only frogfs image; the only writable filesystem is the /config LittleFS
// partition, so all config + cache writes land there (Task 6 carry-item: route
// get_helix_cache_dir to /config). Callers that write create the subtree
// themselves (create_directories), as on desktop.
std::string app_get_cache_dir() {
    return std::string("/config/cache");
}
std::string app_get_config_dir() {
    return std::string("/config");
}
std::string get_helix_cache_dir(const std::string& subdir) {
    return subdir.empty() ? std::string("/config/cache") : "/config/cache/" + subdir;
}

namespace helix {

// --- WiFiManager: Task 13 replaces this stub. src/api/wifi_manager.cpp (the
// real, desktop-shared implementation) and src/api/wifi_backend.cpp are now
// compiled in (app_srcs.txt); WifiBackend::create() dispatches to
// helix::create_platform_wifi_backend() on ESP_PLATFORM, implemented in
// wifi_backend_esp.cpp against esp_wifi. get_wifi_manager()'s singleton
// storage lives in wifi_manager.cpp itself, so no stub is needed here.

// --- ThumbnailProcessor, third batch (see round 2) ----------------------------
void ThumbnailProcessor::set_write_journal(std::weak_ptr<ThumbnailWriteJournal>) {}
void ThumbnailProcessor::set_card_size_hint(int, int) {}
// Callbacks intentionally never invoked — same contract as process_async above.
void ThumbnailProcessor::process_file_async(const std::string&, const std::string&,
                                            const ThumbnailTarget&,
                                            std::function<void(const std::string&)>,
                                            std::function<void(const std::string&)>) {}
ThumbnailTarget ThumbnailProcessor::get_target_for_resolution(int, int, ThumbnailSize) {
    return {};
}
std::string ThumbnailProcessor::get_if_processed(const std::string&, const ThumbnailTarget&) const {
    return {}; // empty string = "not cached", the documented miss value
}

// --- CameraStream ctor/dtor + is_running, third batch (see round 2) -----------
// Members are strings/atomics/std::thread/unique_ptr<uint8_t[]> — all complete
// types, no cascade. The stream thread is never started, so = default is safe.
#if HELIX_HAS_CAMERA
CameraStream::CameraStream() = default;
CameraStream::~CameraStream() = default;
bool CameraStream::is_running() const {
    return false;
}
#endif // HELIX_HAS_CAMERA

// --- SnapshotQrScanner ctor/dtor, third batch (see round 2) -------------------
// QrDecoder (by-value member) is now compiled for real in the slice, so the
// defaulted special members no longer cascade.
#if HELIX_HAS_CAMERA
SnapshotQrScanner::SnapshotQrScanner() = default;
SnapshotQrScanner::~SnapshotQrScanner() = default;
#endif // HELIX_HAS_CAMERA

} // namespace helix

// --- PluginManager readers (dlopen plugin loading; no dynamic linking on
// ESP-IDF). Empty containers = "no plugins discovered, no errors".
namespace helix {
namespace plugin {

std::vector<PluginInfo> PluginManager::get_discovered_plugins() const {
    return {};
}
std::vector<PluginError> PluginManager::get_load_errors() const {
    return {};
}

} // namespace plugin
} // namespace helix

// --- FileDataSource (fopen/fseek/fread streaming; lives in the libhv HTTP
// gcode_data_source.cpp). Constructing it emits the vtable, so all overrides
// are defined; the base GCodeDataSource dtor and virtual defaults are
// header-inline. Members are string/FILE*/uint64_t — no cascade.
namespace helix {
namespace gcode {

FileDataSource::FileDataSource(const std::string&) {}
FileDataSource::~FileDataSource() = default;
FileDataSource::FileDataSource(FileDataSource&&) noexcept {}
FileDataSource& FileDataSource::operator=(FileDataSource&&) noexcept {
    return *this;
}
std::vector<char> FileDataSource::read_range(uint64_t, uint32_t) {
    return {};
}
uint64_t FileDataSource::file_size() const {
    return 0;
}
bool FileDataSource::supports_range_requests() const {
    return false;
}
std::string FileDataSource::source_name() const {
    return {};
}
bool FileDataSource::is_valid() const {
    return false;
}
std::string FileDataSource::indexable_file_path() const {
    return {};
}

} // namespace gcode
} // namespace helix

// --- Bluetooth RFCOMM print path (BlueZ sockets via BluetoothLoader) ----------
namespace helix {
namespace bluetooth {

RfcommSendResult rfcomm_send(const std::string&, int, const std::vector<uint8_t>&,
                             const std::string&) {
    return {}; // success = false
}

} // namespace bluetooth

namespace label {

int resolve_label_printer_channel(const std::string&, int) {
    return -1; // documented "total failure" value
}

} // namespace label
} // namespace helix

// --- TelemetryManager, third batch (see rounds 1-2) ---------------------------
void TelemetryManager::notify_widget_interaction(const std::string&) {}
void TelemetryManager::notify_print_started_in_app() {}

// The lock-free crash/telemetry context globals normally defined in
// telemetry_manager.cpp — subject pipeline and panels write them
// unconditionally.
namespace helix {
namespace telemetry_context {
std::atomic<int> print_state_int{-1};
std::atomic<int> active_panel_int{-1};
std::atomic<bool> gcode_renderer_loaded{false};
} // namespace telemetry_context
} // namespace helix

// --- UpdateChecker, third batch (see round 2) ---------------------------------
// The status callback is never invoked.
void UpdateChecker::refresh_config_snapshot() {}
void UpdateChecker::check_for_updates(Callback) {}
// Release-channel switch: the real body re-checks and re-stamps the config from
// the network, which this slice has no updater for.
void UpdateChecker::on_channel_changed() {}
void UpdateChecker::report_download_status(DownloadStatus, int, const std::string&,
                                           const std::string&) {}
std::optional<UpdateChecker::ReleaseInfo> UpdateChecker::get_cached_update() const {
    return std::nullopt;
}

// --- process/fd stubs, third batch ---------------------------------------------
// No process model on ESP-IDF; callers see failure.
extern "C" {
int dup2(int, int) {
    return -1;
}
}

// ============================================================================
// Round 4 — fourth batch of link-loop demands.
// ============================================================================

// --- EthernetManager (netlink/sysfs interface probe; ESP32 has no ethernet
// on this hardware). The info callback is never invoked. backend_ stays null;
// ethernet_backend.h is included by the manager header, so the unique_ptr
// member destructs fine.
EthernetManager::EthernetManager() {}
EthernetManager::~EthernetManager() = default;
void EthernetManager::get_info_async(std::function<void(const EthernetInfo&)>) {}

// --- app_globals, fourth batch ------------------------------------------------
// Process-global storage for both accessors (get returns what set stored),
// same shape as the MoonrakerManager pair in helixapp_platform_stubs.cpp.
// app_boot.cpp Phase 10 constructs the real JobQueueState — well after LVGL
// init, so its subject registration is safe there — and publishes it here.
static helix::IMoonrakerClient* g_moonraker_client = nullptr;
helix::IMoonrakerClient* get_moonraker_client() {
    return g_moonraker_client;
}
void set_moonraker_client(helix::IMoonrakerClient* client) {
    g_moonraker_client = client;
}
static JobQueueState* g_job_queue_state = nullptr;
JobQueueState* get_job_queue_state() {
    return g_job_queue_state;
}
void set_job_queue_state(JobQueueState* state) {
    g_job_queue_state = state;
}
std::string app_get_install_root() {
    return std::string("/littlefs");
}

// --- DisplayManager, fourth batch (see rounds 1-3) -----------------------------
void DisplayManager::enable_affine_calibration() {}
bool DisplayManager::has_dimming_control() const {
    return false;
}
bool DisplayManager::is_software_rotated() const {
    return false;
}

// --- TelemetryManager, fourth batch (see rounds 1-3) ---------------------------
bool TelemetryManager::is_enabled() const {
    return false;
}
size_t TelemetryManager::queue_size() const {
    return 0;
}
nlohmann::json TelemetryManager::get_queue_snapshot() const {
    return {};
}

// --- UsbManager, fourth batch (see rounds 1-2) ---------------------------------
std::vector<UsbGcodeFile> UsbManager::scan_for_gcode(const std::string&, int) const {
    return {};
}
