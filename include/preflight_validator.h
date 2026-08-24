// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filament_mapper.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix {

struct ToolCheck {
    int tool_index = -1;
    uint32_t intended_color = 0;
    std::string intended_material;
    int mapped_slot = -1;
    bool slot_present = false;
    bool color_ok = true;
    bool material_ok = true;
    enum class Severity { Ok, ColorMismatch, MaterialMismatch, EmptySlot };
    Severity severity = Severity::Ok;
};

struct PreflightResult {
    std::vector<ToolCheck> checks;
    bool has_block() const;    // any EmptySlot
    bool has_advisory() const; // any MaterialMismatch
};

class PreflightValidator {
  public:
    /**
     * @param bypass_active Filament is being fed from the bypass / external spool
     *        rather than the slot system, so no tool can be satisfied by a slot
     *        and the slot-based checks do not apply. Required rather than
     *        defaulted: a caller that forgets it re-introduces a false block.
     */
    static PreflightResult validate(const std::vector<GcodeToolInfo>& tools,
                                    const std::vector<AvailableSlot>& slots,
                                    const std::vector<ToolMapping>& mapping, bool bypass_active);
};

} // namespace helix
