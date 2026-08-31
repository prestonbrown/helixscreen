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

#include "sound_manager.h"
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

namespace helix {

void PrinterCapabilitiesState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterCapabilitiesState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterCapabilitiesState] Initializing subjects (register_xml={})",
                  register_xml);

    // Printer capability subjects (all default to 0=not available)
    INIT_SUBJECT_INT(printer_has_qgl, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_z_tilt, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_bed_mesh, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_nozzle_clean, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_probe, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_heater_bed, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_led, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_accelerometer, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_spoolman, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_speaker, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_timelapse, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_purge_line, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_firmware_retraction, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_bed_moves, 0, subjects_, register_xml); // 0=gantry moves, 1=bed moves
    INIT_SUBJECT_INT(printer_has_chamber_sensor, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_chamber_heater, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_chamber_heater_diagnostics, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_chamber_filter_fan, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_chamber, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_screws_tilt, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_webcam, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(printer_has_extra_fans, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(power_device_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(sensor_count, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    apply_pending_capability_values();
    spdlog::trace("[PrinterCapabilitiesState] Subjects initialized successfully");
}

void PrinterCapabilitiesState::set_capability_int(lv_subject_t& subject, int value) {
    if (!subjects_initialized_) {
        pending_capability_values_[&subject] = value;
        return;
    }
    lv_subject_set_int(&subject, value);
}

void PrinterCapabilitiesState::apply_pending_capability_values() {
    if (pending_capability_values_.empty()) {
        return;
    }
    // Applied AFTER subjects_initialized_ flips, so these go straight through to
    // the subjects. Observers attach later than init_subjects(), so they see the
    // real answer on their first callback rather than the hardcoded default.
    spdlog::debug("[PrinterCapabilitiesState] Seeding {} capability value(s) that arrived "
                  "before subject init",
                  pending_capability_values_.size());
    for (const auto& [subject, value] : pending_capability_values_) {
        lv_subject_set_int(subject, value);
    }
    pending_capability_values_.clear();
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
    pending_capability_values_.clear();
}

void PrinterCapabilitiesState::set_hardware(const PrinterDiscovery& hardware,
                                            const CapabilityOverrides& overrides) {
    // Update subjects using effective values (auto-detect + user overrides)
    // This allows users to force-enable features that weren't detected
    // (e.g., heat soak macro without chamber heater) or force-disable
    // features they don't want to see in the UI.
    set_capability_int(printer_has_qgl_, overrides.has_qgl() ? 1 : 0);
    set_capability_int(printer_has_z_tilt_, overrides.has_z_tilt() ? 1 : 0);
    set_capability_int(printer_has_bed_mesh_, overrides.has_bed_mesh() ? 1 : 0);
    set_capability_int(printer_has_nozzle_clean_, overrides.has_nozzle_clean() ? 1 : 0);

    // Hardware capabilities (no user override support yet - set directly from detection)
    spdlog::debug("[PrinterCapabilitiesState] has_probe={} has_led={} has_accel={}",
                  hardware.has_probe(), hardware.has_led(), hardware.has_accelerometer());
    set_capability_int(printer_has_probe_, hardware.has_probe() ? 1 : 0);
    set_capability_int(printer_has_heater_bed_, hardware.has_heater_bed() ? 1 : 0);
    set_capability_int(printer_has_led_, hardware.has_led() ? 1 : 0);
    set_capability_int(printer_has_accelerometer_, hardware.has_accelerometer() ? 1 : 0);

    // Install M300 (Klipper gcode beeper) backend now that we know whether
    // the printer answers M300 — a beeper output_pin or an M300 macro in the
    // Klipper config (has_speaker covers both) — or the user forced the
    // speaker capability on for a buzzer neither signal detects (e.g. firmware
    // with native M300 handling and no Klipper object at all). Without the
    // override arm, that forced-on setting silently no-ops: the sound settings
    // appear, but nothing ever installs a backend.
    // This MUST happen before flipping printer_has_speaker_ so any UI/handlers
    // observing the subject see a working backend. SoundManager no-ops if
    // a real audio backend (SDL/ALSA/PWM) is already installed. A DISABLE
    // override means "this printer has no speaker", so it keeps the M300
    // backend out too rather than installing a beeper the user disowned.
    const OverrideState speaker_override = overrides.get_override(capability::SPEAKER);
    if (speaker_override != OverrideState::DISABLE &&
        (hardware.has_speaker() || speaker_override == OverrideState::ENABLE)) {
        SoundManager::instance().try_install_m300_backend();
    }

    // Speaker capability — uses override system so presets can disable it
    // for printers without speakers (e.g., K1C has no beeper/buzzer).
    // AUTO mode: true if hardware beeper detected OR local sound backend exists.
    set_capability_int(printer_has_speaker_, overrides.has_speaker() ? 1 : 0);

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
    lv_subject_set_int(
        &printer_has_timelapse_,
        (hardware.has_timelapse() || lv_subject_get_int(&printer_has_timelapse_) != 0) ? 1 : 0);

    // Firmware retraction capability (for G10/G11 retraction settings)
    set_capability_int(printer_has_firmware_retraction_,
                       hardware.has_firmware_retraction() ? 1 : 0);

    // Chamber temperature sensor and heater capabilities
    set_capability_int(printer_has_chamber_sensor_, hardware.has_chamber_sensor() ? 1 : 0);
    set_capability_int(printer_has_chamber_heater_, hardware.has_chamber_heater() ? 1 : 0);
    set_capability_int(printer_has_chamber_,
                       (hardware.has_chamber_sensor() || hardware.has_chamber_heater()) ? 1 : 0);

    // Screws tilt adjust capability
    set_capability_int(printer_has_screws_tilt_, hardware.has_screws_tilt() ? 1 : 0);

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
    if (available && lv_subject_get_int(&printer_has_speaker_) == 0) {
        set_capability_int(printer_has_speaker_, 1);
        spdlog::debug("[PrinterCapabilitiesState] Sound backend available, speaker enabled");
    }
}

void PrinterCapabilitiesState::set_spoolman_available(bool available) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_spoolman_available", [this, available]() {
        set_capability_int(printer_has_spoolman_, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Spoolman availability set: {}", available);
    });
}

void PrinterCapabilitiesState::set_webcam_available(bool available, const std::string& stream_url,
                                                    const std::string& snapshot_url, bool flip_h,
                                                    bool flip_v, int target_fps) {
    // Store URLs before queuing (captured by value for thread safety)
    async_lifetime_.defer("PrinterCapabilitiesState::set_webcam_available", [this, available,
                                                                             stream_url,
                                                                             snapshot_url, flip_h,
                                                                             flip_v, target_fps]() {
        webcam_stream_url_ = available ? stream_url : "";
        webcam_snapshot_url_ = available ? snapshot_url : "";
        webcam_flip_h_ = flip_h;
        webcam_flip_v_ = flip_v;
        webcam_target_fps_ = target_fps > 0 ? target_fps : 15;
        set_capability_int(printer_has_webcam_, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Webcam: available={}, stream_url={}, flip_h={}, "
                      "flip_v={}, target_fps={}",
                      available, stream_url, flip_h, flip_v, webcam_target_fps_);
    });
}

void PrinterCapabilitiesState::set_timelapse_available(bool available) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_timelapse_available", [this, available]() {
        set_capability_int(printer_has_timelapse_, available ? 1 : 0);
        spdlog::debug("[PrinterCapabilitiesState] Timelapse availability set: {}", available);
    });
}

void PrinterCapabilitiesState::set_purge_line(bool has_purge_line) {
    set_capability_int(printer_has_purge_line_, has_purge_line ? 1 : 0);
    spdlog::debug("[PrinterCapabilitiesState] Purge line capability set: {}", has_purge_line);
}

void PrinterCapabilitiesState::set_bed_moves(bool bed_moves) {
    int new_value = bed_moves ? 1 : 0;
    // Only log when value actually changes (this gets called frequently from status updates)
    if (lv_subject_get_int(&printer_bed_moves_) != new_value) {
        set_capability_int(printer_bed_moves_, new_value);
        spdlog::info("[PrinterCapabilitiesState] Bed moves on Z: {}", bed_moves);
    }
}

void PrinterCapabilitiesState::set_has_chamber_sensor(bool available) {
    set_capability_int(printer_has_chamber_sensor_, available ? 1 : 0);
    update_has_chamber();
}

void PrinterCapabilitiesState::set_has_chamber_heater(bool available) {
    set_capability_int(printer_has_chamber_heater_, available ? 1 : 0);
    update_has_chamber();
}

// Diagnostics / filter-fan capabilities are independent of the combined
// printer_has_chamber_ flag: they gate backend-specific surfaces only.
void PrinterCapabilitiesState::set_has_chamber_heater_diagnostics(bool available) {
    lv_subject_set_int(&printer_has_chamber_heater_diagnostics_, available ? 1 : 0);
}

void PrinterCapabilitiesState::set_has_chamber_filter_fan(bool available) {
    lv_subject_set_int(&printer_has_chamber_filter_fan_, available ? 1 : 0);
}

void PrinterCapabilitiesState::update_has_chamber() {
    bool has_any = lv_subject_get_int(&printer_has_chamber_sensor_) != 0 ||
                   lv_subject_get_int(&printer_has_chamber_heater_) != 0;
    set_capability_int(printer_has_chamber_, has_any ? 1 : 0);
}

void PrinterCapabilitiesState::set_power_device_count(int count) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_power_device_count", [this, count]() {
        set_capability_int(power_device_count_, count);
        spdlog::debug("[PrinterCapabilitiesState] Power device count set: {}", count);
    });
}

void PrinterCapabilitiesState::set_sensor_count(int count) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    async_lifetime_.defer("PrinterCapabilitiesState::set_sensor_count", [this, count]() {
        set_capability_int(sensor_count_, count);
        spdlog::debug("[PrinterCapabilitiesState] Sensor count set: {}", count);
    });
}

} // namespace helix
