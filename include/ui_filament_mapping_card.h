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
 * detected AND bypass is not suppressing a single-lane print AND the file
 * uses enough tools to be worth showing (any tool on a multi-tool printer,
 * 2+ on a single extruder) — see should_show(). Whether tapping the card
 * opens anything is a separate question, editable or not: see
 * PrintSelectDetailView::color_card_opens_remap().
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
     * Caches whether the card should be visible (`should_show()`) by running
     * `recompute_visibility()` - AMS availability, the bypass single-lane rule,
     * and the multi-tool-vs-single-extruder tool-count rule, all of which read
     * the current used-tool set. Computes default mappings via
     * FilamentMapper::compute_defaults().
     *
     * **The caller must publish `should_show()` onto the
     * `filament_mapping_visible` subject after this returns.** The card never
     * touches a subject itself. `set_used_tools()` carries the same obligation
     * - it is the other entry point that re-decides visibility - so a caller
     * that runs both publishes once, after the later of the two.
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
     *
     * Visibility is NOT re-decided here (nothing it refreshes is an input to
     * the rule), so unlike update() and set_used_tools() this needs no publish.
     * It does honour the current answer: a card whose `should_show()` is false
     * ends up with no chips rather than a hidden set of stale ones.
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
     * The tool→slot MAPPINGS are preserved — no re-seed (mirrors
     * refresh_slot_data), so a user's manual remap survives.
     *
     * **VISIBILITY IS RE-DECIDED, and the caller must publish it.** `used` is an
     * input to both lane-count rules in `recompute_visibility()`, so the precise
     * set arriving here can flip the answer `update()` could only reach from the
     * slicer palette: a bypassed print of a 3-colour file that really uses one
     * tool wants ONE lane, and the bypass rule hides the card for it. A caller
     * that skips the publish leaves `filament_mapping_visible` describing the
     * palette's answer while the card describes the file's — silently, because
     * nothing about the widget tree looks wrong. `PrintSelectDetailView` does
     * this from `publish_card_visibility()`, called AFTER every
     * `set_used_tools()`.
     *
     * `nullopt` OR an empty set ⇒ no filter (show all), and the same fallback
     * inside the visibility rules. This is the safety rule: it avoids blanking
     * the card pre-parse, and avoids the headless single-extruder case (where
     * the used set is empty forever) hiding everything.
     */
    void set_used_tools(std::optional<std::set<int>> used);

    /**
     * @brief Get current tool-to-slot mappings
     */
    [[nodiscard]] std::vector<helix::ToolMapping> get_mappings() const {
        return mappings_;
    }

    /**
     * @brief Replace the current tool→slot mappings and repaint.
     *
     * The single mapping-store writer. Stores, re-renders the chips, then fires
     * on_mappings_changed_ so downstream consumers (preview colours, pre-flight
     * gate) re-evaluate. The repaint is not optional: the lane number inside each
     * chip is written imperatively during rebuild_compact_view(), so a store
     * without a rebuild leaves the pre-remap lane on screen while the print runs
     * the new one. Safe to call before create() — rebuild_compact_view() returns
     * early when rows_container_ is null.
     */
    void set_mappings(std::vector<helix::ToolMapping> mappings) {
        mappings_ = std::move(mappings);
        rebuild_compact_view();
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
     * True iff AMS is available, bypass is not suppressing a single-lane
     * print, the file declares at least one tool, and the tool count clears
     * the multi-tool-printer-aware floor (any tool on a multi-tool printer,
     * 2+ tools on a single extruder). No longer gated on any backend's tool
     * mapping being editable — a second surface used to draw the chips on
     * non-editable backends (Snapmaker U1, ACE); it is gone, so hiding here
     * would show the user nothing. Whether a tap does anything is a separate
     * question, answered by PrintSelectDetailView::color_card_opens_remap().
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

    /// build_tool_info(), narrowed to the tools a file actually prints with.
    ///
    /// Pure/stateless. Keeps each kept entry's REAL gcode tool number in
    /// `.tool_index` (not its position in the result), because the colour and
    /// pre-flight paths downstream index by tool number: a print that uses only
    /// T0 and T2 must not have T2's colour land at index 1. A tool with no
    /// palette entry is dropped — the slicer palette bounds what can be known.
    ///
    /// Shared by PrintSelectDetailView::get_used_tool_info() and
    /// PrintStatusPanel::build_print_tool_info(), which are the file-browser and
    /// live-print halves of the same question and were literal copies of this
    /// loop. They must agree: the two previews colour the same print.
    ///
    /// @param colors    Per-tool slicer palette ("#RRGGBB"), palette-ordinal indexed.
    /// @param materials Per-tool material names, same indexing.
    /// @param used      Tool numbers the file prints with (empty ⇒ empty result).
    static std::vector<helix::GcodeToolInfo>
    build_used_tool_info(const std::vector<std::string>& colors,
                         const std::vector<std::string>& materials, const std::set<int>& used);

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

    /// Fixed on-screen width of one chip, in pixels. Mirrors the width
    /// `filament_swatch.xml` documents; set from C++ because a numeric width on a
    /// component `<view>` root is not honoured by `lv_xml_create`.
    static constexpr int32_t CHIP_WIDTH = 40;

    /// Chips to assume fit before layout has settled and the row can be measured.
    /// `filament_mapping_rows` measures 181px on the narrowest supported screen
    /// (480x272), and holds 4 chips there for every `space_xs` the theme hands
    /// out (the token is breakpoint-scaled: 2px at 480x272, 5px at 800x480), so 4
    /// is the floor that is safe everywhere. Measured, not derived.
    static constexpr size_t MIN_VISIBLE_CHIPS = 4;

    /// How many chips fit in one row `content_width` px wide with `gap` px
    /// between them; never less than 1.
    ///
    /// Pure/stateless. The chips are drawn in a single non-wrapping row of fixed
    /// height, so everything past the right edge is clipped rather than wrapped —
    /// the card caps what it draws at this number and summarises the remainder in
    /// a "+N" pill. A non-positive width (layout not settled) yields
    /// MIN_VISIBLE_CHIPS. Exposed static so the arithmetic is testable without
    /// LVGL geometry.
    [[nodiscard]] static size_t chips_that_fit(int32_t content_width, int32_t gap);

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
    /// Palette sizes from the last update(), kept so set_used_tools() can re-run
    /// the visibility rule without a palette in hand. Deliberately NOT read off
    /// tool_info_, whose size apply_used_tools_filter() shrinks. The two differ
    /// when a slicer reports materials and no colours (OrcaSlicer on a K2 Plus),
    /// and the two gates ask different questions, so both are kept.
    size_t palette_colour_count_ = 0; ///< gcode_colors.size()
    size_t palette_tool_count_ = 0;   ///< max(colors, materials) = build_tool_info()'s count

    /**
     * @brief Recompute `should_show_` from used_tools_ + the stored palette counts.
     *
     * The single copy of the visibility rule. Called by update() (which stores
     * the counts first) and by set_used_tools() (where the precise used-tool set
     * can flip the answer the palette gave). Pure decision — no widget or
     * mapping side effects — so either caller can run it before doing its work.
     *
     * @return the new value of should_show_, so update() can early-out on false.
     */
    bool recompute_visibility();

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
