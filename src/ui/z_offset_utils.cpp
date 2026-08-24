// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_utils.h"

#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"

#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <lvgl.h>

namespace helix::zoffset {

bool is_auto_saved(ZOffsetCalibrationStrategy strategy) {
    if (strategy == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) {
        // FIRMWARE_MANAGED means firmware or macros handle Z-offset persistence
        // (e.g., FlashForge firmware, Artillery M1 save-zoffset macro).
        // HelixScreen should not offer its own save path.
        spdlog::debug("[ZOffsetUtils] Z-offset auto-saved by firmware (firmware_managed strategy)");
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Z-offset is auto-saved by firmware"), 3000);
        return true;
    }
    return false;
}

void format_delta(int microns, char* buf, size_t buf_size) {
    if (microns == 0) {
        buf[0] = '\0';
        return;
    }
    double mm = static_cast<double>(microns) / 1000.0;
    std::snprintf(buf, buf_size, "%+.3fmm", mm);
}

void format_offset(int microns, char* buf, size_t buf_size) {
    double mm = static_cast<double>(microns) / 1000.0;
    std::snprintf(buf, buf_size, "%+.3fmm", mm);
}

void format_offset_compact(int microns, char* buf, size_t buf_size) {
    // Drop leading zero for |value| < 1mm: "+.050mm" instead of "+0.050mm"
    int abs_microns = microns < 0 ? -microns : microns;
    if (abs_microns < 1000) {
        char sign = microns < 0 ? '-' : '+';
        std::snprintf(buf, buf_size, "%c.%03dmm", sign, abs_microns);
    } else {
        double mm = static_cast<double>(microns) / 1000.0;
        std::snprintf(buf, buf_size, "%+.3fmm", mm);
    }
}

int displayed_z_offset_microns(int live_microns, std::optional<int> persisted_microns,
                               bool print_active) {
    if (print_active || !persisted_microns.has_value()) {
        return live_microns;
    }
    return *persisted_microns;
}

int displayed_z_offset_microns(helix::PrinterState& state) {
    return displayed_z_offset_microns(lv_subject_get_int(state.get_gcode_z_offset_subject()),
                                      state.get_persisted_z_offset_microns(),
                                      lv_subject_get_int(state.get_print_active_subject()) != 0);
}

std::string build_z_adjust_gcode(int base_microns, int live_microns, int delta_microns,
                                 bool all_homed) {
    // MOVE=1 makes the toolhead take up the new offset immediately, which is what
    // makes baby stepping usable. Klipper errors on it when an axis is unhomed.
    const char* move = all_homed ? " MOVE=1" : "";

    if (base_microns == live_microns) {
        return fmt::format("SET_GCODE_OFFSET Z_ADJUST={:.3f}{}",
                           static_cast<double>(delta_microns) / 1000.0, move);
    }
    return fmt::format("SET_GCODE_OFFSET Z={:.3f}{}",
                       static_cast<double>(base_microns + delta_microns) / 1000.0, move);
}

void apply_and_save(IMoonrakerAPI* api, ZOffsetCalibrationStrategy strategy,
                    std::function<void()> on_success,
                    std::function<void(const std::string& error)> on_error) {
    if (!api) {
        spdlog::error("[ZOffsetUtils] apply_and_save called with null API");
        if (on_error)
            on_error("No printer connection");
        return;
    }

    if (strategy == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) {
        // Firmware/macros handle persistence — nothing for us to do
        spdlog::debug("[ZOffsetUtils] apply_and_save: firmware_managed strategy — auto-saved");
        if (on_success)
            on_success();
        return;
    }

    const char* apply_cmd = (strategy == ZOffsetCalibrationStrategy::PROBE_CALIBRATE)
                                ? "Z_OFFSET_APPLY_PROBE"
                                : "Z_OFFSET_APPLY_ENDSTOP";

    const char* strategy_name =
        (strategy == ZOffsetCalibrationStrategy::PROBE_CALIBRATE) ? "probe_calibrate" : "endstop";

    spdlog::info("[ZOffsetUtils] Applying Z-offset with {} strategy (cmd: {})", strategy_name,
                 apply_cmd);

    api->execute_gcode(
        apply_cmd,
        [api, apply_cmd, on_success, on_error]() {
            spdlog::info("[ZOffsetUtils] {} success, executing SAVE_CONFIG", apply_cmd);

            // SAVE_CONFIG triggers a Klipper restart. This callback runs on
            // the WebSocket thread; begin_expected_klippy_restart() is safe
            // here - the suppressions are atomics and the toast hops to the
            // main thread.
            helix::ui::begin_expected_klippy_restart("Saving config... Klipper will restart.");

            api->execute_gcode(
                "SAVE_CONFIG",
                [on_success]() {
                    spdlog::info("[ZOffsetUtils] SAVE_CONFIG success — Klipper restarting");
                    if (on_success)
                        on_success();
                },
                [on_error](const MoonrakerError& err) {
                    // Log in English (developer-facing), hand the user a
                    // translated copy. The message used to be one bare
                    // fmt::format serving both, so the whole sentence was
                    // untranslatable.
                    spdlog::error("[ZOffsetUtils] SAVE_CONFIG failed: {}", err.user_message());
                    if (on_error)
                        on_error(fmt::format(
                            lv_tr(
                                "SAVE_CONFIG failed: {}. Z-offset was applied but not saved. "
                                "Run SAVE_CONFIG manually or the offset will be lost on restart."),
                            err.user_message()));
                });
        },
        [apply_cmd, on_error](const MoonrakerError& err) {
            spdlog::error("[ZOffsetUtils] {} failed: {}", apply_cmd, err.user_message());
            if (on_error)
                on_error(fmt::format(lv_tr("{} failed: {}"), apply_cmd, err.user_message()));
        });
}

bool should_extend_save_timeout(bool restart_latched, unsigned extensions_used,
                                unsigned max_extensions) {
    // Only stall the clock if this save actually triggered a restart. A save
    // that never restarted Klipper and still has not completed is a real hang
    // and must be reported.
    return restart_latched && extensions_used < max_extensions;
}

} // namespace helix::zoffset
