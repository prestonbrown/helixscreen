// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_color_picker.h"
#include "ui_filament_catalog_picker.h"
#include "ui_modal.h"
#include "ui_search_debounce.h"

#include "ams_tag_reading.h"
#include "ams_types.h"
#include "core/static_panel_registry.h"
#include "filament_catalog.h"
#include "helix/xml/indexed_subject_pool.h"
#include "overlay_base.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "subject_managed_panel.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class IMoonrakerAPI;
}
class AmsEditOverlayTestAccess;
class AmsEditOverlayViewTestAccess;

namespace helix::ui {

/**
 * @file ui_ams_edit_overlay.h
 * @brief NavigationManager overlay for editing AMS filament slot properties
 *
 * Single overlay hosting internal views selected by the ams_edit_view int
 * subject: 0=editor (the identity form; the entry view), 1=Spoolman spool picker,
 * 2=color. Header back button routes per-view; header Save action is
 * dirty-gated via the ams_edit_save_disabled subject.
 *
 * The editor view is a cascading dropdown form: Material family, Variant,
 * Brand, Product (each list ends in a
 * browse-the-catalog entry), plus a colour
 * grid, a weight pill (numeric keypad) and a collapsed More section. Below
 * them sit the lane's own settings, Assigned Tool and the Endless Spool
 * backup, each shown only where the backend supports it.
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
    static constexpr int kViewSpoolPicker = 1;
    static constexpr int kViewColor = 2;
    /// ams_edit_color_selected value that rings the grid's custom swatch.
    static constexpr int kCustomColorIndex = -2;

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
    /// The tree (dropdown form, preset swatches, picker, colour view) is large
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
     * @param open_on_picker Open directly on the Spoolman picker (#1071)
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

    /// Presets a colour grid shows: whole rows of @p tile at @p gap across
    /// @p grid_width, @p rows of them, less the custom swatch's tile, capped at
    /// the palette.
    static int grid_preset_count(int grid_width, int tile, int gap, int rows, int palette_size);

    /// Whether @p info carries a colour someone chose: not the unset grey,
    /// and not the bare black some firmware reports for an empty slot.
    static bool slot_has_colour(const SlotInfo& info);

    /// The ams_edit_color_selected value for @p rgb: the index of the first of
    /// @p visible_presets palette entries it matches, kCustomColorIndex for a
    /// set colour none of them holds, -1 when no colour is set.
    static int color_ring_index(uint32_t rgb, bool colour_set, int visible_presets,
                                const std::vector<uint32_t>& palette);

    /// Whether the colour view shows its presets and its custom picker at once
    /// in an @p avail_w x @p avail_h content box: side by side, or stacked, with
    /// @p gap between them. False means one pane at a time.
    static bool color_panes_fit(int avail_w, int avail_h, int presets_w, int presets_h,
                                int custom_w, int custom_h, int gap);

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
    /// True while the editor was opened directly on the Spoolman picker: a
    /// selection there is a one-tap commit.
    bool opened_on_picker_ = false;
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
    /// The editor view after the picker or colour view: entered on first use,
    /// otherwise shown with the staged edits intact.
    void return_to_editor();
    /// After a relink or a QR scan put a different spool under the editor.
    void load_swapped_spool();
    /// The logistics PATCH rides an Update-in-Spoolman save when the identity
    /// prompt owns this save: set by handle_save, consumed by
    /// do_spoolman_save's success path.
    bool logistics_pending_ = false;

    // === Colour view (kViewColor): presets + custom HSV ===
    uint32_t custom_color_ = 0x808080;
    void open_color_view();
    void populate_color_view();
    void apply_color(uint32_t rgb);
    void handle_color_swatch(lv_obj_t* swatch);
    void handle_custom_color_changed(uint32_t rgb);
    void handle_color_hex_changed();
    void handle_color_apply();
    /// Publish whether the colour view's two panes fit together
    /// (ams_edit_color_fits). Measuring shows both panes, which rebuilds the
    /// preset grid, so it measures only room it has not measured before.
    void update_color_fit();
    void show_color_pane(int pane);

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
    /// Fill both layouts' dropdown widgets from the option vectors.
    void refresh_dropdowns();
    void fill_dropdown(const char* stacked_name, const char* split_name,
                       const std::vector<Option>& options_list,
                       const std::string& selected_payload);
    void rebuild_option_lists();
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
    lv_subject_t title_subject_{};     ///< "Gate 3" (lane label)
    lv_subject_t view_mode_subject_{}; ///< kView* ("ams_edit_view")
    lv_subject_t
        picker_state_subject_{}; ///< 0=loading, 1=fetch error (retry), 2=content, 3=no match
    lv_subject_t save_disabled_subject_{};   ///< 1=Save disabled ("ams_edit_save_disabled")
    lv_subject_t save_hidden_subject_{};     ///< 1=header Save hidden ("ams_edit_save_hidden")
    lv_subject_t is_managed_subject_{};      ///< 1=linked Spoolman spool ("ams_edit_is_managed")
    lv_subject_t scan_enabled_subject_{};    ///< 1=QR button may show ("ams_edit_scan_enabled")
    lv_subject_t product_hidden_subject_{};  ///< 1 while Generic selected
    lv_subject_t color_selected_subject_{};  ///< ringed preset index (-1 none)
    lv_subject_t color_col_count_subject_{}; ///< split-column presets (size_color_grids)
    lv_subject_t color_row_count_subject_{}; ///< stacked-grid presets (size_color_grids)
    lv_subject_t color_fits_subject_{};      ///< 1=presets and picker shown together
    lv_subject_t color_pane_subject_{};      ///< one-pane form: 0=presets, 1=picker
    int color_fit_room_w_ = -1;              ///< room update_color_fit last measured
    int color_fit_room_h_ = -1;
    bool color_fit_ = true; ///< that room's verdict
    lv_subject_t summary_text_subject_{};
    lv_subject_t tag_badge_visible_subject_{}; ///< any shown field is the tag's
    lv_subject_t tag_material_subject_{};
    lv_subject_t tag_brand_subject_{};
    lv_subject_t tag_product_subject_{};
    lv_subject_t tag_color_subject_{};
    lv_subject_t tag_temps_subject_{};
    /// The temperatures the temp line shows, for the temps badge.
    int shown_nozzle_min_ = 0;
    int shown_nozzle_max_ = 0;
    int shown_bed_temp_ = 0;
    lv_subject_t temp_text_subject_{};
    lv_subject_t weight_text_subject_{};
    lv_subject_t weight_hint_subject_{};
    lv_subject_t reset_tag_visible_subject_{};
    lv_subject_t add_spoolman_visible_subject_{};
    lv_subject_t clear_visible_subject_{}; ///< 1 = the slot holds a spool to clear
    lv_subject_t more_open_subject_{};
    lv_subject_t spoolman_id_subject_{};        ///< "#19" for the header badge
    lv_subject_t lane_visible_subject_{};       ///< 1 = the slot is a backend lane
    lv_subject_t tool_row_visible_subject_{};   ///< 1 = Assigned Tool row shows
    lv_subject_t backup_row_visible_subject_{}; ///< 1 = Endless Spool row shows
    lv_subject_t backup_editable_subject_{};    ///< 0 = backup shown read-only
    lv_subject_t backup_label_subject_{};       ///< "Backup Gate", in the backend's noun
    char title_buf_[96] = {0};
    char summary_buf_[96] = {0};
    char temp_buf_[96] = {0};
    char weight_buf_[24] = {0};
    char weight_hint_buf_[32] = {0};
    char spoolman_id_buf_[16] = {0};
    char backup_label_buf_[64] = {0};

    // === Picker state (Spoolman spool selection) ===
    std::vector<SpoolInfo> cached_spools_;
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
    void update_sync_button_state();
    void update_temp_display();
    bool is_dirty() const;
    bool selection_dirty() const;
    lv_obj_t* find_widget(const char* name) const;

    // === View switching ===
    void set_view(int view);
    void switch_to_picker();
    void populate_picker();
    void render_spool_list(const std::string& filter);
    void handle_spool_selected(int spool_id);
    void enter_editor();
    /// Apply the editor view's non-XML content (dropdown options, colour ring,
    /// summary, logistics fields) from state this object already holds.
    void populate_editor_view();
    /// Set both colour grids' preset counts from their measured width.
    void size_color_grids();
    /// Presets the showing colour grid displays.
    int visible_preset_count();
    void stage_selection_into_working();
    /// Copy a colour picked in the editor into working_info_, dropping any
    /// stored colour name.
    void stage_pending_color_into_working();
    void handle_quick_swatch(lv_obj_t* swatch);
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
        SlotInfo original; ///< the diff base
        SlotInfo saving;   ///< the slot this save commits
        CompletionCallback owed;
        nlohmann::json chained_spool_patch; ///< More fields riding the Update arm
        int chained_spool_id = 0;
        bool more_saved = false; ///< a More PATCH ran ahead of this commit and landed
        bool stay_open = false;  ///< commit the slot but keep the editor (unsaved More edits)
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
    /// Close that editor with @p committed, or hand it to @p cap's callback.
    void deliver_commit(const SaveCapture& cap, const SlotInfo& committed);
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
    void handle_add_to_spoolman(); ///< Quiet link: create + label
    void handle_reset_to_tag();    ///< Reset every tag-stated field to the tag
    void handle_clear_spool();     ///< Take the spool off the slot and close

    // Pure decision for whether handle_save() should create a NEW Spoolman
    // spool from the working slot: only via the explicit "Add to Spoolman"
    // link (the switch is gone), and only when the metadata is complete.
    static bool should_create_new_spool(const SlotInfo& working_info, bool add_requested);

    // Whether Save is unavailable, from the three things that decide it.
    [[nodiscard]] static bool save_is_disabled(int view, bool save_in_flight, bool dirty);

    /// Whether the slot's brand and material are the linked spool's and cannot
    /// be written right now (Spoolman offline).
    [[nodiscard]] static bool identity_locked(const SlotInfo& original, const SlotInfo& working,
                                              bool spoolman_available);

    static bool is_material_identity_change(const SlotInfo& original, const SlotInfo& edited);

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

    // A save that wrote nothing leaves the editor where it was.
    void stay_open_after_failed_save();

    // Name the first field a Spoolman filament needs and this slot does not
    // carry. One sentence per field, never a list.
    void show_missing_filament_toast(const helix::MissingFilamentFields& missing);
#if HELIX_HAS_LABEL_PRINTER
    void handle_print_label();
#endif
    void handle_scan_qr();
};

inline AmsEditOverlay& get_ams_edit_overlay() {
    return lazy_global<AmsEditOverlay>("AmsEditOverlay");
}

} // namespace helix::ui
