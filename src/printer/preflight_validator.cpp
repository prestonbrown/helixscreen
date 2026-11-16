// SPDX-License-Identifier: GPL-3.0-or-later
#include "preflight_validator.h"

#include "filament_mapper.h"

#include <algorithm>

namespace helix {

bool PreflightResult::has_block() const {
    return std::any_of(checks.begin(), checks.end(), [](const ToolCheck& c) {
        return c.severity == ToolCheck::Severity::EmptySlot ||
               c.severity == ToolCheck::Severity::UnknownMaterial;
    });
}

std::optional<MaterialFix> PreflightResult::material_fix() const {
    for (const ToolCheck& c : checks) {
        if (c.severity == ToolCheck::Severity::UnknownMaterial && c.mapped_slot >= 0 &&
            c.mapped_backend == 0 && !c.intended_material.empty()) {
            return MaterialFix{c.tool_index, c.mapped_slot, c.intended_material};
        }
    }
    return std::nullopt;
}

bool PreflightResult::has_advisory() const {
    return std::any_of(checks.begin(), checks.end(), [](const ToolCheck& c) {
        return c.severity == ToolCheck::Severity::MaterialMismatch;
    });
}

static const ToolMapping* mapping_for_tool(int tool_index,
                                           const std::vector<ToolMapping>& mapping) {
    for (const auto& m : mapping)
        if (m.tool_index == tool_index)
            return &m;
    return nullptr;
}

// Resolve on the (slot, backend) PAIR. slot_index is unique only WITHIN a
// backend, so matching on slot alone reads the wrong lane the moment a second
// AMS is attached - reporting a tool as empty because the OTHER unit's bay of
// the same number is, or clearing it because that one is full. mapped_backend
// defaults to -1 ("auto/primary"), which the callers resolve to backend 0.
static const AvailableSlot* find_slot(int slot_index, int backend_index,
                                      const std::vector<AvailableSlot>& slots) {
    const int backend = backend_index < 0 ? 0 : backend_index;
    for (const auto& s : slots)
        if (s.slot_index == slot_index && s.backend_index == backend)
            return &s;
    return nullptr;
}

PreflightResult PreflightValidator::validate(const std::vector<GcodeToolInfo>& tools,
                                             const std::vector<AvailableSlot>& slots,
                                             const std::vector<ToolMapping>& mapping,
                                             const FirmwareRouting& routing, bool bypass_active,
                                             bool material_needed) {
    PreflightResult out;

    // Bypass / external spool: the filament reaching the nozzle does not come from
    // any slot, so nothing in `slots` can satisfy a tool and every check would
    // report EmptySlot. Without this the user gets an empty-slot block that no
    // configuration can clear, because bypass is deliberately not a slot in
    // AmsState::collect_available_slots().
    //
    // Suppresses the material advisory too, not just the block: the seated lane's
    // material describes filament that is not being printed with.
    if (bypass_active) {
        return out;
    }

    // No multi-material/AMS system: an empty slot list means there is no AMS
    // hardware at all (a present AMS always reports one slot per physical bay,
    // empty or not). There is nothing to map tools to, so the slot-based checks
    // don't apply — filament presence is the physical runout sensor's job,
    // surfaced separately. Returning an empty result avoids a false
    // "%s has no filament loaded" block on single-extruder printers.
    if (slots.empty()) {
        return out;
    }

    for (const auto& t : tools) {
        ToolCheck c;
        c.tool_index = t.tool_index;
        c.intended_color = t.color_rgb;
        c.intended_material = t.material;
        const ToolMapping* m = mapping_for_tool(t.tool_index, mapping);
        if (m != nullptr && m->is_auto && routing.chosen_at_print) {
            // The firmware picks this tool's lane when the print starts, so no
            // lane here can be judged empty or wrong for it.
            continue;
        }
        const AvailableSlot* slot = nullptr;
        if (m != nullptr && m->is_auto) {
            // Auto prints from the lane the firmware routes the tool to.
            slot = FilamentMapper::resolve_printing_slot(*m, slots, routing);
            c.mapped_slot = slot != nullptr ? slot->slot_index : -1;
            c.mapped_backend = slot != nullptr ? slot->backend_index : 0;
        } else {
            c.mapped_slot = m != nullptr ? m->mapped_slot : -1;
            c.mapped_backend = m != nullptr ? m->mapped_backend : 0;
            slot =
                c.mapped_slot >= 0 ? find_slot(c.mapped_slot, m->mapped_backend, slots) : nullptr;
        }
        if (slot == nullptr || slot->is_empty) {
            c.slot_present = false;
            c.severity = ToolCheck::Severity::EmptySlot;
        } else if (material_needed && slot->material_required) {
            // Filament is seated, so this is not an empty slot, but an option
            // this print enables refuses it until a material is set: starting
            // anyway only defers the failure until after the heat, home and
            // probe. The slot prints fine with no such option on, and a material
            // that is merely unreported stays out of this arm, or every backend
            // that reports none would block every print.
            c.slot_present = true;
            c.color_ok = FilamentMapper::colors_match(t.color_rgb, slot->color_rgb);
            c.severity = ToolCheck::Severity::UnknownMaterial;
        } else {
            c.slot_present = true;
            c.color_ok = FilamentMapper::colors_match(t.color_rgb, slot->color_rgb);
            c.material_ok = FilamentMapper::materials_match(t.material, slot->material);
            if (!c.material_ok)
                c.severity = ToolCheck::Severity::MaterialMismatch;
            else if (!c.color_ok)
                c.severity = ToolCheck::Severity::ColorMismatch;
            else
                c.severity = ToolCheck::Severity::Ok;
        }
        out.checks.push_back(std::move(c));
    }
    return out;
}

} // namespace helix
