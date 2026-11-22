// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_catalog.h"

#include "printer_profile_state.h"
#include "translation_loader.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace helix::calibration {

namespace {

/// One row per step: its facts on a printer with no database tier for it.
/// Row order is the run order.
struct StepRow {
    StepKind kind;
    const char* id;
    const char* name;
    Tier default_tier;
    int minutes;
    bool heats;
    bool loud;
    bool moves;
    bool needs_hands;
    PrereqSet prereqs;
};

// clang-format off
constexpr std::array<StepRow, kStepCount> kSteps{{
    {StepKind::BeltTension, "belt_tension", TR_NOOP("Belt tension"), Tier::Recommended, 3,
     false, true, true, false, kPrereqBedClear},
    {StepKind::CutterPosition, "cutter", TR_NOOP("Cutter position"), Tier::Recommended, 2,
     false, false, true, false, kPrereqBedClear},
    {StepKind::ZOffset, "z_offset", TR_NOOP("Z offset"), Tier::Recommended, 5,
     false, false, true, false, kPrereqBedClear | kPrereqBuildPlate},
    {StepKind::Heaters, "heaters", TR_NOOP("Heaters"), Tier::Recommended, 10,
     true, false, false, false, kPrereqBuildPlate},
    {StepKind::BedMesh, "bed_mesh", TR_NOOP("Bed mesh"), Tier::Recommended, 5,
     false, false, true, false, kPrereqBedClear | kPrereqBuildPlate},
    {StepKind::InputShaper, "input_shaper", TR_NOOP("Input shaper"), Tier::Recommended, 5,
     false, true, true, false, kPrereqBedClear | kPrereqAccelerometer},
    {StepKind::PressureAdvance, "pressure_advance", TR_NOOP("Pressure advance"), Tier::Optional, 10,
     true, false, true, false, kPrereqBedClear | kPrereqBuildPlate | kPrereqFilamentLoaded},
    {StepKind::BeltCompare, "belt_compare", TR_NOOP("Belt compare"), Tier::Optional, 5,
     false, true, true, false, kPrereqBedClear | kPrereqAccelerometer},
    {StepKind::BedScrews, "bed_screws", TR_NOOP("Bed screws"), Tier::Optional, 5,
     false, false, true, true, kPrereqBedClear},
    {StepKind::ToolOffsets, "tool_offsets", TR_NOOP("Tool offsets"), Tier::Recommended, 10,
     false, false, true, false, kPrereqBedClear},
    {StepKind::VerticalArtifact, "vertical_artifact", TR_NOOP("Vertical artifact compensation"),
     Tier::OnRequest, 10, false, true, true, false, kPrereqBedClear},
    {StepKind::PressureAdvanceTuning, "pa_tuning", TR_NOOP("Pressure advance tuning"),
     Tier::OnRequest, 15, true, false, true, true,
     kPrereqBedClear | kPrereqBuildPlate | kPrereqFilamentLoaded},
    {StepKind::VibrationCheck, "vibration_check", TR_NOOP("Vertical artifact check"),
     Tier::OnRequest, 10, false, true, true, false, kPrereqBedClear | kPrereqAccelerometer},
}};
// clang-format on

/// Mirrors the Calibration rows' capability conditions.
bool applies(StepKind kind, const CatalogInputs& in) {
    switch (kind) {
    case StepKind::BeltTension:
        return in.can_tension_belts;
    case StepKind::CutterPosition:
        return in.can_calibrate_cutter;
    case StepKind::ZOffset:
        return in.has_probe && !in.z_offset_reapplied_per_head && !in.probe_sets_nozzle_height &&
               !(in.has_tool_offset_cal && in.hide_manual_z_calibration);
    case StepKind::Heaters:
    case StepKind::PressureAdvanceTuning:
        return true;
    case StepKind::BedMesh:
        return in.has_bed_mesh;
    case StepKind::InputShaper:
        return in.has_accelerometer;
    case StepKind::PressureAdvance:
        return in.has_pa_cal;
    case StepKind::BeltCompare:
        return in.has_accelerometer && in.supports_belt_compare;
    case StepKind::BedScrews:
        return in.has_screws_tilt;
    case StepKind::ToolOffsets:
        return in.has_tool_offset_cal;
    case StepKind::VerticalArtifact:
        return in.has_stepper_resonance_cal;
    case StepKind::VibrationCheck:
        return in.supports_ripple_check;
    }
    return false;
}

/// The paper test: the owner slides paper under the nozzle while Z steps down.
bool z_offset_is_paper_test(const CatalogInputs& in) {
    return in.z_strategy != ZOffsetCalibrationStrategy::FIRMWARE_MANAGED &&
           !in.nozzle_contact_probe;
}

bool heaters_all_tuned(const CatalogInputs& in) {
    return in.nozzle_tuned && (!in.has_heated_bed || in.bed_tuned);
}

BedTuning heaters_bed_tuning(const CatalogInputs& in) {
    if (!in.has_heated_bed) {
        return BedTuning::None;
    }
    return in.bed_tuned ? BedTuning::Optional : BedTuning::Required;
}

bool known_step_id(const std::string& id) {
    return std::any_of(kSteps.begin(), kSteps.end(), [&](const StepRow& r) { return id == r.id; });
}

} // namespace

const std::array<const char*, kStepCount>& all_step_ids() {
    static const std::array<const char*, kStepCount> ids = [] {
        std::array<const char*, kStepCount> out{};
        for (size_t i = 0; i < kSteps.size(); ++i) {
            out[i] = kSteps[i].id;
        }
        return out;
    }();
    return ids;
}

std::optional<Tier> parse_tier(std::string_view text) {
    for (Tier t : {Tier::Required, Tier::Recommended, Tier::Optional, Tier::OnRequest}) {
        if (text == tier_id(t)) {
            return t;
        }
    }
    return std::nullopt;
}

const char* tier_id(Tier tier) {
    switch (tier) {
    case Tier::Required:
        return "required";
    case Tier::Recommended:
        return "recommended";
    case Tier::Optional:
        return "optional";
    case Tier::OnRequest:
        return "on_request";
    }
    return "";
}

std::vector<StepInfo> applicable_steps(const CatalogInputs& in) {
    std::vector<StepInfo> out;
    int order = 0;
    for (const StepRow& row : kSteps) {
        ++order;
        if (!applies(row.kind, in)) {
            continue;
        }
        StepInfo s;
        s.kind = row.kind;
        s.id = row.id;
        s.name = row.name;
        s.tier = row.default_tier;
        s.minutes = row.minutes;
        s.heats = row.heats;
        s.loud = row.loud;
        s.moves = row.moves;
        s.needs_hands = row.needs_hands;
        s.prereqs = row.prereqs;
        s.order = order;
        if (row.kind == StepKind::ZOffset && z_offset_is_paper_test(in)) {
            s.needs_hands = true;
            s.prereqs |= kPrereqPaper;
        }
        for (const auto& [id, tier] : in.tier_overrides) {
            if (id == row.id) {
                s.tier = tier;
            }
        }
        if (row.kind == StepKind::Heaters) {
            // Tuning that works needs no new run; only a heater with none does.
            if (heaters_all_tuned(in)) {
                s.tier = Tier::Optional;
            }
            s.bed = heaters_bed_tuning(in);
            s.tunes_bed = s.bed == BedTuning::Required;
        }
        out.push_back(s);
    }
    return out;
}

PrereqSet prerequisites_for(const std::vector<StepInfo>& steps) {
    PrereqSet set = kPrereqNone;
    for (const auto& s : steps) {
        set |= s.prereqs;
    }
    return set;
}

namespace {

/// A PID cycle swings the heater a few degrees either side of its target, so
/// it lasts as long as the heater is slow to move, and a bed's mass lags far
/// more than a nozzle's. Seconds of cycle per second-per-degree of climb:
/// a 0.25 s/°C nozzle cycles in about 30 s, a 1 s/°C bed (AD5M Pro) in
/// about 3 min, a 2 s/°C 350 mm bed (K2 Plus) in about 6 min.
constexpr float kNozzleCycleSecondsPerRate = 120.0f;
constexpr float kBedCycleSecondsPerRate = 180.0f;
/// Mainline Klipper's PID_CALIBRATE runs a fixed number of cycles.
constexpr int kFixedPidCycles = 5;
/// Kalico's runs until its last cycles agree within TOLERANCE. A large bed's
/// spread roughly halves each cycle (0.70 at the fourth sample, 0.30 at the
/// fifth), so it meets its 0.15 around the sixth; a nozzle at 0.02 settles in
/// about as many of its short cycles.
constexpr int kTolerancePidCycles = 6;
/// MPC_CALIBRATE holds the heater at its target while it measures heat loss.
constexpr float kMpcMeasureSeconds = 240.0f;

} // namespace

float heater_calibration_seconds(const HeaterPricing& heater) {
    const float heat = std::max(0.0f, heater.heat_seconds);
    if (heater.mpc) {
        return heat + kMpcMeasureSeconds;
    }
    const float per_rate = heater.bed ? kBedCycleSecondsPerRate : kNozzleCycleSecondsPerRate;
    const int cycles = heater.tolerance ? kTolerancePidCycles : kFixedPidCycles;
    return heat + per_rate * std::max(0.0f, heater.climb_rate) * static_cast<float>(cycles);
}

int heaters_step_minutes(const std::vector<HeaterPricing>& heaters) {
    if (heaters.empty()) {
        return 0;
    }
    float seconds = 0.0f;
    for (const auto& h : heaters) {
        seconds += heater_calibration_seconds(h);
    }
    return std::max(1, static_cast<int>(std::ceil(seconds / 60.0f)));
}

void price_heaters_step(StepInfo& step, const std::vector<HeaterPricing>& heaters) {
    std::vector<HeaterPricing> nozzle;
    for (const auto& h : heaters) {
        if (!h.bed) {
            nozzle.push_back(h);
        }
    }
    const int nozzle_minutes = heaters_step_minutes(nozzle);
    step.bed_minutes = std::max(0, heaters_step_minutes(heaters) - nozzle_minutes);
    step.minutes = nozzle_minutes + (step.tunes_bed ? step.bed_minutes : 0);
}

int total_minutes(const std::vector<StepInfo>& steps) {
    int total = 0;
    for (const auto& s : steps) {
        total += s.minutes;
    }
    return total;
}

int hands_on_minutes(const std::vector<StepInfo>& steps) {
    int total = 0;
    for (const auto& s : steps) {
        if (s.needs_hands) {
            total += s.minutes;
        }
    }
    return total;
}

std::vector<TierOverride>
tier_overrides_from(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::vector<TierOverride> out;
    for (const auto& [id, text] : entries) {
        const auto tier = parse_tier(text);
        if (tier && known_step_id(id)) {
            out.emplace_back(id, *tier);
        }
    }
    return out;
}

} // namespace helix::calibration
