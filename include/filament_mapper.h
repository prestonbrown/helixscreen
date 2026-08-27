// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/// Unique identifier for an AMS slot across backends
using SlotKey = std::pair<int, int>; ///< (slot_index, backend_index)

/// Information about a G-code tool's expected filament
struct GcodeToolInfo {
    int tool_index;       ///< G-code tool number (0-based)
    uint32_t color_rgb;   ///< Expected color (0xRRGGBB)
    std::string material; ///< Expected material type ("PLA", "PETG", etc.)

    /// False when no source could say what color this tool prints in, and
    /// color_rgb is only a neutral stand-in. Not the same as "the slicer chose
    /// grey": Moonraker omits filament_colors entirely for some slicers (every
    /// OrcaSlicer file on a K2 Plus), and the palette is then backfilled from
    /// the G-code footer or the parsed file. Until one of those lands, a tool
    /// has no known color at all.
    ///
    /// Consumers must not treat an unknown color as a claim about the file:
    /// compute_defaults() skips its color-match priority (matching a stand-in
    /// against real lane colors picks a lane for no reason), and the mapping
    /// pill draws the dot as unknown rather than painting a solid grey the user
    /// would read as the file's color. Defaulted true and kept last so existing
    /// aggregate initialisers stay valid.
    bool color_known = true;
};

/// Information about an available AMS slot.
/// This is an intentional abstraction boundary — callers convert from
/// ams_types.h SlotInfo to keep FilamentMapper free of LVGL dependency.
struct AvailableSlot {
    int slot_index;                ///< Global slot index (unique within backend, used for mapping)
    int backend_index;             ///< Which AMS backend (0 = primary)
    uint32_t color_rgb;            ///< Loaded filament color (0xRRGGBB)
    std::string material;          ///< Loaded material type
    bool is_empty;                 ///< True if slot has no filament
    int current_tool_mapping;      ///< What tool this slot is currently mapped to (-1 = none)
    int unit_index = 0;            ///< Unit index within the backend
    int local_slot_index = 0;      ///< Slot index within its unit (for display labels)
    std::string unit_display_name; ///< Unit name for display (empty = single-unit backend)
    /// Comma-separated hex codes for multi-color spools (companion to color_rgb;
    /// empty = single-color). Mirrors SlotInfo::multi_color_hexes. Kept last so
    /// existing positional aggregate initializers stay valid.
    std::string multi_color_hexes;

    /// Remaining filament on this lane in grams, or -1 when unknown.
    /// Mirrors SlotInfo's sentinel: -1 means NO OPINION, never zero. Only a
    /// Spoolman-linked or hand-weighed lane has a figure at all, so an unlinked
    /// bay legitimately has none and must never be warned about.
    /// Kept last for the same reason multi_color_hexes is - positional
    /// aggregate initializers in the tests stay valid.
    float remaining_weight_g = -1.0f;

    /// Unique key for this slot across all backends
    SlotKey key() const {
        return {slot_index, backend_index};
    }
};

/// Result of mapping a single tool
struct ToolMapping {
    int tool_index = -1;            ///< G-code tool number
    int mapped_slot = -1;           ///< AMS slot index (-1 = auto/unmapped)
    int mapped_backend = -1;        ///< Backend index (-1 = auto/primary)
    bool material_mismatch = false; ///< True if slot material != expected material
    bool is_auto = false;           ///< True if using "auto" (no explicit mapping)

    enum class MatchReason {
        FIRMWARE_MAPPING, ///< Matched via current firmware tool-to-slot mapping
        COLOR_MATCH,      ///< Matched by closest color
        AUTO,             ///< No explicit mapping, let firmware decide
    };
    MatchReason reason = MatchReason::AUTO;
};

/// Pure logic class for computing filament-to-tool mappings.
/// No LVGL dependency — takes data in, returns mappings out.
/// All methods are static and thread-safe.
class FilamentMapper {
  public:
    /// Compute default mappings for all tools.
    /// Priority: firmware mapping -> color match -> auto
    static std::vector<ToolMapping> compute_defaults(const std::vector<GcodeToolInfo>& tools,
                                                     const std::vector<AvailableSlot>& slots);

    /// Check if two colors are within matching tolerance.
    /// Uses weighted RGB distance (luminance-weighted) with tolerance of 40 units.
    static bool colors_match(uint32_t color_a, uint32_t color_b);

    /// Find the best matching slot for a given color and material.
    /// Skips non-empty slots with incompatible materials.
    /// Returns the matching slot's key, or {-1, -1} if no match within tolerance.
    static SlotKey find_closest_color_slot(uint32_t target_color,
                                           const std::string& target_material,
                                           const std::vector<AvailableSlot>& slots);

    /// Map tools using only current firmware assignments.
    /// Tools with no firmware assignment become AUTO (unmapped).
    /// Used when "keep current assignments" setting is enabled.
    static std::vector<ToolMapping>
    use_current_assignments(const std::vector<GcodeToolInfo>& tools,
                            const std::vector<AvailableSlot>& slots);

    /// Find tool indices that have no resolved mapping (auto with no match).
    /// These are the tools that would trigger a color mismatch warning.
    static std::vector<int> find_unresolved_tools(const std::vector<ToolMapping>& mappings);

    /// Resolve the per-tool DISPLAY color: the mapped slot's loaded color_rgb
    /// when the tool is explicitly mapped to an existing slot, else the tool's
    /// slicer color_rgb (or 0x808080 when no tool info). Pure; no LVGL/AMS.
    static std::vector<uint32_t> resolve_display_colors(const std::vector<GcodeToolInfo>& tools,
                                                        const std::vector<ToolMapping>& mappings,
                                                        const std::vector<AvailableSlot>& slots);

    /// Toggle-aware mapping: the single place that turns the auto-color-map
    /// preference into a mapping. When @p auto_color_map is true, firmware
    /// mappings are cleared so they don't pre-empt color/type matches and
    /// compute_defaults runs; otherwise use_current_assignments (positional).
    /// This is the hoisted equivalent of FilamentMappingCard's seeding logic,
    /// so the card, the print-file swatch/preflight, and the live render all
    /// resolve the same way. Pure; no LVGL/AMS/Settings dependency (caller
    /// passes the resolved bool).
    static std::vector<ToolMapping> effective_mappings(const std::vector<GcodeToolInfo>& tools,
                                                       const std::vector<AvailableSlot>& slots,
                                                       bool auto_color_map);

    /// Render convenience: effective_mappings + resolve_display_colors, scattered
    /// into a DENSE vector indexed by logical tool number (size = max
    /// tool_index + 1). Tool numbers not present in @p tools get 0x808080; used
    /// tools with no matched lane keep their slicer color. Ready to hand to
    /// ui_gcode_viewer_set_tool_colors (which is logical-tool-indexed). Returns
    /// empty when @p tools is empty. Pure; no LVGL/AMS.
    static std::vector<uint32_t> effective_tool_colors(const std::vector<GcodeToolInfo>& tools,
                                                       const std::vector<AvailableSlot>& slots,
                                                       bool auto_color_map);

    /// Same DENSE tool-indexed color vector as above, but from an already-resolved
    /// @p mappings vector instead of recomputing from @p auto_color_map. This is
    /// the single color engine both the live render and the print-file preview
    /// share: the render passes auto-computed mappings, the preview passes its
    /// card-aware effective mappings (user edits win on editable backends), and a
    /// fully-default (unmapped) @p mappings yields the plain slicer palette. Pure;
    /// no LVGL/AMS. @p mappings must be parallel to @p tools (same order).
    static std::vector<uint32_t> effective_tool_colors(const std::vector<GcodeToolInfo>& tools,
                                                       const std::vector<ToolMapping>& mappings,
                                                       const std::vector<AvailableSlot>& slots);

    /// Per-tool display colors from the APPLIED ROUTING: color(tool N) is the
    /// color of whatever lane actually prints N.
    ///
    /// The one rule, valid for any tool count. There is deliberately no tool
    /// count in the signature and no palette: a single-tool file and an N-tool
    /// file take the identical path, which is what stops a "just for N=1"
    /// special case growing back. The slicer palette answers a different
    /// question ("what SHOULD this print use") that belongs pre-print, in the
    /// detail view's color/type match, not to a print already underway.
    ///
    /// @param tool_to_head Applied routing, index = logical tool, value =
    ///        physical slot/head, -1 = unknown. Comes from the backend
    ///        (AmsBackend::get_tool_mapping()), which owns where that answer
    ///        lives; an EMPTY vector means the backend has no opinion and this
    ///        returns empty rather than assuming identity.
    /// @param slots Current lane state.
    /// @return Dense tool-indexed colors, or EMPTY when nothing is knowable —
    ///         including when every resolved color is the neutral default, since
    ///         pushing an all-grey vector would only overwrite the slicer palette
    ///         the renderer already has. Pure; no LVGL/AMS.
    static std::vector<uint32_t> routed_tool_colors(const std::vector<int>& tool_to_head,
                                                    const std::vector<AvailableSlot>& slots);

    /// Weighted RGB distance between two colors (luminance-weighted).
    /// Uses standard luminance coefficients: R=0.30, G=0.59, B=0.11.
    static int color_distance(uint32_t a, uint32_t b);

    /// Case-insensitive material comparison
    static bool materials_match(const std::string& a, const std::string& b);

    /// Firmware-default physical head a logical tool routes to with no remap.
    ///
    /// Tools 0..3 map to their identity head; anything else (extended tools on a
    /// toolchanger, 4..31 on the Snapmaker U1) falls back to head 0, matching the
    /// firmware default map [0,1,2,3,0,0,...].
    static int default_head_for_tool(int tool);

    /// The genuine remaps in @p mappings, keyed tool -> physical head.
    ///
    /// Identity mappings are omitted: the firmware already routes a tool to
    /// default_head_for_tool(tool), so emitting them would be noise. Entries with
    /// no real slot assignment (mapped_slot < 0) are skipped.
    ///
    /// Single source of truth for PrintSelectDetailView::get_effective_remap()
    /// and the preprint-gcode builders, which used to each carry their own copy.
    static std::map<int, int> identity_filtered_remap(const std::vector<ToolMapping>& mappings);

    /// Format a slot label: "Turtle 1 · Slot 2: PLA" or "Slot 2: PLA"
    static std::string format_slot_label(const AvailableSlot& slot);

    /// The lane number to print on a mapped chip, 1-based, or -1 when the tool
    /// is unmapped or points at a lane that is no longer present.
    ///
    /// Colour alone does not identify a lane: two bays loaded with the same
    /// filament render the same swatch, and the chip then says which colour
    /// will be used without saying which spool it comes from. The number is
    /// what disambiguates them.
    ///
    /// Reports `local_slot_index + 1` - the lane's position within its own
    /// unit - to agree with format_slot_label() and the AMS slot badges. On a
    /// multi-unit setup the global index would name a lane the hardware does
    /// not, calling the second unit's first bay "Slot 5".
    static int mapped_lane_display_number(const ToolMapping& mapping,
                                          const std::vector<AvailableSlot>& slots);

    static constexpr int COLOR_MATCH_TOLERANCE = 50;
};

} // namespace helix
