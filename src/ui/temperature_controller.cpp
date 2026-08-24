// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "temperature_controller.h"

#include "ui_error_reporting.h"
#include "ui_temperature_utils.h"

#include "filament_database.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>

#include "hv/json.hpp"

namespace helix {

TemperatureController::TemperatureController(PrinterState& state, IMoonrakerAPI* api)
    : state_(state), api_(api) {
    // Keypad ceilings mirror temperature_service.cpp keypad_range fields.
    model_[idx(HeaterType::Nozzle)].keypad_max_default = 350.0f;
    model_[idx(HeaterType::Bed)].keypad_max_default = 150.0f;
    model_[idx(HeaterType::Chamber)].keypad_max_default = 80.0f;

    refresh_presets();
}

/**
 * Chamber preset ladder, slot-indexed, in °C.
 *
 * THESE ARE ENCLOSURE TEMPERATURES, NOT MATERIAL-DERIVED VALUES. Do not "DRY
 * this up" by deriving them from the filament database like nozzle and bed.
 *
 * The filament database's chamber_temp_c is 0 for every open-frame material —
 * PLA=0, PETG=0, TPU=0, and only ABS=50. Deriving chamber presets from the
 * slot's material would therefore render Off / Off / 50 / Off and collapse
 * three of the four buttons into duplicates of "Off" on every enclosed printer.
 * That is a functional regression, so chamber deliberately keeps its own
 * generic low → high ladder that is independent of which material occupies the
 * slot. Slot N simply gets the Nth rung.
 *
 * The first three rungs preserve the long-standing 40/50/60 values so existing
 * behavior is unchanged; the fourth extends the ladder for the fourth slot.
 */
inline constexpr std::array<int, presets::PRESET_COUNT> CHAMBER_PRESET_LADDER_C{40, 50, 60, 70};

HeaterPresets compute_heater_presets(HeaterType type) {
    HeaterPresets out{};
    out.off = 0;

    for (int i = 0; i < presets::PRESET_COUNT; ++i) {
        if (type == HeaterType::Chamber) {
            out.material[i] = CHAMBER_PRESET_LADDER_C[i];
            continue;
        }

        // Nozzle and bed presets ARE material-derived: each slot's temperature
        // comes from the filament database entry for whatever material the user
        // assigned to that slot, so reassigning a slot moves its temps with it.
        // find_material() already folds in the user's MaterialSettingsManager
        // override, so a customized material carries its custom temps here.
        const std::string material = presets::name(i);
        auto info = filament::find_material(material);
        if (!info) {
            spdlog::warn("[TempController] Preset slot {} material '{}' not in filament database; "
                         "{} preset defaults to 0",
                         i, material, type == HeaterType::Nozzle ? "nozzle" : "bed");
            out.material[i] = 0;
            continue;
        }
        out.material[i] =
            (type == HeaterType::Nozzle) ? info->nozzle_recommended() : info->bed_temp;
    }
    return out;
}

void TemperatureController::refresh_presets() {
    for (int t = 0; t < HEATER_TYPE_COUNT; ++t) {
        model_[t].presets = compute_heater_presets(static_cast<HeaterType>(t));
    }
    spdlog::debug("[TempController] Presets refreshed for slots [{}, {}, {}, {}]", presets::name(0),
                  presets::name(1), presets::name(2), presets::name(3));
}

std::string TemperatureController::resolved_name(HeaterType type) const {
    switch (type) {
    case HeaterType::Nozzle:
        return state_.active_extruder_name();
    case HeaterType::Bed:
        return "heater_bed";
    case HeaterType::Chamber:
        return state_.temperature_state().chamber_heater_name();
    }
    return "";
}

int TemperatureController::configured_max(HeaterType type) const {
    return model_[idx(type)].configured_max;
}

void TemperatureController::set_configured_max(HeaterType type, int deg) {
    model_[idx(type)].configured_max = deg;
}

KeypadRange TemperatureController::keypad_range(HeaterType type) const {
    const auto& m = model_[idx(type)];
    return {m.keypad_min, heater_effective_max_deg(m.keypad_max_default, m.configured_max)};
}

void TemperatureController::ensure_limits(HeaterType type) {
    if (!api_ || model_[idx(type)].configured_max > 0) {
        return;
    }
    std::string section = resolved_name(type);
    if (section.empty()) {
        return;
    }
    // configfile.config section headers are lower-cased by Moonraker; lower-case
    // defensively to match regardless of how the discovery name was capitalised.
    std::transform(section.begin(), section.end(), section.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto tok = lifetime_.token();
    api_->query_configfile(
        [this, tok, type, section](const nlohmann::json& config) {
            // Background (WS) thread: parse only — no `this` member access.
            int max_deg = 0;
            if (config.contains(section)) {
                const auto& sec = config[section];
                if (sec.contains("max_temp")) {
                    const auto& mt = sec["max_temp"];
                    try {
                        if (mt.is_string()) {
                            max_deg = static_cast<int>(std::stof(mt.get<std::string>()));
                        } else if (mt.is_number()) {
                            max_deg = static_cast<int>(mt.get<double>());
                        }
                    } catch (const std::exception&) {
                        max_deg = 0;
                    }
                }
            }
            if (max_deg <= 0) {
                return; // no usable ceiling — keep the heater default
            }
            // Main thread: mutate state.
            tok.defer("TemperatureController::apply_max",
                      [this, type, max_deg]() { set_configured_max(type, max_deg); });
        },
        [](const MoonrakerError&) {});
}

const HeaterPresets& TemperatureController::presets(HeaterType type) const {
    return model_[idx(type)].presets;
}

bool TemperatureController::preset_visible(HeaterType type, int value_c) const {
    return heater_preset_visible(value_c, model_[idx(type)].configured_max);
}

void TemperatureController::set_target(HeaterType type, double celsius, SendOptions opts) {
    // Swap-preheat guard: when the caller signals "switching material" intent
    // (keep_previous_hot), never drop the nozzle below what's needed to purge the
    // previously loaded filament. Floor the requested target at the hotter of the
    // latched last-nonzero nozzle target and the current actual nozzle temperature.
    // Nozzle only — bed/chamber and any call without the flag are untouched, so
    // cooldown-to-0 and deliberate manual lowers still work.
    if (type == HeaterType::Nozzle && opts.keep_previous_hot) {
        const int actual_deci = lv_subject_get_int(state_.get_active_extruder_temp_subject());
        const double actual_deg =
            static_cast<double>(helix::ui::temperature::deci_to_degrees_f(actual_deci));
        const double latched =
            static_cast<double>(state_.get_active_extruder_last_nonzero_target());
        const double floor_deg = std::max({latched, actual_deg});
        if (celsius < floor_deg) {
            celsius = floor_deg;
            const int shown = static_cast<int>(std::lround(floor_deg));
            spdlog::info("[TemperatureController] Swap-preheat: holding nozzle at {}C to purge "
                         "previous filament (requested lower)",
                         shown);
            NOTIFY_INFO(lv_tr("Holding nozzle at {}°C to purge previous filament."), shown);
            // The holding toast above is now the sole nozzle message. Drop the
            // caller's success callback so it doesn't also fire a "target set to
            // {requested}°C" toast that contradicts the temp we actually held.
            opts.on_success = nullptr;
        }
    }

    const std::string name = resolved_name(type);
    if (name.empty()) {
        // Only the chamber resolves empty in practice (nozzle -> active extruder,
        // bed -> "heater_bed" are always present). Surface the not-found condition
        // only when the caller wants user-visible feedback; silent sends
        // (toast=false, e.g. AMS slot-preheat / cooldown) stay a clean no-op.
        // Mirrors the gcode-send error path: fire on_error if provided, then toast.
        if (opts.toast) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message =
                (type == HeaterType::Chamber) ? "Chamber heater not found" : "Heater not found";
            if (opts.on_error) {
                opts.on_error(err);
            }
            NOTIFY_ERROR("{}", lv_tr(err.message.c_str()));
        }
        return;
    }
    set_target(name, celsius, std::move(opts));
}

void TemperatureController::set_target(const std::string& klipper_name, double celsius,
                                       SendOptions opts) {
    if (!api_ || klipper_name.empty()) {
        return;
    }
    auto on_ok = [opts]() {
        if (opts.on_success)
            opts.on_success();
    };
    auto on_err = [opts](const MoonrakerError& e) {
        if (opts.on_error)
            opts.on_error(e);
        if (opts.toast) {
            NOTIFY_ERROR(lv_tr("Failed to set temperature: {}"), e.user_message());
        }
    };
    // opts.toast is the signal: on_err above raises NOTIFY_ERROR only when it is
    // set, so a toast=false send has an error callback that reaches no human.
    // Claiming otherwise would record the rejection for dedup and silence
    // GcodeErrorRouter's `!!` report — see include/rpc_error_policy.h.
    api_->set_temperature(klipper_name, celsius, std::move(on_ok), std::move(on_err),
                          /*caller_surfaces_errors=*/opts.toast);
}

void TemperatureController::apply_material(double nozzle, double bed, double chamber,
                                           SendOptions opts) {
    set_target(HeaterType::Nozzle, nozzle, opts);
    set_target(HeaterType::Bed, bed, opts);
    const std::string chamber_name = resolved_name(HeaterType::Chamber);
    if (chamber > 0 && !chamber_name.empty()) {
        set_target(chamber_name, chamber, opts);
    }
}

} // namespace helix
