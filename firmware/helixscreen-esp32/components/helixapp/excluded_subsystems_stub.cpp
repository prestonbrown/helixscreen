// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link stubs for excluded UI subsystems in the v1 Core+AMS cut.
//
// The panels/overlays below are gated OFF for the ESP32 build (calibration
// panels, timelapse videos, belt-tension) so their .cpp TUs are NOT in
// app_srcs.txt. Kept code (subject_initializer.cpp, ui_panel_controls.cpp,
// moonraker_advanced_api.cpp, etc.) still references their symbols by address
// or name, so the stubs must resolve at link time.
//
// IMPORTANT: these accessors return a reference into UNINITIALIZED storage —
// the object is never constructed, so its vtable pointer is garbage/zero.
// Any VIRTUAL call on the returned reference (e.g. init_subjects()) dispatches
// through that null vtable and faults (LoadProhibited). subject_initializer.cpp
// used to call init_subjects() on bed_mesh/pid_cal/zoffset_cal/timelapse_videos
// at BOOT (not on navigation) and crashed here; those calls are now gated
// `#if !defined(HELIX_PLATFORM_ESP32)`. Only NON-virtual no-op member stubs
// (e.g. PIDCalibrationPanel::set_temp_control_panel below) are safe to call on
// this storage, because they never dereference `this`. If you add a new kept
// call site that dereferences one of these, gate it too — do NOT rely on the
// stub being "never touched".
//
// Stub strategies:
//   * global-instance accessors (get_global_*_panel): return a reference into
//     static aligned raw storage that is never dereferenced. The real
//     accessors (DEFINE_GLOBAL_PANEL) construct a live panel singleton; here we
//     only need the symbol to resolve so the address-taking call sites link.
//   * row-handler / event-callback initializers: no-op — the XML rows that
//     would fire them are never created.
//   * PIDCalibrationPanel member methods: no-op — set from subject_initializer
//     wiring that runs but has no visible effect on the idle path.
//
// NOTE: PrintStartCollector and M300SoundBackend are deliberately NOT stubbed
// here — kept code constructs them (make_shared) and calls virtuals on them
// (SoundManager asks the backend needs_moonraker_client()), so they need real
// ctors and vtables that a stub cannot supply. They are resolved by adding
// their portable, platform-free real .cpp files to app_srcs.txt instead. See
// the report for details.

#include "ui_overlay_timelapse_install.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_overlay_timelapse_videos.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_tool_offset.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_screws_tilt.h"
#include "ui_stepper_resonance_cal_modal.h"
#include "ui_toast_manager.h"
#include "ui_wizard_preferences.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "macro_manager.h"
#include "timelapse_state.h"
#include "touch_calibration_wrapper.h"

// ===========================================================================
// Global-scope panel / overlay instance accessors (raw-storage references).
// ===========================================================================

// src/ui/calibration/ui_panel_bed_mesh.cpp (DEFINE_GLOBAL_PANEL)
BedMeshPanel& get_global_bed_mesh_panel() {
    alignas(BedMeshPanel) EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(BedMeshPanel)];
    return *reinterpret_cast<BedMeshPanel*>(storage);
}

// src/ui/calibration/ui_panel_calibration_pid.cpp
PIDCalibrationPanel& get_global_pid_cal_panel() {
    alignas(PIDCalibrationPanel)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(PIDCalibrationPanel)];
    return *reinterpret_cast<PIDCalibrationPanel*>(storage);
}

// src/ui/print/ui_overlay_timelapse_videos.cpp
TimelapseVideosOverlay& get_global_timelapse_videos() {
    alignas(TimelapseVideosOverlay)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(TimelapseVideosOverlay)];
    return *reinterpret_cast<TimelapseVideosOverlay*>(storage);
}

// ===========================================================================
// Global-scope free functions (no-op wiring).
// ===========================================================================

// src/ui/print/ui_overlay_timelapse_videos.cpp
void init_global_timelapse_videos(helix::IMoonrakerAPI*) {}
void open_timelapse_videos() {}

// src/ui/print/ui_overlay_timelapse_videos.cpp, helix::ui entry points. Kept code calls
// both: ui_panel_history_list.cpp gates a row on the availability query and opens a
// job's video from it. Returning false is what makes the row inert here.
namespace helix::ui {
void open_timelapse_video(const std::string&) {}
bool timelapse_viewer_available() {
    return false;
}
} // namespace helix::ui

// ===========================================================================
// Timelapse install + settings overlays and TimelapseState (dropped
// 2026-08-12; see app_srcs_excluded.txt for the scope decision).
//
// SCOPE DECISION, not a cleanup. `printer_has_timelapse` and
// `printer_has_webcam` are only written by the second server.info discovery
// call that the ESP client skips, so the Advanced "Timelapse Videos" row, the
// "Setup Timelapse" row (additionally gated on printer_has_webcam) and the
// Printing-settings entry are all permanently hidden. Both subjects are still
// registered for real by the KEPT printer_capabilities_state.cpp.
//
// These two CANNOT be raw storage like the panels above: subject_initializer
// .cpp calls init_subjects() on both unconditionally at boot, and that is a
// pure virtual — raw storage would fault (LoadProhibited) on every boot. So
// each accessor constructs a real object, which requires defining the ctor and
// every OverlayBase override so the vtable emits here with all slots filled.
// Neither real init_subjects() registers an XML subject (settings does
// nothing; install registers one event_cb for XML that is never created), so
// the no-op bodies leave no binding unsatisfied.
//
// src/ui/print/ui_overlay_timelapse_install.cpp
TimelapseInstallOverlay::TimelapseInstallOverlay(helix::IMoonrakerAPI* api) : api_(api) {}
void TimelapseInstallOverlay::register_callbacks() {}
lv_obj_t* TimelapseInstallOverlay::create(lv_obj_t*) {
    return nullptr;
}
void TimelapseInstallOverlay::on_activate() {}
void TimelapseInstallOverlay::on_deactivating(DeactivateReason) {}
void TimelapseInstallOverlay::cleanup() {}

TimelapseInstallOverlay& get_global_timelapse_install() {
    static TimelapseInstallOverlay overlay(nullptr);
    return overlay;
}
void init_global_timelapse_install(helix::IMoonrakerAPI*) {}
void open_timelapse_install() {}

// src/ui/print/ui_overlay_timelapse_settings.cpp
TimelapseSettingsOverlay::TimelapseSettingsOverlay(helix::IMoonrakerAPI* api) : api_(api) {}
void TimelapseSettingsOverlay::register_callbacks() {}
lv_obj_t* TimelapseSettingsOverlay::create(lv_obj_t*) {
    return nullptr;
}
void TimelapseSettingsOverlay::on_activate() {}

TimelapseSettingsOverlay& get_global_timelapse_settings() {
    static TimelapseSettingsOverlay overlay(nullptr);
    return overlay;
}
void init_global_timelapse_settings(helix::IMoonrakerAPI*) {}
void open_timelapse_settings() {}

// src/printer/timelapse_state.cpp — the four subjects it registers are bound
// only by timelapse_videos_overlay.xml, whose overlay is already excluded and
// never created, so a no-op init_subjects() orphans nothing. Plain class (no
// virtuals) with a defaulted private ctor, so instance() can build a real one.
namespace helix {
TimelapseState& TimelapseState::instance() {
    static TimelapseState state;
    return state;
}
void TimelapseState::init_subjects(bool) {}
void TimelapseState::reset() {}
} // namespace helix

// ===========================================================================
// PIDCalibrationPanel member methods (src/ui/calibration/ui_panel_calibration_pid.cpp).
// ===========================================================================

void PIDCalibrationPanel::set_temp_control_panel(TemperatureService*) {}

// ===========================================================================
// More global-scope accessors + wiring initializers. NOTE: these panel classes
// and their accessors are declared at GLOBAL scope in their headers — the
// `namespace helix { ... }` block at the top of each header is only a short
// forward-declaration block that closes before the class/accessor. The callers
// (ui_printer_manager_overlay.cpp etc.) reference the global symbols, so these
// MUST be global (not in namespace helix), or the mangled names won't match.
// ===========================================================================

// src/ui/calibration/ui_panel_input_shaper.cpp (DEFINE_GLOBAL_PANEL)
InputShaperPanel& get_global_input_shaper_panel() {
    alignas(InputShaperPanel)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(InputShaperPanel)];
    return *reinterpret_cast<InputShaperPanel*>(storage);
}

// src/ui/calibration/ui_panel_screws_tilt.cpp (DEFINE_GLOBAL_PANEL)
ScrewsTiltPanel& get_global_screws_tilt_panel() {
    alignas(ScrewsTiltPanel) EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(ScrewsTiltPanel)];
    return *reinterpret_cast<ScrewsTiltPanel*>(storage);
}

// src/ui/calibration/ui_panel_calibration_zoffset.cpp (DEFINE_GLOBAL_PANEL)
ZOffsetCalibrationPanel& get_global_zoffset_cal_panel() {
    alignas(ZOffsetCalibrationPanel)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(ZOffsetCalibrationPanel)];
    return *reinterpret_cast<ZOffsetCalibrationPanel*>(storage);
}

// Advanced-panel staged-calibration rows (Input Shaping / Belt Tension / Z-Offset / Screws Tilt)
// stay VISIBLE — they are features the user should learn exist — but their panels
// are excluded from the v1 cut. Instead of the old silent no-op registration,
// register a real handler that shows the shared "not yet available" toast so the
// tap has visible feedback and can never reach the null-vtable panel stub.
static void esp32_staged_feature_row_cb(lv_event_t* /*e*/) {
    helix::ui::show_feature_unavailable_toast();
}

// src/ui/calibration/ui_panel_belt_tension.cpp
void init_belt_tension_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_belt_tension_row_clicked", esp32_staged_feature_row_cb);
}

// src/ui/calibration/ui_panel_input_shaper.cpp
void init_input_shaper_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_input_shaper_row_clicked", esp32_staged_feature_row_cb);
}

// src/ui/calibration/ui_panel_screws_tilt.cpp — the row shows only on printers with
// screws_tilt_adjust (printer_has_screws_tilt), where a tap needs feedback.
void init_screws_tilt_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_screws_tilt_row_clicked", esp32_staged_feature_row_cb);
}

// src/ui/calibration/ui_panel_calibration_zoffset.cpp
void init_zoffset_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_zoffset_row_clicked", esp32_staged_feature_row_cb);
}
void init_zoffset_event_callbacks() {}

// src/ui/calibration/ui_panel_calibration_tool_offset.cpp — same staged-row treatment: the
// row's visibility already tracks a real capability (printer_has_tool_offset_cal
// is written for real by the KEPT tool_offset_calibration::supported()), so on
// hardware where it would show, the tap must still give feedback rather than
// silently doing nothing.
namespace helix::ui {
void init_tool_offset_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_tool_offset_row_clicked", esp32_staged_feature_row_cb);
}

// src/ui/calibration/ui_stepper_resonance_cal_modal.cpp — same staged-row treatment: the
// row tracks printer_has_stepper_resonance_cal, written by the KEPT
// stepper_resonance_cal::resolve().
void init_stepper_resonance_cal_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_stepper_resonance_cal_clicked",
                             esp32_staged_feature_row_cb);
}

// src/ui/calibration/ui_panel_calibration_tool_offset.cpp (DEFINE_GLOBAL_PANEL) — the
// Controls-panel quick-access button (ui_panel_controls.cpp, KEPT) reaches
// this the same way handle_calibration_zoffset() reaches
// get_global_zoffset_cal_panel() above: only non-virtual access is safe on
// this storage, so that call site must never actually construct/activate it.
ToolOffsetCalibrationPanel& get_global_tool_offset_cal_panel() {
    alignas(ToolOffsetCalibrationPanel)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(ToolOffsetCalibrationPanel)];
    return *reinterpret_cast<ToolOffsetCalibrationPanel*>(storage);
}
} // namespace helix::ui

// src/api/touch_calibration_wrapper.cpp is not in the cut, but the touch
// calibration UI that drives it is (ui_touch_calibration_overlay.cpp,
// touch_calibration_session.cpp). Both entry points below are the wrapper's
// evdev-facing half: the raw reading only exists behind lv_evdev, and the ABS
// range is what lv_evdev is programmed with. This board's touch driver is not
// evdev, so there is no raw stage to read back and no range to store — the
// degradation the header already specifies for a non-evdev backend.
namespace helix {

// No evdev stage behind the pointer: report "no raw reading available", which
// sends the calibration through its affine-only path.
bool get_last_raw_touch(Point&) {
    return false;
}

// Nothing consumes a stored range on this platform (load_touch_range() is only
// called from the DRM/fbdev backends, both excluded), so persisting one would
// write config nothing reads back.
void save_touch_range(const TouchRangeSettings&) {}

// No evdev range stage to hold still during a capture.
void set_touch_capture_active(bool) {}

// ===========================================================================
// MacroManager — helper-macro install/update subsystem
// (src/printer/macro_manager.cpp, not in the v1 Core+AMS cut).
// ===========================================================================
// evaluate_status runs at boot from PrinterState::set_hardware on every
// build; UNKNOWN keeps the plugin-status subject at HelixMacrosStatus::Unknown
// here. The install/update entry points in ui_panel_advanced.cpp are gated to
// a feature-unavailable toast on this platform, so the file-operation and
// restart methods never run and their callbacks never fire.
MacroManager::MacroManager(IMoonrakerAPI& api, const PrinterDiscovery& hardware)
    : api_(api), hardware_(hardware) {}

MacroManager::~MacroManager() {}

MacroInstallStatus MacroManager::evaluate_status(const PrinterDiscovery&) {
    return MacroInstallStatus::UNKNOWN;
}

void MacroManager::install_files(SuccessCallback, ErrorCallback) {}

void MacroManager::update_files(SuccessCallback, ErrorCallback) {}

void MacroManager::request_restart(SuccessCallback, ErrorCallback) {}

bool MacroManager::stages_skips() const {
    return false;
}

void MacroManager::remove_skips(SuccessCallback, ErrorCallback) {}

} // namespace helix

// The firmware runs no first-run wizard, so the registry's Preferences step
// never runs; the accessor only has to resolve.
namespace helix::wizard {
WizardPreferencesStep* get_wizard_preferences_step() {
    return nullptr;
}
} // namespace helix::wizard
