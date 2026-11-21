// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_filament_catalog_picker.h"
#include "ui_modal.h"
#include "ui_search_debounce.h"

#include "ams_tag_reading.h"
#include "ams_types.h"
#include "core/observer_guard.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "filament_catalog.h"
#include "helix/xml/indexed_subject_pool.h"
#include "lane_source_store.h"
#include "lane_spool_history.h"
#include "overlay_base.h"
#include "spool_assign.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "subject_managed_panel.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "hv/json.hpp" // JSON_HPP_OK: chained_spool_patch held by value

namespace helix {
class AmsBackend;
class IMoonrakerAPI;
} // namespace helix
class AmsEditOverlayTestAccess;
class AmsEditOverlayViewTestAccess;

namespace helix::ui {

/**
 * @file ui_ams_edit_overlay.h
 * @brief NavigationManager overlay for editing AMS filament slot properties
 *
 * Single overlay hosting internal views selected by the ams_edit_view int
 * subject: 0=editor (the entry view); colors open the color page
 * (ColorPickerPage) over it. The editor does two jobs,
 * picked by the ams_edit_mode subject (helix::spool_assign::screen_for_lane):
 *
 * - Assign Spool (kModeAssign, a lane with no spool): the describe form on the
 *   left, the user's spools on the right (recents, or the ones matching the
 *   form, exact before close), each with Use; at the bottom Add to Spoolman &
 *   Assign (or Assign without Spoolman), and common spools on a fresh install.
 * - Edit Spool (kModeEdit, a lane holding one): the same form, prefilled, and
 *   the spool with its remaining weight, Save and Clear Lane on the right.
 *   Different Spool switches to Assign.
 *
 * The describe form is a cascading dropdown grid: Material family, Type,
 * Brand, Product (each list ends in a browse-the-catalog entry), a color grid
 * ending in Custom, and the spool size chips. Below them sit the lane's own
 * settings, Assigned Tool and the Endless Spool backup, each shown only where
 * the backend supports it, and the collapsed More section of a linked spool.
 * Selection state lives in SpoolEditSelection; Save stages it into the
 * working SlotInfo. Tag-sourced fields show a badge and reset through
 * helix::ams::reset_to_tag.
 *
 * Lifecycle: on_deactivate() fires both when the overlay is POPPED and when it
 * is merely COVERED (e.g. QR scanner pushed on top). Completion therefore
 * fires only from Save / back-on-editor (guarded by completion_fired_), with
 * a NavigationManager close callback as the backdrop-tap safety net.
 *
 * ## Usage:
 * @code
 * helix::ui::get_ams_edit_overlay().show_for_slot(
 *     parent, slot_index, slot_info, api,
 *     [](const helix::ui::AmsEditOverlay::EditResult& result) {
 *         if (result.saved) { ... apply result.slot_info to backend ... }
 *     });
 * @endcode
 */
class AmsEditOverlay : public OverlayBase {
  public:
    /**
     * @brief Result returned when the editor closes
     */
    struct EditResult {
        bool saved = false;  ///< True if user saved, false if cancelled/dismissed
        int slot_index = -1; ///< Slot that was edited
        SlotInfo slot_info;  ///< Final slot info (valid if saved)
    };

    using CompletionCallback = std::function<void(const EditResult& result)>;

    // View states for the ams_edit_view subject
    static constexpr int kViewEditor = 0;
    // The editor's job, the ams_edit_mode subject.
    static constexpr int kModeAssign = 0;
    static constexpr int kModeEdit = 1;

    /**
     * @brief The identity selection the editor is staging
     *
     * family/variant/brand are option labels; product_id is a catalog id (""
     * = no product pick). An unknown value (a firmware/Spoolman material the
     * catalog lacks) is kept verbatim in family/brand so an untouched
     * open+Save round-trips it.
     */
    struct SpoolEditSelection {
        std::string family;     ///< "PLA" (display_family spelling)
        std::string variant;    ///< "Basic", "CF", "Silk", ...
        std::string brand;      ///< "Generic" when nothing set
        std::string product_id; ///< catalog product id, "" when none

        bool operator==(const SpoolEditSelection& o) const {
            return family == o.family && variant == o.variant && brand == o.brand &&
                   product_id == o.product_id;
        }
    };

    AmsEditOverlay();
    ~AmsEditOverlay() override;

    // Non-copyable, non-movable (singleton; move ops deliberately not ported)
    AmsEditOverlay(const AmsEditOverlay&) = delete;
    AmsEditOverlay& operator=(const AmsEditOverlay&) = delete;

    // === OverlayBase interface ===
    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* xml_component() const override {
        return "ams_edit_overlay";
    }
    void register_callbacks() override;
    [[nodiscard]] const char* get_name() const override {
        return "AMS Slot Editor";
    }
    /// The tree (dropdown form, color row, spools list) is large
    /// and only needed while a spool is being edited.
    bool destroy_on_close() const override {
        return true;
    }
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void on_ui_destroyed() override;
    /// A close that bypassed Save and Back completes the session as "not saved".
    void on_dismissed() override;
    /// The dropdown options, the colour selection ring and the logistics field
    /// text are populated on view entry, not by the XML — re-apply them for
    /// the view that is showing when a hot-reload rebuilds the tree.
    void repopulate() override;

    /**
     * @brief Open the editor for a specific slot (pushes the overlay)
     * @param parent Parent screen (fallback if no active screen)
     * @param slot_index Slot being edited (0-based; -2 = external spool)
     * @param initial_info Initial slot info to populate the editor
     * @param api IMoonrakerAPI for Spoolman sync (may be nullptr)
     * @param on_complete Fired exactly once when the editor closes
     * @param open_on_picker Open on Assign Spool whatever the lane holds (#1071)
     * @return true if the overlay was pushed
     */
    bool show_for_slot(lv_obj_t* parent, int slot_index, const SlotInfo& initial_info,
                       IMoonrakerAPI* api, CompletionCallback on_complete,
                       bool open_on_picker = false);

    // === Pure selection decisions (exposed for tests) ===

    /// Split a slot's material/brand/product into the selection. Unknown
    /// values pass through verbatim (they show as their own selected option).
    static SpoolEditSelection selection_from_slot(const SlotInfo& info);

    /// Rule: changing the family keeps the brand when it makes the new
    /// material, else Generic; the variant resets to Basic and a product that
    /// no longer matches is dropped.
    static SpoolEditSelection apply_family_change(const SpoolEditSelection& sel,
                                                  const std::string& new_family,
                                                  const printer::FilamentCatalog& catalog);

    /// The type string a selection emits: the catalog's sample type for the
    /// (family, variant) pair when the catalog carries one, else the composed
    /// fallback. Never empty for a non-empty family.
    static std::string selected_type(const SpoolEditSelection& sel,
                                     const printer::FilamentCatalog& catalog);

    /// Whether @p info carries a colour someone chose: not the unset grey,
    /// and not the bare black some firmware reports for an empty slot.
    static bool slot_has_colour(const SlotInfo& info);

    /// The most colors a multi-color spool shows as dots, and takes from "+".
    /// A longer list keeps its later colors; they are not shown as dots.
    static constexpr size_t kMaxColors = 4;

    /// "#RRGGBB" entries of a comma-separated color list, unparseable ones dropped.
    static std::vector<uint32_t> split_color_list(const std::string& csv);
    /// The list back as "#RRGGBB,#RRGGBB".
    static std::string join_color_list(const std::vector<uint32_t>& colors);

    /// The filament recents the color row offers beside @p current: the
    /// newest first, @p current left out, at most @p room of them.
    static std::vector<uint32_t> color_row_recents(const std::vector<uint32_t>& recents,
                                                   uint32_t current, size_t room);

    /// Recent chips that fit in a @p row_width row of @p chip tiles @p gap
    /// apart beside @p fixed_tiles other tiles, at most @p cap.
    static size_t recents_room(int row_width, int fixed_tiles, int chip, int gap, size_t cap);

  private:
    // === State ===
    int slot_index_ = -1;
    SlotInfo original_info_; ///< Original info for dirty comparison
    SlotInfo working_info_;  ///< Working copy being edited

    /// Dropdown selection state; staged into working_info_ by handle_save().
    SpoolEditSelection sel_;
    /// True once the user moved a dropdown (family/variant/brand/product) or the
    /// colour: Save rewrites identity only then, so an untouched open+save
    /// changes nothing.
    bool selection_moved_ = false;
    /// True once Reset to tag restored the identity this session: a spool
    /// record landing later must not overwrite it.
    bool identity_reset_ = false;
    uint32_t details_color_ = 0;     ///< pending colour
    bool details_color_set_ = false; ///< true once the user picked a colour
    /// The slot's tag reading, refreshed on open; drives the from-tag badge
    /// and Reset to tag.
    std::optional<ams::TagReading> tag_;
    /// Recents from Spoolman history (empty until the fetch lands / without
    /// Spoolman): first-seen order over the recency-sorted spool list.
    std::vector<std::string> recent_families_;
    std::vector<std::string> recent_brands_;
    /// Assign Spool's spool size: the chip lit (spool_assign size chips) and
    /// the grams it stands for, staged onto the lane by an assign.
    int size_chip_ = 0;
    float size_remaining_g_ = spool_assign::kDefaultSpoolGrams;
    float size_total_g_ = spool_assign::kDefaultSpoolGrams;
    /// The Spoolman spool this session's save created, for the closing toast.
    int created_spool_id_ = 0;
    /// This session's save rewrote a linked Spoolman record, so the closing
    /// toast offers no Undo (the lane alone cannot be put back).
    bool edited_spoolman_record_ = false;
    /// Assign Spool's list load is in flight; a cover that drops it re-arms it.
    bool picker_pending_ = false;
    /// The lane history behind cached_spools_ (without Spoolman), index for index.
    std::vector<lane_spool_history::Entry> history_entries_;
    /// What the rendered list was built from, so a form tap that changes
    /// nothing the list depends on does not rebuild it.
    struct ListedQuery {
        spool_assign::SpoolQuery query;
        std::string filter;
        size_t spool_count = 0;
        std::set<int> in_use; ///< spools other lanes link: their rows offer no Use
        bool operator==(const ListedQuery& o) const;
    };
    /// The spools other lanes link, which this lane cannot take.
    [[nodiscard]] std::set<int> spools_in_other_lanes() const;
    std::optional<ListedQuery> listed_query_;
    IMoonrakerAPI* api_ = nullptr;
    CompletionCallback completion_callback_;
    bool completion_fired_ = false; ///< Guards single-fire completion
    // A Spoolman save is in flight. The editor stays open while it runs, so a
    // second tap on Save is reachable and would start a second save. Back,
    // the weight keypad and the scanner stay shut too: the save's completion
    // closes the editor, and must not pop something pushed on top of it.
    bool save_in_flight_ = false;
    /// Bumped by every show_for_slot(). Save completions run on
    /// object_lifetime_ (they are owed to Spoolman and the slot, not to the
    /// screen) and finish from a SaveCapture: a save commits the slot it
    /// captured through the callback it began with, editor open or not. Only
    /// the editor-side follow-ups (closing it, keeping it open on a failure,
    /// the identity prompt) check the session is still the one showing.
    uint32_t session_ = 0;
    /// True once the editor view has been populated this session. A picker-entry
    /// session reaches the editor first through enter_editor(); later returns
    /// keep the staged edits.
    bool editor_entered_ = false;
    /// The linked spool's record fetch: requested and not yet landed (cleared
    /// when a cover drops it), and the spool id whose record did land.
    bool linked_fetch_pending_ = false;
    int details_spool_id_ = 0;
    bool recents_pending_ = false;
    bool recents_loaded_ = false;
    // Spool logistics fields (More section) for the linked spool record.
    SpoolInfo detail_original_; ///< as fetched on entry
    SpoolInfo detail_working_;  ///< live field edits
    /// The catalog the dropdowns read; loaded per open.
    std::optional<printer::FilamentCatalog> catalog_;

    void populate_detail_fields();
    void read_detail_fields();
    /// Re-seed detail_original_/detail_working_ from working_info_ for the
    /// spool it currently names - on open, and after a picker relink or a QR
    /// scan swaps the spool underneath the editor. The fields show blank until
    /// that spool's record lands.
    void reseed_details_for_spool();
    /// Fetch the linked spool's record once: it feeds both the identity sync
    /// and the More fields.
    void fetch_linked_spool();
    /// Apply a fetched record, unless the slot no longer names that spool. A
    /// selection or field the user already changed is kept.
    void apply_linked_spool(int spool_id, const SpoolInfo& spool);
    /// The editor view after the spools list: entered on first use,
    /// otherwise shown with the staged edits intact.
    void return_to_editor();
    /// After a QR scan put a different spool under the editor.
    void load_swapped_spool();
    /// A scanned spool's record staged onto the lane, for review on Edit Spool.
    void apply_scanned_spool(const SpoolInfo& spool);
    /// The logistics PATCH rides an Update-in-Spoolman save when the identity
    /// prompt owns this save: set by handle_save, consumed by
    /// do_spoolman_save's success path.
    bool logistics_pending_ = false;

    // === Color row and color page ===
    /// The color the editor shows: the pending pick, else the slot's.
    uint32_t pending_color() const;
    /// Push the color page (ColorPickerPage) editing the draft color live.
    void open_color_page();
    /// Stage @p rgb as the draft color (a recent chip, a page pick).
    void apply_color(uint32_t rgb);
    void handle_recent_color(int index);
    /// Publish the color line and row: name, current swatch, recents.
    void update_color_row();
    std::vector<uint32_t> color_row_;
    /// The color row's content width, chip and gap, as size_color_row()
    /// last measured them; 0 until it has.
    int row_width_ = 0;
    int row_chip_ = 0;
    int row_gap_ = 0;
    /// Measure the color row and publish its recents.
    void size_color_row();
    /// Recent chips the row has room for beside what the draft shows.
    size_t recent_room() const;

    // === Multi-color draft ===
    // The draft's colors: the pending pick alone, or two or more. A
    // multi-color edit lives in draft_multi_/draft_mode_ while multi_set_, and
    // reaches working_info_ only on Save, like a single pick.
    std::vector<uint32_t> draft_colors() const;
    bool draft_is_multi() const {
        return draft_colors().size() >= 2;
    }
    /// The draft mode, "gradient" or "coaxial", for a multi-color draft.
    std::string draft_mode() const;
    /// Whether this lane can store several colors (AmsBackend::stores_multi_color,
    /// or a lane linked to a Spoolman spool).
    bool multi_color_capable() const;
    /// A multi-color draft the lane can store: its colors show as dots to edit.
    /// Elsewhere a multi-color spool shows read-only and a pick replaces it.
    bool multi_color_editable() const {
        return draft_is_multi() && multi_color_capable();
    }
    /// How many of the draft's colors show as dots: at most kMaxColors.
    int shown_colors() const {
        return static_cast<int>(std::min(draft_colors().size(), kMaxColors));
    }
    /// Put @p picked in the draft, each through helix::ams::picked_color():
    /// two or more stay a list, one becomes the single pending color.
    void set_draft_colors(const std::vector<uint32_t>& picked);
    void handle_seg_tap(int index);
    void handle_seg_hold(int index);
    void add_color();
    void handle_color_mode(int mode_index);
    /// Back to the slot's own colors: no pick, no multi-color edit.
    void reset_color_draft();
    bool multi_set_ = false;
    std::string draft_multi_;
    std::string draft_mode_;
    int selected_seg_ = 0;

    // === Identity dropdowns ===
    /// One option's label and the value picking it stages.
    struct Option {
        std::string label;
        std::string payload; ///< family / variant / brand / catalog id
        bool more = false;   ///< the trailing "Browse catalog..." entry
    };
    std::vector<Option> material_options_;
    std::vector<Option> variant_options_;
    std::vector<Option> brand_options_;
    std::vector<Option> product_options_;
    /// Fill the dropdown widgets from the option vectors.
    void refresh_dropdowns();
    void fill_dropdown(const char* name, const std::vector<Option>& options_list,
                       const std::string& selected_payload);
    void rebuild_option_lists();
    /// The materials the active filament system's firmware accepts
    /// (AmsBackend::get_supported_materials()), or nullopt for any material:
    /// no backend, or one that publishes no list or an empty one.
    [[nodiscard]] static std::optional<std::vector<std::string>> firmware_materials();
    void handle_option_selected(int row, int index);

    // === Lane settings: Assigned Tool and Endless Spool backup ===
    /// The lane's standing settings. Not part of the slot's identity: they are
    /// written to the backend through helix::ui::lane_settings, on Save.
    struct LaneSettings {
        int tool = -1;   ///< tool mapped to this lane, -1 for none
        int backup = -1; ///< the lane's Endless Spool backup, -1 for none
    };
    LaneSettings lane_original_; ///< as the backend held them on open
    LaneSettings lane_working_;  ///< as the dropdowns stage them
    /// The backup dropdown's slots after "None" (lane_settings::backup_candidates()).
    std::vector<int> backup_candidates_;
    int lane_tool_count_ = 0;
    /// On open: read the lane's settings from the backend and decide which rows show.
    void load_lane_settings();
    /// Fill both dropdowns from the staged settings.
    void populate_lane_dropdowns();
    void handle_tool_selected(int index);
    void handle_backup_selected(int index);
    /// A staged change Save would write. "None" for the tool writes nothing: a
    /// mapping is cleared by mapping that tool elsewhere.
    [[nodiscard]] bool lane_settings_dirty() const;
    /// Write the staged changes through the backend, taking each one that was
    /// written as the lane's setting. False when a write was refused: that
    /// field stays staged, so the next Save retries it.
    [[nodiscard]] bool commit_lane_settings();

    // === Subjects for XML binding ===
    SubjectManager subjects_;
    helix::Subject title_subject_{};          ///< "Assign Spool" / "Edit Spool"
    helix::Subject subtitle_subject_{};       ///< "Lane 3 · Turtle 1" ("ams_edit_subtitle")
    helix::Subject mode_subject_{};           ///< kMode* ("ams_edit_mode")
    helix::Subject list_title_subject_{};     ///< "Recent Spools" ("ams_edit_list_title")
    helix::Subject common_visible_subject_{}; ///< 1 = the common spools grid shows
    helix::Subject
        bottom_action_subject_{};        ///< spool_assign::BottomAction ("ams_edit_bottom_action")
    helix::Subject size_chip_subject_{}; ///< the lit size chip ("ams_edit_size_chip")
    helix::Subject partial_text_subject_{}; ///< "Partial…" or "400 g" ("ams_edit_partial_text")
    helix::Subject saves_to_subject_{};     ///< "Saves to Spoolman spool #12" ("ams_edit_saves_to")
    char subtitle_buf_[128] = {0};
    char list_title_buf_[48] = {0};
    char partial_text_buf_[24] = {0};
    char saves_to_buf_[64] = {0};
    helix::Subject view_mode_subject_{}; ///< kView* ("ams_edit_view")
    /// The spools list: 0=loading, 1=fetch error (retry), 2=content, 3=nothing
    /// matches, 4=no spools at all ("edit_picker_state").
    helix::Subject picker_state_subject_{};
    helix::Subject save_disabled_subject_{};  ///< 1=Save disabled ("ams_edit_save_disabled")
    helix::Subject is_managed_subject_{};     ///< 1=linked Spoolman spool ("ams_edit_is_managed")
    helix::Subject scan_enabled_subject_{};   ///< 1=QR button may show ("ams_edit_scan_enabled")
    helix::Subject product_hidden_subject_{}; ///< 1 while Generic selected
    helix::Subject color_current_subject_{};  ///< the color row's current swatch
    helix::Subject color_current_multi_subject_{}; ///< its multi-color CSV, empty for one
    helix::Subject color_name_subject_{};          ///< the color line's name
    helix::Subject recent_count_subject_{};        ///< recent chips shown in the row
    helix::Subject multi_subject_{};               ///< 1 while the draft's colors show as dots
    helix::Subject multi_capable_subject_{};       ///< 1 where the lane can store several
    helix::Subject seg_count_subject_{};           ///< color dots shown
    helix::Subject seg_selected_subject_{};        ///< the ringed dot
    helix::Subject seg_label_subject_{};           ///< "Color 2: Gold" in multi mode
    helix::Subject color_mode_subject_{};          ///< dropdown: 0 gradient, 1 coaxial
    char seg_label_buf_[64]{};
    char color_name_buf_[64]{};
    char color_current_multi_buf_[128]{};
    helix::Subject tag_badge_visible_subject_{}; ///< any shown field is the tag's
    helix::Subject tag_material_subject_{};
    helix::Subject tag_brand_subject_{};
    helix::Subject tag_product_subject_{};
    helix::Subject tag_color_subject_{};
    helix::Subject tag_temps_subject_{};
    /// The temperatures the temp line shows, for the temps badge.
    int shown_nozzle_min_ = 0;
    int shown_nozzle_max_ = 0;
    int shown_bed_temp_ = 0;
    helix::Subject temp_text_subject_{};
    helix::Subject weight_text_subject_{};
    helix::Subject weight_hint_subject_{};
    helix::Subject reset_tag_visible_subject_{};
    helix::Subject clear_visible_subject_{}; ///< 1 = the slot holds a spool to clear
    helix::Subject more_open_subject_{};
    helix::Subject lane_open_subject_{};    ///< 1 = Lane Settings disclosed ("ams_edit_lane_open")
    helix::Subject spoolman_id_subject_{};  ///< "#19" for the header badge
    helix::Subject lane_visible_subject_{}; ///< 1 = the slot is a backend lane
    helix::Subject tool_row_visible_subject_{};   ///< 1 = Assigned Tool row shows
    helix::Subject tool_editable_subject_{};      ///< 0 = tool shown read-only
    helix::Subject tool_refusal_subject_{};       ///< why the tool cannot be changed here
    helix::Subject backup_row_visible_subject_{}; ///< 1 = Endless Spool row shows
    helix::Subject backup_editable_subject_{};    ///< 0 = backup shown read-only
    helix::Subject backup_label_subject_{};       ///< "Backup Gate", in the backend's noun
    char title_buf_[96] = {0};
    /// The save's progress, bound in XML ("ams_edit_save_state"): kSaveIdle,
    /// kSaveWaiting while the commit waits for Moonraker to hold it,
    /// kSaveFailed with save_error_subject_ saying why.
    helix::Subject save_state_subject_{};
    helix::Subject save_error_subject_{}; ///< "ams_edit_save_error"
    /// The line a waiting save shows ("ams_edit_save_wait_text"): what the
    /// filament system is doing ahead of it, else "Saving...".
    helix::Subject save_wait_text_subject_{};
    char save_wait_text_buf_[160] = {0};
    /// Follows the filament system's action while a save waits, so the line
    /// names the operation the save is queued behind.
    ObserverGuard save_wait_action_observer_;
    /// 1 = the lane and its linked spool disagree ("ams_edit_spool_disagrees")
    helix::Subject spool_disagrees_subject_{};
    helix::Subject spool_disagreement_text_subject_{}; ///< "ams_edit_spool_disagreement_text"
    char spool_disagreement_buf_[128] = {0};
    struct FormSnapshot {
        SlotInfo working;
        SpoolEditSelection selection;
        bool selection_moved = false;
        uint32_t details_color = 0;
        bool details_color_set = false;
        bool multi_set = false;
        std::string draft_multi;
        std::string draft_mode;
    };
    std::optional<FormSnapshot> note_action_form_;
    char save_error_buf_[192] = {0};
    char temp_buf_[96] = {0};
    char weight_buf_[24] = {0};
    char weight_hint_buf_[128] = {0};
    char spoolman_id_buf_[16] = {0};
    char backup_label_buf_[64] = {0};
    char tool_refusal_buf_[160] = {0};

    // === The spools list (Assign Spool's right column) ===
    /// The user's spools, recency first: Spoolman's, or without it the lane
    /// history's (spools with no id).
    std::vector<SpoolInfo> cached_spools_;
    /// What cached_spools_ were read from: false = the lane history.
    bool spools_from_spoolman_ = false;
    /// The rows the list shows, into cached_spools_; a row's user_data is
    /// its position here.
    std::vector<spool_assign::RankedSpool> listed_;
    /// cached_spools_' searchable text, built once per fetch so a debounced
    /// re-filter does not rebuild+lowercase every spool's string again.
    std::vector<std::string> cached_searchables_;
    /// Debounce for the picker's search input.
    helix::ui::SearchDebounce picker_search_debounce_;
    /// The "Browse catalog..." sheet (created lazily, reused).
    std::unique_ptr<FilamentCatalogPickerModal> catalog_sheet_;

    // === Internal Methods ===
    void deinit_subjects();
    void update_ui();
    void update_summary();
    /// Title, subtitle and the Edit panel's lines for the current mode.
    void update_header_and_panel();
    /// What the describe form says now, as a spool query.
    [[nodiscard]] spool_assign::SpoolQuery current_query() const;
    void update_sync_button_state();
    void update_temp_display();
    bool is_dirty() const;
    bool selection_dirty() const;
    lv_obj_t* find_widget(const char* name) const;

    // === View switching ===
    void set_view(int view);
    void set_mode(int mode);
    /// Edit Spool's header Different Spool: the same lane, on Assign Spool.
    void handle_different_spool();
    /// Start Assign Spool from an empty form, whatever the lane held.
    void begin_fresh_assign();
    /// Back on an Assign Spool opened over a lane's spool returns to Edit
    /// Spool on it. False when there is no such spool to return to.
    bool back_to_edit_spool();
    /// Load the user's spools for the list: Spoolman's, else the lane history.
    void populate_picker();
    /// Rebuild the list from the cached spools, the form and the search text.
    void render_spool_list(const std::string& filter);
    /// Re-rank the list after a form change, keeping the search text.
    void refresh_spool_list();
    /// Use on the listed row @p row: link (or, from the lane history, describe)
    /// the lane with that spool and close.
    void handle_row_use(size_t row);
    /// A common spool tile: fill the form with its material and color.
    void handle_common_spool(size_t index);
    void handle_size_chip(int chip);
    void apply_size_chip(int chip, float remaining_g, float total_g);
    void handle_remaining_step(float delta_g);
    /// Assign without Spoolman, or without adding the spool to it.
    void handle_assign_local();
    /// Stage the size chips' grams onto the lane (Assign Spool only).
    void stage_size_into_working();
    /// Stage everything Assign Spool's form says onto the lane.
    void stage_assign_form();
    /// The toast an assign or edit closes with: "Lane 2: Polymaker PLA" and
    /// Undo, with the Spoolman id when the save created one.
    void show_assigned_toast(const SlotInfo& before, const SlotInfo& after, bool created,
                             bool undoable);
    /// Write the lane's staged settings before an Assign Spool action; false
    /// (editor kept open, refusal toasted) when the backend refused one.
    bool commit_lane_settings_for_assign();
    void enter_editor();
    /// Apply the editor view's non-XML content (dropdown options, colour ring,
    /// summary, logistics fields) from state this object already holds.
    void populate_editor_view();
    void stage_selection_into_working();
    /// Copy a colour picked in the editor into working_info_, dropping any
    /// stored colour name.
    void stage_pending_color_into_working();
    void handle_picker_search(const char* text);
    void fetch_recents();
    void refresh_tag_state();
    /// The slot as the form shows it now (staged selection, pending colour,
    /// the temperature line's values).
    SlotInfo shown_slot() const;
    /// Set each "from tag" badge from the tag's provenance over shown_slot().
    void update_tag_badges();

    // === Completion / close orchestration ===
    void fire_completion(bool saved); ///< Idempotent; does NOT pop the overlay
    void close_editor(bool saved);    ///< fire_completion + NavigationManager go_back
    void commit_and_close();

    /// Everything a save needs to finish after the editor is gone.
    struct SaveCapture {
        uint32_t session = 0;
        int slot_index = -1;
        SlotInfo original;        ///< the diff base
        SlotInfo saving;          ///< the slot this save commits
        SlotInfo spoolman_target; ///< what Spoolman is asked to hold
        CompletionCallback owed;
        nlohmann::json chained_spool_patch; ///< More fields riding the Update arm
        int chained_spool_id = 0;
        bool more_saved = false; ///< a More PATCH ran ahead of this commit and landed
        bool stay_open = false;  ///< commit the slot but keep the editor (unsaved More edits)
        std::vector<uint32_t> picked_colors; ///< the colors this save commits, if any were picked
    };
    SaveCapture capture_save() const;
    /// Saves still running per slot, editor open or not. show_for_slot()
    /// refuses a slot with one pending: its editor would show, and could save,
    /// the slot as it was before that save commits.
    std::map<int, int> pending_saves_;
    bool save_pending_for(int slot_index) const;
    void begin_slot_save(int slot_index);
    void end_slot_save(int slot_index);
    /// The editor that began @p cap is still showing it.
    bool save_is_live(const SaveCapture& cap) const;
    /// A committed save's picked colors join the filament recents, first leading.
    static void remember_saved_colors(const SaveCapture& cap);
    /// Close that editor with @p committed, or hand it to @p cap's callback.
    void deliver_commit(const SaveCapture& cap, const SlotInfo& committed);
    /// Hand @p committed to the live editor's callback, then wait for the lane
    /// to hold it: the editor closes on the echo and stays open with an inline
    /// error, keeping the user's values, when the commit is refused or its
    /// write fails or times out.
    void commit_then_await(const SaveCapture& cap, const SlotInfo& committed);
    void finish_awaited_save(const SaveCapture& cap, helix::ams::PendingOutcome outcome,
                             const std::string& reason);
    void set_save_state(int state, const std::string& error = {});
    void update_save_wait_text();
    /// The commit decision (offline lock, relink, Spoolman save, local) over a
    /// capture; commit_and_close() runs it over the current state.
    void commit_capture(const SaveCapture& cap);
    void run_spoolman_save(const SaveCapture& cap, helix::SpoolmanSlotSaver::LinkIntent intent);

    friend class ::AmsEditOverlayTestAccess;
    friend class ::AmsEditOverlayViewTestAccess;

    // === Event Handlers ===
    void handle_back(); ///< Header back: per-view routing
    void handle_save();
    void prompt_unsaved_changes();  ///< Back with unsaved edits: Save/Discard
    void handle_weight_tap();       ///< Weight pill → numeric keypad
    void apply_weight(float grams); ///< Keypad result → remaining weight
    void handle_more_toggle();
    void handle_add_to_spoolman();          ///< Add to Spoolman & Assign
    void handle_reset_to_tag();             ///< Reset every tag-stated field to the tag
    void handle_clear_spool();              ///< Take the spool off the slot and close
    void handle_reapply_linked_spool();     ///< Write the linked spool's identity to the lane
    void handle_unlink_disagreeing_spool(); ///< Keep the lane's identity, drop the link

    // Pure decision for whether handle_save() should create a NEW Spoolman
    // spool from the working slot: only via the explicit "Add to Spoolman"
    // link (the switch is gone), and only when the metadata is complete.
    static bool should_create_new_spool(const SlotInfo& working_info, bool add_requested);

    static constexpr int kSaveIdle = 0;
    static constexpr int kSaveWaiting = 1;
    static constexpr int kSaveFailed = 2;
    /// What a waiting save is queued behind: the load, unload or other
    /// filament operation @p info reports, naming the head or lane it works
    /// on. Empty while the filament system is idle.
    [[nodiscard]] static std::string save_wait_reason(const AmsBackend& backend,
                                                      const AmsSystemInfo& info);
    /// The inline line a save that did not stick shows under Save.
    [[nodiscard]] static std::string not_saved_message(helix::ams::PendingOutcome outcome,
                                                       const std::string& reason);

    // Whether Save is unavailable, from the three things that decide it.
    [[nodiscard]] static bool save_is_disabled(int view, bool save_in_flight, bool dirty);

    /// Whether the slot's brand and material are the linked spool's and cannot
    /// be written right now (Spoolman offline).
    [[nodiscard]] static bool identity_locked(const SlotInfo& original, const SlotInfo& working,
                                              bool spoolman_available);

    static bool is_material_identity_change(const SlotInfo& original, const SlotInfo& edited);

    struct SpoolContradiction {
        bool material = false;
        bool color = false;
        [[nodiscard]] bool any() const {
            return material || color;
        }
    };

    // Pure: which of the lane's material and colour the spool's record states
    // differently. A field either side leaves unstated contradicts nothing.
    static SpoolContradiction contradiction_with_spool(const SlotInfo& lane,
                                                       const SpoolInfo& spool);

    // True when the lane's firmware owns its identity and the linked spool's
    // loaded record contradicts what the lane held when the editor opened.
    [[nodiscard]] bool lane_contradicts_linked_spool() const;

    // The two sides of a save as Spoolman sees them. On a lane contradicting
    // its spool, base holds the spool's identity and target holds that identity
    // with only the fields the user moved, so the lane's own values never reach
    // the spool. Elsewhere they are the original and the working slot.
    struct SpoolmanView {
        SlotInfo base;
        SlotInfo target;
    };
    [[nodiscard]] SpoolmanView spoolman_view() const;

    // A note action stages onto the form before saving. A save that does not
    // happen puts the form back on what it showed, so it matches the lane.
    void remember_form_for_note_action();
    void restore_form_after_note_action();

    void update_spool_disagreement();
    void apply_material_defaults(const std::string& material);

    // Pure: true when saving would push a changed filament identity onto a
    // LINKED Spoolman spool — material change or color beyond match
    // tolerance on a slot that stays linked.
    static bool needs_identity_confirmation(const SlotInfo& original, const SlotInfo& edited);

    // Pure. False when this save is going to raise the identity prompt, and
    // therefore must not write to Spoolman yet.
    static bool may_write_spoolman_now(const SlotInfo& original, const SlotInfo& edited);

    /// @param intent What the user chose in the identity prompt.
    void do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent intent =
                              helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked);
    void prompt_identity_change_then_save();

    /// Ask whether the save for @p slot_index should use @p filament_name, an
    /// existing Spoolman filament close to the one it would create; @p answer
    /// gets the reply exactly once (a dismissal is a Cancel).
    void
    offer_similar_filament(const std::string& filament_name, int slot_index,
                           std::function<void(helix::SpoolmanSlotSaver::SimilarChoice)> answer);

    // A save that wrote nothing leaves the editor where it was.
    void stay_open_after_failed_save();

    /// The first field a Spoolman filament needs and this slot does not
    /// carry, as one sentence, or nullptr.
    static const char* missing_filament_message(const helix::MissingFilamentFields& missing);
    /// A save that did not go through: the inline line under Save while the
    /// editor that began it is showing (@p live), a toast once it has closed.
    void report_save_problem(bool live, const char* message);
#if HELIX_HAS_LABEL_PRINTER
    void handle_print_label();
    /// The spool record a label for the lane's spool prints.
    [[nodiscard]] SpoolInfo label_spool() const;
    static void print_label_for(const SpoolInfo& spool_info);
#endif
    void handle_scan_qr();
};

inline AmsEditOverlay& get_ams_edit_overlay() {
    return lazy_global<AmsEditOverlay>("AmsEditOverlay");
}

} // namespace helix::ui
