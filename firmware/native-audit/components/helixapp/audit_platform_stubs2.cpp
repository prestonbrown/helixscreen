// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link stubs, round 2 — platform-bound singletons/utilities demanded by the
// slice link. One row each in the audit categorization table.

#include "app_globals.h"
#include "bluetooth_loader.h"
#include "bt_print_utils.h"
#include "camera_stream.h"
#include "display_manager.h"
#include "ethernet_manager.h"
#include "gcode_data_source.h"
#include "host_identity.h"
#include "ipp_printer.h"
#include "logging_init.h"
#include "makeid_bt_printer.h"
#include "mdns_discovery.h"
#include "platform_info.h"
#include "plugin_manager.h"
#include "snapshot_qr_scanner.h"
#include "system/crash_handler.h"
#include "system/debug_bundle_collector.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "thumbnail_processor.h"
#include "ui_spoolman_overlay.h"
#include "usb_manager.h"
#include "wifi_manager.h"

#include "hv/WebSocketClient.h"

#include <cstdio>
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
} // namespace helix

// --- app_globals slice seam (Linux app-lifecycle hub) ------------------------
// get_printer_state is already stubbed in audit_stubs.cpp.
bool is_wizard_active() {
    return false;
}
MoonrakerAPI* get_moonraker_api() {
    return nullptr;
}
std::string app_get_runtime_dir() {
    return std::string("/littlefs");
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
void SpoolmanOverlay::show(lv_obj_t*) {}
SpoolmanOverlay& get_spoolman_overlay() {
    static SpoolmanOverlay overlay;
    return overlay;
}
} // namespace ui
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
void CameraStream::start(const std::string&, const std::string&, FrameCallback, ErrorCallback) {}
void CameraStream::stop() {}
bool CameraStream::configure_from_printer(std::string&, std::string&) {
    return false;
}

// --- DebugBundleCollector (libhv HTTPS upload of diagnostics) ----------------
// The result callback is never invoked.
void DebugBundleCollector::upload_async(const BundleOptions&, ResultCallback) {}

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
void SnapshotQrScanner::start(const std::string&, FrameCallback, QrResultCallback, ErrorCallback) {}
void SnapshotQrScanner::stop() {}
void SnapshotQrScanner::frame_consumed() {}

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
std::string app_get_cache_dir() {
    return std::string("/littlefs");
}
std::string app_get_config_dir() {
    return std::string("/littlefs");
}
std::string get_helix_cache_dir(const std::string&) {
    return std::string("/littlefs");
}

namespace helix {

// --- WiFiManager (wpa_supplicant/wpa_cli backend; ESP32 uses esp_wifi) -------
// The slice UI reaches it through get_wifi_manager(), so the whole public
// surface plus ctor/dtor is stubbed. backend_ stays null; wifi_backend.h is
// included by wifi_manager.h, so unique_ptr<WifiBackend> destructs fine.
// "No hardware / disabled / not connected" is the honest no-op state.
WiFiManager::WiFiManager(bool) {}
WiFiManager::~WiFiManager() = default;
std::vector<WiFiNetwork> WiFiManager::scan_once() {
    return {};
}
void WiFiManager::start_scan(std::function<void(const std::vector<WiFiNetwork>&)>) {}
void WiFiManager::stop_scan() {}
void WiFiManager::connect(const std::string&, const std::string&,
                          std::function<void(bool success, const std::string& error)>) {}
void WiFiManager::disconnect() {}
bool WiFiManager::is_connected() {
    return false;
}
std::string WiFiManager::get_connected_ssid() {
    return {};
}
std::string WiFiManager::get_ip_address() {
    return {};
}
std::string WiFiManager::get_mac_address() {
    return {};
}
int WiFiManager::get_signal_strength() {
    return 0;
}
bool WiFiManager::supports_5ghz() {
    return false;
}
bool WiFiManager::has_hardware() {
    return false;
}
bool WiFiManager::is_enabled() {
    return false;
}
bool WiFiManager::set_enabled(bool) {
    return false;
}
void WiFiManager::retry_async() {}
void WiFiManager::add_state_observer(helix::LifetimeToken, std::function<void()>) {}
void WiFiManager::init_self_reference(std::shared_ptr<WiFiManager>) {}

std::shared_ptr<WiFiManager> get_wifi_manager() {
    static std::shared_ptr<WiFiManager> mgr = std::make_shared<WiFiManager>(true);
    return mgr;
}

// --- ThumbnailProcessor, third batch (see round 2) ----------------------------
ThumbnailTarget ThumbnailProcessor::get_target_for_resolution(int, int, ThumbnailSize) {
    return {};
}
std::string ThumbnailProcessor::get_if_processed(const std::string&, const ThumbnailTarget&) const {
    return {}; // empty string = "not cached", the documented miss value
}

// --- CameraStream ctor/dtor + is_running, third batch (see round 2) -----------
// Members are strings/atomics/std::thread/unique_ptr<uint8_t[]> — all complete
// types, no cascade. The stream thread is never started, so = default is safe.
CameraStream::CameraStream() = default;
CameraStream::~CameraStream() = default;
bool CameraStream::is_running() const {
    return false;
}

// --- SnapshotQrScanner ctor/dtor, third batch (see round 2) -------------------
// QrDecoder (by-value member) is now compiled for real in the slice, so the
// defaulted special members no longer cascade.
SnapshotQrScanner::SnapshotQrScanner() = default;
SnapshotQrScanner::~SnapshotQrScanner() = default;

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
void UpdateChecker::check_for_updates(Callback) {}
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
// Both accessors document "may be nullptr if not initialized" — the honest
// slice answer. Constructing the real JobQueueState (its .cpp IS in the
// slice) at static-init time would run subject registration before LVGL
// init, so the nullptr contract is used instead.
helix::MoonrakerClient* get_moonraker_client() {
    return nullptr;
}
JobQueueState* get_job_queue_state() {
    return nullptr;
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
