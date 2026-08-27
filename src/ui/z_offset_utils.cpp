// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_utils.h"
#include "ui_modal.h"
#include <optional>
#include "static_subject_registry.h"
#include "app_globals.h"
#include "tool_state.h"
#include "tool_offsets.h"

#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_toast_manager.h"

#include "config.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "save_config_restart.h"
#include "toolhead_homing.h"
#include "z_offset_persistence.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
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

void apply_and_save(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                    ZOffsetCalibrationStrategy strategy, std::function<void()> on_success,
                    std::function<void(const std::string& error)> on_error, PrinterState* ps) {
    // Both success paths below (the FIRMWARE_MANAGED early return and the
    // APPLY -> SAVE_CONFIG chain) funnel through this wrapper, so the pending
    // Z-offset delta is cleared exactly once, wherever the save actually
    // completed — including firmware-managed printers, where the offset is
    // genuinely persisted even though HelixScreen sent nothing.
    auto on_saved = [ps, on_success = std::move(on_success)]() {
        if (ps) {
            ps->clear_pending_z_offset_delta();
        }
        if (on_success) {
            on_success();
        }
    };

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

            // The watch owns the whole SAVE_CONFIG contract: it arms the
            // expected-restart suppressions, sends the save, absorbs the rpc the
            // restart drops, and reports success only once Klipper is back READY.
            // Sending it raw here reported every successful save as a failure and
            // never ran on_saved(), so the pending Z-offset delta was left set
            // (prestonbrown/helixscreen#1359).
            //
            // This callback lands on the WebSocket thread and begin() installs an
            // observer, hence the _from_background form.
            save_watch.begin_from_background(
                api, "Saving config... Klipper will restart.", on_saved,
                [on_error](const std::string& err) {
                    // Log in English (developer-facing), hand the user a
                    // translated copy. The message used to be one bare
                    // fmt::format serving both, so the whole sentence was
                    // untranslatable.
                    spdlog::error("[ZOffsetUtils] SAVE_CONFIG failed: {}", err);
                    if (on_error)
                        on_error(fmt::format(
                            lv_tr(
                                "SAVE_CONFIG failed: {}. Z-offset was applied but not saved. "
                                "Run SAVE_CONFIG manually or the offset will be lost on restart."),
                            err));
                });
        },
        [apply_cmd, on_error](const MoonrakerError& err) {
            spdlog::error("[ZOffsetUtils] {} failed: {}", apply_cmd, err.user_message());
            if (on_error)
                on_error(fmt::format(lv_tr("{} failed: {}"), apply_cmd, err.user_message()));
        });
}

int persisted_step_index() {
    Config* config = Config::get_instance();
    if (!config) {
        return kZStepDefaultIndex;
    }
    int idx = config->get<int>(config->df() + "z_offset/step_index", kZStepDefaultIndex);
    if (idx < 0 || idx >= static_cast<int>(std::size(kZStepAmountsMm))) {
        return kZStepDefaultIndex;
    }
    return idx;
}

void set_persisted_step_index(int idx) {
    if (idx < 0 || idx >= static_cast<int>(std::size(kZStepAmountsMm))) {
        // Reject rather than clamp-and-write: the read path (persisted_step_index())
        // already fully defends against a corrupt on-disk value, so clamping here
        // buys no safety — it would just give a future caller bug a way to silently
        // overwrite the user's real setting with the default.
        spdlog::warn("[zoffset] rejecting out-of-range step index {} — leaving persisted "
                     "value unchanged",
                     idx);
        return;
    }
    Config* config = Config::get_instance();
    if (!config) {
        return;
    }
    config->set<int>(config->df() + "z_offset/step_index", idx);
    config->save();
}

AdjustResult adjust(IMoonrakerAPI* api, PrinterState* ps, double session_base_mm,
                    double current_offset_mm, double delta_mm) {
    // Bound how far one session may travel from the offset it opened on, so a
    // stuck button cannot walk the nozzle into the bed. Clamping the absolute
    // offset instead snapped a legitimately large one down to the limit on the
    // first tap - a nose dive rather than a guard rail. The window is widened to
    // always contain the current offset, so should the base ever go stale the
    // worst outcome is a refused step rather than a jump.
    const double min_offset =
        std::min(session_base_mm - kZOffsetMaxSessionTravelMm, current_offset_mm);
    const double max_offset =
        std::max(session_base_mm + kZOffsetMaxSessionTravelMm, current_offset_mm);

    double new_offset = current_offset_mm + delta_mm;
    if (new_offset < min_offset || new_offset > max_offset) {
        spdlog::warn("[zoffset] {:.3f}mm clamped to [{:.3f}, {:.3f}]", new_offset, min_offset,
                     max_offset);
        new_offset = std::clamp(new_offset, min_offset, max_offset);
        delta_mm = new_offset - current_offset_mm;
        if (std::abs(delta_mm) < 0.0005) {
            return AdjustResult{0.0, current_offset_mm, false, true};
        }
    }

    // Round to the micron so repeated additions cannot drift.
    new_offset = std::round(new_offset * 1000.0) / 1000.0;

    const int delta_microns = static_cast<int>(std::lround(delta_mm * 1000.0));
    const int new_microns = static_cast<int>(std::lround(new_offset * 1000.0));
    const int base_microns = new_microns - delta_microns;
    // Read the live offset before the optimistic write below overwrites it.
    const int live_microns = ps ? lv_subject_get_int(ps->get_gcode_z_offset_subject()) : 0;
    const bool adjusting_from_persisted = ps && base_microns != live_microns;

    if (ps) {
        ps->add_pending_z_offset_delta(delta_microns);
        // Publish immediately rather than waiting for Moonraker to broadcast.
        if (auto* subj = ps->get_gcode_z_offset_subject()) {
            lv_subject_set_int(subj, new_microns);
        }
        // When the base came from the firmware-persisted value we are about to
        // send an absolute Z=, which ZMOD's override stores verbatim. Move the
        // persisted subject with it so the Controls row does not show the stale
        // number until save_variables is broadcast back.
        if (adjusting_from_persisted) {
            if (auto* subj = ps->get_persisted_z_offset_subject()) {
                lv_subject_set_int(subj, new_microns);
            }
        }
    }

    if (!api) {
        return AdjustResult{delta_mm, new_offset, false, false};
    }

    const bool all_homed = ps && helix::toolhead_is_homed(*ps);

    // Relative Z_ADJUST resolves against homing_origin, so it is only right when
    // the base we adjusted from IS the live offset. See build_z_adjust_gcode().
    std::string gcode = build_z_adjust_gcode(base_microns, live_microns, delta_microns, all_homed);

    // ZMOD persists the adjustment as `z - _TEST_POINT.temp_z_offset`, and
    // through 1.7.2 that variable survived END_PRINT/CANCEL_PRINT - so while
    // idle it holds the LAST print's probe delta and the stored offset drifts
    // by it (ghzserg/zmod#699; fixed upstream after 1.7.2, where the clear
    // becomes a no-op). Clear it on the same script, before the override reads
    // it. Never mid-print: there the subtraction excludes the live per-print
    // transient and is correct.
    if (ps && lv_subject_get_int(ps->get_print_active_subject()) == 0) {
        const std::string clear = stale_probe_delta_clear_gcode(ps->get_discovery());
        if (!clear.empty()) {
            gcode = clear + "\n" + gcode;
        }
    }

    const double sent_delta = delta_mm;
    api->execute_gcode(
        gcode, [sent_delta]() { spdlog::debug("[zoffset] adjusted {:+.3f}mm", sent_delta); },
        [](const MoonrakerError& err) {
            spdlog::error("[zoffset] adjust failed: {}", err.message);
            NOTIFY_ERROR(lv_tr("Z-offset failed: {}"), err.user_message());
        });

    return AdjustResult{delta_mm, new_offset, true, false};
}

namespace {

/// Owns the header button's SaveConfigWatch. Not a function-local static of the
/// watch itself: its destructor resets an ObserverGuard, which at process exit
/// would run after lv_deinit() had freed the subject it observes (#705). Held
/// in an optional and destroyed from a StaticSubjectRegistry deinit instead,
/// which runs while LVGL is still up.
std::optional<helix::ui::SaveConfigWatch>& shared_save_watch_storage() {
    static std::optional<helix::ui::SaveConfigWatch> storage;
    return storage;
}

/// The watch, constructing it on first use and registering the teardown that
/// destroys it while LVGL is still up. Deliberately NOT reached through the
/// same call that re-creates it: the deinit callback touches the storage
/// directly, or resetting it here would immediately construct a fresh one.
helix::ui::SaveConfigWatch& shared_save_watch() {
    auto& storage = shared_save_watch_storage();
    if (!storage) {
        storage.emplace();
        StaticSubjectRegistry::instance().register_deinit(
            "zoffset::shared_save_watch", [] { shared_save_watch_storage().reset(); });
    }
    return *storage;
}

} // namespace

namespace {

/// The save itself, once any confirmation has been answered.
void run_shared_save() {
    IMoonrakerAPI* api = get_moonraker_api();
    PrinterState& ps = get_printer_state();
    if (!api) {
        NOTIFY_ERROR("{}", lv_tr("No printer connection"));
        return;
    }
    const bool global_dirty = lv_subject_get_int(ps.get_gcode_z_offset_subject()) != 0;

    NOTIFY_INFO(lv_tr("Saving Z-offset..."));
    save_dirty_offsets(
        api, shared_save_watch(), ps.get_z_offset_calibration_strategy(), ps.get_discovery(),
        global_dirty, []() { NOTIFY_SUCCESS("{}", lv_tr("Z-offset saved")); },
        [](const std::string& error) { NOTIFY_ERROR("{}", error); }, &ps);
}

} // namespace

void save_dirty_offsets_shared() {
    PrinterState& ps = get_printer_state();
    const bool global_dirty = lv_subject_get_int(ps.get_gcode_z_offset_subject()) != 0;
    const bool tools_dirty =
        lv_subject_get_int(helix::ToolState::instance().get_any_tool_z_dirty_subject()) == 1;

    // Only warn when a restart is actually coming. The machine-wide save always
    // ends in SAVE_CONFIG, and so does a tool-only save on firmware that stages
    // its parameters — but a firmware that persists immediately restarts
    // nothing, and a confirmation promising a disconnect would be a lie.
    const bool restart_expected =
        global_dirty ||
        (tools_dirty && helix::tool_offsets::persist_requires_save_config(ps.get_discovery()));

    if (!restart_expected) {
        run_shared_save();
        return;
    }

    // Same warning the Controls button gives. A one-tap header button that
    // silently restarts Klipper mid-session is the worse failure.
    helix::ui::modal_show_confirmation(
        lv_tr("Save Z-Offset?"),
        lv_tr("This will save the Z-offset and restart Klipper to write the configuration. The "
              "printer will briefly disconnect."),
        ModalSeverity::Warning, lv_tr("Save"),
        [](lv_event_t*) { run_shared_save(); }, nullptr, nullptr);
}

void save_dirty_offsets(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                        ZOffsetCalibrationStrategy strategy, const helix::PrinterDiscovery& hw,
                        bool global_dirty, std::function<void()> on_success,
                        std::function<void(const std::string& error)> on_error, PrinterState* ps) {
    if (!api) {
        if (on_error) {
            on_error("No printer connection");
        }
        return;
    }

    auto& ts = helix::ToolState::instance();
    const std::vector<int> dirty_tools = ts.dirty_tool_z_indices();

    // Tool offsets first: on klipper-toolchanger SAVE_TOOL_PARAMETER only STAGES
    // a config change, which the machine-wide SAVE_CONFIG below then commits in
    // the same restart. Sending them after it would leave them staged until
    // some later save happened to flush them.
    for (int tool : dirty_tools) {
        const int microns =
            static_cast<int>(std::lround(ts.tool_z_offset_mm(tool) * 1000.0));
        const std::string gcode = helix::tool_offsets::save_tool_z_gcode(hw, tool, microns);
        if (gcode.empty()) {
            continue;
        }
        api->execute_gcode(gcode, nullptr, nullptr);
        // Marked here rather than in a callback: execute_gcode's error path only
        // logs, so there is no rejection signal to wait for. A later status
        // frame carrying a different value re-dirties the tool anyway.
        ts.mark_tool_z_saved(tool);
    }

    if (global_dirty) {
        // Commits its own change AND any tool parameters staged above.
        apply_and_save(api, save_watch, strategy, std::move(on_success), std::move(on_error), ps);
        return;
    }

    if (!dirty_tools.empty() && helix::tool_offsets::persist_requires_save_config(hw)) {
        // Nothing machine-wide to apply, but the staged tool parameters still
        // need committing — and that restarts Klipper, so the caller must be
        // told to expect it exactly as apply_and_save() would.
        api->execute_gcode("SAVE_CONFIG", nullptr, nullptr);
    }

    if (on_success) {
        on_success();
    }
}

} // namespace helix::zoffset
