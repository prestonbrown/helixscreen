// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link-time stubs the slice link loop forced. AUDIT RULE (same as Task 2):
// every stub here = one row in the categorization table in
// docs/devel/ESP32_NATIVE_AUDIT.md, with the reason it is legitimately
// platform-bound (or a note that Phase 2 must port it for real).

#include "panel_widget_manager.h"
#include "printer_state.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "temperature_controller.h"
#include "usb_manager.h"

// --- app_globals.cpp seam -------------------------------------------------
// The real src/app_globals.cpp is the Linux app-lifecycle hub (includes
// <SDL.h>, MoonrakerAPI wiring, signal handling). The slice needs only the
// PrinterState singleton accessor; definition copied from its pattern.
helix::PrinterState& get_printer_state() {
    static helix::PrinterState instance;
    return instance;
}

// Update UX gating (app_globals.cpp on Linux). ESP32 updates ship as firmware
// OTA, never through the in-app updater, so every answer is constant-true: the
// settings UI hides its update affordances.
//
// On Linux the install and check gates are deliberately different predicates
// (an unwritable install tree still gets to LOOK for an update). Here they
// collapse, because the reason is updates_externally_managed() rather than
// writability, and that suppresses both.
bool updates_externally_managed() {
    return true;
}
bool update_install_suppressed() {
    return true;
}
bool update_checks_suppressed() {
    return true;
}

// --- crash_handler seam (Task 2 bucket D: ucontext.h) ----------------------
// Signal-context register dumps are the Linux crash pipeline; ESP32 gets
// esp coredump instead. UpdateQueue/observer code registers diagnostic tag
// pointers unconditionally — accept and drop them.
namespace crash_handler {
void register_callback_tag_ptr(volatile const char* const*) {}
void register_previous_tag_ring(volatile const char* const*, volatile const uint32_t*, unsigned int,
                                volatile const unsigned int*) {}
} // namespace crash_handler

// --- telemetry seam (Task 2 bucket D: hv/requests.h) -----------------------
// Telemetry posts over libhv HTTP; a port would use esp_http_client. The
// subject pipeline calls notify_* hooks unconditionally — no-op them.
TelemetryManager& TelemetryManager::instance() {
    static TelemetryManager t;
    return t;
}
TelemetryManager::~TelemetryManager() = default;
void TelemetryManager::notify_panel_changed(const std::string&) {}
void TelemetryManager::notify_klippy_state_changed(int) {}
ObserverGuard TelemetryManager::init_print_outcome_observer() {
    return {};
}

// --- USB manager seam (Linux udev/mount stack) -----------------------------
// SubjectInitializer::init_post() owns a UsbManager; USB drive detection is
// out of the audit's scope (and this K-Touch has no USB-host wiring).
UsbManager::UsbManager(bool force_mock) : force_mock_(force_mock) {}
UsbManager::~UsbManager() = default;
bool UsbManager::start() {
    return false;
}
void UsbManager::stop() {}
void UsbManager::set_drive_callback(DriveCallback) {}

// --- app_globals subjects (pattern copy from src/app_globals.cpp) ----------
// The real file is Linux-bound (SDL); the subject block is portable and the
// slice needs it REAL (notification pipeline + home edit mode + beta flag
// bind into XML). Replicated verbatim minus logging.
#include "ui_modal.h"

#include "config.h"
#include "src/xml/lv_xml.h"
#include "static_subject_registry.h"

static lv_subject_t g_notification_subject;
static lv_subject_t g_show_beta_features_subject;
static lv_subject_t g_home_edit_mode_subject;
static bool g_subjects_initialized = false;

lv_subject_t& get_notification_subject() {
    return g_notification_subject;
}
lv_subject_t& get_home_edit_mode_subject() {
    return g_home_edit_mode_subject;
}
// Wizard-active gate (app_globals.cpp on Linux). ESP32 has no first-run wizard
// flow driving it, so it initializes to 0 and only PLR/offer code observes it.
static lv_subject_t g_wizard_active_subject;
lv_subject_t& get_wizard_active_subject() {
    return g_wizard_active_subject;
}

void app_globals_deinit_subjects();

void app_globals_init_subjects() {
    if (g_subjects_initialized) {
        return;
    }
    lv_subject_init_pointer(&g_notification_subject, nullptr);
    helix::Config* config = helix::Config::get_instance();
    bool beta_enabled = config && config->is_beta_features_enabled();
    lv_subject_init_int(&g_show_beta_features_subject, beta_enabled ? 1 : 0);
    lv_xml_register_subject(nullptr, "show_beta_features", &g_show_beta_features_subject);
    lv_subject_init_int(&g_home_edit_mode_subject, 0);
    lv_xml_register_subject(nullptr, "home_edit_mode", &g_home_edit_mode_subject);
    lv_subject_init_int(&g_wizard_active_subject, 0); // not XML-bound, observed programmatically
    helix::ui::modal_init_subjects();
    g_subjects_initialized = true;
    StaticSubjectRegistry::instance().register_deinit("AppGlobals", app_globals_deinit_subjects);
}

void app_globals_deinit_subjects() {
    g_subjects_initialized = false;
}

// Moonraker manager global accessor. app_boot.cpp now wires a real
// MoonrakerManager, so this needs real process-global storage (get returns what
// set stored) rather than the audit's nullptr slice behavior. The api/client
// storage + setters live in helixapp_platform_stubs2.cpp next to their existing
// getters. Forward decl only — opaque pointer storage needs no header.
class MoonrakerManager;
static MoonrakerManager* g_moonraker_manager = nullptr;
MoonrakerManager* get_moonraker_manager() {
    return g_moonraker_manager;
}
void set_moonraker_manager(MoonrakerManager* manager) {
    g_moonraker_manager = manager;
}

// Owned by the app lifecycle on Linux (app_globals.cpp); absent in the slice.
// All are pointer-returning with documented may-be-null contracts.
class PrintHistoryManager;
PrintHistoryManager* get_print_history_manager() {
    return nullptr;
}
// Real accessor (mirrors src/app_globals.cpp): SubjectInitializer constructs the
// controller in init_panel_subjects() and registers it as a PanelWidgetManager
// shared resource, so the lookup works on ESP32 exactly as it does on Linux.
helix::TemperatureController* get_temperature_controller() {
    return helix::PanelWidgetManager::instance().shared_resource<helix::TemperatureController>();
}
void app_request_restart_service() {} // systemd restart request — no-op on ESP
class TemperatureHistoryManager;
TemperatureHistoryManager* get_temperature_history_manager() {
    return nullptr;
}

// Font-face aliases (all faces AssetManager::register_fonts() references
// outside helixcore's 11 medium-tier faces, plus the mdi/noto_sans/
// source_code_pro tiers this component no longer compiles for real) live in
// font_aliases.cpp, not here — single source of truth, see that file's header.
