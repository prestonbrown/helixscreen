// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include "ams_types.h"

#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>

// Forward declaration
class AmsBackend;
class AmsContextMenuTestAccess;

namespace helix::ui {

/**
 * @file ui_ams_context_menu.h
 * @brief Context menu for AMS slot operations
 *
 * Displays a popup menu near a slot with options to load, unload,
 * edit, or assign a Spoolman spool. Automatically positions itself
 * relative to the target slot widget.
 *
 * Extends the generic ContextMenu with AMS-specific features:
 * - Slot loaded/can-load subjects for button states
 * - Tool mapping dropdown
 * - Endless spool backup dropdown
 *
 * ## Usage:
 * @code
 * helix::ui::AmsContextMenu menu;
 * menu.set_action_callback([](MenuAction action, int slot_index) {
 *     switch (action) {
 *         case MenuAction::LOAD: // load filament...
 *         case MenuAction::UNLOAD: // unload filament...
 *         case MenuAction::EDIT: // show edit modal...
 *         case MenuAction::SPOOLMAN: // show spoolman picker...
 *     }
 * });
 * menu.show_near_widget(parent, slot_index, slot_widget);
 * @endcode
 */
class AmsContextMenu : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(AmsContextMenu)

  public:
    /// Answers "may slot `candidate` stand in for slot `slot`?"
    ///
    /// Always AmsBackend::endless_spool_backup_eligibility() in production. Taken
    /// as a parameter by the two pure functions below purely so they are testable
    /// without a backend; the production call sites bind it to the virtual and to
    /// nothing else.
    using BackupEligibleFn =
        std::function<helix::printer::BackupEligibility(int slot, int candidate)>;

    /// Init and publish the two XML subjects this menu's layout binds. Idempotent,
    /// and called from the constructor, so production never needs it. Public for
    /// tests that build ams_context_menu.xml without a menu instance: the names
    /// must resolve before lv_xml_create(), or the state bindings are silently
    /// dropped.
    static void init_subjects();

    friend class ::AmsContextMenuTestAccess;

  public:
    enum class MenuAction {
        CANCELLED,        ///< User dismissed menu without action
        LOAD,             ///< Load filament from this slot
        UNLOAD,           ///< Unload filament from toolhead
        EJECT,            ///< Eject filament from lane (release spool)
        RECOVER_POSITION, ///< Retract filament stranded past the hub back into the lane
        SELECT_GATE,      ///< Select this gate as the active gate (Happy Hare)
        CHECK_GATE,       ///< Check filament state of this gate (Happy Hare)
        EDIT,             ///< Edit slot properties
        CLEAR_SPOOL,      ///< Clear assigned spool from empty slot
        SPOOLMAN,         ///< Assign Spoolman spool
        SCAN_QR           ///< Scan QR code to assign spool
    };

    using ActionCallback = std::function<void(MenuAction action, int slot_index)>;

    AmsContextMenu();
    ~AmsContextMenu() override;

    // Non-copyable
    AmsContextMenu(const AmsContextMenu&) = delete;
    AmsContextMenu& operator=(const AmsContextMenu&) = delete;

    // Movable
    AmsContextMenu(AmsContextMenu&& other) noexcept;
    AmsContextMenu& operator=(AmsContextMenu&& other) noexcept;

    /**
     * @brief Show context menu near a slot widget
     * @param parent Parent screen for the menu
     * @param slot_index Slot this menu is for (0-based)
     * @param near_widget Widget to position menu near (typically slot widget)
     * @param is_loaded True if the slot is loaded/active (enables Unload, suppresses Load)
     * @param backend Optional backend pointer for tool mapping/endless spool features
     * @return true if menu was shown successfully
     */
    bool show_near_widget(lv_obj_t* parent, int slot_index, lv_obj_t* near_widget,
                          bool is_loaded = false, AmsBackend* backend = nullptr);

    /**
     * @brief Show context menu for external spool (bypass/direct feed)
     *
     * Shows a reduced menu with only EDIT and CLEAR_SPOOL actions
     * (no LOAD/UNLOAD/EJECT since external spool is not managed by backend).
     *
     * @param parent Parent screen for the menu
     * @param anchor_widget Widget to position menu near (for click point)
     * @return true if menu was shown successfully
     */
    bool show_for_external_spool(lv_obj_t* parent, lv_obj_t* anchor_widget);

    /**
     * @brief Get slot index the menu is currently shown for
     */
    [[nodiscard]] int get_slot_index() const {
        return get_item_index();
    }

    /**
     * @brief Set callback for menu actions
     */
    void set_action_callback(ActionCallback callback);

  protected:
    const char* xml_component_name() const override {
        return "ams_context_menu";
    }
    void on_created(lv_obj_t* menu_obj) override;
    /// A tap outside a single-select action menu chooses nothing, so it reports
    /// CANCELLED through this menu's own callback rather than the base's.
    void on_backdrop_clicked() override;

  private:
    // === AMS-specific state ===
    ActionCallback action_callback_;

    /**
     * @brief Common pattern: hide, then invoke the callback with the slot index
     */
    void dispatch_ams_action(MenuAction action);

    // === Subjects for button enable/disable states ===
    //
    // Static, like BufferStatusModal's, and for the same two reasons. The XML
    // registry is keyed by name for the whole process, so per-instance storage
    // cannot work here: three owners construct an AmsContextMenu (AmsPanel,
    // AmsOverviewPanel, ExternalSpoolMenu) and all three publish the same two
    // names, so the last registration wins and the first owner to be destroyed
    // withdraws — or worse, silently outlives — a name the others still serve.
    // Before this was static, a destroyed menu left "ams_slot_can_load" pointing
    // into freed storage, and the next lv_xml_create() binding it wrote an
    // observer through a reused allocation (nightly ASan, 2026-08-16).
    //
    // Sharing the values across the three owners is correct rather than merely
    // tolerable: only one context menu is on screen at a time, and both values
    // are set in on_created() immediately before the menu is shown.
    static lv_subject_t slot_is_loaded_subject_; ///< 1 = loaded (Unload enabled), 0 = not loaded
    static lv_subject_t slot_can_load_subject_;  ///< 1 = has filament (Load enabled), 0 = empty
    static bool subjects_initialized_;

    // === Backend reference for dropdown operations ===
    AmsBackend* backend_ = nullptr;
    int total_slots_ = 0;

    // === Dropdown widget pointers ===
    lv_obj_t* tool_dropdown_ = nullptr;
    lv_obj_t* backup_dropdown_ = nullptr;

    // === Pending state for on_created ===
    // True when the slot is loaded/active per backend->can_unload_from_toolhead().
    // Gates BOTH the Unload action (enabled) and the Load action (suppressed) —
    // a slot the firmware considers seated should not offer Load.
    bool pending_is_loaded_ = false;

    /// Which operation the Unload button performs for the open slot. Selected in
    /// on_created() from live backend state; drives both label and dispatch.
    enum class UnloadMode {
        Unload,          ///< Heated unload from the toolhead
        RecoverPosition, ///< Retract filament stranded past the hub (AFC)
        Eject,           ///< Cold retract of lane filament to the spool
        ForceEject,      ///< Presence-ignoring retract of an empty lane (AD5X)
        Unavailable,     ///< Nothing to do for this slot
    };
    UnloadMode unload_mode_ = UnloadMode::Unavailable;

    bool external_spool_mode_ = false; ///< True when showing menu for external spool (bypass)

    // === Event Handlers ===
    void handle_load();
    void handle_unload();
    void handle_gate_select();
    void handle_gate_check();
    void handle_edit();
    void handle_clear_spool();
    void handle_spoolman();
    void handle_scan_qr();
    void handle_tool_changed();
    void handle_backup_changed();

    // === Dropdown Configuration ===
    void configure_dropdowns();
    void populate_tool_dropdown();
    void populate_backup_dropdown();
    std::string build_tool_options() const;
    std::string build_backup_options() const;
    /// backend_->endless_spool_backup_eligibility() as a callable, or an
    /// always-eligible stub when there is no backend (matching the old code,
    /// which skipped every compatibility check in that case).
    BackupEligibleFn backend_eligible_fn() const;
    int get_current_tool_for_slot() const;
    int get_current_backup_for_slot() const;

    // === Static Callback Registration ===
    static void register_callbacks();
    // Pure: whether the context menu should offer "Clear Spool" for this slot.
    //
    // Deliberately independent of whether filament is physically present. The
    // affordance was previously gated on an EMPTY slot, so it disappeared as soon
    // as a spool went in — exactly when a stale assignment does damage, since that
    // is when the wrong metadata is printed with and when an edit aims a Spoolman
    // write at the previous spool. An empty lane's stale metadata is cosmetic.
    static bool should_show_clear_spool(const SlotInfo& slot);

    // Pure: should the endless-spool backup row be shown at all?
    //
    // Availability alone is not enough. A read-only backend still earns the row
    // (greyed out) so the user can SEE the backup the firmware is using - but
    // only when the backend actually reports a per-slot relation to show. CFS is
    // available and read-only with no per-slot mapping whatsoever (the box picks
    // the refill spool from its own material groups), and it used to reach here
    // with an empty config: the dropdown then read "None" forever, which is
    // indistinguishable from "no backup configured".
    //
    // @param caps         The backend's capabilities.
    // @param has_relation Whether get_endless_spool_config() reported anything.
    static bool decide_show_backup_row(const helix::printer::EndlessSpoolCapabilities& caps,
                                       bool has_relation);

    // Pure: the backup dropdown's option list, "(incompatible)"-tagged.
    //
    // The eligibility rule is the BACKEND's, reached through
    // AmsBackend::endless_spool_backup_eligibility(). This used to call
    // filament::are_materials_compatible() directly, which meant AD5X IFS's
    // stricter firmware rule (exact material AND exact colour AND port present)
    // could never reach the label, and no backend had any say. The base virtual
    // IS the old material-compatibility rule, so AFC / Happy Hare / CFS options
    // are byte-identical to before.
    //
    // @param total_slots Number of slots to offer.
    // @param item_index  The slot the menu is open on; skipped in the list.
    // @param eligible    The backend's rule.
    // @return Newline-separated dropdown options, starting with "None".
    static std::string build_backup_options_for(int total_slots, int item_index,
                                                const BackupEligibleFn& eligible);

    // Pure: should the change-handler refuse this selection?
    //
    // Same rule as the option label, so a tagged option and a refused write can
    // never disagree. "None" (backup < 0) is always allowed - clearing a backup
    // needs no compatibility.
    static bool decide_backup_refused(int item_index, int backup_slot,
                                      const BackupEligibleFn& eligible);

    // Pure: selects the Unload button's operation for the open slot.
    //
    // Order encodes a deliberate priority ruling (see call site in on_created()):
    // a confidently-attributed stranded lane outranks Eject, but an unattributed
    // one (some backends share one physical sensor across every lane on a unit,
    // so "can recover" can be true for every lane at once with no way to say
    // whose filament tripped it) defers to Eject so a seated lane keeps its
    // Eject button — the unattributed Recover only catches lanes with nothing
    // left to eject.
    //
    // @param toolhead_unload      Slot unloads via the heated toolhead path
    // @param can_recover          backend_->can_recover_lane_position(slot_index)
    // @param recovery_attributed  backend_->lane_recovery_is_attributed()
    // @param supports_eject       backend_->supports_lane_eject()
    // @param slot_has_filament    SlotInfo::is_present() for this slot
    // @param supports_force_eject backend_->supports_force_eject()
    // @param slot_empty           !slot_has_filament
    static UnloadMode decide_unload_mode(bool toolhead_unload, bool can_recover,
                                         bool recovery_attributed, bool supports_eject,
                                         bool slot_has_filament, bool supports_force_eject,
                                         bool slot_empty);

    // Pure: whether the Load button is offered for the open slot.
    //
    // A thin adapter over helix::ui::compute_op_button_gating() — the one rule
    // the filament panel and the AMS sidebar answer from too. Kept as a named
    // predicate because the menu's inputs need translating: `toolhead_unload` is
    // the narrowed loaded signal (not the broadened recovery one), and presence
    // arrives as a tri-state so an UNKNOWN lane is not read as empty.
    //
    // `print_blocks_op` is helix::ui::print_blocks_filament_op(), the mirror of
    // AmsSubscriptionBackend::refuse_if_printing(). Do NOT pass the raw
    // print_active subject: PRINTING always refuses, but a PAUSED print now
    // ALLOWS the op on every backend whose filament macro does not home itself
    // (only AD5X IFS does). Greying the paused case is the bug in both
    // directions — offering what will be refused strands a runout-paused user
    // (bundle JX2FVRB9), and refusing what the backend accepts hides the
    // pause-then-swap recovery Klipper just told them to perform.
    static bool decide_can_load(bool system_busy, bool toolhead_unload,
                                std::optional<bool> slot_has_filament, bool print_blocks_op);

    // Pure: whether the Unload button is offered for the open slot.
    //
    // Only the heated toolhead unload is subject to the print gate; the cold lane
    // ops (Eject / RecoverPosition / ForceEject) do not move the toolhead and the
    // backend permits them via check_preconditions(false), which never consults
    // print state at all. Blocking the whole button would over-refuse and strand
    // filament the user could have ejected. That asymmetry is expressed to the
    // shared rule as OpButtonState::unload_is_cold_lane_op.
    //
    // `print_blocks_op`: see decide_can_load above — the computed predicate, not
    // the raw print_active subject.
    //
    // `cold_ops_print_gated` is AmsBackend::cold_lane_ops_refused_during_print():
    // true on a backend whose firmware refuses the cold ops mid-print too (AFC's
    // cmd_LANE_UNLOAD has its own is_printing() guard), which withdraws the
    // exemption above rather than offering a button into a certain refusal.
    // Required, not defaulted — a silently omitted `false` re-offers exactly the
    // dead-end button this parameter exists to remove.
    static bool decide_unload_enabled(bool system_busy, UnloadMode mode, bool print_blocks_op,
                                      bool cold_ops_print_gated);

    static bool callbacks_registered_;

    // === Static Callbacks ===
    /// The menu on screen as an AmsContextMenu, or nullptr. Thin wrapper over
    /// ContextMenu::active_as() that also logs the unexpected empty case.
    static AmsContextMenu* get_active_instance();
    static void on_load_cb(lv_event_t* e);
    static void on_unload_cb(lv_event_t* e);
    static void on_gate_select_cb(lv_event_t* e);
    static void on_gate_check_cb(lv_event_t* e);
    static void on_edit_cb(lv_event_t* e);
    static void on_clear_spool_cb(lv_event_t* e);
    static void on_spoolman_cb(lv_event_t* e);
    static void on_scan_qr_cb(lv_event_t* e);
    static void on_tool_changed_cb(lv_event_t* e);
    static void on_backup_changed_cb(lv_event_t* e);
};

} // namespace helix::ui
