// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_catalog.h"

#include "printer_profile_state.h"
#include "translation_loader.h"

#include <algorithm>
#include <array>

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
        return in.has_probe && !in.z_offset_reapplied_per_head &&
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
