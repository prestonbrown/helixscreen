// SPDX-License-Identifier: GPL-3.0-or-later
#include "runout_other_head.h"

#include "filament_mapper.h"
#include "filament_variants.h"
#include "i_moonraker_api.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fmt/format.h>

namespace helix::runout_other_head {

namespace {

const HeadFacts* find_head(const RunoutFacts& facts, int head) {
    for (const auto& h : facts.heads) {
        if (h.head == head) {
            return &h;
        }
    }
    return nullptr;
}

/// First colour as 0xRRGGBB, or nullopt when the record names none it can read.
std::optional<uint32_t> first_rgb(const FilamentRecord& r) {
    if (r.colors.empty() || r.colors.front().size() != 6) {
        return std::nullopt;
    }
    uint32_t rgb = 0;
    for (char c : r.colors.front()) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        const int v = std::isdigit(static_cast<unsigned char>(c))
                          ? c - '0'
                          : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
        rgb = (rgb << 4) | static_cast<uint32_t>(v);
    }
    return rgb;
}

bool same_nozzle_size(double a, double b) {
    return std::fabs(a - b) < 1e-6;
}

HeadVerdict head_verdict(const HeadFacts& h, const HeadFacts* runout,
                         const FilamentRecord& original) {
    HeadVerdict v{.head = h.head};
    if (runout == nullptr || !runout->nozzle_diameter || runout->nozzle_flow.empty() ||
        !h.nozzle_diameter || h.nozzle_flow.empty()) {
        v.refusal = HeadRefusal::NozzleUnknown;
    } else if (!same_nozzle_size(*h.nozzle_diameter, *runout->nozzle_diameter)) {
        v.refusal = HeadRefusal::NozzleSize;
    } else if (h.nozzle_flow != runout->nozzle_flow) {
        v.refusal = HeadRefusal::NozzleFlow;
    } else if (!h.loaded_by_feeder) {
        v.refusal = HeadRefusal::NotFedByFeeder;
    } else if (!h.record || h.record->type.empty()) {
        v.refusal = HeadRefusal::NoFilamentRecord;
    } else if (!filament::materials_compatible(h.record->type, original.type) ||
               !filament::grades_match(h.record->type, original.type)) {
        // The rest of the print runs at the temperatures sliced for the
        // original filament.
        v.refusal = HeadRefusal::MaterialMismatch;
    }
    if (v.eligible()) {
        const auto a = first_rgb(*h.record);
        const auto b = first_rgb(original);
        v.color_differs = h.record->colors.size() != original.colors.size() || !a || !b ||
                          !FilamentMapper::colors_match(*a, *b);
    }
    return v;
}

// --- Provider rows ---------------------------------------------------------

/// Snapmaker U1. The firmware takes no tool map while paused and has no
/// "resume on head N". Its Auto-Replenish (INNER_AUTO_REPLENISH_FILAMENT, the
/// command its own runout handler runs) is the only path that re-routes the
/// remaining tool changes: it looks for a head whose record (vendor, type,
/// subtype and colour record, compared exactly) matches the runout head's
/// backed-up record, re-maps every logical tool on the empty head to it, turns
/// the empty head off, records the switch for power-loss recovery, heats the
/// new head to the empty one's temperature and RESUMEs. SET_PRINT_FILAMENT_CONFIG
/// refreshes that backup, and the firmware accepts it while paused, so labelling
/// the empty head with the chosen head's record steers the search there.
/// Source: Snapmaker/u1-klipper klippy/extras/print_task_config.py
/// (cmd_SET_PRINT_FILAMENT_CONFIG, backup_filament_info,
/// cmd_INNER_AUTO_REPLENISH_FILAMENT).

bool safe_hex6(const std::string& s) {
    return s.size() == 6 && std::all_of(s.begin(), s.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) != 0;
           });
}

std::optional<std::string> u1_label_line(int empty_head, const FilamentRecord& r) {
    if (!IMoonrakerAPI::is_safe_text_param(r.vendor) || r.vendor.empty() ||
        !IMoonrakerAPI::is_safe_material_param(r.type) ||
        !IMoonrakerAPI::is_safe_material_param(r.subtype) || r.colors.empty() ||
        r.colors.size() > 5 || !std::all_of(r.colors.begin(), r.colors.end(), safe_hex6) ||
        r.alpha < 0 || r.alpha > 255 || r.multi_mode < 0 || r.multi_mode > 255) {
        return std::nullopt;
    }
    std::string colors;
    for (const auto& c : r.colors) {
        colors += colors.empty() ? c : "," + c;
    }
    // The multi-colour form reproduces the firmware's colour record field for
    // field, which its exact comparison needs. FORCE=1: the empty head may
    // still carry an official (RFID) record, which refuses an unforced write.
    const std::string vendor = r.vendor.find(' ') == std::string::npos
                                   ? r.vendor
                                   : IMoonrakerAPI::quoted_gcode_param_value(r.vendor);
    return fmt::format("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER={} VENDOR={} FILAMENT_TYPE={} "
                       "FILAMENT_SUBTYPE={} COLOR_NUMS={} COLORS={} MULTI_MODE={} ALPHA={} FORCE=1",
                       empty_head, vendor, IMoonrakerAPI::gcode_param_value(r.type),
                       IMoonrakerAPI::gcode_param_value(r.subtype), r.colors.size(), colors,
                       r.multi_mode, r.alpha);
}

ContinuePlan u1_plan(const RunoutFacts& facts, const std::vector<HeadVerdict>& verdicts,
                     int chosen) {
    ContinuePlan plan;
    const FilamentRecord& label = *find_head(facts, chosen)->record;
    const auto line = u1_label_line(facts.runout_head, label);
    if (!line) {
        plan.refusal = PlanRefusal::UnsafeRecord;
        return plan;
    }
    // The firmware walks heads in order and takes the first whose record
    // matches the label; any head holding the identical record is as good.
    plan.continues_on = chosen;
    for (const auto& v : verdicts) {
        const HeadFacts* h = find_head(facts, v.head);
        if (v.eligible() && h->record && *h->record == label) {
            plan.continues_on = v.head;
            break;
        }
    }
    // Auto-Replenish does nothing while switched off. The script runs in
    // order and the replenish returns once its RESUME has run, so a known-off
    // switch is put back straight after.
    const bool known_on = facts.auto_switch_enabled.value_or(false);
    plan.switches_auto_switch_on = !known_on;
    if (!known_on) {
        plan.gcode.emplace_back("SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=1");
    }
    plan.gcode.push_back(*line);
    plan.gcode.push_back(
        fmt::format("INNER_AUTO_REPLENISH_FILAMENT EXTRUDER={}", facts.runout_head));
    if (facts.auto_switch_enabled == false) {
        plan.gcode.emplace_back("SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=0");
    }
    return plan;
}

struct Provider {
    AmsType type;
    ContinuePlan (*plan)(const RunoutFacts&, const std::vector<HeadVerdict>&, int chosen);
};

// One row per filament system that can move a paused job onto another head.
// AFC and Happy Hare take a tool remap while paused and are the next rows.
constexpr Provider kProviders[] = {
    {AmsType::SNAPMAKER, &u1_plan},
};

const Provider* find_provider(AmsType type) {
    for (const auto& p : kProviders) {
        if (p.type == type) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

std::vector<HeadVerdict> assess(const RunoutFacts& facts) {
    std::vector<HeadVerdict> out;
    if (facts.runout_head < 0 || !facts.original) {
        return out;
    }
    const HeadFacts* runout = find_head(facts, facts.runout_head);
    for (const auto& h : facts.heads) {
        if (h.head == facts.runout_head) {
            out.push_back({.head = h.head, .refusal = HeadRefusal::IsRunoutHead});
            continue;
        }
        out.push_back(head_verdict(h, runout, *facts.original));
    }
    return out;
}

const char* refusal_text(HeadRefusal refusal) {
    switch (refusal) {
    case HeadRefusal::None:
        return "";
    case HeadRefusal::IsRunoutHead:
        return TR_NOOP("Ran out");
    case HeadRefusal::NozzleUnknown:
        return TR_NOOP("Nozzle not reported");
    case HeadRefusal::NozzleSize:
        return TR_NOOP("Different nozzle size");
    case HeadRefusal::NozzleFlow:
        return TR_NOOP("Different nozzle flow type");
    case HeadRefusal::NotFedByFeeder:
        return TR_NOOP("Not loaded by its feeder");
    case HeadRefusal::NoFilamentRecord:
        return TR_NOOP("Filament not identified");
    case HeadRefusal::MaterialMismatch:
        return TR_NOOP("Different material");
    }
    return "";
}

bool supported(AmsType type) {
    return find_provider(type) != nullptr;
}

ContinuePlan build_plan(AmsType type, const RunoutFacts& facts, int chosen_head) {
    ContinuePlan refused;
    const Provider* provider = find_provider(type);
    if (provider == nullptr) {
        refused.refusal = PlanRefusal::Unsupported;
        return refused;
    }
    if (!facts.paused) {
        refused.refusal = PlanRefusal::NotPaused;
        return refused;
    }
    if (facts.active_head != facts.runout_head) {
        refused.refusal = PlanRefusal::RunoutHeadNotActive;
        return refused;
    }
    const auto verdicts = assess(facts);
    const auto chosen =
        std::find_if(verdicts.begin(), verdicts.end(),
                     [chosen_head](const HeadVerdict& v) { return v.head == chosen_head; });
    if (chosen == verdicts.end() || !chosen->eligible()) {
        refused.refusal = PlanRefusal::HeadNotEligible;
        return refused;
    }
    ContinuePlan plan = provider->plan(facts, verdicts, chosen_head);
    if (plan.ok()) {
        spdlog::info("[RunoutOtherHead] runout head {} -> chosen {}, firmware continues on {}",
                     facts.runout_head, chosen_head, plan.continues_on);
    } else {
        plan.gcode.clear();
        plan.continues_on = -1;
    }
    return plan;
}

} // namespace helix::runout_other_head
