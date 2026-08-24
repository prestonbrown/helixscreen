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
//   * parse_shaper_csv: returns a default-constructed (empty) result — only
//     reachable after a real input-shaper calibration run, which cannot happen
//     on Stage A.
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
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_screws_tilt.h"
#include "ui_toast_manager.h"

#include "color_sensor_manager.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "shaper_csv_parser.h"
#include "timelapse_state.h"

// ===========================================================================
// Global-scope panel / overlay instance accessors (raw-storage references).
// ===========================================================================

// src/ui/ui_panel_bed_mesh.cpp (DEFINE_GLOBAL_PANEL)
BedMeshPanel& get_global_bed_mesh_panel() {
    alignas(BedMeshPanel) EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(BedMeshPanel)];
    return *reinterpret_cast<BedMeshPanel*>(storage);
}

// src/ui/ui_panel_calibration_pid.cpp
PIDCalibrationPanel& get_global_pid_cal_panel() {
    alignas(PIDCalibrationPanel)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(PIDCalibrationPanel)];
    return *reinterpret_cast<PIDCalibrationPanel*>(storage);
}

// src/ui/ui_overlay_timelapse_videos.cpp
TimelapseVideosOverlay& get_global_timelapse_videos() {
    alignas(TimelapseVideosOverlay)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(TimelapseVideosOverlay)];
    return *reinterpret_cast<TimelapseVideosOverlay*>(storage);
}

// ===========================================================================
// Global-scope free functions (no-op wiring).
// ===========================================================================

// src/ui/ui_overlay_timelapse_videos.cpp
void init_global_timelapse_videos(IMoonrakerAPI*) {}
void open_timelapse_videos() {}

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
// src/ui/ui_overlay_timelapse_install.cpp
TimelapseInstallOverlay::TimelapseInstallOverlay(IMoonrakerAPI* api) : api_(api) {}
void TimelapseInstallOverlay::init_subjects() {}
lv_obj_t* TimelapseInstallOverlay::create(lv_obj_t*) {
    return nullptr;
}
void TimelapseInstallOverlay::on_activate() {}
void TimelapseInstallOverlay::on_deactivate() {}
void TimelapseInstallOverlay::cleanup() {}

TimelapseInstallOverlay& get_global_timelapse_install() {
    static TimelapseInstallOverlay overlay(nullptr);
    return overlay;
}
void init_global_timelapse_install(IMoonrakerAPI*) {}
void open_timelapse_install() {}

// src/ui/ui_overlay_timelapse_settings.cpp
TimelapseSettingsOverlay::TimelapseSettingsOverlay(IMoonrakerAPI* api) : api_(api) {}
void TimelapseSettingsOverlay::init_subjects() {}
lv_obj_t* TimelapseSettingsOverlay::create(lv_obj_t*) {
    return nullptr;
}
void TimelapseSettingsOverlay::on_activate() {}
void TimelapseSettingsOverlay::on_deactivate() {}
void TimelapseSettingsOverlay::cleanup() {}

TimelapseSettingsOverlay& get_global_timelapse_settings() {
    static TimelapseSettingsOverlay overlay(nullptr);
    return overlay;
}
void init_global_timelapse_settings(IMoonrakerAPI*) {}
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
// PIDCalibrationPanel member methods (src/ui/ui_panel_calibration_pid.cpp).
// ===========================================================================

void PIDCalibrationPanel::set_temp_control_panel(TemperatureService*) {}
void PIDCalibrationPanel::show() {}

// ===========================================================================
// More global-scope accessors + wiring initializers. NOTE: these panel classes
// and their accessors are declared at GLOBAL scope in their headers — the
// `namespace helix { ... }` block at the top of each header is only a short
// forward-declaration block that closes before the class/accessor. The callers
// (ui_printer_manager_overlay.cpp etc.) reference the global symbols, so these
// MUST be global (not in namespace helix), or the mangled names won't match.
// ===========================================================================

// src/ui/ui_panel_input_shaper.cpp (DEFINE_GLOBAL_PANEL)
InputShaperPanel& get_global_input_shaper_panel() {
    alignas(InputShaperPanel)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(InputShaperPanel)];
    return *reinterpret_cast<InputShaperPanel*>(storage);
}

// src/ui/ui_panel_screws_tilt.cpp (DEFINE_GLOBAL_PANEL)
ScrewsTiltPanel& get_global_screws_tilt_panel() {
    alignas(ScrewsTiltPanel) EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(ScrewsTiltPanel)];
    return *reinterpret_cast<ScrewsTiltPanel*>(storage);
}

// src/ui/ui_panel_calibration_zoffset.cpp (DEFINE_GLOBAL_PANEL)
ZOffsetCalibrationPanel& get_global_zoffset_cal_panel() {
    alignas(ZOffsetCalibrationPanel)
        EXT_RAM_BSS_ATTR static unsigned char storage[sizeof(ZOffsetCalibrationPanel)];
    return *reinterpret_cast<ZOffsetCalibrationPanel*>(storage);
}

// Advanced-panel staged-calibration rows (Input Shaping / Belt Tension / Z-Offset)
// stay VISIBLE — they are features the user should learn exist — but their panels
// are excluded from the v1 cut. Instead of the old silent no-op registration,
// register a real handler that shows the shared "not yet available" toast so the
// tap has visible feedback and can never reach the null-vtable panel stub.
static void esp32_staged_feature_row_cb(lv_event_t* /*e*/) {
    helix::ui::show_feature_unavailable_toast();
}

// src/ui/ui_panel_belt_tension.cpp
void init_belt_tension_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_belt_tension_row_clicked", esp32_staged_feature_row_cb);
}

// src/ui/ui_panel_input_shaper.cpp
void init_input_shaper_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_input_shaper_row_clicked", esp32_staged_feature_row_cb);
}

// src/ui/ui_panel_screws_tilt.cpp — Advanced screws-tilt row stays a no-op; it is
// XML-gated on printer_has_screws_tilt and reached crash-safely (unregistered cb).
void init_screws_tilt_row_handler() {}

// src/ui/ui_panel_calibration_zoffset.cpp
void init_zoffset_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_zoffset_row_clicked", esp32_staged_feature_row_cb);
}
void init_zoffset_event_callbacks() {}

// ===========================================================================
// ColorSensorManager (src/sensors/color_sensor_manager.cpp).
//
// The TD-1 colour sensor stack is dead on EVERY platform, not just this one:
// `sensors_` is only ever filled by discover_from_moonraker(), whose sole
// caller is SensorRegistry::discover_all() — and nothing in the tree calls
// that (sensor_registry.cpp is not compiled anywhere). So the shipped
// behaviour is a permanently empty sensor list, which is exactly what these
// stubs reproduce.
//
// Unlike the panel accessors above, this one CANNOT be raw storage:
// printer_state.cpp calls update_from_status() on the returned reference for
// every status frame, and that is a virtual dispatch through the vptr. So
// instance() constructs a real object, which means defining the ctor/dtor and
// every ISensorManager override so the vtable emits here with all slots filled.
// ===========================================================================

namespace helix::sensors {

ColorSensorManager::ColorSensorManager() = default;
ColorSensorManager::~ColorSensorManager() = default;

ColorSensorManager& ColorSensorManager::instance() {
    static ColorSensorManager mgr;
    return mgr;
}

// sensors_overlay.xml binds `color_sensor_count` to hide the Color Sensors
// section, so the subjects must exist even though nothing ever fills them.
// Registering all three keeps the zero-sensor state identical to the real
// manager's; without this the overlay logs "No subject was found" on open.
void ColorSensorManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_STRING_N(color_hex_, color_hex_buf_.data(), COLOR_HEX_BUF_SIZE, "",
                                "filament_color_hex", subjects_);
    UI_MANAGED_SUBJECT_INT(td_value_, -1, "filament_td_value", subjects_);
    UI_MANAGED_SUBJECT_INT(sensor_count_, 0, "color_sensor_count", subjects_);
    subjects_initialized_ = true;
}

std::string ColorSensorManager::category_name() const {
    return "color";
}
void ColorSensorManager::discover_from_moonraker(const nlohmann::json&) {}
void ColorSensorManager::update_from_status(const nlohmann::json&) {}
void ColorSensorManager::load_config(const nlohmann::json&) {}
nlohmann::json ColorSensorManager::save_config() const {
    return nlohmann::json::object();
}

std::vector<ColorSensorConfig> ColorSensorManager::get_sensors() const {
    return {};
}
size_t ColorSensorManager::sensor_count() const {
    return 0;
}

} // namespace helix::sensors

// parse_shaper_csv IS genuinely helix::calibration-namespaced (called qualified
// from moonraker_advanced_api.cpp).
namespace helix {
namespace calibration {

// src/calibration/shaper_csv_parser.cpp — default (empty) result; only reachable
// after a real shaper calibration run, which cannot happen on Stage A.
ShaperCsvData parse_shaper_csv(const std::string&, char) {
    return ShaperCsvData{};
}

} // namespace calibration
} // namespace helix
