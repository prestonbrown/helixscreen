// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_context_menu.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_toast_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_database.h"
#include "filament_op_slot_resolver.h"
#include "filament_variants.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::ui {

// Static member initialization
bool AmsContextMenu::callbacks_registered_ = false;
bool AmsContextMenu::subjects_initialized_ = false;
lv_subject_t AmsContextMenu::slot_is_loaded_subject_;
lv_subject_t AmsContextMenu::slot_can_load_subject_;

// ============================================================================
// Construction / Destruction
// ============================================================================

void AmsContextMenu::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_int(&slot_is_loaded_subject_, 0);
    lv_subject_init_int(&slot_can_load_subject_, 1);

    lv_xml_register_subject(nullptr, "ams_slot_is_loaded", &slot_is_loaded_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_can_load", &slot_can_load_subject_);

    subjects_initialized_ = true;
}

AmsContextMenu::AmsContextMenu() {
    init_subjects();
    spdlog::debug("[AmsContextMenu] Constructed");
}

AmsContextMenu::~AmsContextMenu() {
    // Subjects are static and stay registered for the process lifetime — see the
    // header. Deiniting them here is what left the XML registry resolving
    // "ams_slot_can_load" to dead storage once any one of the three owners went
    // away, whether or not another was still using the name.
    spdlog::trace("[AmsContextMenu] Destroyed");
}

AmsContextMenu::AmsContextMenu(AmsContextMenu&& other) noexcept
    : ContextMenu(std::move(other)), action_callback_(std::move(other.action_callback_)),
      backend_(other.backend_), total_slots_(other.total_slots_),
      tool_dropdown_(other.tool_dropdown_), backup_dropdown_(other.backup_dropdown_),
      pending_is_loaded_(other.pending_is_loaded_) {
    // Nothing to transfer for the subjects: they are static and shared. Copying
    // the lv_subject_t values here was never sound anyway — observers hold the
    // address they were registered against, so a by-value move left every one of
    // them pointing at the moved-from object.
    other.backend_ = nullptr;
    other.total_slots_ = 0;
    other.tool_dropdown_ = nullptr;
    other.backup_dropdown_ = nullptr;
}

AmsContextMenu& AmsContextMenu::operator=(AmsContextMenu&& other) noexcept {
    if (this != &other) {
        // Let base class handle its state, including the active-menu registry
        ContextMenu::operator=(std::move(other));

        action_callback_ = std::move(other.action_callback_);
        backend_ = other.backend_;
        total_slots_ = other.total_slots_;
        tool_dropdown_ = other.tool_dropdown_;
        backup_dropdown_ = other.backup_dropdown_;
        pending_is_loaded_ = other.pending_is_loaded_;

        // Subjects are static and shared — nothing to transfer. See the move ctor.

        other.backend_ = nullptr;
        other.total_slots_ = 0;
        other.tool_dropdown_ = nullptr;
        other.backup_dropdown_ = nullptr;
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void AmsContextMenu::set_action_callback(ActionCallback callback) {
    action_callback_ = std::move(callback);
}

bool AmsContextMenu::show_near_widget(lv_obj_t* parent, int slot_index, lv_obj_t* near_widget,
                                      bool is_loaded, AmsBackend* backend) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Store AMS-specific state BEFORE base class calls on_created
    backend_ = backend;
    pending_is_loaded_ = is_loaded;
    external_spool_mode_ = false;

    // Get total slots from backend
    if (backend_) {
        total_slots_ = backend_->get_system_info().total_slots;
    } else {
        total_slots_ = 0;
    }

    // Base class handles: XML creation, on_created callback, positioning, and
    // claiming the active-menu slot the static callbacks resolve through.
    bool result = ContextMenu::show_near_widget(parent, slot_index, near_widget);

    spdlog::debug("[AmsContextMenu] Shown for slot {}", slot_index);
    return result;
}

bool AmsContextMenu::show_for_external_spool(lv_obj_t* parent, lv_obj_t* anchor_widget) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Configure for external spool mode (no backend operations)
    backend_ = nullptr;
    pending_is_loaded_ = false;
    total_slots_ = 0;
    external_spool_mode_ = true;

    // Base class handles: XML creation, on_created callback, positioning, and
    // claiming the active-menu slot the static callbacks resolve through.
    bool result = ContextMenu::show_near_widget(parent, -2, anchor_widget);
    if (!result) {
        external_spool_mode_ = false;
    }

    spdlog::debug("[AmsContextMenu] Shown for external spool");
    return result;
}

// ============================================================================
// ContextMenu override
// ============================================================================

void AmsContextMenu::on_created(lv_obj_t* menu_obj) {
    int slot_index = get_item_index();

    // External spool mode: hide backend-related buttons, show only EDIT/CLEAR
    if (external_spool_mode_) {
        // Hide Load/Unload buttons (not applicable to external spool)
        lv_obj_t* btn_load = lv_obj_find_by_name(menu_obj, "btn_load");
        if (btn_load)
            lv_obj_add_flag(btn_load, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* btn_unload = lv_obj_find_by_name(menu_obj, "btn_unload");
        if (btn_unload)
            lv_obj_add_flag(btn_unload, LV_OBJ_FLAG_HIDDEN);

        // Disable subject-driven states so hidden buttons stay hidden
        lv_subject_set_int(&slot_is_loaded_subject_, 0);
        lv_subject_set_int(&slot_can_load_subject_, 0);

        // Set header to "External Spool"
        lv_obj_t* slot_header = lv_obj_find_by_name(menu_obj, "slot_header");
        if (slot_header) {
            lv_label_set_text(slot_header, lv_tr("External Spool"));
        }

        // Show Clear Spool button when external spool has an assignment.
        // btn_edit ("Spool Info") is always shown so users can reopen the edit modal.
        auto ext_info = AmsState::instance().get_external_spool_info();
        bool has_assignment =
            ext_info.has_value() && (ext_info->spoolman_id > 0 || !ext_info->material.empty());

        lv_obj_t* btn_clear = lv_obj_find_by_name(menu_obj, "btn_clear_spool");
        if (btn_clear && has_assignment) {
            lv_obj_clear_flag(btn_clear, LV_OBJ_FLAG_HIDDEN);
        }

        // Show "Select Spool" and "Scan QR Code" if Spoolman is available
        auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
        bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;

        lv_obj_t* btn_spoolman = lv_obj_find_by_name(menu_obj, "btn_spoolman");
        if (btn_spoolman && has_spoolman) {
            lv_obj_clear_flag(btn_spoolman, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_t* btn_scan_qr = lv_obj_find_by_name(menu_obj, "btn_scan_qr");
        if (btn_scan_qr && has_spoolman) {
#if !defined(HELIX_PLATFORM_ESP32)
            // No camera on the v1 Core+AMS cut — keep Scan QR hidden (default).
            lv_obj_clear_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
#endif
        }

        // No dropdowns for external spool
        return;
    }

    // Whether a print blocks these ops is print_blocks_filament_op(), the mirror
    // of AmsSubscriptionBackend::refuse_if_printing(): PRINTING always, PAUSED
    // only on a backend whose filament macro homes itself (AD5X IFS). Reading
    // the raw print_active subject here — which is 1 for both — would keep the
    // menu greyed through the runout pause that is the whole recovery workflow.
    const auto lifecycle = static_cast<PrintState>(
        lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
    const bool print_blocks_op = helix::ui::print_blocks_filament_op(
        lifecycle, backend_ && backend_->filament_ops_self_home());

    // Check if system is busy (operation in progress). AmsSystemInfo::is_busy()
    // is the same predicate AmsSubscriptionBackend::check_preconditions() uses to
    // refuse — this used to open-code it, which is how the filament panel came to
    // have no busy term at all.
    bool system_busy = false;
    if (backend_) {
        AmsSystemInfo info = backend_->get_system_info();
        system_busy = info.is_busy();
        if (system_busy) {
            spdlog::debug("[AmsContextMenu] System busy ({}), disabling Load/Unload",
                          ams_action_to_string(info.action));
        }
    }

    // Get slot info for filament presence check. Tri-state: slot_presence()
    // reports SlotStatus::UNKNOWN as "unanswerable" rather than "empty", so a
    // backend that publishes no per-lane presence does not grey Load.
    std::optional<bool> slot_has_filament;
    // LIVE load state (Task 3): firmware seated+loaded OR filament at this slot's
    // toolhead sensor — drives Load/Unload availability instead of static RFID
    // presence so the menu matches real load state the moment a sensor flips.
    bool slot_is_loaded_live = false;
    if (backend_) {
        SlotInfo slot_info = backend_->get_slot_info(slot_index);
        slot_has_filament = helix::ui::slot_presence(slot_info);
        slot_is_loaded_live = backend_->slot_is_actively_loaded(slot_index) ||
                              backend_->slot_has_filament_at_toolhead(slot_index);
    }

    // Treat the slot as loaded if EITHER the caller's snapshot (can_unload_from_
    // toolhead, computed at open) OR the live per-slot accessors say so. The OR
    // keeps Unload available for the firmware's active slot even after a runout
    // clears the head sensor (#995), while live signals enable real-time accuracy.
    const bool is_loaded = pending_is_loaded_ || slot_is_loaded_live;

    // Whether this slot's unload action is a heated toolhead unload (true) or a
    // cold per-lane eject (false). Defaults to is_loaded for most backends; AD5X
    // IFS refines it with seated-channel authority so a NON-seated lane reads
    // "Eject" even when the firmware dropped its active-slot pointer and
    // is_loaded was broadened by the recovery clause (raza616, 5HR3HHS6). This
    // drives both the button label and the dispatched action (handle_unload).
    const bool toolhead_unload =
        backend_ ? backend_->slot_unloads_to_toolhead(slot_index, is_loaded) : is_loaded;

    // Select the Unload button's operation, most specific first. Each mode has a
    // distinct label and a distinct dispatch; see UnloadMode and decide_unload_mode().
    //
    // The unload-mode decision keeps the strict reading of presence — "is there
    // something to eject" has no useful third answer, and an unknown lane must
    // not grow an Eject button out of nowhere. Only the Load gate treats UNKNOWN
    // as unanswerable.
    const bool slot_filament_present = slot_has_filament.value_or(false);
    const bool slot_empty = !slot_filament_present;
    const bool supports_eject = backend_ && backend_->supports_lane_eject();
    const bool can_recover = backend_ && backend_->can_recover_lane_position(slot_index);
    const bool recovery_attributed = backend_ && backend_->lane_recovery_is_attributed();
    const bool supports_force_eject = backend_ && backend_->supports_force_eject();

    unload_mode_ =
        decide_unload_mode(toolhead_unload, can_recover, recovery_attributed, supports_eject,
                           slot_filament_present, supports_force_eject, slot_empty);

    const bool unload_enabled =
        decide_unload_enabled(system_busy, unload_mode_, print_blocks_op,
                              backend_ && backend_->cold_lane_ops_refused_during_print());
    lv_subject_set_int(&slot_is_loaded_subject_, unload_enabled ? 1 : 0);

    lv_obj_t* btn_unload = lv_obj_find_by_name(menu_obj, "btn_unload");
    if (btn_unload) {
        switch (unload_mode_) {
        case UnloadMode::RecoverPosition:
        case UnloadMode::ForceEject:
            ui_button_set_text(btn_unload, lv_tr("Recover"));
            ui_button_set_icon(btn_unload, "eject");
            break;
        case UnloadMode::Eject:
            ui_button_set_text(btn_unload, lv_tr("Eject"));
            ui_button_set_icon(btn_unload, "eject");
            break;
        case UnloadMode::Unload:
        case UnloadMode::Unavailable:
            break; // XML defaults: "Unload"
        }
    }

    // QIDI Box: a lane with ejectable filament but [force_move] enable_force_move
    // off means eject is unavailable (supports_lane_eject() is false). Surface a
    // one-line hint pointing at the config instead of silently omitting the
    // action — for QIDI, !supports_eject here can only mean force_move is off
    // (the box always supports eject otherwise). See #1041.
    if (backend_ && backend_->get_type() == AmsType::QIDI_BOX && !supports_eject &&
        !pending_is_loaded_ && slot_filament_present) {
        lv_obj_t* hint = lv_obj_find_by_name(menu_obj, "eject_force_move_hint");
        if (hint) {
            lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Determine if slot has filament for Load button state.
    // Gate on ACTUAL toolhead-loaded state (toolhead_unload), NOT the broadened
    // is_loaded recovery signal. is_loaded folds in can_unload_from_toolhead, which
    // reads true for the firmware's seated channel regardless of whether filament
    // actually reached the head. A cold-lane-eject lane (filament parked in the
    // lane but NOT at the toolhead) is a valid Load target, so it must stay
    // enabled. For non-AD5X backends slot_unloads_to_toolhead() returns the hint
    // unchanged, so !toolhead_unload == !is_loaded and behavior is unaffected.
    // Disable Load if: system busy, slot empty, OR filament is already at the head.
    bool can_load =
        decide_can_load(system_busy, toolhead_unload, slot_has_filament, print_blocks_op);
    lv_subject_set_int(&slot_can_load_subject_, can_load ? 1 : 0);
    if (!can_load) {
        spdlog::debug("[AmsContextMenu] Load disabled for slot {}: busy={}, loaded={} "
                      "(live={}), has_filament={}, print_blocks_op={}",
                      slot_index, system_busy, is_loaded, slot_is_loaded_live,
                      slot_has_filament ? (*slot_has_filament ? "yes" : "no") : "unknown",
                      print_blocks_op);
    }

    // Show Select Gate button if backend supports it (e.g. Happy Hare)
    if (backend_ && backend_->supports_gate_select()) {
        lv_obj_t* btn_gate_select = lv_obj_find_by_name(menu_obj, "btn_gate_select");
        if (btn_gate_select) {
            lv_obj_remove_flag(btn_gate_select, LV_OBJ_FLAG_HIDDEN);
            if (system_busy) {
                lv_obj_add_state(btn_gate_select, LV_STATE_DISABLED);
            }
        }
    }

    // Show Check Gate button if backend supports it (e.g. Happy Hare)
    if (backend_ && backend_->supports_gate_check()) {
        lv_obj_t* btn_gate_check = lv_obj_find_by_name(menu_obj, "btn_gate_check");
        if (btn_gate_check) {
            lv_obj_remove_flag(btn_gate_check, LV_OBJ_FLAG_HIDDEN);
            if (system_busy) {
                lv_obj_add_state(btn_gate_check, LV_STATE_DISABLED);
            }
        }
    }

    // Show Clear Spool whenever the slot carries an assignment, present or not
    // (should_show_clear_spool). It used to be gated on an EMPTY slot, so it
    // disappeared the moment a spool went in — exactly when stale metadata does
    // damage, since that is when it gets printed with and when an edit aims a
    // Spoolman write at the previous spool.
    if (backend_) {
        SlotInfo slot_info = backend_->get_slot_info(slot_index);
        lv_obj_t* btn_clear = lv_obj_find_by_name(menu_obj, "btn_clear_spool");
        if (btn_clear && should_show_clear_spool(slot_info)) {
            lv_obj_clear_flag(btn_clear, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Update the slot header text (1-based for user display)
    lv_obj_t* slot_header = lv_obj_find_by_name(menu_obj, "slot_header");
    if (slot_header) {
        char header_text[32];
        snprintf(header_text, sizeof(header_text), lv_tr("Slot %d"), slot_index + 1);
        lv_label_set_text(slot_header, header_text);
    }

    // Show "Select Spool" and "Scan QR Code" buttons if Spoolman is available
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    lv_obj_t* btn_spoolman = lv_obj_find_by_name(menu_obj, "btn_spoolman");
    if (btn_spoolman && has_spoolman) {
        lv_obj_clear_flag(btn_spoolman, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t* btn_scan_qr = lv_obj_find_by_name(menu_obj, "btn_scan_qr");
    if (btn_scan_qr && has_spoolman) {
#if !defined(HELIX_PLATFORM_ESP32)
        // No camera on the v1 Core+AMS cut — keep Scan QR hidden (default).
        lv_obj_clear_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
#endif
    }

    // Configure dropdowns based on backend capabilities
    configure_dropdowns();
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsContextMenu::dispatch_ams_action(MenuAction action) {
    int slot = get_item_index();
    ActionCallback callback_copy = action_callback_;

    hide();

    if (callback_copy) {
        callback_copy(action, slot);
    }
}

void AmsContextMenu::on_backdrop_clicked() {
    spdlog::debug("[AmsContextMenu] Backdrop clicked");
    dispatch_ams_action(MenuAction::CANCELLED);
}

void AmsContextMenu::handle_load() {
    spdlog::info("[AmsContextMenu] Load requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::LOAD);
}

void AmsContextMenu::handle_unload() {
    switch (unload_mode_) {
    case UnloadMode::RecoverPosition:
        spdlog::info("[AmsContextMenu] Position recovery requested for slot {}", get_item_index());
        dispatch_ams_action(MenuAction::RECOVER_POSITION);
        break;
    case UnloadMode::Eject:
        spdlog::info("[AmsContextMenu] Eject requested for slot {}", get_item_index());
        dispatch_ams_action(MenuAction::EJECT);
        break;
    case UnloadMode::ForceEject:
        spdlog::info("[AmsContextMenu] Recover/force-eject requested for slot {}",
                     get_item_index());
        dispatch_ams_action(MenuAction::EJECT);
        break;
    case UnloadMode::Unload:
    case UnloadMode::Unavailable:
        spdlog::info("[AmsContextMenu] Unload requested for slot {}", get_item_index());
        dispatch_ams_action(MenuAction::UNLOAD);
        break;
    }
}

void AmsContextMenu::handle_gate_select() {
    spdlog::info("[AmsContextMenu] Select gate requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::SELECT_GATE);
}

void AmsContextMenu::handle_gate_check() {
    spdlog::info("[AmsContextMenu] Check gate requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::CHECK_GATE);
}

void AmsContextMenu::handle_edit() {
    spdlog::info("[AmsContextMenu] Edit requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::EDIT);
}

bool AmsContextMenu::should_show_clear_spool(const SlotInfo& slot) {
    return slot.spoolman_id > 0 || !slot.material.empty();
}

AmsContextMenu::UnloadMode
AmsContextMenu::decide_unload_mode(bool toolhead_unload, bool can_recover, bool recovery_attributed,
                                   bool supports_eject, bool slot_has_filament,
                                   bool supports_force_eject, bool slot_empty) {
    if (toolhead_unload) {
        return UnloadMode::Unload;
    }
    if (can_recover && recovery_attributed) {
        // Filament stranded past the hub — a failed load or unload left it in the
        // bowden, where plain unload() cannot reach it. The backend named this
        // exact lane as the one needing recovery, so this is a confident
        // diagnosis: outrank Eject.
        return UnloadMode::RecoverPosition;
    }
    if (supports_eject && slot_has_filament) {
        return UnloadMode::Eject;
    }
    if (can_recover) {
        // Unattributed strand: some backends (AFC) share one hub sensor across
        // every lane on a unit, so can_recover can read true for every lane at
        // once with no way to say whose filament tripped it. Ranking this below
        // Eject means a seated lane (slot_has_filament above) keeps its Eject
        // button; this arm only catches lanes with nothing to eject, offering
        // Recover as a last resort rather than hiding it entirely.
        return UnloadMode::RecoverPosition;
    }
    if (supports_force_eject && slot_empty) {
        // Empty/runout lane: a cold presence-ignoring retract can clear a snapped
        // chunk the sensor cannot see (#996).
        return UnloadMode::ForceEject;
    }
    return UnloadMode::Unavailable;
}

bool AmsContextMenu::decide_can_load(bool system_busy, bool toolhead_unload,
                                     std::optional<bool> slot_has_filament, bool print_blocks_op) {
    helix::ui::OpButtonState state;
    state.system_busy = system_busy;
    state.print_blocks_op = print_blocks_op;
    state.slot_is_loaded = toolhead_unload;
    state.slot_has_filament = slot_has_filament;
    return !helix::ui::compute_op_button_gating(state).load_disabled;
}

bool AmsContextMenu::decide_unload_enabled(bool system_busy, UnloadMode mode, bool print_blocks_op,
                                           bool cold_ops_print_gated) {
    helix::ui::OpButtonState state;
    state.system_busy = system_busy;
    state.print_blocks_op = print_blocks_op;
    state.unload_available = (mode != UnloadMode::Unavailable);
    // Unload is the only mode that runs the heated toolhead path (and, on AD5X,
    // a self-homing firmware macro). Eject / RecoverPosition / ForceEject are
    // cold lane-side retracts that leave the toolhead where the print left it,
    // so they stay available for clearing a snapped strand mid-pause.
    //
    // Unless the firmware refuses them anyway: AFC's cmd_LANE_UNLOAD opens with
    // its own is_printing() check, so on that backend the exemption would offer a
    // button into a guaranteed refusal. cold_ops_print_gated is
    // AmsBackend::cold_lane_ops_refused_during_print() — see it for why this is
    // per-backend rather than a blanket rule.
    state.unload_is_cold_lane_op = (mode != UnloadMode::Unload) && !cold_ops_print_gated;
    return !helix::ui::compute_op_button_gating(state).unload_disabled;
}

void AmsContextMenu::handle_clear_spool() {
    spdlog::info("[AmsContextMenu] Clear spool requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::CLEAR_SPOOL);
}

void AmsContextMenu::handle_spoolman() {
    spdlog::info("[AmsContextMenu] Spoolman select requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::SPOOLMAN);
}

void AmsContextMenu::handle_scan_qr() {
    spdlog::info("[AmsContextMenu] Scan QR requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::SCAN_QR);
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsContextMenu::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"ams_context_load_cb", on_load_cb},
        {"ams_context_unload_cb", on_unload_cb},
        {"ams_context_gate_select_cb", on_gate_select_cb},
        {"ams_context_gate_check_cb", on_gate_check_cb},
        {"ams_context_edit_cb", on_edit_cb},
        {"ams_context_clear_spool_cb", on_clear_spool_cb},
        {"ams_context_spoolman_cb", on_spoolman_cb},
        {"ams_context_scan_qr_cb", on_scan_qr_cb},
        {"ams_context_tool_changed_cb", on_tool_changed_cb},
        {"ams_context_backup_changed_cb", on_backup_changed_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[AmsContextMenu] Callbacks registered");
}

// ============================================================================
// Static Callbacks (Instance Lookup via ContextMenu::active())
// ============================================================================

AmsContextMenu* AmsContextMenu::get_active_instance() {
    auto* self = ContextMenu::active_as<AmsContextMenu>();
    if (!self) {
        spdlog::warn("[AmsContextMenu] No active instance for event");
    }
    return self;
}

void AmsContextMenu::on_load_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_load();
    }
}

void AmsContextMenu::on_unload_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_unload();
    }
}

void AmsContextMenu::on_gate_select_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_gate_select();
    }
}

void AmsContextMenu::on_gate_check_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_gate_check();
    }
}

void AmsContextMenu::on_edit_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_edit();
    }
}

void AmsContextMenu::on_clear_spool_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_clear_spool();
    }
}

void AmsContextMenu::on_spoolman_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_spoolman();
    }
}

void AmsContextMenu::on_scan_qr_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_scan_qr();
    }
}

void AmsContextMenu::on_tool_changed_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_tool_changed();
    }
}

void AmsContextMenu::on_backup_changed_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_backup_changed();
    }
}

// ============================================================================
// Dropdown Handlers
// ============================================================================

void AmsContextMenu::handle_tool_changed() {
    if (!tool_dropdown_ || !backend_) {
        return;
    }

    int selected = static_cast<int>(lv_dropdown_get_selected(tool_dropdown_));
    // Option 0 = "None", options 1+ = T0, T1, T2...
    int tool_number = selected - 1; // -1 = None

    spdlog::info("[AmsContextMenu] Tool mapping changed for slot {}: tool {}", get_item_index(),
                 tool_number >= 0 ? tool_number : -1);

    if (tool_number >= 0) {
        // Warn if another tool already maps to this slot
        auto mapping = backend_->get_tool_mapping();
        for (size_t i = 0; i < mapping.size(); ++i) {
            if (static_cast<int>(i) != tool_number && mapping[i] == get_item_index()) {
                spdlog::warn("[AmsContextMenu] Tool {} will share slot {} with tool {}",
                             tool_number, get_item_index(), i);
                std::string msg = fmt::format(lv_tr("T{} shares slot with T{}"), tool_number, i);
                ToastManager::instance().show(ToastSeverity::WARNING, msg.c_str());
                break;
            }
        }

        auto result = backend_->set_tool_mapping(tool_number, get_item_index());
        if (!result.success()) {
            // notify_ams_error() logs the technical message itself.
            helix::ui::notify_ams_error(result, lv_tr("Tool mapping failed"));
        }
    }
    // Note: "None" selection doesn't clear mapping - user needs to map another slot to that tool
}

void AmsContextMenu::handle_backup_changed() {
    if (!backup_dropdown_ || !backend_) {
        return;
    }

    int selected = static_cast<int>(lv_dropdown_get_selected(backup_dropdown_));

    // Convert dropdown index back to actual slot index
    // Dropdown: None=0, then all slots except current slot
    int backup_slot = -1; // Default to None
    if (selected > 0) {
        // Find the actual slot index by counting through slots (skipping current)
        int dropdown_idx = 0;
        for (int i = 0; i < total_slots_; ++i) {
            if (i != get_item_index()) {
                dropdown_idx++;
                if (dropdown_idx == selected) {
                    backup_slot = i;
                    break;
                }
            }
        }
    }

    // Ask the BACKEND whether this pairing is allowed. Same rule that tagged the
    // option "(incompatible)" in build_backup_options(), so the label and the
    // refusal cannot disagree.
    if (decide_backup_refused(get_item_index(), backup_slot, backend_eligible_fn())) {
        const std::string current_material = backend_->get_slot_info(get_item_index()).material;
        const std::string backup_material = backend_->get_slot_info(backup_slot).material;
        spdlog::warn("[AmsContextMenu] Backend rejected backup slot {} for slot {} ({} / {})",
                     backup_slot, get_item_index(), current_material, backup_material);

        // Name the materials only when they are in fact the problem. A backend
        // with a stricter rule (AD5X IFS matches colour and port presence too)
        // can refuse two slots holding the same material, and a message saying
        // "PLA cannot use PLA as backup" would be nonsense.
        std::string msg;
        if (!current_material.empty() && !backup_material.empty() &&
            !filament::materials_compatible(current_material, backup_material)) {
            msg = fmt::format(lv_tr("Incompatible materials: {} cannot use {} as backup"),
                              current_material, backup_material);
        } else {
            msg = lv_tr("That slot cannot stand in for this one");
        }
        ToastManager::instance().show(ToastSeverity::ERROR, msg.c_str());

        // Reset dropdown to "None" (index 0)
        lv_dropdown_set_selected(backup_dropdown_, 0);
        return;
    }

    spdlog::info("[AmsContextMenu] Backup slot changed for slot {}: backup {}", get_item_index(),
                 backup_slot >= 0 ? backup_slot : -1);

    auto result = backend_->set_endless_spool_backup(get_item_index(), backup_slot);
    if (!result.success()) {
        spdlog::warn("[AmsContextMenu] Failed to set endless spool backup: {}", result.user_msg);
    } else {
        // Bump slots version to trigger endless spool arrow redraw
        AmsState::instance().bump_slots_version();
    }

    // Close the context menu after selection
    hide();
}

// ============================================================================
// Dropdown Configuration
// ============================================================================

void AmsContextMenu::configure_dropdowns() {
    if (!menu()) {
        return;
    }

    // Find dropdown widgets
    tool_dropdown_ = lv_obj_find_by_name(menu(), "tool_dropdown");
    backup_dropdown_ = lv_obj_find_by_name(menu(), "backup_dropdown");

    // Find row containers and divider
    lv_obj_t* tool_row = lv_obj_find_by_name(menu(), "tool_dropdown_row");
    lv_obj_t* backup_row = lv_obj_find_by_name(menu(), "backup_dropdown_row");
    lv_obj_t* divider = lv_obj_find_by_name(menu(), "dropdown_divider");

    bool show_any_dropdown = false;

    // Tool mapping dropdown - hidden until we have a good UX for remapping
    // (currently 1:1 lane-to-tool mapping is the only conflict-free option)
    // if (backend_) {
    //     auto tool_caps = backend_->get_tool_mapping_capabilities();
    //     if (tool_caps.supported) {
    //         populate_tool_dropdown();
    //         if (tool_row) {
    //             lv_obj_remove_flag(tool_row, LV_OBJ_FLAG_HIDDEN);
    //         }
    //         if (tool_dropdown_ && !tool_caps.editable) {
    //             lv_obj_add_state(tool_dropdown_, LV_STATE_DISABLED);
    //         }
    //         show_any_dropdown = true;
    //     }
    // }
    (void)tool_row;

    // Configure endless spool dropdown — see decide_show_backup_row().
    if (backend_) {
        auto es_caps = backend_->get_endless_spool_capabilities();
        const bool has_relation = !backend_->get_endless_spool_config().empty();
        if (decide_show_backup_row(es_caps, has_relation)) {
            populate_backup_dropdown();
            if (backup_row) {
                lv_obj_remove_flag(backup_row, LV_OBJ_FLAG_HIDDEN);
            }
            // Disable dropdown if not editable
            if (backup_dropdown_ && !es_caps.editable()) {
                lv_obj_add_state(backup_dropdown_, LV_STATE_DISABLED);
            }
            show_any_dropdown = true;
            spdlog::debug("[AmsContextMenu] Endless spool row shown (editable={})",
                          es_caps.editable());
        } else if (es_caps.available()) {
            spdlog::debug("[AmsContextMenu] Endless spool available but has no per-slot "
                          "relation to show - row stays hidden");
        }
    }

    // Show divider only if any dropdown is visible
    if (divider && show_any_dropdown) {
        lv_obj_remove_flag(divider, LV_OBJ_FLAG_HIDDEN);
    }
}

bool AmsContextMenu::decide_show_backup_row(const helix::printer::EndlessSpoolCapabilities& caps,
                                            bool has_relation) {
    if (!caps.available()) {
        return false;
    }
    // Editable implies there is something to write even before anything is set.
    return caps.editable() || has_relation;
}

void AmsContextMenu::populate_tool_dropdown() {
    if (!tool_dropdown_) {
        return;
    }

    std::string options = build_tool_options();
    lv_dropdown_set_options(tool_dropdown_, options.c_str());

    int current_tool = get_current_tool_for_slot();
    // Map tool number to dropdown index: None=0, T0=1, T1=2, etc.
    int selected_index = (current_tool >= 0) ? (current_tool + 1) : 0;
    lv_dropdown_set_selected(tool_dropdown_, static_cast<uint32_t>(selected_index));

    spdlog::debug("[AmsContextMenu] Tool dropdown populated: slot {} maps to tool {}",
                  get_item_index(), current_tool);
}

void AmsContextMenu::populate_backup_dropdown() {
    if (!backup_dropdown_) {
        return;
    }

    std::string options = build_backup_options();
    lv_dropdown_set_options(backup_dropdown_, options.c_str());

    int current_backup = get_current_backup_for_slot();
    // Map backup slot to dropdown index, accounting for skipped current slot
    // Dropdown: None=0, then all slots except current slot
    int selected_index = 0; // Default to None
    if (current_backup >= 0) {
        // Count how many slots appear before the backup slot in the dropdown
        // (which skips the current slot)
        selected_index = 1; // Start after "None"
        for (int i = 0; i < current_backup; ++i) {
            if (i != get_item_index()) {
                selected_index++;
            }
        }
    }
    lv_dropdown_set_selected(backup_dropdown_, static_cast<uint32_t>(selected_index));

    spdlog::debug("[AmsContextMenu] Backup dropdown populated: slot {} backup is {}",
                  get_item_index(), current_backup);
}

std::string AmsContextMenu::build_tool_options() const {
    std::string options = lv_tr("None");
    // Add tool options T0, T1, T2... based on total slots
    for (int i = 0; i < total_slots_; ++i) {
        options += "\nT" + std::to_string(i);
    }
    return options;
}

AmsContextMenu::BackupEligibleFn AmsContextMenu::backend_eligible_fn() const {
    AmsBackend* backend = backend_;
    if (backend == nullptr) {
        // No backend: tag nothing and refuse nothing. Matches the old code, which
        // skipped every compatibility check when backend_ was null.
        return [](int, int) { return helix::printer::BackupEligibility::Eligible; };
    }
    return [backend](int slot, int candidate) {
        return backend->endless_spool_backup_eligibility(slot, candidate);
    };
}

std::string AmsContextMenu::build_backup_options() const {
    return build_backup_options_for(total_slots_, get_item_index(), backend_eligible_fn());
}

std::string AmsContextMenu::build_backup_options_for(int total_slots, int item_index,
                                                     const BackupEligibleFn& eligible) {
    std::string options = lv_tr("None");

    // Add slot options Slot 1, Slot 2... based on total slots.
    // Skip the current slot (can't be backup for itself).
    for (int i = 0; i < total_slots; ++i) {
        if (i == item_index) {
            continue;
        }
        options += "\n" + fmt::format(lv_tr("Slot {}"), i + 1);
        if (item_index >= 0 && eligible) {
            switch (eligible(item_index, i)) {
            case helix::printer::BackupEligibility::Incompatible:
                options += std::string(" ") + lv_tr("(incompatible)");
                break;
            case helix::printer::BackupEligibility::GradeDiffers:
                // Selectable, and labelled so the choice is an informed one:
                // same polymer, but a filled grade that prints slower and wears
                // a brass nozzle.
                options += std::string(" ") + lv_tr("(different grade)");
                break;
            case helix::printer::BackupEligibility::Eligible:
                break;
            }
        }
    }
    return options;
}

bool AmsContextMenu::decide_backup_refused(int item_index, int backup_slot,
                                           const BackupEligibleFn& eligible) {
    if (backup_slot < 0 || item_index < 0) {
        return false; // "None" clears a backup; nothing to be compatible with.
    }
    // Only the hard verdict refuses. A grade difference is tagged in the option
    // list and then allowed: the print surviving its runout is worth more than
    // the grade being exact, and the user has been told which lane it is.
    return eligible &&
           eligible(item_index, backup_slot) == helix::printer::BackupEligibility::Incompatible;
}

int AmsContextMenu::get_current_tool_for_slot() const {
    if (!backend_) {
        return -1;
    }

    // Get tool mapping and find which tool maps to this slot
    auto mapping = backend_->get_tool_mapping();
    for (size_t tool = 0; tool < mapping.size(); ++tool) {
        if (mapping[tool] == get_item_index()) {
            return static_cast<int>(tool);
        }
    }
    return -1; // No tool maps to this slot
}

int AmsContextMenu::get_current_backup_for_slot() const {
    if (!backend_) {
        return -1;
    }

    // One shared projection for the group relation — see
    // helix::printer::endless_spool_backup_for(). A backend with no per-slot
    // relation at all (CFS) yields -1 here, which is why configure_dropdowns()
    // hides the row rather than rendering a permanent "None".
    return helix::printer::endless_spool_backup_for(backend_->get_endless_spool_config(),
                                                    get_item_index());
}

} // namespace helix::ui
