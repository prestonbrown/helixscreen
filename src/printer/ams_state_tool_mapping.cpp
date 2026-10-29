// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_tool_mapping.cpp
 * @brief AmsState: the tool -> lane mapping questions: available slots, seeds, routing and render
 * colors
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ams_remap.h"
#include "ams_state.h"
#include "ams_state_internal.h"
#include "ams_tool_topology.h"
#include "filament_mapper.h"
#include "settings_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

namespace helix {
using ams_state_detail::assert_main_thread;

// Declared in ams_tool_topology.h; see there for what nullopt means to callers.
std::optional<helix::ToolTopology> build_ams_topology(AmsBackend* backend, int backend_index) {
    if (!backend || !backend->owns_tool_mapping_table())
        return std::nullopt;
    return build_ams_topology(backend, backend_index, backend->get_system_info());
}

std::optional<helix::ToolTopology> build_ams_topology(AmsBackend* backend, int backend_index,
                                                      const AmsSystemInfo& info) {
    if (!backend)
        return std::nullopt;
    // Table ownership, NOT remap capability. Two different questions that part
    // company on the U1: it can remap and owns no table.
    if (!backend->owns_tool_mapping_table())
        return std::nullopt;

    std::vector<int> mapping = backend->get_tool_mapping();
    if (mapping.empty()) {
        // Fallback: default 1:1 from slot count
        int n = info.total_slots;
        mapping.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            mapping[static_cast<size_t>(i)] = i;
    }

    helix::ToolTopology topo;
    topo.tool_count = static_cast<int>(mapping.size());
    // Which head each tool prints through: the extruder its slot feeds.
    topo.tool_extruder.reserve(mapping.size());
    // And whether its lane is the one that head holds: a hub feeds several
    // lanes into one head, and the head keeps whichever is seated there while
    // it is parked.
    topo.tool_seated.reserve(mapping.size());
    for (const int slot : mapping) {
        const SlotInfo* info_slot = slot >= 0 ? info.get_slot_global(slot) : nullptr;
        topo.tool_extruder.push_back(info_slot ? info_slot->extruder_name : std::string());
        topo.tool_seated.push_back(slot >= 0 && backend->slot_is_actively_loaded(slot));
    }
    topo.tool_to_slot = std::move(mapping);
    topo.active_tool = backend->get_current_tool();
    topo.allows_empty_carriage = backend->carriage_can_be_empty();
    topo.backend_index = backend_index;
    topo.heads_share_heater = backend->heads_share_heater();
    return topo;
}

std::vector<uint32_t> AmsState::routed_tool_colors() const {
    auto* backend = get_backend();
    if (!backend) {
        spdlog::debug("[AmsState] routed_tool_colors: no AMS backend");
        return {};
    }

    // The applied routing, asked as a capability question. A backend that tracks
    // a separate firmware routing table (Snapmaker U1's extruder_map_table)
    // answers from it; one whose physical map IS the routing (AFC, Happy Hare,
    // a plain tool changer) answers from that. Vendor knowledge stays inside the
    // backend — nothing here names a firmware.
    // Whether the attachment map may stand in when the backend publishes no
    // routing is a real decision, so it is a named one (FilamentMapper::
    // effective_routing) rather than an `if` here: on a tool changer that map is
    // which slot each head owns, never which head prints a tool, and letting it
    // stand in silently converts "no opinion" into a confident identity answer.
    std::vector<int> routing = helix::FilamentMapper::effective_routing(
        backend->get_tool_mapping(), system_info()->tool_to_slot_map,
        /*attachment_is_routing=*/!is_tool_changer(backend->get_type()));

    auto colors = helix::FilamentMapper::routed_tool_colors(routing, collect_available_slots(),
                                                            backend->tool_mapping_origin());
    spdlog::debug("[AmsState] routed_tool_colors: {} routing entries -> {} color(s)",
                  routing.size(), colors.size());
    return colors;
}

std::vector<helix::AvailableSlot> AmsState::collect_available_slots() const {
    std::vector<helix::AvailableSlot> slots;
    assert_main_thread();

    const auto& backends = registry_.backends();
    for (size_t bi = 0; bi < backends.size(); ++bi) {
        const auto& backend = backends[bi];
        if (!backend) {
            continue;
        }

        const auto snapshot = system_info(static_cast<int>(bi));
        if (!snapshot) {
            continue;
        }
        const AmsSystemInfo& info = *snapshot;
        bool multi_unit = info.units.size() > 1;

        for (size_t pos = 0; pos < info.units.size(); ++pos) {
            const AmsUnit& unit = info.units[pos];
            if (unit.absent) {
                continue;
            }
            for (const auto& slot_info : unit.slots) {
                helix::AvailableSlot as;
                as.slot_index = slot_info.global_index;
                as.local_slot_index = slot_info.slot_index;
                as.backend_index = static_cast<int>(bi);
                as.color_rgb = slot_info.color_rgb;
                as.multi_color_hexes = slot_info.multi_color_hexes;
                as.finish = slot_info.finish();
                as.material = slot_info.material;
                as.material_required = slot_info.material_required;
                as.is_empty = (slot_info.status == SlotStatus::EMPTY ||
                               slot_info.status == SlotStatus::UNKNOWN);
                as.remaining_weight_g = slot_info.remaining_weight_g;
                as.current_tool_mapping = slot_info.mapped_tool;
                as.unit_index = unit.unit_index;
                as.noun = backend->lane_noun();
                if (multi_unit) {
                    // The name the overview's unit card shows, so both surfaces agree.
                    as.unit_display_name =
                        ams_draw::get_unit_display_name(unit, static_cast<int>(pos));
                }
                slots.push_back(std::move(as));
            }
        }
    }

    spdlog::debug("[AmsState] Collected {} available slots from {} backends", slots.size(),
                  backends.size());
    return slots;
}

std::vector<helix::ToolMapping>
AmsState::seed_tool_mappings(const std::vector<helix::GcodeToolInfo>& tools,
                             const std::vector<helix::AvailableSlot>& slots) const {
    // A strategy whose Print carries out no stored pick prints every tool from the
    // head the firmware routes it to, so the seed every gate reads says exactly
    // that. A colour match or a free-lane guess there would name a lane Print
    // never uses.
    auto* backend = get_backend(0);
    if (backend && !helix::printer::print_carries_out_stored_pick(backend->get_remap_strategy())) {
        return helix::FilamentMapper::effective_mappings(tools, slots, /*auto_color_map=*/false,
                                                         collect_firmware_routing(),
                                                         helix::FreeLaneGuess::Refused);
    }
    return seed_tool_mappings(tools, slots, effective_auto_match());
}

std::vector<helix::ToolMapping>
AmsState::seed_tool_mappings(const std::vector<helix::GcodeToolInfo>& tools,
                             const std::vector<helix::AvailableSlot>& slots,
                             bool auto_color_map) const {
    auto* backend = get_backend(0);
    const auto guess =
        backend && helix::printer::remap_carries_out_seed(backend->get_remap_strategy())
            ? helix::FreeLaneGuess::Allowed
            : helix::FreeLaneGuess::Refused;
    return helix::FilamentMapper::effective_mappings(tools, slots, auto_color_map,
                                                     collect_firmware_routing(), guess);
}

helix::FirmwareRouting AmsState::collect_firmware_routing() const {
    assert_main_thread();
    auto* backend = get_backend(0);
    if (!backend) {
        return helix::FirmwareRouting::identity();
    }
    helix::FirmwareRouting routing = backend->firmware_default_routing();

    // A default read from the live table is the last print's remap while its
    // restore is pending; the map that remap displaced is the printer's own.
    const auto& pending = pending_tool_map_restore_;
    if (!pending.empty() && pending.backend_index == 0 &&
        pending.matches_type(backend->get_type()) && !routing.head_for_tool.empty() &&
        routing.head_for_tool == backend->get_tool_mapping()) {
        routing.head_for_tool = pending.mapping;
        routing.fallback_head = -1;
    }
    return routing;
}

const helix::PendingToolMapRestore& AmsState::pending_tool_map_restore() const {
    assert_main_thread();
    return pending_tool_map_restore_;
}

void AmsState::set_pending_tool_map_restore(int backend_index, AmsType backend_type,
                                            std::vector<int> mapping) {
    assert_main_thread();
    pending_tool_map_restore_.backend_index = backend_index;
    pending_tool_map_restore_.backend_type = backend_type;
    pending_tool_map_restore_.mapping = std::move(mapping);
    pending_tool_map_restore_.outlived_a_print = false;
    ++pending_tool_map_restore_.epoch;
}

uint64_t AmsState::claim_pending_tool_map_restore() {
    assert_main_thread();
    pending_tool_map_restore_.outlived_a_print = true;
    return ++pending_tool_map_restore_.epoch;
}

bool AmsState::set_pending_tool_map_entry(int tool, int slot) {
    assert_main_thread();
    auto& mapping = pending_tool_map_restore_.mapping;
    if (pending_tool_map_restore_.empty() || tool < 0 || tool >= static_cast<int>(mapping.size())) {
        return false;
    }
    mapping[static_cast<size_t>(tool)] = slot;
    return true;
}

void AmsState::clear_pending_tool_map_restore() {
    assert_main_thread();
    pending_tool_map_restore_.backend_index = -1;
    pending_tool_map_restore_.backend_type = AmsType::NONE;
    pending_tool_map_restore_.mapping.clear();
    pending_tool_map_restore_.outlived_a_print = false;
    ++pending_tool_map_restore_.epoch;
}

bool AmsState::effective_auto_match() const {
    assert_main_thread();
    // Ask whether the user's choice can be carried out at all, not whether the
    // mapping card is editable. Editability is only one of the two routes, and
    // the backend it gets wrong is the one where the toggle does the most
    // damage: the U1 honors every pick through its pre-print send, so pinning
    // auto-match there left color proximity rewriting the print's routing with
    // no way for the user to decline it.
    bool user_choice_honored = false;
    if (auto* backend = get_backend(0)) {
        user_choice_honored = helix::printer::can_remap(*backend);

        // HOLD — kPreprintSeedFollowsUserSetting (ams_state.h) carries the full
        // reasoning and the hardware evidence that would lift it. A backend that
        // cannot remap PERSISTENTLY can only have answered true through its
        // pre-print send, so this is exactly the case being held, and putting it
        // back leaves the U1 seeding as it does today. The predicate itself is
        // untouched: it is still the one rule the print-start warning shares, and
        // gating it there would resurrect a false toast on the U1.
        if (!kPreprintSeedFollowsUserSetting &&
            !(helix::printer::remap_is_persistent(backend->get_remap_strategy()) &&
              backend->remap_ready())) {
            user_choice_honored = false;
        }
    }
    return !user_choice_honored || helix::SettingsManager::instance().get_auto_color_map();
}
} // namespace helix
