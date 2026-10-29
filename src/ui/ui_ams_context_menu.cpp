// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_context_menu.h"

#include "ui_ams_detail.h"
#include "ui_ams_edit_follow_up.h"
#include "ui_ams_edit_overlay.h"
#include "ui_ams_sidebar.h"
#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_overlay_qr_scanner.h"
#include "ui_panel_calibration_pa.h"
#include "ui_plugin_menu_rows.h"

#include "ams_backend.h"
#include "ams_bypass_policy.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"
#include "filament_variants.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "pa_calibration.h"
#include "plugin_menu_context.h"
#include "printer_state.h"
#include "spoolman_types.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <optional>

namespace helix::ui {

// Static member initialization
bool AmsContextMenu::callbacks_registered_ = false;
bool AmsContextMenu::subjects_initialized_ = false;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_is_loaded_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_can_load_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_can_purge_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_unload_hint_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_unload_hint_visible_subject_;
HELIX_PSRAM_BSS char AmsContextMenu::slot_unload_hint_buf_[128];
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_eject_hint_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_eject_hint_visible_subject_;
HELIX_PSRAM_BSS char AmsContextMenu::slot_eject_hint_buf_[128];
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_can_clear_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_clear_hint_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_clear_hint_visible_subject_;
HELIX_PSRAM_BSS char AmsContextMenu::slot_clear_hint_buf_[192];
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_preload_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_load_hint_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_load_hint_visible_subject_;
HELIX_PSRAM_BSS char AmsContextMenu::slot_load_hint_buf_[192];
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_mount_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_park_subject_;
HELIX_PSRAM_BSS lv_subject_t AmsContextMenu::slot_pa_cal_subject_;

// ============================================================================
// Construction / Destruction
// ============================================================================

void AmsContextMenu::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_int(&slot_is_loaded_subject_, 0);
    lv_subject_init_int(&slot_can_load_subject_, 1);
    lv_subject_init_int(&slot_can_purge_subject_, 1);
    lv_subject_init_int(&slot_unload_hint_visible_subject_, 0);
    lv_subject_init_string(&slot_unload_hint_subject_, slot_unload_hint_buf_, nullptr,
                           sizeof(slot_unload_hint_buf_), "");
    lv_subject_init_int(&slot_eject_hint_visible_subject_, 0);
    lv_subject_init_string(&slot_eject_hint_subject_, slot_eject_hint_buf_, nullptr,
                           sizeof(slot_eject_hint_buf_), "");
    lv_subject_init_int(&slot_can_clear_subject_, 1);
    lv_subject_init_int(&slot_clear_hint_visible_subject_, 0);
    lv_subject_init_string(&slot_clear_hint_subject_, slot_clear_hint_buf_, nullptr,
                           sizeof(slot_clear_hint_buf_), "");
    lv_subject_init_int(&slot_preload_subject_, 0);
    lv_subject_init_int(&slot_load_hint_visible_subject_, 0);
    lv_subject_init_string(&slot_load_hint_subject_, slot_load_hint_buf_, nullptr,
                           sizeof(slot_load_hint_buf_), "");
    lv_subject_init_int(&slot_mount_subject_, 0);
    lv_subject_init_int(&slot_park_subject_, 0);
    lv_subject_init_int(&slot_pa_cal_subject_, 0);

    lv_xml_register_subject(nullptr, "ams_slot_is_loaded", &slot_is_loaded_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_can_load", &slot_can_load_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_can_purge", &slot_can_purge_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_unload_hint", &slot_unload_hint_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_unload_hint_visible",
                            &slot_unload_hint_visible_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_eject_hint", &slot_eject_hint_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_eject_hint_visible",
                            &slot_eject_hint_visible_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_can_clear", &slot_can_clear_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_clear_hint", &slot_clear_hint_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_clear_hint_visible",
                            &slot_clear_hint_visible_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_preload", &slot_preload_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_load_hint", &slot_load_hint_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_load_hint_visible",
                            &slot_load_hint_visible_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_mount", &slot_mount_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_park", &slot_park_subject_);
    lv_xml_register_subject(nullptr, "ams_slot_pa_cal", &slot_pa_cal_subject_);

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
      backend_(other.backend_), pending_is_loaded_(other.pending_is_loaded_) {
    // Nothing to transfer for the subjects: they are static and shared. Copying
    // the lv_subject_t values here was never sound anyway — observers hold the
    // address they were registered against, so a by-value move left every one of
    // them pointing at the moved-from object.
    other.backend_ = nullptr;
}

AmsContextMenu& AmsContextMenu::operator=(AmsContextMenu&& other) noexcept {
    if (this != &other) {
        // Let base class handle its state, including the active-menu registry
        ContextMenu::operator=(std::move(other));

        action_callback_ = std::move(other.action_callback_);
        backend_ = other.backend_;
        pending_is_loaded_ = other.pending_is_loaded_;

        // Subjects are static and shared — nothing to transfer. See the move ctor.

        other.backend_ = nullptr;
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

    // Base class handles: XML creation, on_created callback, positioning, and
    // claiming the active-menu slot the static callbacks resolve through.
    bool result = ContextMenu::show_near_widget(parent, slot_index, near_widget);

    spdlog::debug("[AmsContextMenu] Shown for slot {}", slot_index);
    return result;
}

bool AmsContextMenu::show_for_external_spool(lv_obj_t* parent, lv_obj_t* anchor_widget,
                                             bool offer_toggle) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Configure for external spool mode (no backend operations)
    backend_ = nullptr;
    pending_is_loaded_ = false;
    external_spool_mode_ = true;
    external_offer_toggle_ = offer_toggle;

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

    // Hidden until the lane path below says otherwise; the external spool
    // has no gate to preload.
    lv_subject_set_int(&slot_preload_subject_, 0);

    // External spool mode: Load and Unload apply — kExternalSpoolSlot is a
    // target plan_load()/plan_unload() both resolve, and this menu is where a
    // user looking at the bypass spool goes for them
    // (prestonbrown/helixscreen#1486).
    if (external_spool_mode_) {
        // backend_ is deliberately null in this mode, so the gating terms come
        // off the live backend instead. Same predicate as every other surface:
        // a bypass Load offered here and refused by the Filament panel would be
        // the divergence compute_op_button_gating() exists to prevent.
        AmsBackend* const backend = AmsState::instance().get_backend();
        AmsSystemInfo ext_sys;
        if (backend) {
            ext_sys = backend->get_system_info();
        }
        const bool bypass_loaded =
            helix::ui::read_unload_target_loaded(backend, ext_sys, helix::ui::kExternalSpoolSlot);

        const helix::ui::OpButtonState ext_state = helix::ui::build_external_spool_gating_state(
            bypass_loaded, backend && ext_sys.is_busy(),
            get_printer_state().print_state().get_print_lifecycle(),
            backend && backend->filament_ops_self_home());

        const helix::ui::OpButtonGating ext_gating = helix::ui::compute_op_button_gating(ext_state);
        lv_subject_set_int(&slot_can_load_subject_, ext_gating.load_disabled ? 0 : 1);
        lv_subject_set_int(&slot_is_loaded_subject_, ext_gating.unload_disabled ? 0 : 1);
        // The lane menu retitles Unload per UnloadMode; bypass has only the one.
        unload_mode_ = UnloadMode::Unload;

        // These subjects are static and outlive any one show, while the widget
        // tree is rebuilt from XML each time. A lane menu shown earlier leaves
        // its answers behind, so a bypass menu that did not clear them would
        // render that lane's unload refusal under the bypass spool's own Unload.
        lv_subject_copy_string(&slot_unload_hint_subject_, "");
        lv_subject_set_int(&slot_unload_hint_visible_subject_, 0);
        lv_subject_copy_string(&slot_load_hint_subject_, "");
        lv_subject_set_int(&slot_load_hint_visible_subject_, 0);
        lv_subject_set_int(&slot_mount_subject_, 0);
        lv_subject_set_int(&slot_park_subject_, 0);
        lv_subject_set_int(&slot_pa_cal_subject_, 0);
        lv_subject_copy_string(&slot_eject_hint_subject_, "");
        lv_subject_set_int(&slot_eject_hint_visible_subject_, 0);

        lv_subject_set_int(&slot_can_purge_subject_, ext_gating.purge_disabled ? 0 : 1);
        if (lv_obj_t* btn_purge =
                helix::ui::find_required(menu_obj, "btn_purge", "AmsContextMenu")) {
            lv_obj_remove_flag(btn_purge, LV_OBJ_FLAG_HIDDEN);
        }

        // The bypass toggle is on this menu so the external spool answers every
        // question asked of it in one place. bypass_toggle_offered() is the same
        // answer BypassToggleController refuses on, asked before the button is
        // drawn rather than after the user taps it.
        if (lv_obj_t* btn_bypass =
                helix::ui::find_required(menu_obj, "btn_bypass_toggle", "AmsContextMenu")) {
            const bool bypass_offered =
                backend &&
                helix::bypass_toggle_offered(helix::bypass_available_for(ext_sys.supports_bypass),
                                             ext_sys.has_hardware_bypass_sensor);
            if (bypass_offered) {
                lv_obj_remove_flag(btn_bypass, LV_OBJ_FLAG_HIDDEN);
                // Names the action, not the state, so it reads as a command.
                ui_button_set_text(btn_bypass, backend->is_bypass_active()
                                                   ? lv_tr("Disable Bypass")
                                                   : lv_tr("Enable Bypass"));
            }
        }

        // Set header to "External Spool"
        lv_obj_t* slot_header = lv_obj_find_by_name(menu_obj, "slot_header");
        if (slot_header) {
            lv_label_set_text(slot_header, lv_tr("External Spool"));
        }

        // Show Clear Spool button when external spool has an assignment.
        // btn_edit ("Edit Spool") is always shown so users can reopen the editor.
        auto ext_info = AmsState::instance().get_external_spool_info();
        bool has_assignment =
            ext_info.has_value() && (ext_info->spoolman_id > 0 || !ext_info->material.empty());

        lv_obj_t* btn_clear =
            helix::ui::find_required(menu_obj, "btn_clear_spool", "AmsContextMenu");
        if (btn_clear && has_assignment) {
            lv_obj_clear_flag(btn_clear, LV_OBJ_FLAG_HIDDEN);
        }

        // The bypass clear erases the external spool's own assignment, not a
        // lane's metadata, so a print never blocks it. The subjects are shared
        // with the lane menu (one menu on screen at a time), so a lane's
        // refusal must not bleed into this menu's button and hint.
        lv_subject_set_int(&slot_can_clear_subject_, 1);
        lv_subject_copy_string(&slot_clear_hint_subject_, "");
        lv_subject_set_int(&slot_clear_hint_visible_subject_, 0);

        // Show "Select Spool" and "Scan QR Code" if Spoolman is available
        auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
        bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;

        lv_obj_t* btn_spoolman =
            helix::ui::find_required(menu_obj, "btn_spoolman", "AmsContextMenu");
        if (btn_spoolman && has_spoolman) {
            lv_obj_clear_flag(btn_spoolman, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_t* btn_scan_qr = helix::ui::find_required(menu_obj, "btn_scan_qr", "AmsContextMenu");
        if (btn_scan_qr && has_spoolman) {
#if !defined(HELIX_PLATFORM_ESP32)
            // No camera on the v1 Core+AMS cut — keep Scan QR hidden (default).
            lv_obj_clear_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
#endif
        }

        return;
    }

    // Whether a print blocks these ops is print_blocks_filament_op(), the mirror
    // of AmsSubscriptionBackend::refuse_if_printing(): PRINTING always, PAUSED
    // only on a backend whose filament macro homes itself (AD5X IFS). Reading
    // the raw print_active subject here — which is 1 for both — would keep the
    // menu greyed through the runout pause that is the whole recovery workflow.
    const auto lifecycle = get_printer_state().print_state().get_print_lifecycle();
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

    const SlotOpDecision ops =
        decide_slot_ops(backend_, slot_index, pending_is_loaded_, system_busy, print_blocks_op,
                        backend_ && backend_->toolhead_filament_unaccounted().value_or(false));

    // What this slot's head can do with filament, and Mount/Park where it can
    // do nothing itself. A reason here is a capability refusal: it withdraws the
    // filament action whatever the slot's own state says.
    const int head = backend_ ? backend_->head_for_slot(slot_index).value_or(-1) : -1;
    const helix::ui::FilamentActionSet acts = helix::ui::decide_slot_actions(
        helix::ui::read_filament_head_view(backend_, get_printer_state()), head);
    // The dispatched action, not just the label: handle_unload() reads this.
    unload_mode_ = ops.unload_mode;
    // Only the heated toolhead unload is the head's filament op; Eject and
    // Recover are lane-side and answer to the lane alone.
    const bool unload_withdrawn =
        ops.unload_mode == UnloadMode::Unload && !acts.unload.reason.empty();
    const bool unload_enabled = ops.unload_enabled && !unload_withdrawn;
    lv_subject_set_int(&slot_is_loaded_subject_, unload_enabled ? 1 : 0);

    lv_obj_t* btn_unload = helix::ui::find_required(menu_obj, "btn_unload", "AmsContextMenu");
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
            break; // XML default: "Unload"
        }
    }

    lv_subject_set_int(&slot_mount_subject_, acts.mount.visible ? (acts.mount.enabled ? 2 : 1) : 0);
    lv_subject_set_int(&slot_park_subject_, acts.park.visible ? (acts.park.enabled ? 2 : 1) : 0);

#if defined(HELIX_PLATFORM_ESP32)
    // The calibration screen is not in the v1 Core+AMS cut.
    lv_subject_set_int(&slot_pa_cal_subject_, 0);
#else
    lv_subject_set_int(&slot_pa_cal_subject_,
                       decide_offers_pa_calibration(
                           helix::pacal::is_supported(get_printer_state().get_discovery()),
                           ops.toolhead_unload, print_blocks_op, system_busy)
                           ? 1
                           : 0);
#endif

    // A disabled button with no stated reason reads as a bug. The head's reason
    // comes first; otherwise the backend's, only when it has something
    // actionable to say - "nothing is loaded" is not worth a line, because
    // there is visibly nothing to unload.
    const std::string load_hint = acts.load.reason;
    std::string unload_hint;
    if (unload_withdrawn) {
        unload_hint = acts.unload.reason;
    } else if (!ops.unload_enabled && backend_) {
        unload_hint = backend_->unload_blocked_reason(slot_index);
    }
    // One reason covering both directions is said once.
    if (unload_hint == load_hint) {
        unload_hint.clear();
    }
    // Already translated by the backend, which had the literal to extract.
    lv_subject_copy_string(&slot_load_hint_subject_, load_hint.c_str());
    lv_subject_set_int(&slot_load_hint_visible_subject_, load_hint.empty() ? 0 : 1);
    lv_subject_copy_string(&slot_unload_hint_subject_, unload_hint.c_str());
    lv_subject_set_int(&slot_unload_hint_visible_subject_, unload_hint.empty() ? 0 : 1);

    // Eject is offered only on a lane that is not the loaded one (the loaded lane shows
    // Unload). When the backend cannot eject it, it says why instead of the action
    // silently disappearing.
    const std::string eject_hint = (backend_ && !pending_is_loaded_)
                                       ? backend_->eject_unavailable_reason(slot_index)
                                       : std::string();
    lv_subject_copy_string(&slot_eject_hint_subject_, eject_hint.c_str());
    lv_subject_set_int(&slot_eject_hint_visible_subject_, eject_hint.empty() ? 0 : 1);

    const bool can_load = ops.can_load && acts.load.reason.empty();
    lv_subject_set_int(&slot_can_load_subject_, can_load ? 1 : 0);
    if (!can_load) {
        spdlog::debug("[AmsContextMenu] Load disabled for slot {}: busy={}, loaded={} "
                      "(live={}), has_filament={}, print_blocks_op={}",
                      slot_index, system_busy, ops.is_loaded, ops.live_loaded,
                      ops.presence ? (*ops.presence ? "yes" : "no") : "unknown", print_blocks_op);
    }

    // Show Check Gate button if backend supports it (e.g. Happy Hare)
    if (backend_ && backend_->supports_gate_check()) {
        lv_obj_t* btn_gate_check =
            helix::ui::find_required(menu_obj, "btn_gate_check", "AmsContextMenu");
        if (btn_gate_check) {
            lv_obj_remove_flag(btn_gate_check, LV_OBJ_FLAG_HIDDEN);
            if (system_busy) {
                lv_obj_add_state(btn_gate_check, LV_STATE_DISABLED);
            }
        }
    }

    // Preload moves filament only between spool and gate, but the firmware
    // refuses it mid-print and while any filament is loaded, and reports that
    // refusal only to its console.
    if (backend_ && backend_->supports_lane_preload()) {
        const bool preload_blocked =
            system_busy || print_blocks_op || backend_->is_filament_loaded();
        lv_subject_set_int(&slot_preload_subject_, preload_blocked ? 1 : 2);
    }

    // Show Clear Spool whenever the slot carries an assignment, present or not
    // (should_show_clear_spool). It used to be gated on an EMPTY slot, so it
    // disappeared the moment a spool went in — exactly when stale metadata does
    // damage, since that is when it gets printed with and when an edit aims a
    // Spoolman write at the previous spool.
    if (backend_) {
        SlotInfo slot_info = backend_->get_slot_info(slot_index);
        lv_obj_t* btn_clear =
            helix::ui::find_required(menu_obj, "btn_clear_spool", "AmsContextMenu");
        if (btn_clear && should_show_clear_spool(slot_info)) {
            lv_obj_clear_flag(btn_clear, LV_OBJ_FLAG_HIDDEN);
        }

        // The lane feeding the running print is the lane whose material and
        // colour the print's own surfaces are displaying; clearing it mid-job
        // deletes that out from under the job. The button greys out with the
        // reason beside it, and ams_dispatch_backend_action() refuses the same
        // case independently: a menu rendered before the print started keeps
        // its old subject values until it is rebuilt.
        const bool clear_blocked = helix::ui::clear_spool_blocked_by_print(
            lifecycle, backend_->slot_is_actively_loaded(slot_index));
        lv_subject_set_int(&slot_can_clear_subject_, clear_blocked ? 0 : 1);
        const std::string clear_hint =
            clear_blocked ? helix::ui::clear_spool_blocked_hint(backend_->lane_noun(), slot_index)
                          : std::string();
        lv_subject_copy_string(&slot_clear_hint_subject_, clear_hint.c_str());
        lv_subject_set_int(&slot_clear_hint_visible_subject_, clear_hint.empty() ? 0 : 1);
    }

    // The header names the position in the backend's own word ("Lane 3",
    // "Gate 3"), 1-based. lane_label() owns both halves, so there is no buffer
    // here a translated noun can overrun.
    lv_obj_t* slot_header = lv_obj_find_by_name(menu_obj, "slot_header");
    if (slot_header) {
        lv_label_set_text(slot_header, helix::ui::lane_label(menu_lane_noun(), slot_index).c_str());
    }

    // Show "Select Spool" and "Scan QR Code" buttons if Spoolman is available
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    lv_obj_t* btn_spoolman = helix::ui::find_required(menu_obj, "btn_spoolman", "AmsContextMenu");
    if (btn_spoolman && has_spoolman) {
        lv_obj_clear_flag(btn_spoolman, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t* btn_scan_qr = helix::ui::find_required(menu_obj, "btn_scan_qr", "AmsContextMenu");
    if (btn_scan_qr && has_spoolman) {
#if !defined(HELIX_PLATFORM_ESP32)
        // No camera on the v1 Core+AMS cut — keep Scan QR hidden (default).
        lv_obj_clear_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
#endif
    }

    append_slot_plugin_rows(menu_obj, slot_index);
}

void AmsContextMenu::append_slot_plugin_rows(lv_obj_t* menu_obj, int slot_index) {
    if (!backend_ || !plugin_menu::any_declared(plugin_menu::MenuKind::FilamentSlot))
        return;
    const AmsSystemInfo info = backend_->get_system_info();
    const plugin_menu::json context = plugin_menu::slot_context(
        backend_->get_slot_info(slot_index), slot_index, info.unit_for_slot(slot_index),
        helix::ui::lane_label(menu_lane_noun(), slot_index));
    helix::ui::append_plugin_rows(
        *this, lv_obj_find_by_name(menu_obj, "col_plugins"),
        plugin_menu::resolve(plugin_menu::MenuKind::FilamentSlot, context), context,
        /*divider=*/false, PluginRowLayout::Column);
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

void AmsContextMenu::handle_purge() {
    spdlog::info("[AmsContextMenu] Purge requested");
    dispatch_ams_action(MenuAction::PURGE);
}

void AmsContextMenu::handle_bypass_toggle() {
    spdlog::info("[AmsContextMenu] Bypass toggle requested");
    dispatch_ams_action(MenuAction::TOGGLE_BYPASS);
}

void AmsContextMenu::handle_gate_check() {
    spdlog::info("[AmsContextMenu] Check gate requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::CHECK_GATE);
}

void AmsContextMenu::handle_preload() {
    spdlog::info("[AmsContextMenu] Preload requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::PRELOAD);
}

void AmsContextMenu::handle_mount() {
    spdlog::info("[AmsContextMenu] Mount requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::MOUNT);
}

void AmsContextMenu::handle_park() {
    spdlog::info("[AmsContextMenu] Park requested from slot {}", get_item_index());
    dispatch_ams_action(MenuAction::PARK);
}

void AmsContextMenu::handle_calibrate_pa() {
    spdlog::info("[AmsContextMenu] Calibrate PA requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::CALIBRATE_PA);
}

void AmsContextMenu::handle_edit() {
    spdlog::info("[AmsContextMenu] Edit requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::EDIT);
}

bool AmsContextMenu::decide_offers_pa_calibration(bool printer_measures_pa, bool filament_at_nozzle,
                                                  bool print_blocks_op, bool system_busy) {
    return printer_measures_pa && filament_at_nozzle && !print_blocks_op && !system_busy;
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

AmsContextMenu::SlotOpDecision
AmsContextMenu::decide_slot_ops(const AmsBackend* backend, int slot_index, bool pending_is_loaded,
                                bool system_busy, bool print_blocks_op, bool toolhead_unaccounted) {
    SlotOpDecision d;

    if (backend) {
        // Tri-state: slot_presence() reports SlotStatus::UNKNOWN as
        // "unanswerable" rather than "empty", so a backend that publishes no
        // per-lane presence does not grey Load.
        d.presence = helix::ui::slot_presence(backend->get_slot_info(slot_index));
        // Firmware seated+loaded OR filament at this slot's toolhead sensor, so
        // the gates follow real load state the moment a sensor flips instead of
        // static RFID presence.
        d.live_loaded = backend->slot_is_actively_loaded(slot_index) ||
                        backend->slot_has_filament_at_toolhead(slot_index);
    }

    // Loaded if EITHER the caller's open-time snapshot or the live accessors say
    // so. The OR keeps Unload available for the firmware's active slot even
    // after a runout clears the head sensor (#995).
    d.is_loaded = pending_is_loaded || d.live_loaded;

    // Whether this slot's unload is a heated toolhead unload (true) or a cold
    // per-lane eject (false). Most backends return the hint unchanged; AD5X IFS
    // refines it with seated-channel authority so a NON-seated lane reads
    // "Eject" even where the recovery clause broadened is_loaded.
    d.toolhead_unload =
        backend ? backend->slot_unloads_to_toolhead(slot_index, d.is_loaded) : d.is_loaded;

    // The unload mode keeps the STRICT reading of presence - "is there something
    // to eject" has no useful third answer, and an unknown lane must not grow an
    // Eject button out of nowhere. Only the Load gate below treats UNKNOWN as
    // unanswerable.
    const bool present = d.presence.value_or(false);
    // Filament parked in the toolhead is not loaded (Load keeps reading
    // toolhead_unload) but still has a heated unload to run.
    d.parked_in_toolhead = backend && backend->slot_filament_parked_in_toolhead(slot_index);
    d.unload_mode = decide_unload_mode(d.toolhead_unload || d.parked_in_toolhead,
                                       backend && backend->can_recover_lane_position(slot_index),
                                       backend && backend->lane_recovery_is_attributed(),
                                       backend && backend->supports_lane_eject(), present,
                                       backend && backend->supports_force_eject(), !present);

    d.unload_enabled =
        decide_unload_enabled(system_busy, d.unload_mode, print_blocks_op,
                              backend && backend->cold_lane_ops_refused_during_print());

    // Filament at the toolhead that no lane claims: the lane this menu names
    // may not be the seated one, so its Unload is a guess and a cold Eject of
    // the lane that IS seated grinds un-cut filament. Withdraw the button
    // without touching the mode - the sidebar's active-head Unload covers the
    // state, letting the firmware resolve the real channel.
    if (toolhead_unaccounted) {
        d.unload_enabled = false;
    }

    // Load gates on the NARROWED signal (toolhead_unload), never the broadened
    // is_loaded: is_loaded folds in the open-time snapshot, which reads true for
    // the firmware's seated channel whether or not filament reached the head. A
    // cold-lane-eject lane - filament parked in the lane but NOT at the toolhead
    // - is a valid Load target and has to stay enabled.
    d.can_load = decide_can_load(system_busy, d.toolhead_unload, d.presence, print_blocks_op,
                                 backend && backend->load_feeds_filament());
    return d;
}

bool AmsContextMenu::decide_can_load(bool system_busy, bool toolhead_unload,
                                     std::optional<bool> slot_has_filament, bool print_blocks_op,
                                     bool load_feeds_filament) {
    helix::ui::OpButtonState state;
    state.load_feeds_filament = load_feeds_filament;
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
        {"ams_context_gate_check_cb", on_gate_check_cb},
        {"ams_context_preload_cb", on_preload_cb},
        {"ams_context_mount_cb", on_mount_cb},
        {"ams_context_park_cb", on_park_cb},
        {"ams_context_pa_cal_cb", on_pa_cal_cb},
        {"ams_context_edit_cb", on_edit_cb},
        {"ams_context_clear_spool_cb", on_clear_spool_cb},
        {"ams_context_spoolman_cb", on_spoolman_cb},
        {"ams_context_scan_qr_cb", on_scan_qr_cb},
        {"ams_context_purge_cb", on_purge_cb},
        {"ams_context_bypass_toggle_cb", on_bypass_toggle_cb},
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

void AmsContextMenu::on_gate_check_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_gate_check();
    }
}

void AmsContextMenu::on_mount_cb(lv_event_t* /*e*/) {
    if (auto* self = get_active_instance()) {
        self->handle_mount();
    }
}

void AmsContextMenu::on_park_cb(lv_event_t* /*e*/) {
    if (auto* self = get_active_instance()) {
        self->handle_park();
    }
}

void AmsContextMenu::on_pa_cal_cb(lv_event_t* /*e*/) {
    if (auto* self = get_active_instance()) {
        self->handle_calibrate_pa();
    }
}

void AmsContextMenu::on_preload_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_preload();
    }
}

void AmsContextMenu::on_edit_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_edit();
    }
}

void AmsContextMenu::on_purge_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_purge();
    }
}

void AmsContextMenu::on_bypass_toggle_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_bypass_toggle();
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

LaneNoun AmsContextMenu::menu_lane_noun() const {
    return backend_ ? backend_->lane_noun() : helix::ui::active_lane_noun();
}

void open_slot_editor(lv_obj_t* parent_screen, IMoonrakerAPI* api, int slot_index,
                      bool open_on_picker) {
    if (!parent_screen) {
        spdlog::warn("[AMS] Cannot show slot editor - no parent screen");
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    get_ams_edit_overlay().show_for_slot(
        parent_screen, slot_index, backend->get_slot_info(slot_index), api,
        [](const AmsEditOverlay::EditResult& result) {
            if (!result.saved || result.slot_index < 0) {
                return;
            }
            AmsBackend* backend = AmsState::instance().get_backend();
            if (!backend) {
                return;
            }
            // Capture the pre-edit slot BEFORE the commit: its unlink arm (clear the
            // server active spool) needs the old link.
            SlotInfo original = backend->get_slot_info(result.slot_index);
            AmsError err = AmsState::instance().commit_slot_edit(result.slot_index, original,
                                                                 result.slot_info);
            if (err.success() || err.partially_applied) {
                offer_edit_follow_up(result.slot_index, original, result.slot_info);
            }
            if (!err.success()) {
                notify_ams_error(err);
                return;
            }
            NOTIFY_INFO(lv_tr("{} updated"), lane_label(backend->lane_noun(), result.slot_index));
        },
        open_on_picker);
}

namespace {

void scan_qr_into_slot(const SlotMenuHost& host, int slot) {
    spdlog::info("{} SCAN_QR action for slot {}", host.log_tag, slot);
#if !defined(                                                                                      \
    HELIX_PLATFORM_ESP32) // the ESP32 build has no QR scanner overlay; its button stays hidden
    const std::string tag = host.log_tag;
    get_qr_scanner_overlay().show(host.parent_screen, slot, [slot, tag](const SpoolInfo& spool) {
        if (!AmsState::instance().get_backend()) {
            return;
        }
        const auto link = AmsState::instance().link_spool_to_slot(slot, spool);
        if (link.error.success() || link.error.partially_applied) {
            helix::ui::offer_edit_follow_up(slot, link.original, link.linked);
        }
        if (!link.error.success()) {
            notify_ams_error(link.error);
            return;
        }
        spdlog::info("{} QR scan assigned spool #{} to slot {}", tag, spool.id, slot);
    });
#endif
}

void dispatch_slot_menu_action(const SlotMenuHost& host, AmsContextMenu::MenuAction action,
                               int slot) {
    using MenuAction = AmsContextMenu::MenuAction;

    // EJECT / RECOVER_POSITION / CHECK_GATE / PRELOAD / CLEAR_SPOOL are pure backend
    // calls; see ams_dispatch_backend_action().
    if (ams_dispatch_backend_action(action, slot,
                                    host.path_canvas ? host.path_canvas() : nullptr)) {
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    AmsOperationSidebar* sidebar = host.sidebar ? host.sidebar() : nullptr;

    switch (action) {
    case MenuAction::LOAD:
        if (!backend) {
            NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
            return;
        }
        if (const AmsSystemInfo info = backend->get_system_info();
            info.action != AmsAction::IDLE && info.action != AmsAction::ERROR) {
            NOTIFY_WARNING(lv_tr("Multi-Filament System is busy: {}"),
                           ams_action_to_string(info.action));
            return;
        }
        // Preheat-aware load goes through the sidebar rather than a direct load.
        if (sidebar) {
            sidebar->handle_load_with_preheat(slot);
        }
        break;

    case MenuAction::UNLOAD:
        if (!backend) {
            NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
            return;
        }
        // Through the sidebar so start_operation(UNLOAD) builds the correct stepper
        // (mirrors how LOAD routes via handle_load_with_preheat).
        if (sidebar) {
            sidebar->handle_unload(slot);
        } else if (AmsError error = backend->unload_filament(slot);
                   error.result != AmsResult::SUCCESS) {
            notify_ams_error(error);
        }
        break;

    case MenuAction::EDIT:
        open_slot_editor(host.parent_screen, host.api, slot);
        break;

    case MenuAction::SPOOLMAN:
        open_slot_editor(host.parent_screen, host.api, slot, /*open_on_picker=*/true);
        break;

    case MenuAction::SCAN_QR:
        scan_qr_into_slot(host, slot);
        break;

    case MenuAction::CALIBRATE_PA:
#if !defined(HELIX_PLATFORM_ESP32)
        if (backend) {
            const SlotInfo info = backend->get_slot_info(slot);
            open_pa_cal_panel_for_filament(host.parent_screen, info.material, info.nozzle_temp_min,
                                           info.nozzle_temp_max);
        }
#endif
        break;

    default:
        break;
    }
}

} // namespace

void open_slot_context_menu(AmsContextMenu& menu, const SlotMenuHost& host, int slot_index,
                            lv_obj_t* near_widget, lv_point_t click_pt) {
    if (!host.parent_screen || !near_widget) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    // A bay of a box that is not on the bus offers nothing to act on.
    if (backend && backend->get_system_info().slot_absent(slot_index)) {
        spdlog::debug("{} Slot {} belongs to an absent unit - no menu", host.log_tag, slot_index);
        return;
    }

    menu.set_action_callback([host](AmsContextMenu::MenuAction action, int slot) {
        dispatch_slot_menu_action(host, action, slot);
    });

    // Whether to offer Unload is decoupled from the display LOADED status, so a runout
    // that clears the head sensor does not disable Unload on the firmware's active slot
    // (#995).
    const bool is_loaded = backend && backend->can_unload_from_toolhead(slot_index);

    menu.set_click_point(click_pt);
    menu.show_near_widget(host.parent_screen, slot_index, near_widget, is_loaded, backend);
}

} // namespace helix::ui
