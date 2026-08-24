// SPDX-License-Identifier: GPL-3.0-or-later
#include "preflight_validator.h"

#include "filament_mapper.h"

#include <algorithm>

namespace helix {

bool PreflightResult::has_block() const {
    return std::any_of(checks.begin(), checks.end(), [](const ToolCheck& c) {
        return c.severity == ToolCheck::Severity::EmptySlot;
    });
}

bool PreflightResult::has_advisory() const {
    return std::any_of(checks.begin(), checks.end(), [](const ToolCheck& c) {
        return c.severity == ToolCheck::Severity::MaterialMismatch;
    });
}

static int slot_for_tool(int tool_index, const std::vector<ToolMapping>& mapping) {
    for (const auto& m : mapping)
        if (m.tool_index == tool_index)
            return m.mapped_slot;
    return -1;
}
static const AvailableSlot* find_slot(int slot_index, const std::vector<AvailableSlot>& slots) {
    for (const auto& s : slots)
        if (s.slot_index == slot_index)
            return &s;
    return nullptr;
}

PreflightResult PreflightValidator::validate(const std::vector<GcodeToolInfo>& tools,
                                             const std::vector<AvailableSlot>& slots,
                                             const std::vector<ToolMapping>& mapping,
                                             bool bypass_active) {
    PreflightResult out;

    // Bypass / external spool: the filament reaching the nozzle does not come from
    // any slot, so nothing in `slots` can satisfy a tool and every check would
    // report EmptySlot. The gcode still names T0, so without this the user gets a
    // "T0 has no filament loaded — this print will run out." block that no
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
    // "T0 has no filament loaded" block on single-extruder printers.
    if (slots.empty()) {
        return out;
    }

    for (const auto& t : tools) {
        ToolCheck c;
        c.tool_index = t.tool_index;
        c.intended_color = t.color_rgb;
        c.intended_material = t.material;
        c.mapped_slot = slot_for_tool(t.tool_index, mapping);
        const AvailableSlot* slot = c.mapped_slot >= 0 ? find_slot(c.mapped_slot, slots) : nullptr;
        if (slot == nullptr || slot->is_empty) {
            c.slot_present = false;
            c.severity = ToolCheck::Severity::EmptySlot;
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
