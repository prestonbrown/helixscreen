// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_ripple_store.h"

#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix::calibration::ripple {

using json = nlohmann::json;

namespace {

const char* motor_name(MotorId m) {
    return m == MotorId::StepperX ? "stepper_x" : "stepper_y";
}

std::optional<MotorId> motor_from(const json& obj) {
    const json* v = obj.is_object() ? json_util::find_member(obj, "motor") : nullptr;
    if (!v || !v->is_string()) {
        return std::nullopt;
    }
    const auto s = v->get<std::string>();
    if (s == "stepper_x") {
        return MotorId::StepperX;
    }
    if (s == "stepper_y") {
        return MotorId::StepperY;
    }
    return std::nullopt;
}

json sweeps_to_json(const std::vector<MotorSweep>& sweeps) {
    json out = json::array();
    for (const auto& sweep : sweeps) {
        json speeds = json::array();
        for (const auto& s : sweep.speeds) {
            json r = json::array();
            for (const auto& v : s.ripple) {
                r.push_back(v ? json(*v) : json(nullptr));
            }
            speeds.push_back(
                {{"s", s.speed_mm_s}, {"r", r}, {"b", s.broadband}, {"f", s.frames_used}});
        }
        out.push_back({{"motor", motor_name(sweep.motor)}, {"speeds", speeds}});
    }
    return out;
}

std::optional<std::vector<MotorSweep>> sweeps_from_json(const json& j) {
    if (!j.is_array()) {
        return std::nullopt;
    }
    std::vector<MotorSweep> out;
    for (const auto& e : j) {
        if (!e.is_object()) {
            return std::nullopt;
        }
        const auto motor = motor_from(e);
        const auto* speeds = json_util::find_member(e, "speeds");
        if (!motor || !speeds || !speeds->is_array()) {
            return std::nullopt;
        }
        MotorSweep sweep;
        sweep.motor = *motor;
        for (const auto& s : *speeds) {
            const auto* r = s.is_object() ? json_util::find_member(s, "r") : nullptr;
            const auto speed = s.is_object() ? json_util::read_field<float>(s, "s") : std::nullopt;
            if (!r || !r->is_array() || r->size() != 3 || !speed) {
                return std::nullopt;
            }
            SpeedResult res;
            res.speed_mm_s = *speed;
            for (size_t k = 0; k < 3; ++k) {
                if (!(*r)[k].is_null()) {
                    const auto v = json_util::read_scalar<float>((*r)[k]);
                    if (!v) {
                        return std::nullopt;
                    }
                    res.ripple[k] = *v;
                }
            }
            res.broadband = json_util::safe_float(s, "b");
            res.frames_used = json_util::safe_int(s, "f");
            sweep.speeds.push_back(res);
        }
        out.push_back(std::move(sweep));
    }
    return out;
}

} // namespace

json ripple_result_to_json(const SweepResult& r) {
    json bands = json::array();
    for (const auto& b : r.bands) {
        bands.push_back({{"motor", motor_name(b.motor)},
                         {"lo", b.lo_mm_s},
                         {"hi", b.hi_mm_s},
                         {"harmonic", b.harmonic},
                         {"peak_speed", b.peak_speed_mm_s},
                         {"peak", b.peak_mm_s2},
                         {"freq", b.freq_hz}});
    }
    json skipped = json::array();
    for (const auto& s : r.skipped) {
        skipped.push_back({{"motor", motor_name(s.motor)}, {"reason", s.reason}});
    }
    return {{"version", kRippleSchemaVersion},
            {"sample_rate_hz", r.sample_rate_hz},
            {"ceiling_hz", r.ceiling_hz},
            {"unix_time", r.unix_time},
            {"sweeps", sweeps_to_json(r.sweeps)},
            {"compensated_sweeps", sweeps_to_json(r.compensated_sweeps)},
            {"bands", bands},
            {"skipped", skipped}};
}

std::optional<SweepResult> ripple_result_from_json(const json& j) {
    if (!j.is_object() || json_util::read_integer_field(j, "version") != kRippleSchemaVersion) {
        return std::nullopt;
    }
    SweepResult r;
    r.sample_rate_hz = json_util::safe_float(j, "sample_rate_hz");
    r.ceiling_hz = json_util::safe_float(j, "ceiling_hz");
    r.unix_time = json_util::safe_int64(j, "unix_time");
    const auto* sweeps = json_util::find_member(j, "sweeps");
    const auto* comp = json_util::find_member(j, "compensated_sweeps");
    const auto* bands = json_util::find_member(j, "bands");
    if (!sweeps || !bands || !bands->is_array()) {
        return std::nullopt;
    }
    auto s = sweeps_from_json(*sweeps);
    std::optional<std::vector<MotorSweep>> c = std::vector<MotorSweep>{};
    if (comp) {
        c = sweeps_from_json(*comp);
    }
    if (!s || !c) {
        return std::nullopt;
    }
    r.sweeps = std::move(*s);
    r.compensated_sweeps = std::move(*c);
    for (const auto& b : *bands) {
        const auto motor = motor_from(b);
        const auto lo = motor ? json_util::read_field<float>(b, "lo") : std::nullopt;
        const auto hi = motor ? json_util::read_field<float>(b, "hi") : std::nullopt;
        if (!motor || !lo || !hi) {
            return std::nullopt;
        }
        RippleBand band;
        band.motor = *motor;
        band.lo_mm_s = *lo;
        band.hi_mm_s = *hi;
        band.harmonic = json_util::safe_int(b, "harmonic", 1);
        band.peak_speed_mm_s = json_util::safe_float(b, "peak_speed");
        band.peak_mm_s2 = json_util::safe_float(b, "peak");
        band.freq_hz = json_util::safe_float(b, "freq");
        r.bands.push_back(band);
    }
    if (const auto* skipped = json_util::find_member(j, "skipped");
        skipped && skipped->is_array()) {
        for (const auto& e : *skipped) {
            const auto motor = e.is_object() ? motor_from(e) : std::nullopt;
            if (motor) {
                r.skipped.push_back({*motor, json_util::safe_string(e, "reason")});
            }
        }
    }
    return r;
}

RippleResultCache& RippleResultCache::instance() {
    static RippleResultCache cache;
    return cache;
}

void RippleResultCache::load(IMoonrakerAPI& api) {
    const uint64_t gen = ++generation_;
    api.database_get_item(
        kRippleDbNamespace, kRippleDbKey,
        lifetime_.bg_cb("RippleResultCache::load",
                        [this, gen](const json& value) {
                            if (gen != generation_) {
                                return;
                            }
                            result_ = ripple_result_from_json(value);
                            spdlog::debug("[RippleResult] Loaded: {}",
                                          result_ ? "a saved sweep" : "none");
                        }),
        lifetime_.bg_cb("RippleResultCache::load_none", [this, gen](const MoonrakerError& err) {
            spdlog::debug("[RippleResult] No saved sweep: {}", err.message);
            if (gen == generation_) {
                result_.reset();
            }
        }));
}

void RippleResultCache::save(IMoonrakerAPI& api, const SweepResult& result) {
    ++generation_;
    result_ = result;
    api.database_post_item(kRippleDbNamespace, kRippleDbKey, ripple_result_to_json(result), nullptr,
                           [](const MoonrakerError& err) {
                               spdlog::warn("[RippleResult] Could not save the sweep: {}",
                                            err.message);
                           });
}

void RippleResultCache::clear() {
    ++generation_;
    result_.reset();
}

std::vector<RippleBand> RippleResultCache::bands() const {
    return result_ ? result_->bands : std::vector<RippleBand>{};
}

} // namespace helix::calibration::ripple
