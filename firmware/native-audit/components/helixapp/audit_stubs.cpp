// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link-time stubs the slice link loop forced. AUDIT RULE (same as Task 2):
// every stub here = one row in the categorization table in
// docs/devel/ESP32_NATIVE_AUDIT.md, with the reason it is legitimately
// platform-bound (or a note that Phase 2 must port it for real).

#include "printer_state.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "usb_manager.h"

// --- app_globals.cpp seam -------------------------------------------------
// The real src/app_globals.cpp is the Linux app-lifecycle hub (includes
// <SDL.h>, MoonrakerAPI wiring, signal handling). The slice needs only the
// PrinterState singleton accessor; definition copied from its pattern.
helix::PrinterState& get_printer_state() {
    static helix::PrinterState instance;
    return instance;
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
#include "config.h"
#include "static_subject_registry.h"
#include "ui_modal.h"

#include "src/xml/lv_xml.h"

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
    helix::ui::modal_init_subjects();
    g_subjects_initialized = true;
    StaticSubjectRegistry::instance().register_deinit("AppGlobals", app_globals_deinit_subjects);
}

void app_globals_deinit_subjects() {
    g_subjects_initialized = false;
}

class MoonrakerManager;
MoonrakerManager* get_moonraker_manager() {
    return nullptr; // audit: no manager; slice drives PrinterState directly
}

// Owned by the app lifecycle on Linux (app_globals.cpp); absent in the slice.
// All are pointer-returning with documented may-be-null contracts.
class PrintHistoryManager;
PrintHistoryManager* get_print_history_manager() {
    return nullptr;
}
namespace helix {
class TemperatureController;
}
helix::TemperatureController* get_temperature_controller() {
    return nullptr;
}
void app_request_restart_service() {} // systemd restart request — no-op on ESP
class TemperatureHistoryManager;
TemperatureHistoryManager* get_temperature_history_manager() {
    return nullptr;
}

// Font faces referenced by AssetManager::register_fonts but not in the audit
// font set: the whole source_code_pro mono family, the tier-5/6 mdi 80/96/128
// (mdi_icons_128.c alone is 7.2MB of source), and micro-only noto_sans_8.
// The large ones sit behind runtime breakpoint guards that are false on the
// 800x480 K-Touch; source_code_pro_10..16 CAN be selected and will render as
// the alias face — acceptable, the audit verifies structure not glyph
// fidelity. A real K-Touch build trims via HELIX_MAX_FONT_TIER (Phase 2).
extern "C" {
extern const lv_font_t noto_sans_16; // any always-linked face works
extern const lv_font_t mdi_icons_128;
extern const lv_font_t mdi_icons_80;
extern const lv_font_t mdi_icons_96;
extern const lv_font_t noto_sans_8;
extern const lv_font_t source_code_pro_10;
extern const lv_font_t source_code_pro_12;
// non-const: canonical declaration in ui_fonts.h dropped const so embedded
// targets populate this moved face at runtime from a .bin (Plan A fonts->frogfs)
extern lv_font_t source_code_pro_14;
extern const lv_font_t source_code_pro_16;
extern const lv_font_t source_code_pro_18;
extern const lv_font_t source_code_pro_20;
extern const lv_font_t source_code_pro_24;
extern const lv_font_t source_code_pro_8;
}
extern "C" const lv_font_t mdi_icons_128 = noto_sans_16;
extern "C" const lv_font_t mdi_icons_80 = noto_sans_16;
extern "C" const lv_font_t mdi_icons_96 = noto_sans_16;
extern "C" const lv_font_t noto_sans_8 = noto_sans_16;
extern "C" const lv_font_t source_code_pro_10 = noto_sans_16;
extern "C" const lv_font_t source_code_pro_12 = noto_sans_16;
extern "C" lv_font_t source_code_pro_14 = noto_sans_16;
extern "C" const lv_font_t source_code_pro_16 = noto_sans_16;
extern "C" const lv_font_t source_code_pro_18 = noto_sans_16;
extern "C" const lv_font_t source_code_pro_20 = noto_sans_16;
extern "C" const lv_font_t source_code_pro_24 = noto_sans_16;
extern "C" const lv_font_t source_code_pro_8 = noto_sans_16;
