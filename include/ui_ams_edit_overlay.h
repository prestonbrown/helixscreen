// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_color_picker.h"
#include "ui_filament_catalog_selector.h"
#include "ui_modal.h"

#include "ams_types.h"
#include "overlay_base.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "subject_managed_panel.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hv/json.hpp"

class IMoonrakerAPI;
class AmsEditOverlayTestAccess;
class AmsEditOverlayViewTestAccess;

namespace helix::ui {

/**
 * @file ui_ams_edit_overlay.h
 * @brief NavigationManager overlay for editing AMS filament slot properties
 *
 * Single overlay hosting internal views selected by the ams_edit_view int
 * subject (spec §13): 0=overview, 1=Spoolman spool picker, 2=unified spool-edit
 * (identity + color + logistics), 3=color. Header back button routes per-view;
 * header Save action is dirty-gated via the ams_edit_save_disabled subject.
 *
 * Lifecycle: on_deactivate() fires both when the overlay is POPPED and when it
 * is merely COVERED (e.g. QR scanner pushed on top). Completion therefore
 * fires only from Save / back-on-overview (guarded by completion_fired_), with
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

    // View states for the ams_edit_view subject (spec §13 / review §3)
    static constexpr int VIEW_OVERVIEW = 0;
    static constexpr int VIEW_SPOOL_PICKER = 1;
    static constexpr int VIEW_SPOOL_EDIT = 2; // unified identity + color + logistics
    static constexpr int VIEW_COLOR = 3;

    AmsEditOverlay();
    ~AmsEditOverlay() override;

    // Non-copyable, non-movable (singleton; move ops deliberately not ported)
    AmsEditOverlay(const AmsEditOverlay&) = delete;
    AmsEditOverlay& operator=(const AmsEditOverlay&) = delete;

    // === OverlayBase interface ===
    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void register_callbacks() override;
    [[nodiscard]] const char* get_name() const override {
        return "AMS Slot Editor";
    }
    void on_activate() override;
    void on_deactivate() override;
    void on_ui_destroyed() override;
    /// The catalog selector's options, the color swatch and the logistics field
    /// text are populated on view entry, not by the XML — re-apply them for the
    /// view that is showing when a hot-reload rebuilds the tree.
    void repopulate() override;

    /**
     * @brief Open the editor for a specific slot (pushes the overlay)
     * @param parent Parent screen (fallback if no active screen)
     * @param slot_index Slot being edited (0-based; -2 = external spool)
     * @param initial_info Initial slot info to populate the overview
     * @param api IMoonrakerAPI for Spoolman sync (may be nullptr)
     * @param on_complete Fired exactly once when the editor closes
     * @param open_on_picker Open directly on the Spoolman picker (#1071)
     * @return true if the overlay was pushed
     */
    bool show_for_slot(lv_obj_t* parent, int slot_index, const SlotInfo& initial_info,
                       IMoonrakerAPI* api, CompletionCallback on_complete,
                       bool open_on_picker = false);

  private:
    // === State ===
    int slot_index_ = -1;
    SlotInfo original_info_; ///< Original info for dirty comparison
    SlotInfo working_info_;  ///< Working copy being edited
    // Explicit "Save to Spoolman" opt-in captured from the filament-details
    // toggle (spec §3.3). Reset per show_for_slot; false = local-only save.
    bool save_to_spoolman_opt_in_ = false;
    // Whether the slot was Spoolman-tracked when the spool-edit view was
    // entered. Captured in enter_spool_edit() before any unlink zeroes
    // spoolman_id, so handle_spool_edit_save() can tell a genuine untracked
    // slot (commit local weight overrides) apart from a just-unlinked one
    // (skip weight staging — the on-screen fields were Spoolman's, and the
    // slot's weights are already correct). See Finding 1.
    bool spool_edit_entered_tracked_ = false;
    // True when the editor was opened directly on the Spoolman picker
    // (context-menu "Select spool", #1071). In that mode a picker selection is
    // a one-tap commit: apply the spool, then commit_and_close() the whole
    // overlay instead of returning to the overview (task #13). Cleared once the
    // picker is re-entered the normal way (Change Filament -> switch_to_picker),
    // which restores today's return-to-overview behavior.
    bool opened_on_picker_ = false;
    IMoonrakerAPI* api_ = nullptr;
    CompletionCallback completion_callback_;
    bool completion_fired_ = false; ///< Guards single-fire completion

    /// Cached overlay widget for lazy_create_and_push_overlay
    lv_obj_t* cached_overlay_widget_ = nullptr;

    // === Spool logistics (managed-slot fields inside VIEW_SPOOL_EDIT) ===
    SpoolInfo detail_original_; ///< as fetched on view entry
    SpoolInfo detail_working_;  ///< live field edits

    void populate_detail_fields();
    void read_detail_fields();
    static void on_detail_field_changed_cb(lv_event_t* e);

    // === Spool-edit view (VIEW_SPOOL_EDIT): identity + color + logistics ===
    FilamentCatalogSelector details_selector_;
    uint32_t details_color_ = 0;     ///< pending color chosen in the details view
    bool details_color_set_ = false; ///< true once the user picked a color there

    // === Color view (VIEW_COLOR): one screen, presets + custom HSV ===
    // Always returns to the spool-edit view — the only entry point is the
    // spool-edit "custom color" pencil (the overview swatch was retired in
    // Task 6), so there is no return-view state to track.
    uint32_t custom_color_ = 0x808080;

    void open_color_view();
    /// Paint the HSV picker, preview swatch and hex field from custom_color_.
    void populate_color_view();
    void apply_color(uint32_t rgb);
    void handle_color_swatch(lv_obj_t* swatch);
    void handle_custom_color_changed(uint32_t rgb);
    void handle_color_hex_changed();
    void handle_color_apply();
    static void on_color_swatch_cb(lv_event_t* e);
    static void on_color_apply_cb(lv_event_t* e);
    static void on_color_hex_changed_cb(lv_event_t* e);

    // === Subjects for XML binding ===
    SubjectManager subjects_;
    lv_subject_t slot_indicator_subject_;
    lv_subject_t temp_nozzle_subject_;
    lv_subject_t temp_bed_subject_;
    lv_subject_t remaining_pct_subject_;
    lv_subject_t view_mode_subject_;     ///< kView* ("ams_edit_view")
    lv_subject_t picker_state_subject_;  ///< 0=loading, 1=empty, 2=content
    lv_subject_t save_disabled_subject_; ///< 1=Save disabled ("ams_edit_save_disabled")
    lv_subject_t save_hidden_subject_;   ///< 1=header Save hidden ("ams_edit_save_hidden")
    lv_subject_t is_managed_subject_;    ///< 1=linked Spoolman spool ("ams_edit_is_managed")
    lv_subject_t chip_text_subject_;     ///< card identity label text
    lv_subject_t spoolman_id_subject_;   ///< "#19" beside the Spoolman mark, "" when untracked
    char chip_text_buf_[96] = {0};
    char spoolman_id_buf_[16] = {0};

    char slot_indicator_buf_[32] = {0};
    char temp_nozzle_buf_[32] = {0};
    char temp_bed_buf_[24] = {0};
    char remaining_pct_buf_[48] = {0};

    // === Picker state (Spoolman spool selection) ===
    std::vector<SpoolInfo> cached_spools_;

    // === Internal Methods ===
    void deinit_subjects();
    void update_ui();
    void update_temp_display();
    bool is_dirty() const;
    void update_sync_button_state();
    lv_obj_t* find_widget(const char* name) const;

    // === View switching ===
    void
    set_view(int view); ///< Sole writer of view_mode_subject_; also drives save_hidden_subject_
    void switch_to_picker();
    void switch_to_form();
    void populate_picker();
    void render_spool_list(const std::string& filter);
    void handle_spool_selected(int spool_id);
    void enter_spool_edit();
    /// Apply the spool-edit view's non-XML content (catalog selector options,
    /// color swatch, logistics fields) from state this object already holds.
    /// @return false if the catalog fragment is missing from the tree
    bool populate_spool_edit_view();
    // Apply the spool-edit view's identity/color/logistics edits to the slot.
    // finish=true (header Save) completes + closes the editor on a successful
    // apply; finish=false returns to the overview (tests / non-terminal calls).
    void handle_spool_edit_save(bool finish = false);
    void handle_quick_swatch(lv_obj_t* swatch);
    void handle_picker_search(const char* text);
    void update_spoolman_button_state();

    // === Completion / close orchestration ===
    void fire_completion(bool saved); ///< Idempotent; does NOT pop the overlay
    void close_editor(bool saved);    ///< fire_completion + NavigationManager go_back
    // The overview Save commit path: sync active spool, push Spoolman changes
    // (SpoolmanSlotSaver), then fire_completion + close. Shared by the header
    // Save on the overview AND the spool-edit "finish" path.
    void commit_and_close();

    friend class ::AmsEditOverlayTestAccess;
    friend class ::AmsEditOverlayViewTestAccess;

    // === Event Handlers ===
    void handle_back();            ///< Header back: per-view routing (cancel on overview)
    void handle_card_clicked();    ///< Card tap: opens the spool-edit view
    void handle_change_filament(); ///< Row tap: picker (Spoolman) or spool-edit
    void handle_setup_entry();
    void handle_save();

    // Pure decision for whether handle_save() should create a NEW Spoolman
    // spool from the working slot: only for an unlinked slot, only when the
    // user explicitly opted in via the "Save to Spoolman" toggle (spec §3.3 —
    // replaces the silent auto-create / user-edit heuristic, #1071), and only
    // when the metadata is complete.
    static bool should_create_new_spool(const SlotInfo& working_info, bool save_to_spoolman);

    static bool is_material_identity_change(const SlotInfo& original, const SlotInfo& edited);

    // Which weight fields the untracked branch of handle_spool_edit_save() may
    // stage into working_info_.
    struct WeightStaging {
        bool stage_remaining = false;
        bool stage_total = false;
    };

    // Pure. `entered_tracked` is spool_edit_entered_tracked_ — whether the editor
    // was OPENED on a Spoolman-linked slot; the *_filled flags are "the textarea
    // is non-empty" (blank means unchanged).
    //
    // remaining_weight_g is unambiguous: it means the same thing whether or not
    // the slot arrived linked, so a filled field always stages. Dropping it on an
    // unlink-in-place is what suppressed AFC's SET_WEIGHT (gated on
    // remaining_weight_g > 0) and forced the user to save a second time.
    //
    // total_weight_g is ambiguous: when the editor was opened on a LINKED slot the
    // on-screen "Spool wt" came from Spoolman's spool_weight — the empty-spool CORE
    // weight, not the filament total — so staging it would clobber a correct total.
    static WeightStaging decide_weight_staging(bool entered_tracked, bool remaining_filled,
                                               bool total_filled);

    // Pure: true when saving would push a changed filament identity onto a
    // LINKED Spoolman spool — material change or color beyond match tolerance
    // on a slot that stays linked. Generalized to ALL Spoolman backends
    // (spec §6; formerly AD5X-only).
    static bool needs_identity_confirmation(const SlotInfo& original, const SlotInfo& edited);

    // Pure. False when this save is going to raise the "Different filament?"
    // prompt, and therefore must not write to Spoolman yet.
    //
    // The logistics two-PATCH in handle_spool_edit_save() runs well before
    // commit_and_close() evaluates the prompt, so without this gate a save that
    // was about to ask had already written — and Cancel, documented as a true
    // abort, could not take it back.
    static bool may_write_spoolman_now(const SlotInfo& original, const SlotInfo& edited);

    /// @param intent What the user chose in the identity prompt. Defaults to the
    /// legacy inference for the paths that never prompt.
    void do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent intent =
                              helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked);
    void prompt_identity_change_then_save();
    /// Primary action: "It's a new spool" — create + rebind, old spool untouched.
    static void on_identity_confirm_cb(lv_event_t* e);
    static void on_identity_cancel_cb(lv_event_t* e);
    // Re-bind + repopulate details_selector_ against the still-open spool-edit
    // view's fragment. handle_spool_edit_save() unconditionally detaches +
    // clears the selector before reaching commit_and_close() (needed so the
    // async Spoolman-write paths and eventual overlay teardown never touch a
    // dangling registry entry) — safe whenever that flow was going to leave
    // the view, but the identity-confirm Cancel abort does NOT leave the
    // view, so the now-inert selector must be brought back to life or
    // vendor/type/product picking silently stops working.
    void reattach_details_selector();
    // Bind + configure + populate details_selector_ against the spool-edit
    // view's fragment (find + attach + configure + preselect). Shared by
    // enter_spool_edit() and reattach_details_selector(). Returns false (and
    // logs) when the fragment is missing so the caller can bail. Does NOT
    // seed the pending color — enter_spool_edit() does that itself, and the
    // reattach path deliberately must not (re-seeding would make the
    // "Different filament?" dialog vanish on re-Save).
    bool setup_details_selector();
    // When Spoolman is connected, fetch the live vendor list and merge it into
    // details_selector_ so a Spoolman-only vendor (one present on the server but
    // absent from the bundled filaments.json catalog) reaches the vendor
    // dropdown and a seeded brand round-trips instead of snapping to "Generic".
    // No-op when Spoolman is not connected. The fetch is async (HTTP/bg thread):
    // the dropdown is catalog-only until the vendors arrive, then re-populates
    // and re-runs the seed selection. Keeps the selector Spoolman-agnostic — the
    // fetch lives here and only hands the selector a list of vendor names.
    void maybe_merge_spoolman_vendors();
#if HELIX_HAS_LABEL_PRINTER
    void handle_print_label();
#endif
    void handle_scan_qr();
    void handle_tool_changed(int index);

    // === Static Callback Registration ===
    static bool callbacks_registered_;

    // === Static Callbacks ===
    static void on_back_cb(lv_event_t* e);
    static void on_card_clicked_cb(lv_event_t* e);
    static void on_change_filament_cb(lv_event_t* e);
    static void on_setup_entry_cb(lv_event_t* e);
    static void on_quick_swatch_cb(lv_event_t* e);
    static void on_custom_color_cb(lv_event_t* e);
    static void on_save_cb(lv_event_t* e);
    static void on_print_label_cb(lv_event_t* e);
    static void on_scan_qr_cb(lv_event_t* e);
    static void on_picker_search_cb(lv_event_t* e);
    static void on_picker_retry_cb(lv_event_t* e);
    static void on_spool_item_cb(lv_event_t* e);
    static void on_spool_item_edit_cb(lv_event_t* e);
    static void on_tool_changed_cb(lv_event_t* e);

    /**
     * @brief Resolve the editor instance for a static XML callback.
     * The editor is a process-lifetime singleton, so this is just the accessor
     * (replaces the modal's s_active_instance_ routing).
     */
    static AmsEditOverlay* get_instance_from_event(lv_event_t* e);
};

/**
 * @brief Global instance accessor (creates on first use, registers cleanup
 *        with StaticPanelRegistry)
 */
AmsEditOverlay& get_ams_edit_overlay();

} // namespace helix::ui
