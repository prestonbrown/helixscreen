// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_mapping_modal.h"

#include "filament_mapper.h"

#include <lvgl.h>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Compact filament mapping card for the print detail view
 *
 * Shows a compact row of color swatch pairs (gcode_color -> slot_color)
 * for each tool mapping. Tapping the card opens the FilamentMappingModal
 * for full interaction.
 *
 * Visibility: declarative. The card itself does NOT toggle the LVGL HIDDEN
 * flag. After each `update()` it caches whether it should be visible
 * (`should_show()`); the print detail view publishes that on a subject the
 * XML binds via `bind_flag_if_eq` — see print_file_detail.xml's
 * `filament_mapping_visible` binding. Card visible iff AMS/toolchanger is
 * detected AND the file uses at least one tool AND at least one backend
 * advertises editable tool-mapping capabilities.
 */
class FilamentMappingCard {
  public:
    FilamentMappingCard() = default;
    ~FilamentMappingCard() = default;

    // Non-copyable (holds LVGL widget pointers)
    FilamentMappingCard(const FilamentMappingCard&) = delete;
    FilamentMappingCard& operator=(const FilamentMappingCard&) = delete;

    /**
     * @brief Attach to XML widgets after instantiation
     *
     * @param card_widget The filament_mapping_card ui_card
     * @param rows_container The filament_mapping_rows container (used for compact swatch row)
     * @param warning_container The filament_mapping_warning container
     */
    void create(lv_obj_t* card_widget, lv_obj_t* rows_container, lv_obj_t* warning_container);

    /**
     * @brief Update with new file data + current AMS state
     *
     * Caches whether the card should be visible (`should_show()`), based on
     * AMS availability AND the file having at least one tool. Computes
     * default mappings via FilamentMapper::compute_defaults().
     *
     * The detail view is responsible for publishing the cached state onto
     * the `filament_mapping_visible` subject after this call returns.
     *
     * @param gcode_colors Per-tool hex color strings (e.g., "#FF0000")
     * @param gcode_materials Per-tool material strings (e.g., "PLA")
     */
    void update(const std::vector<std::string>& gcode_colors,
                const std::vector<std::string>& gcode_materials);

    /**
     * @brief Re-pull loaded slot colors/presence from AmsState and re-render,
     * WITHOUT recomputing tool->slot mappings — preserves the user's manual
     * remap and the auto assignment. Used by the detail view's live AMS-change
     * handler.
     */
    void refresh_slot_data();

    /**
     * @brief Restrict the card to only the tools the gcode actually uses.
     *
     * The card is populated from the full slicer palette (all filaments of a
     * project). A print that only uses tools 2 and 3 should show only T2 and
     * T3, not all four chips. The detail view pushes the real used-tool set
     * (`tools_used_effective()`) at the reliable post-parse hooks.
     *
     * Recompacts the card's `tool_info_` / `mappings_` in lockstep to the
     * entries whose `.tool_index` is in `used` (preserving order and the real
     * `.tool_index` used for the "T%d" label), then rebuilds the compact view.
     * Mappings-preserving — no recompute (mirrors refresh_slot_data).
     *
     * `nullopt` OR an empty set ⇒ no filter (show all). This is the safety
     * rule: it avoids blanking the card pre-parse, and avoids the headless
     * single-extruder case (where the used set is empty forever) hiding
     * everything.
     */
    void set_used_tools(std::optional<std::set<int>> used);

    /**
     * @brief Get current tool-to-slot mappings
     */
    [[nodiscard]] std::vector<helix::ToolMapping> get_mappings() const {
        return mappings_;
    }

    /**
     * @brief Replace the current tool→slot mappings.
     *
     * Stores the provided mappings into the card's internal store and fires
     * on_mappings_changed_ so downstream consumers (color sync, pre-flight gate)
     * re-evaluate. Used by the U1 native-remap flow, where the inline card
     * widget is hidden but its mappings_ store still feeds get_effective_remap()
     * and recompute_preflight(). Safe to call when widgets are not created.
     */
    void set_mappings(std::vector<helix::ToolMapping> mappings) {
        mappings_ = std::move(mappings);
        if (on_mappings_changed_) {
            on_mappings_changed_();
        }
    }

    /**
     * @brief Get per-tool gcode info (colors, materials)
     */
    [[nodiscard]] std::vector<helix::GcodeToolInfo> get_tool_info() const {
        return tool_info_;
    }

    /**
     * @brief Get available AMS slots (for material mismatch lookups)
     */
    [[nodiscard]] const std::vector<helix::AvailableSlot>& get_available_slots() const {
        return available_slots_;
    }

    /**
     * @brief Get per-tool mapped colors (RGB values from chosen slots)
     *
     * Returns a vector of uint32_t colors, one per tool. For auto/unmapped
     * tools, returns the gcode tool's original color.
     */
    [[nodiscard]] std::vector<uint32_t> get_mapped_colors() const;

    using MappingsChangedCallback = std::function<void()>;

    /**
     * @brief Register callback for when user changes mappings via the modal
     */
    void set_on_mappings_changed(MappingsChangedCallback cb) {
        on_mappings_changed_ = std::move(cb);
    }

    using TapCallback = std::function<void()>;

    /**
     * @brief Override what happens when the card is tapped.
     *
     * When set, a tap on the card fires this callback INSTEAD of opening the
     * card's internal mapping modal. The print detail view uses this to route
     * the tap to PrintSelectPanel::open_remap_modal(), so there is exactly one
     * remap opener and one modal instance across all backends. When unset, the
     * card falls back to opening its own modal (open_mapping_modal()).
     */
    void set_on_tap(TapCallback cb) {
        on_tap_ = std::move(cb);
    }

    /**
     * @brief Check if any mappings have material mismatches
     */
    [[nodiscard]] bool has_mismatch() const;

    /**
     * @brief Whether the card should be visible after the latest `update()`
     *
     * True iff AMS is available, at least one backend advertises editable
     * tool-mapping capabilities, and the file declares at least one tool.
     */
    [[nodiscard]] bool should_show() const {
        return should_show_;
    }

    /**
     * @brief Null widget pointers (called during destroy-on-close)
     */
    void on_ui_destroyed();

    /**
     * @brief Open the tool→slot filament mapping modal.
     *
     * Also invoked internally when the card itself is tapped. Exposed so the
     * pre-flight gate's "Remap…" button can reuse the exact same modal wiring
     * (data population + on_mappings_updated → on_mappings_changed_) for
     * native-routing backends rather than duplicating it.
     */
    void open_mapping_modal();

    /// Build GcodeToolInfo list from color/material strings.
    ///
    /// Pure/stateless: derives one GcodeToolInfo per tool (tool_index = i,
    /// color_rgb parsed from colors[i], material = materials[i]) using only its
    /// arguments. Exposed static so callers can source per-tool info directly
    /// from the same color/material data that feeds the card (Moonraker
    /// metadata, populated on all platforms) without coupling to a card
    /// INSTANCE whose tool_info_ is only populated on some code paths.
    static std::vector<helix::GcodeToolInfo>
    build_tool_info(const std::vector<std::string>& colors,
                    const std::vector<std::string>& materials);

    /// Compact parallel tool_info / mappings vectors to only the used tools.
    ///
    /// Pure/stateless: filters BOTH vectors in lockstep, keeping only entries
    /// whose `.tool_index` is in `used`, preserving order and `.tool_index`.
    /// `nullopt` or an empty set ⇒ no-op (show all) — the safety rule that
    /// prevents blanking the card pre-parse or on the headless single-extruder
    /// path. Exposed static so the seam is unit-testable without LVGL/AMS state.
    static void apply_used_tools_filter(std::vector<helix::GcodeToolInfo>& tool_info,
                                        std::vector<helix::ToolMapping>& mappings,
                                        const std::optional<std::set<int>>& used);

    /// Find a tool by its real gcode `.tool_index`, not by vector position.
    ///
    /// `tool_info` may be used-filtered (compacted), so position no longer
    /// equals `.tool_index`. Callers that have a `tool_index` (e.g. from a
    /// ToolMapping or an unresolved-tools list) MUST look up through here rather
    /// than `tool_info[tool_index]`. Returns nullptr if no entry matches.
    static const helix::GcodeToolInfo*
    find_by_tool_index(const std::vector<helix::GcodeToolInfo>& tool_info, int tool_index);

  private:
    /// Build compact swatch pair row in rows_container_
    void rebuild_compact_view();

    /// Check if any mappings have material mismatches
    bool has_any_mismatch() const;

    lv_obj_t* card_ = nullptr;
    lv_obj_t* rows_container_ = nullptr;
    lv_obj_t* warning_container_ = nullptr;

    bool should_show_ = false; ///< Cached visibility intent — see should_show()

    std::vector<helix::ToolMapping> mappings_;
    std::vector<helix::GcodeToolInfo> tool_info_;
    std::vector<helix::AvailableSlot> available_slots_;

    /// Tools the gcode actually uses (pushed post-parse by the detail view).
    /// nullopt / empty ⇒ show the full palette. Re-applied at the end of
    /// update() so a set pushed before a later update() survives the rebuild.
    std::optional<std::set<int>> used_tools_;

    /// Fingerprint of the inputs (tools + mappings + slot state) behind the
    /// last render of the compact view. rebuild_compact_view() skips the
    /// destroy/recreate when a freshly computed fingerprint matches this and
    /// children still exist. Cleared in on_ui_destroyed() — a recycled card
    /// must re-render against fresh widgets even with identical data.
    std::string last_render_fingerprint_;

    FilamentMappingModal mapping_modal_;
    MappingsChangedCallback on_mappings_changed_;
    TapCallback on_tap_; ///< If set, tap fires this instead of opening the internal modal
};

} // namespace helix::ui
