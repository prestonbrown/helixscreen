// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_mapping_modal.h"
#include "ui_filament_slot_picker.h"

#include "core/observer_guard.h"
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
 * One chip per used G-code tool: the file's colour and material over the mapped
 * slot's spool and label, outlined by how well the two match
 * (helix::chip_severity). The chips are filament_swatch.xml repeated by
 * filament_chip_row.xml; the card only publishes the filament_chip_* subjects
 * they bind and creates no widget. Tapping the card opens the
 * FilamentMappingModal for full interaction.
 *
 * Visibility: declarative. The card itself does NOT toggle the LVGL HIDDEN
 * flag. After each `update()` it caches whether it should be visible
 * (`should_show()`); the print detail view publishes that on a subject the
 * XML binds via `bind_flag_if_eq` — see print_file_detail.xml's
 * `filament_mapping_visible` binding. Card visible iff AMS/toolchanger is
 * detected AND bypass is not suppressing a single-lane print AND the file
 * uses enough tools to be worth showing (any tool on a multi-tool printer,
 * 2+ on a single extruder), or, on a single extruder with no filament system,
 * when a used tool names a material (shows_loaded_for()) — see should_show().
 * Whether tapping the card
 * opens anything is a separate question, editable or not: see
 * PrintSelectDetailView::current_remap_block().
 */
class FilamentMappingCard {
  public:
    FilamentMappingCard() = default;
    ~FilamentMappingCard();

    // Non-copyable (holds LVGL widget pointers)
    FilamentMappingCard(const FilamentMappingCard&) = delete;
    FilamentMappingCard& operator=(const FilamentMappingCard&) = delete;

    /**
     * @brief Attach to XML widgets after instantiation
     *
     * @param card_widget The filament_mapping_card ui_card
     * @param rows_container The filament_mapping_rows container, measured for how many chips fit
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

    /// update() for a palette re-read of the file already shown. Same seeding,
    /// but the user's picks survive it.
    void refresh_palette(const std::vector<std::string>& gcode_colors,
                         const std::vector<std::string>& gcode_materials);

    /**
     * @brief Re-pull loaded slot colors/presence from AmsState and re-render.
     * Used by the detail view's live AMS-change handler.
     *
     * A mapping the user picked is kept; every other one is re-seeded from the
     * current lanes and routing, so the card shows what the printer will run and
     * an auto color match is never one made against lanes that have moved.
     *
     * Visibility IS re-decided first, since the slots feed the rule (the slot
     * count, availability, bypass), so the caller publishes afterwards. A card
     * whose `should_show()` turns false ends up with no chips rather than a
     * hidden set of stale ones; one that was hidden has no mappings to keep,
     * and the detail view rebuilds it with update() instead.
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
     * The tool→slot MAPPINGS are filtered, never re-seeded, so a user's
     * manual remap survives.
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

    /// The set last given to set_used_tools(); nullopt or empty is not known yet.
    [[nodiscard]] const std::optional<std::set<int>>& used_tools() const {
        return used_tools_;
    }

    /**
     * @brief Get current tool-to-slot mappings
     */
    [[nodiscard]] std::vector<helix::ToolMapping> get_mappings() const {
        return mappings_;
    }

    /**
     * @brief Replace the current tool→slot mappings and repaint.
     *
     * The single mapping-store writer. Stores, republishes the chips, then fires
     * on_mappings_changed_ so downstream consumers (preview colours, pre-flight
     * gate) re-evaluate. Safe to call before create(): publish_chips() returns
     * early when rows_container_ is null.
     */
    void set_mappings(std::vector<helix::ToolMapping> mappings) {
        mappings_ = std::move(mappings);
        publish_chips();
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

    using ChipTapCallback = std::function<void(size_t chip, lv_obj_t* chip_obj)>;

    /**
     * @brief Decide what a tap on one chip does.
     *
     * The print detail view routes it through its remap gate and calls
     * open_slot_picker() only when a remap is available. When unset, a chip tap
     * opens the picker directly.
     */
    void set_on_chip_tap(ChipTapCallback cb) {
        on_chip_tap_ = std::move(cb);
    }

    /**
     * @brief Open the slot dropdown for chip @p chip, anchored under @p trigger.
     *
     * Headed "T4 · ABS-GF", offering Auto, then every slot ranked by
     * rank_slots_for_tool() with its slot_fit_text(), then "More options…". A
     * slot or Auto is recorded through FilamentMapper::pick_slot() and stored
     * with set_mappings(); "More options…" opens the full mapping modal the way
     * a tap on the card does.
     */
    void open_slot_picker(size_t chip, lv_obj_t* trigger);

    using PickGate = std::function<bool()>;

    /// Asked when a dropdown pick lands, before it is recorded; false drops it.
    /// Unset records every pick.
    void set_pick_gate(PickGate gate) {
        pick_gate_ = std::move(gate);
    }

    /// Close the chip dropdown if it is open.
    void hide_slot_picker() {
        slot_picker_.hide();
    }

    /// The chip dropdown, for tests.
    [[nodiscard]] const FilamentSlotPicker& slot_picker() const {
        return slot_picker_;
    }

    /// filament_swatch.xml's chip tap: user_data is the chip index.
    static void chip_clicked_cb(lv_event_t* e);

    /**
     * @brief Whether the card should be visible after the latest `update()`
     *
     * True iff AMS is available, bypass is not suppressing a single-lane
     * print, the file declares at least one tool, and the tool count clears
     * the multi-tool-printer-aware floor (any tool on a multi-tool printer,
     * 2+ tools on a single extruder); or, with no filament system and one
     * extruder, a used tool names a material (shows_loaded()). Not gated on any
     * backend's tool mapping being editable: whether a tap does anything is a
     * separate question, answered by PrintSelectDetailView::current_remap_block().
     */
    [[nodiscard]] bool should_show() const {
        return should_show_;
    }

    /// Whether the card shows the loaded filament against the file instead of a
    /// lane mapping: a single extruder with no filament system. Its chips read
    /// "Loaded", "Swap" or "?" and offer no remap.
    [[nodiscard]] bool shows_loaded() const {
        return shows_loaded_;
    }

    /// The visibility rule for a single extruder with no filament system: true
    /// when a used tool (every tool, for an empty or nullopt set) names a
    /// material. Reads AmsState and ToolState.
    [[nodiscard]] static bool shows_loaded_for(const std::vector<std::string>& materials,
                                               const std::optional<std::set<int>>& used_tools);

    /// The lane-mapping visibility rule, without the card's widgets:
    /// AMS availability, the bypass single-lane rule and the lane count. A
    /// start with no file view asks it to know whether the view's card would
    /// hold a mapping for the same file.
    /// @param palette_colour_count Slicer colors in the palette
    /// @param palette_tool_count   max(colors, materials), build_tool_info()'s count
    /// @param used_tools           The scan's used tools; empty or nullopt reads as unknown
    [[nodiscard]] static bool shows_mapping_for(size_t palette_colour_count,
                                                size_t palette_tool_count,
                                                const std::optional<std::set<int>>& used_tools);

    /**
     * @brief Null widget pointers (called during destroy-on-close)
     */
    void on_ui_destroyed();

    /**
     * @brief Open the tool→slot filament mapping modal.
     *
     * Also invoked internally when the card itself is tapped. Exposed so the
     * pre-flight gate's "Map Filament…" button can reuse the exact same modal wiring
     * (data population + on_mappings_updated → on_mappings_changed_) for
     * native-routing backends rather than duplicating it.
     */
    void open_mapping_modal();

    /// What the card holds for a file whose mapping the user has not edited.
    struct Mapping {
        size_t palette_colour_count = 0; ///< colors.size()
        size_t palette_tool_count = 0;   ///< max(colors, materials), build_tool_info()'s count
        bool shows = false;              ///< shows_mapping_for() those counts and the used set
        std::vector<helix::GcodeToolInfo> tool_info; ///< narrowed to the used tools
        std::vector<helix::AvailableSlot> slots;     ///< empty unless shows
        std::vector<helix::ToolMapping> mappings;    ///< seeded, then narrowed; empty unless shows
    };

    /// The card's rule for a file, in its order: the tools from the palette,
    /// the visibility rule, the slots and the seeded mappings when it shows,
    /// then the used-tools filter. update() assigns from it, and a start with
    /// no file view (print_start_inputs.cpp) asks it, so the two cannot reach
    /// different gates for one file. Reads AmsState; no card state.
    [[nodiscard]] static Mapping compute_mapping(const std::vector<std::string>& colors,
                                                 const std::vector<std::string>& materials,
                                                 const std::optional<std::set<int>>& used_tools);

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

    /// Width of one chip when the #filament_chip_width token cannot be read
    /// (the theme is not up yet); the token's 800x480 value.
    static constexpr int32_t kChipWidth = 64;

    /// Chips the card has subjects for. A file using more tools shows the first
    /// ones and a "+N" pill for the rest.
    static constexpr size_t kMaxFilamentChips = 16;

    /// Register the filament_chip_* subjects filament_chip_row.xml binds. Must
    /// run before that XML is created; idempotent.
    static void init_subjects();

    /// Chips to assume fit before layout has settled and the row can be measured.
    /// `filament_mapping_rows` measures 181px on the narrowest supported screen
    /// (480x272), which holds three of that breakpoint's chips.
    static constexpr size_t kMinVisibleChips = 3;

    /// How many chips @p chip_width px wide fit in one row `content_width` px
    /// wide with `gap` px between them; never less than 1.
    ///
    /// Pure/stateless. The chips are drawn in a single non-wrapping row of fixed
    /// height, so everything past the right edge is clipped rather than wrapped —
    /// the card caps what it draws at this number and summarises the remainder in
    /// a "+N" pill. A non-positive width (layout not settled) yields
    /// kMinVisibleChips.
    [[nodiscard]] static size_t chips_that_fit(int32_t content_width, int32_t gap,
                                               int32_t chip_width = kChipWidth);

    /// Find a tool by its real gcode `.tool_index`, not by vector position.
    ///
    /// `tool_info` may be used-filtered (compacted), so position no longer
    /// equals `.tool_index`. Callers that have a `tool_index` (e.g. from a
    /// ToolMapping or an unresolved-tools list) MUST look up through here rather
    /// than `tool_info[tool_index]`. Returns nullptr if no entry matches.
    static const helix::GcodeToolInfo*
    find_by_tool_index(const std::vector<helix::GcodeToolInfo>& tool_info, int tool_index);

  private:
    /// Publish the filament_chip_* subjects for the current tools, mappings and
    /// slots: severity and colours are judged against the slots as they are now.
    void publish_chips();

    lv_obj_t* card_ = nullptr;
    lv_obj_t* rows_container_ = nullptr;
    lv_obj_t* warning_container_ = nullptr;

    bool should_show_ = false;  ///< Cached visibility intent — see should_show()
    bool shows_loaded_ = false; ///< should_show_ by shows_loaded_for(), not a mapping
    /// Palette sizes from the last update(), kept so set_used_tools() can re-run
    /// the visibility rule without a palette in hand. Deliberately NOT read off
    /// tool_info_, whose size apply_used_tools_filter() shrinks. The two differ
    /// when a slicer reports materials and no colours (OrcaSlicer on a K2 Plus),
    /// and the two gates ask different questions, so both are kept.
    size_t palette_colour_count_ = 0; ///< gcode_colors.size()
    size_t palette_tool_count_ = 0;   ///< max(colors, materials) = build_tool_info()'s count
    /// The palette from the last update(), so refresh_slot_data() can re-seed
    /// exactly as update() seeded.
    std::vector<std::string> palette_colors_;
    std::vector<std::string> palette_materials_;

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

    /// recompute_visibility(), clearing mappings_ and available_slots_ when the
    /// card is hidden or shows what is loaded. Every path that can hide the card
    /// or switch it to loaded goes through this. @return should_show_
    bool recompute_visibility_or_clear();

    /// Seed mappings_, available_slots_ and tool_info_ from the stored palette
    /// when the card maps lanes and holds no mappings (shown again after being
    /// hidden, or switched from showing what is loaded). @return true if seeded
    bool seed_if_unseeded();

    /// update()'s body. @p keep_picks: the user's picks laid over the fresh seed
    /// (empty for a new file).
    void seed_from_palette(const std::vector<std::string>& gcode_colors,
                           const std::vector<std::string>& gcode_materials,
                           const std::vector<helix::ToolMapping>& keep_picks);

    std::vector<helix::ToolMapping> mappings_;
    std::vector<helix::GcodeToolInfo> tool_info_;
    std::vector<helix::AvailableSlot> available_slots_;

    /// Tools the gcode actually uses (pushed post-parse by the detail view).
    /// nullopt / empty ⇒ show the full palette. Re-applied at the end of
    /// update() so a set pushed before a later update() survives the rebuild.
    std::optional<std::set<int>> used_tools_;

    /// Republishes the chips on a language switch: "Pick" is translated in C++.
    ObserverGuard language_observer_;

    /// The card's tap: on_tap_ when set, else the internal modal.
    void open_full_mapping();

    FilamentMappingModal mapping_modal_;
    FilamentSlotPicker slot_picker_;
    ChipTapCallback on_chip_tap_;
    PickGate pick_gate_;
    MappingsChangedCallback on_mappings_changed_;
    TapCallback on_tap_; ///< If set, tap fires this instead of opening the internal modal
};

} // namespace helix::ui
