// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_utils.h"

#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"

#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "save_config_restart.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <lvgl.h>

namespace helix::zoffset {

const char* no_save_step_message(ZOffsetTuneHint hint) {
    switch (hint) {
    case ZOffsetTuneHint::SAVE_ON_CONTROLS:
        return nullptr;
    case ZOffsetTuneHint::AUTO_SAVED:
        return "Z-offset is auto-saved by firmware";
    case ZOffsetTuneHint::TEMPORARY:
        return "Temporary adjustment for this print";
    case ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE:
        return "Temporary for this print; a tool change resets it";
    }
    return nullptr;
}

bool no_save_step(ZOffsetTuneHint hint) {
    const char* message = no_save_step_message(hint);
    if (message == nullptr) {
        return false;
    }
    spdlog::debug("[ZOffsetUtils] No Save step for this printer: {}", message);
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr(message), 3000);
    return true;
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

namespace {

/// Wraps @p on_success so a completed save also clears @p ps's pending Z-offset
/// delta, on every success path, firmware-managed printers included: there the
/// offset is persisted even though HelixScreen sent nothing. Only success clears
/// it: a delta dropped after a failed save would tell the user an offset is
/// persisted when it is not. Main thread only, which every success path satisfies.
std::function<void()> clearing_pending_delta(PrinterState* ps, std::function<void()> on_success) {
    return [ps, on_success = std::move(on_success)]() {
        if (ps) {
            ps->clear_pending_z_offset_delta();
        }
        if (on_success) {
            on_success();
        }
    };
}

/// SAVE_CONFIG through @p save_watch, from the WebSocket thread.
///
/// The watch owns the whole SAVE_CONFIG contract: it arms the expected-restart
/// suppressions, sends the save, absorbs the rpc the restart drops, and reports
/// success only once Klipper is back READY (prestonbrown/helixscreen#1359).
/// begin() installs an observer, hence the _from_background form.
void begin_save_config(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                       std::function<void()> on_saved,
                       std::function<void(const std::string& error)> on_error) {
    save_watch.begin_from_background(
        api, "Saving config... Klipper will restart.", std::move(on_saved),
        [on_error = std::move(on_error)](const std::string& err) {
            // English in the log for developers, a translated copy for the user.
            spdlog::error("[ZOffsetUtils] SAVE_CONFIG failed: {}", err);
            if (on_error)
                on_error(fmt::format(
                    lv_tr("SAVE_CONFIG failed: {}. Z-offset was applied but not saved. "
                          "Run SAVE_CONFIG manually or the offset will be lost on restart."),
                    err));
        });
}

} // namespace

void apply_and_save(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                    ZOffsetCalibrationStrategy strategy, std::function<void()> on_success,
                    std::function<void(const std::string& error)> on_error, PrinterState* ps) {
    auto on_saved = clearing_pending_delta(ps, std::move(on_success));

    if (!api) {
        spdlog::error("[ZOffsetUtils] apply_and_save called with null API");
        if (on_error)
            on_error("No printer connection");
        return;
    }

    if (strategy == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) {
        // Firmware/macros handle persistence — nothing for us to do
        spdlog::debug("[ZOffsetUtils] apply_and_save: firmware_managed strategy — auto-saved");
        on_saved();
        return;
    }

    const char* apply_cmd = (strategy == ZOffsetCalibrationStrategy::PROBE_CALIBRATE)
                                ? "Z_OFFSET_APPLY_PROBE"
                                : "Z_OFFSET_APPLY_ENDSTOP";

    const char* strategy_name =
        (strategy == ZOffsetCalibrationStrategy::PROBE_CALIBRATE) ? "probe_calibrate" : "endstop";

    spdlog::info("[ZOffsetUtils] Applying Z-offset with {} strategy (cmd: {})", strategy_name,
                 apply_cmd);

    // ERROR_OWNERSHIP_OK: the error callback forwards to on_error, which every
    // caller surfaces (a toast in ControlsPanel and PrintTuneOverlay, the
    // calibration result in ZOffsetCalibrationPanel). The gate reads only the
    // callback body, where that hand-off looks like logging and nothing else.
    api->execute_gcode(
        apply_cmd,
        [api, apply_cmd, &save_watch, on_saved, on_error]() {
            spdlog::info("[ZOffsetUtils] {} success, executing SAVE_CONFIG", apply_cmd);
            begin_save_config(api, save_watch, on_saved, on_error);
        },
        [apply_cmd, on_error](const MoonrakerError& err) {
            spdlog::error("[ZOffsetUtils] {} failed: {}", apply_cmd, err.user_message());
            if (on_error)
                on_error(fmt::format(lv_tr("{} failed: {}"), apply_cmd, err.user_message()));
        });
}

void save_accepted_calibration(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                               ZOffsetCalibrationStrategy strategy,
                               std::function<void()> on_success,
                               std::function<void(const std::string& error)> on_error,
                               PrinterState* ps) {
    if (!api || strategy == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) {
        apply_and_save(api, save_watch, strategy, std::move(on_success), std::move(on_error), ps);
        return;
    }
    // ACCEPT has already staged the calibrated value. Z_OFFSET_APPLY_* would
    // overwrite it with the value Klipper loaded at start minus any live gcode
    // Z offset, so the save only commits.
    spdlog::info("[ZOffsetUtils] Saving the accepted calibration with SAVE_CONFIG");
    begin_save_config(api, save_watch, clearing_pending_delta(ps, std::move(on_success)),
                      std::move(on_error));
}

} // namespace helix::zoffset
