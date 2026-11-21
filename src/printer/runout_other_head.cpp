// SPDX-License-Identifier: GPL-3.0-or-later
#include "runout_other_head.h"

#include "config.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "filament_variants.h"
#include "i_moonraker_api.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>

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

/// The filament database knows the material, as written or as its base polymer.
bool known_material(const std::string& type) {
    return filament::find_material(type).has_value() ||
           filament::find_material(filament::extract_base_material(type)).has_value();
}

/// The routing moved the rest of the print off the runout head: no logical tool
/// is routed to it any more.
bool routing_left_runout_head(const RunoutFacts& facts) {
    return !facts.routing.empty() &&
           std::none_of(facts.routing.begin(), facts.routing.end(),
                        [&facts](int head) { return head == facts.runout_head; });
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
    } else if (!h.record || h.record->vendor.empty() || h.record->type.empty() ||
               h.record->subtype.empty() || h.record->colors.empty()) {
        // A firmware matches the whole record, so a partial one cannot be
        // written onto the empty head.
        v.refusal = HeadRefusal::NoFilamentRecord;
    } else if (!known_material(h.record->type) || !known_material(original.type)) {
        // The shared compatibility rule passes an unknown material against
        // anything; the switch heats the new head to the original's
        // temperature, so here nothing known is nothing proven.
        v.refusal = HeadRefusal::MaterialUnknown;
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
        // The firmware heats the new head to the runout head's last
        // temperature, which a variant (Silk 230, Matte 215) does not share.
        v.variant_differs =
            h.record->type != original.type || h.record->subtype != original.subtype;
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

/// The record the firmware's own clear writes when a head runs out or is
/// emptied: no vendor, type or subtype, one white color. A head carrying it is
/// never an Auto-Replenish candidate.
FilamentRecord u1_no_filament() {
    return FilamentRecord{.vendor = "NONE",
                          .type = "NONE",
                          .subtype = "NONE",
                          .colors = {"FFFFFF"},
                          .alpha = 0xFF,
                          .multi_mode = 0};
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
    // The vendor is free text: quoted always, because an unquoted apostrophe
    // opens a quoted string in the firmware's parameter split and swallows
    // every parameter after it.
    const std::string vendor = IMoonrakerAPI::quoted_gcode_param_value(r.vendor);
    return fmt::format("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER={} VENDOR={} FILAMENT_TYPE={} "
                       "FILAMENT_SUBTYPE={} COLOR_NUMS={} COLORS={} MULTI_MODE={} ALPHA={} FORCE=1",
                       empty_head, vendor, IMoonrakerAPI::gcode_param_value(r.type),
                       IMoonrakerAPI::gcode_param_value(r.subtype), r.colors.size(), colors,
                       r.multi_mode, r.alpha);
}

bool u1_record_writable(const HeadFacts& h) {
    return h.record && u1_label_line(0, *h.record).has_value();
}

/// The resume step of the firmware's own Auto-Replenish, on @p head.
std::string u1_resume_on(int head) {
    return fmt::format("RESUME REPLENISH=1 REPLENISH_EXTRUDER={}", head);
}

ContinuePlan u1_plan(const RunoutFacts& facts, const std::vector<HeadVerdict>& /*verdicts*/,
                     int chosen) {
    ContinuePlan plan;
    // Turning Auto-Replenish on with nothing known to restore would leave it on.
    if (!facts.auto_switch_enabled) {
        plan.refusal = PlanRefusal::AutoSwitchUnknown;
        return plan;
    }
    const FilamentRecord& label = *find_head(facts, chosen)->record;
    const auto line = u1_label_line(facts.runout_head, label);
    if (!line) {
        plan.refusal = PlanRefusal::UnsafeRecord;
        return plan;
    }
    // The firmware walks the heads in index order and takes the first that its
    // feeder loaded, whose nozzle matches and whose record matches the label
    // exactly. It knows every nozzle; a head whose nozzle is not reported here
    // is assumed to match, and only a nozzle known to differ is passed over.
    const HeadFacts* runout = find_head(facts, facts.runout_head);
    plan.continues_on = chosen;
    for (const auto& h : facts.heads) {
        if (h.head == facts.runout_head || !h.loaded_by_feeder || !h.record ||
            !(*h.record == label)) {
            continue;
        }
        const bool size_differs = runout != nullptr && runout->nozzle_diameter &&
                                  h.nozzle_diameter &&
                                  !same_nozzle_size(*h.nozzle_diameter, *runout->nozzle_diameter);
        const bool flow_differs = runout != nullptr && !runout->nozzle_flow.empty() &&
                                  !h.nozzle_flow.empty() && h.nozzle_flow != runout->nozzle_flow;
        if (!size_differs && !flow_differs) {
            plan.continues_on = h.head;
            break;
        }
    }
    // Auto-Replenish does nothing while switched off. The script runs in
    // order and the replenish returns once its RESUME has run, so the switch
    // is put back straight after; restore_gcode covers a script that stopped.
    plan.switches_auto_switch_on = !*facts.auto_switch_enabled;
    if (plan.switches_auto_switch_on) {
        plan.gcode.emplace_back("SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=1");
        plan.restore_gcode = "SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=0";
    }
    plan.gcode.push_back(*line);
    plan.gcode.push_back(
        fmt::format("INNER_AUTO_REPLENISH_FILAMENT EXTRUDER={}", facts.runout_head));
    if (!plan.restore_gcode.empty()) {
        plan.gcode.push_back(plan.restore_gcode);
    }

    // The label stays on the empty head after the switch, and the firmware
    // replaces it only on an official RFID read or an edit: a tagless spool
    // loaded there later would print as the borrowed filament. The firmware's
    // own clear shape takes it out of every later match. The write accepts any
    // print state, and it leaves the backup the firmware compares alone.
    const std::string none_line = *u1_label_line(facts.runout_head, u1_no_filament());
    plan.clear_label_gcode = none_line;
    // Undoing a label that did not switch: writing the original back refreshes
    // the backup a later Resume or replenish compares; the head's own record
    // then goes back to what it read before (the firmware cleared it at the
    // runout, so usually no filament).
    const FilamentRecord before =
        runout != nullptr && runout->record ? *runout->record : u1_no_filament();
    if (const auto original_line = u1_label_line(facts.runout_head, *facts.original)) {
        plan.unlabel_gcode = *original_line + "\n";
    }
    plan.unlabel_gcode += u1_label_line(facts.runout_head, before).value_or(none_line);
    // What the firmware's Auto-Replenish runs once it has moved the routing:
    // the resume step alone, on the new head.
    plan.resume_gcode = u1_resume_on(plan.continues_on);
    return plan;
}

struct Provider {
    AmsType type;
    ContinuePlan (*plan)(const RunoutFacts&, const std::vector<HeadVerdict>&, int chosen);
    /// Whether the plan can use this head's record; null when every record will do.
    bool (*record_usable)(const HeadFacts&);
    /// What resumes a print the routing already moved onto a head.
    std::string (*resume_on)(int head);
};

// One row per filament system that can move a paused job onto another head.
// AFC and Happy Hare take a tool remap while paused and are the next rows.
constexpr Provider kProviders[] = {
    // VENDOR_OK: this capability's provider table, one row per filament system
    {AmsType::SNAPMAKER, &u1_plan, &u1_record_writable, &u1_resume_on},
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
    // What refuses every other head at once, most fundamental first.
    HeadRefusal all = HeadRefusal::None;
    if (facts.printer_busy) {
        all = HeadRefusal::PrinterBusy;
    } else if (routing_left_runout_head(facts)) {
        all = HeadRefusal::AlreadyMoved;
    } else if (facts.active_head != facts.runout_head) {
        all = HeadRefusal::RunoutHeadNotActive;
    } else if (runout != nullptr && runout->feeder_can_supply) {
        // Filament still at the runout head's feeder (a jam, a tangle): the
        // firmware resumes on that head and never looks at another.
        all = HeadRefusal::RunoutHeadStillFeeds;
    }
    for (const auto& h : facts.heads) {
        if (h.head == facts.runout_head) {
            out.push_back({.head = h.head, .refusal = HeadRefusal::IsRunoutHead});
        } else if (all != HeadRefusal::None) {
            out.push_back({.head = h.head, .refusal = all});
        } else {
            out.push_back(head_verdict(h, runout, *facts.original));
        }
    }
    return out;
}

std::vector<HeadVerdict> assess(AmsType type, const RunoutFacts& facts) {
    auto out = assess(facts);
    const Provider* provider = find_provider(type);
    if (provider == nullptr || provider->record_usable == nullptr) {
        return out;
    }
    for (auto& v : out) {
        const HeadFacts* h = find_head(facts, v.head);
        if (v.eligible() && h != nullptr && !provider->record_usable(*h)) {
            v = {.head = v.head, .refusal = HeadRefusal::RecordNotWritable};
        }
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
    case HeadRefusal::RunoutHeadStillFeeds:
        return TR_NOOP("Filament is still at the empty head's feeder");
    case HeadRefusal::NoFilamentRecord:
        return TR_NOOP("Filament not identified");
    case HeadRefusal::MaterialMismatch:
        return TR_NOOP("Different material");
    case HeadRefusal::MaterialUnknown:
        return TR_NOOP("Material not recognized");
    case HeadRefusal::PrinterBusy:
        return TR_NOOP("The printer is already resuming the print");
    case HeadRefusal::AlreadyMoved:
        return TR_NOOP("The print already moved to another toolhead");
    case HeadRefusal::RunoutHeadNotActive:
        return TR_NOOP("Another toolhead is active");
    case HeadRefusal::RecordNotWritable:
        return TR_NOOP("Its filament record cannot be copied");
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
    if (facts.printer_busy || routing_left_runout_head(facts)) {
        refused.refusal = PlanRefusal::PrinterBusy;
        return refused;
    }
    if (facts.active_head != facts.runout_head) {
        refused.refusal = PlanRefusal::RunoutHeadNotActive;
        return refused;
    }
    const auto verdicts = assess(type, facts);
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

int moved_onto(const RunoutFacts& before, const RunoutFacts& now) {
    if (before.routing.empty() || now.routing.size() != before.routing.size()) {
        return -1;
    }
    int onto = -1;
    for (size_t i = 0; i < before.routing.size(); ++i) {
        if (before.routing[i] != before.runout_head) {
            continue;
        }
        const int head = now.routing[i];
        if (head == before.runout_head || (onto >= 0 && head != onto)) {
            return -1;
        }
        onto = head;
    }
    return onto;
}

bool moved_to(const RunoutFacts& before, const RunoutFacts& now, int target) {
    return target >= 0 && moved_onto(before, now) == target;
}

SwitchStep judge_switch(const RunoutFacts& before, const RunoutFacts& now, int target,
                        SendReply reply, uint32_t ms_since_reply, uint32_t ms_since_send) {
    const bool given_up = ms_since_send >= kSwitchGiveUpMs;
    const bool answered = reply == SendReply::Ok || reply == SendReply::Refused;
    const bool settled = answered && ms_since_reply >= kSwitchSettleMs;
    if (before.routing.empty()) {
        // Nothing to judge the target by: only the pause ending counts.
        if (before.paused && !now.paused) {
            return SwitchStep::Done;
        }
        return settled || given_up ? SwitchStep::Fail : SwitchStep::Wait;
    }
    const int onto = moved_onto(before, now);
    if (onto >= 0) {
        if (!now.paused) {
            return onto == target ? SwitchStep::Done : SwitchStep::Diverted;
        }
        // Still paused: the resume after the routing write is heating and
        // purging, or it failed. A refused reply is the script stopping there.
        if (reply == SendReply::Refused || settled || given_up) {
            return onto == target ? SwitchStep::Stranded : SwitchStep::Diverted;
        }
        return SwitchStep::Wait;
    }
    return settled || given_up ? SwitchStep::Fail : SwitchStep::Wait;
}

bool label_written(const RunoutFacts& now, const FilamentRecord& label) {
    const HeadFacts* h = find_head(now, now.runout_head);
    return h != nullptr && h->record && *h->record == label;
}

bool worth_offering(const RunoutFacts& facts) {
    const HeadFacts* h = find_head(facts, facts.runout_head);
    return h != nullptr && !h->feeder_can_supply && !facts.printer_busy &&
           !routing_left_runout_head(facts);
}

int stranded_on(const RunoutFacts& facts) {
    if (!facts.paused || facts.printer_busy || !routing_left_runout_head(facts) ||
        facts.active_head < 0 || facts.active_head == facts.runout_head) {
        return -1;
    }
    return facts.active_head;
}

std::string resume_on_gcode(AmsType type, int head) {
    const Provider* provider = find_provider(type);
    return provider != nullptr && head >= 0 ? provider->resume_on(head) : std::string{};
}

bool runout_head_in_charge(const RunoutFacts& facts) {
    return !facts.printer_busy && facts.active_head == facts.runout_head &&
           !routing_left_runout_head(facts);
}

PendingRestoreStep pending_restore_step(bool pending, bool switch_in_flight,
                                        std::optional<bool> auto_switch_enabled) {
    if (!pending || switch_in_flight || !auto_switch_enabled) {
        return PendingRestoreStep::Keep;
    }
    return *auto_switch_enabled ? PendingRestoreStep::Send : PendingRestoreStep::Clear;
}

namespace {
constexpr const char* kPendingRestoreKey = "runout_other_head/pending_restore";
} // namespace

std::string read_pending_restore() {
    const Config* config = Config::get_instance();
    if (config == nullptr) {
        return {};
    }
    const nlohmann::json* j = config->try_get_json(config->df() + kPendingRestoreKey);
    return j != nullptr && j->is_string() ? j->get<std::string>() : std::string{};
}

void write_pending_restore(const std::string& gcode) {
    Config* config = Config::get_instance();
    if (config == nullptr || read_pending_restore() == gcode) {
        return;
    }
    const std::string key = config->df() + kPendingRestoreKey;
    if (gcode.empty()) {
        config->set<nlohmann::json>(key, nullptr);
    } else {
        config->set<nlohmann::json>(key, gcode);
    }
    config->save();
}

} // namespace helix::runout_other_head
