// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_capabilities_state.cpp
 * @brief Printer capabilities state management extracted from PrinterState
 *
 * Manages capability subjects that control UI feature visibility based on
 * hardware detection and user overrides. Extracted from PrinterState as
 * part of god class decomposition.
 */

#include "printer_capabilities_state.h"

#include "ui_update_queue.h"

#include "pa_calibration.h"
#include "sound_manager.h"
#include "state/subject_macros.h"
#include "tool_offset_calibration.h"
#include "webcam_selection.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

struct CapabilityDef {
    Capability cap;
    const char* name; ///< XML subject name
    int initial;
};

constexpr CapabilityDef CAPABILITY_DEFS[] = {
    {Capability::HasQgl, "printer_has_qgl", 0},
    {Capability::HasZTilt, "printer_has_z_tilt", 0},
    {Capability::HasBedMesh, "printer_has_bed_mesh", 0},
    {Capability::HasNozzleClean, "printer_has_nozzle_clean", 0},
    {Capability::HasProbe, "printer_has_probe", 0},
    {Capability::HasHeaterBed, "printer_has_heater_bed", 0},
    {Capability::HasLed, "printer_has_led", 0},
    {Capability::HasAccelerometer, "printer_has_accelerometer", 0},
    {Capability::HasSpoolman, "printer_has_spoolman", 0},
    {Capability::HasSpeaker, "printer_has_speaker", 0},
    {Capability::HasTimelapse, "printer_has_timelapse", 0},
    {Capability::HasJobQueue, "printer_has_job_queue", 0},
    {Capability::HasPurgeLine, "printer_has_purge_line", 0},
    {Capability::HasFirmwareRetraction, "printer_has_firmware_retraction", 0},
    {Capability::HasIndividualXyzHoming, "printer_has_individual_xyz_homing", 1},
    {Capability::SupportsBeltCompare, "printer_supports_belt_compare", 0},
    {Capability::BedMoves, "printer_bed_moves", 0},
    {Capability::IsEnclosed, "printer_is_enclosed", 0},
    {Capability::CanBedDry, "printer_can_bed_dry", 0},
    {Capability::HasChamberSensor, "printer_has_chamber_sensor", 0},
    {Capability::HasChamberHeater, "printer_has_chamber_heater", 0},
    {Capability::HasChamberHeaterDiagnostics, "printer_has_chamber_heater_diagnostics", 0},
    {Capability::HasChamberFilterFan, "printer_has_chamber_filter_fan", 0},
    {Capability::HasChamberElementTemp, "printer_has_chamber_element_temp", 0},
    {Capability::HasChamberDryer, "printer_has_chamber_dryer", 0},
    {Capability::HasChamber, "printer_has_chamber", 0},
    {Capability::HasScrewsTilt, "printer_has_screws_tilt", 0},
    {Capability::HasExcludeObject, "printer_has_exclude_object", 0},
    {Capability::HasToolOffsetCal, "printer_has_tool_offset_cal", 0},
    {Capability::HideManualZCalibration, "hide_manual_z_calibration", 0},
    {Capability::HasPaCal, "printer_has_pa_cal", 0},
    {Capability::HasWebcam, "printer_has_webcam", 0},
    {Capability::WebcamCount, "webcam_count", 0},
    {Capability::HasExtraFans, "printer_has_extra_fans", 0},
    {Capability::PowerDeviceCount, "power_device_count", 0},
    {Capability::SensorCount, "sensor_count", 0},
};
static_assert(std::size(CAPABILITY_DEFS) == CAPABILITY_COUNT);

constexpr bool defs_in_enum_order() {
    for (size_t i = 0; i < CAPABILITY_COUNT; ++i) {
        if (static_cast<size_t>(CAPABILITY_DEFS[i].cap) != i) {
            return false;
        }
    }
    return true;
}
static_assert(defs_in_enum_order());

} // namespace

void PrinterCapabilitiesState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterCapabilitiesState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterCapabilitiesState] Initializing subjects (register_xml={})",
                  register_xml);

    for (const auto& def : CAPABILITY_DEFS) {
        lv_subject_t* s = subject(def.cap);
        lv_subject_init_int(s, def.initial);
        subjects_.register_subject(s, register_xml ? def.name : nullptr);
        if (register_xml) {
            helix::xml::register_subject_in_current_scope(def.name, s);
        }
    }

    subjects_initialized_ = true;
    apply_pending_capability_values();
    spdlog::trace("[PrinterCapabilitiesState] Subjects initialized successfully");
}

void PrinterCapabilitiesState::set_capability(Capability cap, int value) {
    if (!subjects_initialized_) {
        pending_capability_values_[static_cast<size_t>(cap)] = value;
        return;
    }
    lv_subject_set_int(subject(cap), value);
}

int PrinterCapabilitiesState::capability_value(Capability cap) const {
    const size_t i = static_cast<size_t>(cap);
    if (!subjects_initialized_) {
        return pending_capability_values_[i].value_or(CAPABILITY_DEFS[i].initial);
    }
    return lv_subject_get_int(subject(cap));
}

void PrinterCapabilitiesState::apply_pending_capability_values() {
    // Applied AFTER subjects_initialized_ flips, so these go straight through to
    // the subjects. Observers attach later than init_subjects(), so they see the
    // real answer on their first callback rather than the hardcoded default.
    for (size_t i = 0; i < CAPABILITY_COUNT; ++i) {
        if (pending_capability_values_[i]) {
            lv_subject_set_int(&capability_subjects_[i], *pending_capability_values_[i]);
            pending_capability_values_[i].reset();
        }
    }
}

void PrinterCapabilitiesState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterCapabilitiesState] Deinitializing subjects");

    // Expire any setter callbacks still queued on the UpdateQueue. They capture
    // `this` and write the subjects torn down below; without this the next drain
    // notifies a freed observer list (#1165, #1146).
    async_lifetime_.invalidate();

    subjects_.deinit_all();
    subjects_initialized_ = false;
    // Answers latched for the subjects just torn down describe the OLD printer.
    // The latch exists to bridge the gap before the FIRST init, so anything
    // still held here would be replayed onto a machine it never described.
    pending_capability_values_.fill(std::nullopt);
}

void PrinterCapabilitiesState::set_hardware(const PrinterDiscovery& hardware,
                                            const CapabilityOverrides& overrides) {
    // Update subjects using effective values (auto-detect + user overrides)
    // This allows users to force-enable features that weren't detected
    // (e.g., heat soak macro without chamber heater) or force-disable
    // features they don't want to see in the UI.
    set_capability(Capability::HasQgl, overrides.has_qgl() ? 1 : 0);
    set_capability(Capability::HasZTilt, overrides.has_z_tilt() ? 1 : 0);
    set_capability(Capability::HasBedMesh, overrides.has_bed_mesh() ? 1 : 0);
    set_capability(Capability::HasNozzleClean, overrides.has_nozzle_clean() ? 1 : 0);

    // Hardware capabilities (no user override support yet - set directly from detection)
    spdlog::debug("[PrinterCapabilitiesState] has_probe={} has_led={} has_accel={}",
                  hardware.has_probe(), hardware.has_led(), hardware.has_accelerometer());
    set_capability(Capability::HasProbe, hardware.has_probe() ? 1 : 0);
    set_capability(Capability::HasHeaterBed, hardware.has_heater_bed() ? 1 : 0);
    set_capability(Capability::HasLed, hardware.has_led() ? 1 : 0);
    set_capability(Capability::HasAccelerometer, hardware.has_accelerometer() ? 1 : 0);

    // Install M300 (Klipper gcode beeper) backend now that we know whether
    // the printer answers M300 — a beeper output_pin or an M300 macro in the
    // Klipper config (has_speaker covers both) — or the user forced the
    // speaker capability on for a buzzer neither signal detects (e.g. firmware
    // with native M300 handling and no Klipper object at all). Without the
    // override arm, that forced-on setting silently no-ops: the sound settings
    // appear, but nothing ever installs a backend.
    // This MUST happen before flipping printer_has_speaker_ so any UI/handlers
    // observing the subject see a working backend. Real detection
    // (has_speaker) additionally takes the buzzer channel away from the PWM
    // sysfs backend: klippy's tone_player writes that same channel for every
    // M300/TONE it handles, so the two backends fight — klippy becomes the
    // single writer. The forced override alone never displaces an installed
    // backend (M300 may be unhandled there → the "!! Unknown command:M300"
    // feedback loop). A DISABLE override means "this printer has no speaker",
    // so it keeps the M300 backend out too rather than installing a beeper
    // the user disowned.
    const OverrideState speaker_override = overrides.get_override(capability::SPEAKER);
    if (speaker_override != OverrideState::DISABLE &&
        (hardware.has_speaker() || speaker_override == OverrideState::ENABLE)) {
        SoundManager::instance().try_install_m300_backend(hardware.has_speaker());
    }

    // Speaker capability — uses override system so presets can disable it
    // for printers without speakers (e.g., K1C has no beeper/buzzer).
    // AUTO mode: true if hardware beeper detected OR local sound backend exists.
    set_capability(Capability::HasSpeaker, overrides.has_speaker() ? 1 : 0);

    // Timelapse capability. moonraker-timelapse is a Moonraker component
    // (moonraker.conf), not a Klipper object, so it never appears in
    // printer.objects.list — for that install hardware.has_timelapse() is
    // false and the authoritative source is component detection via
    // set_timelapse_available() (see moonraker_discovery_sequence.cpp). OR the
    // hardware-derived flag with the current value so this batch never clobbers
    // a component-detected true back to false (which hid the timelapse
    // pre-print option, #1094). set_timelapse_available(false) is called first
    // in the discovery sequence, so switching to a printer without timelapse
    // still clears correctly. A Klipper [timelapse] object, if one ever exists,
    // still enables it through hardware.has_timelapse().
    set_capability(
        Capability::HasTimelapse,
        (hardware.has_timelapse() || capability_value(Capability::HasTimelapse) != 0) ? 1 : 0);

    // Firmware retraction capability (for G10/G11 retraction settings)
    set_capability(Capability::HasFirmwareRetraction, hardware.has_firmware_retraction() ? 1 : 0);

    // Chamber temperature sensor and heater capabilities
    set_capability(Capability::HasChamberSensor, hardware.has_chamber_sensor() ? 1 : 0);
    set_capability(Capability::HasChamberHeater, hardware.has_chamber_heater() ? 1 : 0);
    set_capability(Capability::HasChamber,
                   (hardware.has_chamber_sensor() || hardware.has_chamber_heater()) ? 1 : 0);

    // Screws tilt adjust capability
    set_capability(Capability::HasScrewsTilt, hardware.has_screws_tilt() ? 1 : 0);
    set_capability(Capability::HasExcludeObject, hardware.has_exclude_object() ? 1 : 0);

    // Automatic tool offset calibration: the module owns what "can" means.
    set_capability(Capability::HasToolOffsetCal,
                   helix::tool_offset_calibration::supported(hardware) ? 1 : 0);

    // Automatic pressure advance calibration. Which firmwares can measure it,
    // and how, belongs to helix::pacal - this only asks whether one matched.
    set_capability(Capability::HasPaCal, helix::pacal::is_supported(hardware) ? 1 : 0);

    // Spoolman requires async check - default to 0, updated separately via set_spoolman_available()

    spdlog::debug("[PrinterCapabilitiesState] Hardware set: probe={}, heater_bed={}, LED={}, "
                  "accelerometer={}, speaker={}, timelapse={}, fw_retraction={}, chamber_sensor={}",
                  hardware.has_probe(), hardware.has_heater_bed(), hardware.has_led(),
                  hardware.has_accelerometer(), hardware.has_speaker(), hardware.has_timelapse(),
                  hardware.has_firmware_retraction(), hardware.has_chamber_sensor());
    spdlog::debug("[PrinterCapabilitiesState] Hardware set (with overrides): {}",
                  overrides.summary());
}

void PrinterCapabilitiesState::set_sound_backend_available(bool available) {
    if (available && capability_value(Capability::HasSpeaker) == 0) {
        set_capability(Capability::HasSpeaker, 1);
        spdlog::debug("[PrinterCapabilitiesState] Sound backend available, speaker enabled");
    }
}

void PrinterCapabilitiesState::set_spoolman_available(bool available) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_spoolman_available", [this, available]() {
        set_capability(Capability::HasSpoolman, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Spoolman availability set: {}", available);
    });
}

void PrinterCapabilitiesState::set_webcams(std::vector<WebcamInfo> cams) {
    async_lifetime_.defer(
        "PrinterCapabilitiesState::set_webcams", [this, cams = std::move(cams)]() mutable {
            webcams_ = std::move(cams);
            auto feed = webcam::auto_pick(webcams_);
            webcam_stream_url_ = feed ? feed->stream_url : "";
            webcam_snapshot_url_ = feed ? feed->snapshot_url : "";
            webcam_flip_h_ = feed ? feed->flip_horizontal : false;
            webcam_flip_v_ = feed ? feed->flip_vertical : false;
            webcam_target_fps_ = (feed && feed->target_fps > 0) ? feed->target_fps : 15;
            int named = 0;
            for (const auto& cam : webcams_) {
                if (!cam.name.empty())
                    ++named;
            }
            set_capability(Capability::HasWebcam, feed ? 1 : 0);
            set_capability(Capability::WebcamCount, named);
            spdlog::debug("[PrinterCapabilitiesState] Webcams: {} listed ({} named), "
                          "auto-pick={} stream_url={} flip_h={} flip_v={} target_fps={}",
                          webcams_.size(), named, feed ? feed->name : "<none>", webcam_stream_url_,
                          webcam_flip_h_, webcam_flip_v_, webcam_target_fps_);
        });
}

void PrinterCapabilitiesState::set_webcam_available(bool available, const std::string& stream_url,
                                                    const std::string& snapshot_url, bool flip_h,
                                                    bool flip_v, int target_fps) {
    std::vector<WebcamInfo> cams;
    if (available) {
        WebcamInfo cam;
        cam.stream_url = stream_url;
        cam.snapshot_url = snapshot_url;
        if (!stream_url.empty()) {
            cam.service = "mjpegstreamer";
        }
        cam.flip_horizontal = flip_h;
        cam.flip_vertical = flip_v;
        cam.target_fps = target_fps;
        cams.push_back(std::move(cam));
    }
    set_webcams(std::move(cams));
}

void PrinterCapabilitiesState::set_timelapse_available(bool available) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_timelapse_available", [this, available]() {
        set_capability(Capability::HasTimelapse, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Timelapse availability set: {}", available);
    });
}

void PrinterCapabilitiesState::set_job_queue_available(bool available) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_job_queue_available", [this, available]() {
        set_capability(Capability::HasJobQueue, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Job queue availability "
                      "set: {}",
                      available);
    });
}

void PrinterCapabilitiesState::set_purge_line(bool has_purge_line) {
    set_capability(Capability::HasPurgeLine, has_purge_line ? 1 : 0);
    spdlog::debug("[PrinterCapabilitiesState] Purge line capability set: {}", has_purge_line);
}

void PrinterCapabilitiesState::set_hide_manual_z_calibration(bool hide) {
    set_capability(Capability::HideManualZCalibration, hide ? 1 : 0);
    spdlog::debug("[PrinterCapabilitiesState] Hide manual Z calibration: {}", hide);
}

void PrinterCapabilitiesState::set_has_individual_xyz_homing(bool has_individual_xyz_homing) {
    int new_value = has_individual_xyz_homing ? 1 : 0;
    if (capability_value(Capability::HasIndividualXyzHoming) != new_value) {
        set_capability(Capability::HasIndividualXyzHoming, new_value);
        spdlog::info("[PrinterCapabilitiesState] Has individual XYZ homing: {}",
                     has_individual_xyz_homing);
    }
}

void PrinterCapabilitiesState::set_supports_belt_compare(bool supports) {
    int new_value = supports ? 1 : 0;
    if (capability_value(Capability::SupportsBeltCompare) != new_value) {
        set_capability(Capability::SupportsBeltCompare, new_value);
        spdlog::info("[PrinterCapabilitiesState] Supports belt compare: {}", supports);
    }
}

void PrinterCapabilitiesState::set_bed_moves(bool bed_moves) {
    int new_value = bed_moves ? 1 : 0;
    // Only log when value actually changes (this gets called frequently from status updates)
    if (capability_value(Capability::BedMoves) != new_value) {
        set_capability(Capability::BedMoves, new_value);
        spdlog::info("[PrinterCapabilitiesState] Bed moves on Z: {}", bed_moves);
    }
}

void PrinterCapabilitiesState::set_bed_drying(bool enclosed, bool can_bed_dry) {
    if (capability_value(Capability::IsEnclosed) != (enclosed ? 1 : 0)) {
        set_capability(Capability::IsEnclosed, enclosed ? 1 : 0);
        spdlog::info("[PrinterCapabilitiesState] Enclosed: {}", enclosed);
    }
    if (capability_value(Capability::CanBedDry) != (can_bed_dry ? 1 : 0)) {
        set_capability(Capability::CanBedDry, can_bed_dry ? 1 : 0);
        spdlog::info("[PrinterCapabilitiesState] Bed drying available: {}", can_bed_dry);
    }
}

void PrinterCapabilitiesState::set_has_chamber_sensor(bool available) {
    set_capability(Capability::HasChamberSensor, available ? 1 : 0);
    update_has_chamber();
}

void PrinterCapabilitiesState::set_has_chamber_heater(bool available) {
    set_capability(Capability::HasChamberHeater, available ? 1 : 0);
    update_has_chamber();
}

// Diagnostics / filter-fan capabilities are independent of the combined
// printer_has_chamber_ flag: they gate backend-specific surfaces only.
void PrinterCapabilitiesState::set_has_chamber_heater_diagnostics(bool available) {
    set_capability(Capability::HasChamberHeaterDiagnostics, available ? 1 : 0);
}

void PrinterCapabilitiesState::set_has_chamber_filter_fan(bool available) {
    set_capability(Capability::HasChamberFilterFan, available ? 1 : 0);
}

void PrinterCapabilitiesState::set_has_chamber_element_temp(bool available) {
    set_capability(Capability::HasChamberElementTemp, available ? 1 : 0);
}

void PrinterCapabilitiesState::set_has_chamber_dryer(bool available) {
    set_capability(Capability::HasChamberDryer, available ? 1 : 0);
}

void PrinterCapabilitiesState::update_has_chamber() {
    bool has_any = capability_value(Capability::HasChamberSensor) != 0 ||
                   capability_value(Capability::HasChamberHeater) != 0;
    set_capability(Capability::HasChamber, has_any ? 1 : 0);
}

void PrinterCapabilitiesState::set_power_device_count(int count) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_power_device_count", [this, count]() {
        set_capability(Capability::PowerDeviceCount, count);
        spdlog::debug("[PrinterCapabilitiesState] Power device count set: {}", count);
    });
}

void PrinterCapabilitiesState::set_sensor_count(int count) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_sensor_count", [this, count]() {
        set_capability(Capability::SensorCount, count);
        spdlog::debug("[PrinterCapabilitiesState] Sensor count set: {}", count);
    });
}

} // namespace helix
