// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filament_mapper.h"

#include <cstdint>
#include <optional>
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
    enum class Severity { Ok, ColorMismatch, MaterialMismatch, EmptySlot, UnknownMaterial };
    Severity severity = Severity::Ok;
};

/// A material the file names for a slot that has none set, which one tap can
/// write to the slot.
struct MaterialFix {
    int tool_index = -1;
    int slot = -1;
    std::string material;
};

struct PreflightResult {
    std::vector<ToolCheck> checks;
    bool has_block() const;    // any EmptySlot or UnknownMaterial
    bool has_advisory() const; // any MaterialMismatch
    /// The first UnknownMaterial check whose file names a material for its
    /// tool. None when the file names none: guessing would calibrate and print
    /// the slot at another material's settings.
    std::optional<MaterialFix> material_fix() const;
};

class PreflightValidator {
  public:
    /**
     * @param routing The firmware routing an Auto mapping prints through: an Auto
     *        tool is judged against the lane it routes to
     *        (FilamentMapper::resolve_printing_slot()), and blocks as EmptySlot
     *        when it routes to none. An Auto tool on a routing whose lanes are
     *        picked at print time (FirmwareRouting::chosen_at_print) gets no
     *        check. Required: a caller that guesses it judges the wrong lane.
     * @param bypass_active Filament is being fed from the bypass / external spool
     *        rather than the slot system, so no tool can be satisfied by a slot
     *        and the slot-based checks do not apply. Required rather than
     *        defaulted: a caller that forgets it re-introduces a false block.
     * @param material_needed The print enables an option that refuses a slot
     *        with no material set (PrePrintOption::needs_material), so such a
     *        slot is a block (UnknownMaterial). Without one it prints as is.
     *        Required for the same reason as @p bypass_active.
     */
    static PreflightResult validate(const std::vector<GcodeToolInfo>& tools,
                                    const std::vector<AvailableSlot>& slots,
                                    const std::vector<ToolMapping>& mapping,
                                    const FirmwareRouting& routing, bool bypass_active,
                                    bool material_needed);
};

} // namespace helix
