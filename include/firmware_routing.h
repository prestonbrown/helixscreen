// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

namespace helix {

/**
 * @brief The physical head a logical tool routes to with NO remap applied.
 *
 * This is the firmware's own DEFAULT map, and it is not the same thing as the
 * firmware's CURRENT map. Two AMS shapes exist and they disagree:
 *
 *   - lane-per-tool (AFC, Happy Hare, klipper-toolchanger, CFS, QIDI, ACE):
 *     tool N owns lane N. An 8-lane AFC maps lanes 0..7 to T0..T7.
 *   - fixed-head (Snapmaker U1): four physical heads and up to 32 logical
 *     tools, so the table is [0,1,2,3,0,0,...] - measured live on a U1.
 *   - table-driven (FlashForge AD5X IFS): an arbitrary 16-entry tool->port map
 *     that the firmware publishes and the user can change.
 *
 * Hardcoding any one of these in shared code makes the others silently wrong,
 * which is exactly what happened: a four-head constant sent every AFC tool
 * above 3 to lane 0. Backends declare their own shape via
 * AmsBackend::firmware_default_routing().
 *
 * Deliberately a plain value with no AMS dependency, so FilamentMapper stays
 * pure and every rule over it is testable without a backend.
 *
 * @note NOT the live map. AmsSystemInfo::tool_to_slot_map carries what the
 *       firmware is doing right now; this carries what it would do unaided.
 *       Conflating the two is the bug this type exists to prevent - and the
 *       live map is not even uniformly available (an AFC tracks it, a U1
 *       freezes it at 1:1 while its real map lives elsewhere).
 */
struct FirmwareRouting {
    /// head_for_tool[tool] = physical head, or -1 for "this tool has no head".
    /// EMPTY means identity - every tool routes to its own index.
    std::vector<int> head_for_tool;

    /// Head for tools past the end of the table. -1 means unmapped.
    int fallback_head = -1;

    /// Physical head for @p tool, or -1 when the tool has none.
    [[nodiscard]] int head(int tool) const {
        if (tool < 0) {
            return -1;
        }
        if (head_for_tool.empty()) {
            return tool; // identity
        }
        if (tool < static_cast<int>(head_for_tool.size())) {
            return head_for_tool[static_cast<size_t>(tool)];
        }
        return fallback_head;
    }

    /// Lane-per-tool: tool N owns lane N, with no upper bound.
    [[nodiscard]] static FirmwareRouting identity() {
        return {};
    }

    /// Fixed-head machine: heads 0..@p head_count-1, everything beyond on
    /// @p fallback. The Snapmaker U1 is fixed_heads(4, 0).
    [[nodiscard]] static FirmwareRouting fixed_heads(int head_count, int fallback = 0) {
        FirmwareRouting routing;
        routing.head_for_tool.reserve(static_cast<size_t>(head_count > 0 ? head_count : 0));
        for (int i = 0; i < head_count; ++i) {
            routing.head_for_tool.push_back(i);
        }
        routing.fallback_head = fallback;
        return routing;
    }
};

} // namespace helix
