// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "temperature_controller.h"

#include "ui_error_reporting.h"
#include "ui_temperature_utils.h"

#include "chamber_heater_backend.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"
#include "spool_latch_gate.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>

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
        return state_.temperature_state().active_extruder_name();
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

float TemperatureController::effective_keypad_max(HeaterType type, float fallback_deg) {
    ensure_limits(type);
    const float cap = keypad_range(type).max;
    return cap > 0.0f ? cap : fallback_deg;
}

float TemperatureController::keypad_max_for(const std::string& klipper_name,
                                            float fallback_deg) const {
    if (api_) {
        const auto& limits = api_->get_safety_limits();
        if (limits.has_max_temp_for(klipper_name))
            return static_cast<float>(limits.max_temp_for(klipper_name));
    }
    return fallback_deg;
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
    section = helix::text_io::to_lower(section);

    auto tok = lifetime_.token();
    // Snapshot the backend ceiling before the query: the callback below runs
    // on the WebSocket thread, which must not read `this` members.
    const double conservative_max = conservative_chamber_max_;
    api_->query_configfile(
        [this, tok, type, section, conservative_max](const nlohmann::json& config) {
            // Background (WS) thread: parse only — no `this` member access.
            int max_deg = 0;
            if (config.contains(section)) {
                const auto& sec = config[section];
                if (sec.contains("max_temp")) {
                    const auto& mt = sec["max_temp"];
                    if (mt.is_string()) {
                        const auto parsed =
                            helix::text_io::parse_leading<float>(mt.get_ref<const std::string&>());
                        if (parsed) {
                            max_deg = static_cast<int>(*parsed);
                        }
                    } else if (mt.is_number()) {
                        max_deg = static_cast<int>(mt.get<double>());
                    }
                }
            }
            if (max_deg <= 0) {
                // No configfile ceiling. Appliance backends carry a
                // conservative cap (e.g. stock chamber limit 60); generic has
                // none (0) and keeps the heater default.
                if (type == HeaterType::Chamber && conservative_max > 0) {
                    max_deg = static_cast<int>(conservative_max);
                } else {
                    return; // no usable ceiling — keep the heater default
                }
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
        const int actual_deci =
            lv_subject_get_int(state_.temperature_state().get_active_extruder_temp_subject());
        const double actual_deg =
            static_cast<double>(helix::ui::temperature::deci_to_degrees_f(actual_deci));
        const double latched = static_cast<double>(
            state_.temperature_state().get_active_extruder_last_nonzero_target());
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
    // Callers' callbacks toast, which is main-thread only; the printer's answer
    // arrives on the WebSocket thread.
    auto on_ok = [opts]() {
        if (opts.on_success)
            helix::ui::run_on_main("TemperatureController::on_success", opts.on_success);
    };
    auto on_err = [opts](const MoonrakerError& e) {
        if (opts.on_error)
            helix::ui::run_on_main("TemperatureController::on_error",
                                   [cb = opts.on_error, e]() { cb(e); });
        if (opts.toast) {
            helix::ui::notify_error_tr(TR_NOOP("Failed to set temperature: {}"), e);
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

void TemperatureController::set_chamber_actions(std::string reset_gcode, std::string filter_fan_pin,
                                                double conservative_max) {
    chamber_reset_gcode_ = std::move(reset_gcode);
    chamber_filter_fan_pin_ = std::move(filter_fan_pin);
    conservative_chamber_max_ = conservative_max;
}

void TemperatureController::reset_chamber_fault() {
    if (api_ && !chamber_reset_gcode_.empty()) {
        spdlog::info("[TemperatureController] Chamber fault reset");
        api_->execute_gcode(chamber_reset_gcode_, nullptr, nullptr);
    }
}

void TemperatureController::set_chamber_dryer(const chamber::ChamberHeaterBackend* backend,
                                              bool has_heated_bed) {
    chamber_dryer_backend_ = backend;
    chamber_dryer_has_bed_ = has_heated_bed;
    // The cycle can end on the appliance's side (its timer, its own button),
    // so the bed assist follows the reported state rather than our commands.
    if (!dryer_active_observer_ && chamber_dryer().supported) {
        dryer_active_observer_ = ui::observe<int>(
            state_.temperature_state().get_chamber_dryer_active_subject(), this,
            [](TemperatureController* self, int active) {
                self->on_chamber_dryer_active(active != 0);
            },
            state_.get_subjects_lifetime());
    }
}

DryerInfo TemperatureController::chamber_dryer() const {
    return chamber_dryer_backend_ ? chamber_dryer_backend_->dryer_capabilities() : DryerInfo{};
}

// Spools may sit on the bed during a run, and plastic spool flanges soften
// around 60-70 C, so the assist stays at 70 C; a bed that cannot reach it gets
// its own ceiling.
int TemperatureController::chamber_dryer_bed_assist_c() const {
    if (!chamber_dryer_has_bed_) {
        return 0;
    }
    constexpr int kBedAssistC = 70;
    const int bed_max = static_cast<int>(keypad_range(HeaterType::Bed).max);
    return bed_max > 0 ? std::min(kBedAssistC, bed_max) : kBedAssistC;
}

void TemperatureController::start_chamber_drying(float temp_c, int duration_min, bool heat_bed,
                                                 bool hold_idle) {
    const DryerInfo dryer = chamber_dryer();
    if (!api_ || !dryer.supported) {
        return;
    }
    lv_subject_t* job = state_.print_state().get_job_holds_machine_subject();
    const bool job_active = job && lv_subject_get_int(job) != 0;
    if (job_active && !dryer.allows_during_print) {
        spdlog::info("[TemperatureController] Chamber drying refused: a job holds the machine");
        return;
    }
    const std::string gcode = chamber_dryer_backend_->dryer_start_gcode(
        dryer.clamp_temp(temp_c), dryer.clamp_duration(duration_min));
    spdlog::info("[TemperatureController] Chamber drying start: {}", gcode);
    const uint32_t run_id = ++next_dry_run_id_;
    dry_run_ = {true, false, 0, 0, run_id};
    auto tok = lifetime_.token();
    api_->execute_gcode(gcode, nullptr, [this, tok, run_id](const MoonrakerError&) {
        if (tok.expired()) {
            return;
        }
        tok.defer("TemperatureController::drying_refused", [this, run_id]() {
            if (dry_run_.id == run_id) {
                end_dry_run("drying start refused");
            }
        });
    });
    if (hold_idle) {
        hold_idle_timeout(run_id, dryer.clamp_duration(duration_min) * 60);
    }

    const int bed_c = chamber_dryer_bed_assist_c();
    if (!heat_bed || bed_c <= 0) {
        return;
    }
    if (job_active) {
        spdlog::info("[TemperatureController] Bed assist refused: a job holds the machine");
        return;
    }
    dry_run_.bed_c = bed_c;
    set_target(HeaterType::Bed, bed_c);
}

// A dry run moves nothing, so Klipper's idle_timeout would fire mid-run and run
// its gcode, TURN_OFF_HEATERS on most printers, zeroing the bed assist and the
// appliance's own heater. Hold it off for the run plus a margin, but only once
// the configured value is known: without it there is nothing to restore, so
// nothing is held.
void TemperatureController::hold_idle_timeout(uint32_t run_id, int run_s) {
    constexpr int kMarginS = 30 * 60;
    read_configured_idle_timeout([this, run_id, run_s](int configured_s) {
        if (!dry_run_.armed || dry_run_.id != run_id || !api_) {
            return;
        }
        dry_run_.idle_restore_s = configured_s;
        api_->execute_gcode(fmt::format("SET_IDLE_TIMEOUT TIMEOUT={}", run_s + kMarginS), nullptr,
                            nullptr);
    });
}

void TemperatureController::read_configured_idle_timeout(std::function<void(int)> on_main) {
    if (!api_) {
        return;
    }
    constexpr int kKlipperDefaultS = 600;
    auto tok = lifetime_.token();
    api_->query_configfile(
        [tok, on_main = std::move(on_main)](const nlohmann::json& config) {
            // Background (WS) thread: parse only.
            int configured_s = kKlipperDefaultS;
            if (config.contains("idle_timeout") && config["idle_timeout"].contains("timeout")) {
                const auto& t = config["idle_timeout"]["timeout"];
                if (t.is_string()) {
                    const auto parsed =
                        helix::text_io::parse_leading<float>(t.get_ref<const std::string&>());
                    if (!parsed) {
                        return;
                    }
                    configured_s = static_cast<int>(*parsed);
                } else if (t.is_number()) {
                    configured_s = static_cast<int>(t.get<double>());
                }
            }
            if (configured_s <= 0) {
                return;
            }
            tok.defer("TemperatureController::read_configured_idle_timeout",
                      [on_main, configured_s]() { on_main(configured_s); });
        },
        [](const MoonrakerError&) {});
}

std::vector<std::string> TemperatureController::chamber_dryer_tokens() const {
    std::vector<std::string> tokens;
    if (!chamber_dryer_backend_ || !chamber_dryer().supported) {
        return tokens;
    }
    const DryerInfo dryer = chamber_dryer();
    for (const std::string& script :
         {chamber_dryer_backend_->dryer_start_gcode(dryer.clamp_temp(dryer.min_temp_c),
                                                    dryer.clamp_duration(60)),
          std::string(chamber_dryer_backend_->dryer_stop_gcode())}) {
        for (std::string& token : gcode_line_tokens(script)) {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

void TemperatureController::stop_chamber_drying() {
    if (!api_ || !chamber_dryer().supported) {
        return;
    }
    spdlog::info("[TemperatureController] Chamber drying stop");
    api_->execute_gcode(std::string(chamber_dryer_backend_->dryer_stop_gcode()), nullptr, nullptr);
    end_dry_run("drying stopped");
}

void TemperatureController::on_chamber_dryer_active(bool running) {
    if (!dry_run_.armed) {
        return;
    }
    if (running) {
        dry_run_.seen_running = true;
    } else if (dry_run_.seen_running) {
        end_dry_run("drying cycle ended");
    }
}

// A print that started since owns both the bed and the idle timeout, so neither
// is touched then. Otherwise the held timeout goes back to its configured value,
// and the bed goes off only while it is still ours: a different target someone
// set by hand owns it now. A target still reading 0 is ours too: the confirming
// frame may not have arrived yet, and an off sent to a cold bed costs nothing.
void TemperatureController::end_dry_run(const char* why) {
    if (!dry_run_.armed) {
        return;
    }
    const int target_c = dry_run_.bed_c;
    const int restore_s = dry_run_.idle_restore_s;
    dry_run_ = {};
    lv_subject_t* job = state_.print_state().get_job_holds_machine_subject();
    if (job && lv_subject_get_int(job) != 0) {
        spdlog::info("[TemperatureController] Drying ended ({}): a job owns the machine", why);
        return;
    }
    if (restore_s > 0 && api_) {
        api_->execute_gcode(fmt::format("SET_IDLE_TIMEOUT TIMEOUT={}", restore_s), nullptr,
                            nullptr);
    }
    if (target_c <= 0) {
        return;
    }
    lv_subject_t* bed_target = state_.temperature_state().get_bed_target_subject();
    const int bed_target_deci = bed_target ? lv_subject_get_int(bed_target) : 0;
    if (bed_target_deci != 0 && bed_target_deci != target_c * 10) {
        spdlog::info("[TemperatureController] Bed assist ended ({}): target changed since", why);
        return;
    }
    spdlog::info("[TemperatureController] Bed assist off ({})", why);
    set_target(HeaterType::Bed, 0);
}

void TemperatureController::set_chamber_filter_fan(bool on) {
    if (api_ && !chamber_filter_fan_pin_.empty()) {
        // Bare pin name — SET_PIN takes the object name, not the prefixed
        // type ("output_pin dragonbreath_filter" → "dragonbreath_filter").
        // find(' ') == npos for an already-bare name yields substr(0): the
        // whole string, so both spellings work.
        const std::string pin =
            chamber_filter_fan_pin_.substr(chamber_filter_fan_pin_.find(' ') + 1);
        spdlog::info("[TemperatureController] Chamber filter fan {}", on ? "on" : "off");
        api_->execute_gcode(fmt::format("SET_PIN PIN={} VALUE={}", pin, on ? 1 : 0), nullptr,
                            nullptr);
    }
}

} // namespace helix
