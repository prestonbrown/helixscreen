// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_ams.h"

#include "ui_ams_detail.h"
#include "ui_ams_device_operations_overlay.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_ams_slot_layout.h"
#include "ui_endless_spool_arrows.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_filament_path_canvas.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_spool_canvas.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "color_utils.h"
#include "config.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <unordered_map>

using namespace helix;

// Default slot width for endless arrows canvas (when layout not yet computed)
static constexpr int32_t DEFAULT_SLOT_WIDTH = 80;

// Logo path mapping moved to AmsState::get_logo_path()

// Voron printer check moved to PrinterDetector::is_voron_printer()

// Lazy registration flag - widgets and XML registered on first use
static bool s_ams_widgets_registered = false;

/**
 * @brief Register AMS widgets and XML component (lazy, called once on first use)
 *
 * Registers:
 * - spool_canvas: 3D filament spool visualization widget
 * - ams_slot: Individual slot widget with spool and status
 * - filament_path_canvas: Filament routing visualization
 * - ams_panel.xml: Main panel component
 * - ams_context_menu.xml: Slot context menu component
 */
static void ensure_ams_widgets_registered() {
    if (s_ams_widgets_registered) {
        return;
    }

    spdlog::info("[AMS Panel] Lazy-registering AMS widgets and XML components");

    // Register custom widgets (order matters - dependencies first)
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();

    // Register sidebar callbacks BEFORE XML parsing (callbacks must exist when parser sees them)
    helix::ui::AmsOperationSidebar::register_callbacks_static();

    // Register dryer card callbacks BEFORE XML parsing (callbacks must exist when parser sees them)
    helix::ui::AmsDryerCard::register_callbacks_static();

    // Register AMS device operations overlay callbacks BEFORE XML parsing
    helix::ui::get_ams_device_operations_overlay().register_callbacks();

    // Context menu callbacks registered by helix::ui::AmsContextMenu class
    // Edit modal and color picker callbacks registered by helix::ui::AmsEditModal class

    // Register XML components (dryer card must be registered before ams_panel since it's used
    // there)
    lv_xml_register_component_from_file("A:ui_xml/ams_dryer_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/dryer_presets_modal.xml");
    // NOTE: Old AMS settings panels removed - Device Operations overlay is registered in
    // xml_registration.cpp
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml");
    lv_xml_register_component_from_file("A:ui_xml/spoolman_spool_item.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_edit_modal.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_loading_error_modal.xml");
    // NOTE: color_picker.xml is registered at startup in xml_registration.cpp

    s_ams_widgets_registered = true;
    spdlog::debug("[AMS Panel] Widget and XML registration complete");
}

// XML event callbacks now handled by helix::ui::AmsOperationSidebar class
// Dryer card callbacks now handled by helix::ui::AmsDryerCard class
// Context menu callbacks handled by helix::ui::AmsContextMenu class
// Edit modal callbacks handled by helix::ui::AmsEditModal class

// ============================================================================
// Construction
// ============================================================================

AmsPanel::AmsPanel(PrinterState& printer_state, MoonrakerAPI* api) : PanelBase(printer_state, api) {
    spdlog::debug("[AmsPanel] Constructed");
}

// ============================================================================
// PanelBase Interface
// ============================================================================

void AmsPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // AmsState handles all subject registration centrally
    // We just ensure it's initialized before panel creation
    AmsState::instance().init_subjects(true);

    // NOTE: Backend creation is handled by:
    // - main.cpp (mock mode at startup)
    // - AmsState::init_backend_from_capabilities() (real printer connection)
    // Panel should NOT create backends - it just observes the existing one.

    // Register observers for state changes
    // Using observer factory for action and slot_count; others use traditional callbacks
    using helix::ui::observe_int_sync;

    slots_version_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_slots_version_subject(), this, [](AmsPanel* self, int) {
            if (!self->subjects_initialized_ || !self->panel_)
                return;
            spdlog::trace("[AmsPanel] Gates version changed - refreshing slots");
            self->refresh_slots();
        });

    // Simplified action observer - only handles panel-specific concerns
    // (path canvas heat glow and error modal). Step progress is handled by sidebar_.
    action_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_ams_action_subject(), this, [](AmsPanel* self, int action_int) {
            if (!self->subjects_initialized_ || !self->panel_)
                return;
            auto action = static_cast<AmsAction>(action_int);

            // Path canvas heat glow (panel-specific)
            if (self->path_canvas_) {
                bool heating = (action == AmsAction::HEATING);
                ui_filament_path_canvas_set_heat_active(self->path_canvas_, heating);
            }

            // Error modal (panel-specific)
            if (action == AmsAction::ERROR) {
                if (!self->error_modal_ || !self->error_modal_->is_visible()) {
                    self->show_loading_error_modal();
                }
            }
        });

    current_slot_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_current_slot_subject(), this, [](AmsPanel* self, int slot) {
            if (!self->subjects_initialized_ || !self->panel_)
                return;
            spdlog::debug("[AmsPanel] Current slot changed: {}", slot);
            self->update_current_slot_highlight(slot);
            self->update_path_canvas_from_backend();

            // Auto-set active Spoolman spool when slot becomes active.
            // Skip when the backend manages active spool itself (e.g., AFC).
            if (slot >= 0 && self->api_) {
                auto* backend = AmsState::instance().get_backend();
                if (backend && !backend->manages_active_spool()) {
                    SlotInfo slot_info = backend->get_slot_info(slot);
                    if (slot_info.spoolman_id > 0) {
                        spdlog::info(
                            "[AmsPanel] Slot {} has Spoolman ID {}, setting as active spool", slot,
                            slot_info.spoolman_id);
                        self->api_->spoolman().set_active_spool(
                            slot_info.spoolman_id,
                            []() { spdlog::debug("[AmsPanel] Active spool set successfully"); },
                            [](const MoonrakerError& err) {
                                spdlog::warn("[AmsPanel] Failed to set active spool: {}",
                                             err.message);
                            });
                    }
                }
            }
        });

    // Slot count observer for dynamic slot creation (non-scoped mode only)
    slot_count_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_slot_count_subject(), this, [](AmsPanel* self, int new_count) {
            if (!self->panel_)
                return;
            // When scoped to a unit, on_activate() handles slot creation with correct offsets.
            // Don't let the global slot count observer override that.
            if (self->scoped_unit_index_ >= 0)
                return;
            spdlog::debug("[AmsPanel] Slot count changed to {}", new_count);
            self->create_slots(new_count);
        });

    // Path state observers for filament path visualization
    auto path_handler = [](AmsPanel* self, int) {
        if (!self->subjects_initialized_ || !self->panel_)
            return;
        spdlog::debug("[AmsPanel] Path state changed - updating path canvas");
        self->update_path_canvas_from_backend();
    };
    path_segment_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_path_filament_segment_subject(), this, path_handler);
    path_topology_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_path_topology_subject(), this, path_handler);

    // Backend count observer for multi-backend selector
    backend_count_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_backend_count_subject(), this,
        [](AmsPanel* self, int /*count*/) { self->rebuild_backend_selector(); });

    // Observe external spool color changes to reactively update bypass in path canvas.
    // NOTE: set_external_spool_info() calls lv_subject_set_int() directly (not via
    // ui_queue_update) which is safe because all current callers are on the LVGL thread.
    // If callers from background threads are added, those must use ui_queue_update().
    external_spool_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](AmsPanel* self, int /*color_int*/) {
            // Update path canvas bypass indicator
            if (self->path_canvas_) {
                // Use full spool info check (not just color != 0) to handle black spools correctly
                auto ext_spool = AmsState::instance().get_external_spool_info();
                bool has_spool = ext_spool.has_value();
                ui_filament_path_canvas_set_bypass_has_spool(self->path_canvas_, has_spool);
                if (has_spool) {
                    ui_filament_path_canvas_set_bypass_color(
                        self->path_canvas_, static_cast<uint32_t>(ext_spool->color_rgb));
                }
            }

            // Update bypass spool holder (3D spool in left column)
            self->update_bypass_spool_from_state();
        });

    // UI module subjects are now encapsulated in their respective classes:
    // - helix::ui::AmsEditModal
    // - helix::ui::AmsColorPicker

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized via AmsState + observers registered", get_name());
}

void AmsPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::debug("[{}] Setting up...", get_name());

    // Use standard overlay panel setup (header bar, responsive padding)
    ui_overlay_panel_setup_standard(panel_, parent_screen_, "overlay_header", "overlay_content");

    // Setup UI components
    setup_system_header();
    setup_slots();
    setup_path_canvas();

    // Setup bypass spool holder (left column, below path canvas)
    setup_bypass_spool();

    // Setup endless spool arrows
    setup_endless_arrows();

    // Setup shared sidebar component
    sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(printer_state_, api_);
    sidebar_->setup(panel_);
    sidebar_->init_observers();

    // Initial UI sync from backend state
    refresh_slots();

    spdlog::debug("[{}] Setup complete!", get_name());
}

void AmsPanel::on_activate() {
    spdlog::debug("[{}] Activated - syncing from backend", get_name());

    // Sync state when panel becomes visible
    AmsState::instance().sync_from_backend();

    // Create/recreate slots based on scope
    if (scoped_unit_index_ >= 0) {
        // Scoped: show only this unit's slots
        auto* backend = AmsState::instance().get_backend();
        if (backend) {
            AmsSystemInfo info = backend->get_system_info();
            if (scoped_unit_index_ < static_cast<int>(info.units.size())) {
                int unit_slots = info.units[scoped_unit_index_].slot_count;
                spdlog::info("[{}] Scoped to unit {} with {} slots", get_name(), scoped_unit_index_,
                             unit_slots);
                create_slots(unit_slots);
                setup_system_header();
            }
        }

        // Hide elements that don't apply to a single-unit scoped view:
        // path canvas (hub/bypass/toolhead routing), bypass toggle, dryer card
        lv_obj_t* path_container = lv_obj_find_by_name(panel_, "path_container");
        if (path_container)
            lv_obj_add_flag(path_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* bypass_row = lv_obj_find_by_name(panel_, "bypass_row");
        if (bypass_row)
            lv_obj_add_flag(bypass_row, LV_OBJ_FLAG_HIDDEN);
        // In scoped view, force-hide dryer (system-level feature, not per-unit)
        lv_obj_t* dryer_card = lv_obj_find_by_name(panel_, "dryer_card");
        if (dryer_card)
            lv_obj_add_flag(dryer_card, LV_OBJ_FLAG_HIDDEN);
        // Hide bypass spool holder in scoped view (bypass is system-level)
        if (bypass_spool_box_) {
            lv_obj_delete(bypass_spool_box_);
            bypass_spool_box_ = nullptr;
            bypass_spool_ = nullptr;
        }
    } else {
        // Non-scoped: show all system slots
        int slot_count = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
        if (slot_count != current_slot_count_) {
            create_slots(slot_count);
        }
        setup_system_header();

        // Restore elements hidden by scoped view
        lv_obj_t* path_container = lv_obj_find_by_name(panel_, "path_container");
        if (path_container)
            lv_obj_remove_flag(path_container, LV_OBJ_FLAG_HIDDEN);
        // bypass_row visibility managed by bind_flag_if_eq on ams_supports_bypass subject
        // dryer_card visibility managed by bind_flag_if_eq on dryer_supported subject
    }

    refresh_slots();

    // Ensure filament path canvas redraws after being stopped on deactivate
    if (path_canvas_) {
        ui_filament_path_canvas_refresh(path_canvas_);
    }

    // Sync sidebar step progress and preheat feedback from current state
    if (sidebar_) {
        sidebar_->sync_from_state();
    }

    // Sync Spoolman active spool with currently loaded slot
    sync_spoolman_active_spool();

    // Start Spoolman polling for slot weight updates
    AmsState::instance().start_spoolman_polling();
}

void AmsPanel::sync_spoolman_active_spool() {
    if (!api_) {
        return;
    }

    int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());
    if (current_slot < 0) {
        return; // No active slot
    }

    auto* backend = AmsState::instance().get_backend();
    if (!backend || backend->manages_active_spool()) {
        return;
    }

    SlotInfo slot_info = backend->get_slot_info(current_slot);
    if (slot_info.spoolman_id > 0) {
        spdlog::debug("[{}] Syncing Spoolman: slot {} → spool ID {}", get_name(), current_slot,
                      slot_info.spoolman_id);
        api_->spoolman().set_active_spool(
            slot_info.spoolman_id, []() {},
            [](const MoonrakerError& err) {
                spdlog::warn("[AmsPanel] Failed to sync active spool: {}", err.message);
            });
    }
}

void AmsPanel::on_deactivate() {
    AmsState::instance().stop_spoolman_polling();

    // Stop filament path animations to avoid burning CPU in the background
    if (path_canvas_) {
        ui_filament_path_canvas_stop_animations(path_canvas_);
    }

    spdlog::debug("[{}] Deactivated", get_name());
    // Note: UI destruction is handled by NavigationManager close callback
    // registered in get_global_ams_panel()
}

void AmsPanel::clear_panel_reference() {
    // Mark subjects uninitialized FIRST — observer callbacks check this and bail out
    subjects_initialized_ = false;

    // Reset extracted UI modules (they handle their own RAII cleanup)
    sidebar_.reset();
    context_menu_.reset();
    edit_modal_.reset();
    error_modal_.reset();

    // Nullify widget pointers BEFORE resetting observers — any cascading
    // observer callbacks during teardown will see null and bail out.
    panel_ = nullptr;
    parent_screen_ = nullptr;
    slot_grid_ = nullptr;
    detail_widgets_ = AmsDetailWidgets{};
    path_canvas_ = nullptr;
    bypass_spool_box_ = nullptr;
    bypass_spool_ = nullptr;
    endless_arrows_ = nullptr;
    current_slot_count_ = 0;

    for (int i = 0; i < MAX_VISIBLE_SLOTS; ++i) {
        slot_widgets_[i] = nullptr;
        label_widgets_[i] = nullptr;
    }

    // Now reset observer guards
    slots_version_observer_.reset();
    action_observer_.reset();
    current_slot_observer_.reset();
    slot_count_observer_.reset();
    path_segment_observer_.reset();
    path_topology_observer_.reset();
    backend_count_observer_.reset();
    external_spool_observer_.reset();

    spdlog::debug("[AMS Panel] Cleared all widget references");
}

void AmsPanel::set_unit_scope(int unit_index) {
    spdlog::info("[AmsPanel] Setting unit scope to {}", unit_index);
    scoped_unit_index_ = unit_index;
}

void AmsPanel::clear_unit_scope() {
    spdlog::debug("[AmsPanel] Clearing unit scope");
    scoped_unit_index_ = -1;
}

// ============================================================================
// Setup Helpers
// ============================================================================

void AmsPanel::setup_system_header() {
    // System logo + name in header bar are declaratively bound to
    // ams_system_logo / ams_system_name subjects (updated by AmsState::sync_from_backend).
    //
    // Only the scoped-unit case needs imperative override, since subjects
    // hold system-level info, not per-unit info.
    if (scoped_unit_index_ < 0) {
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }

    const auto& info = backend->get_system_info();
    if (scoped_unit_index_ >= static_cast<int>(info.units.size())) {
        return;
    }

    const AmsUnit& unit = info.units[scoped_unit_index_];

    // Override header with unit-specific logo + name
    lv_obj_t* system_logo = lv_obj_find_by_name(panel_, "system_logo");
    if (system_logo) {
        ams_draw::apply_logo(system_logo, unit, info);
    }

    lv_obj_t* name_label = lv_obj_find_by_name(panel_, "system_name_label");
    if (name_label) {
        std::string display_name = ams_draw::get_unit_display_name(unit, scoped_unit_index_);
        lv_label_set_text(name_label, display_name.c_str());
    }

    spdlog::info("[{}] Scoped to unit {}: '{}'", get_name(), scoped_unit_index_, unit.name);
}

void AmsPanel::rebuild_backend_selector() {
    if (!panel_) {
        return;
    }

    lv_obj_t* row = lv_obj_find_by_name(panel_, "backend_selector_row");
    if (!row) {
        return;
    }

    auto& ams = AmsState::instance();
    int count = ams.backend_count();

    if (count <= 1) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);

    // Clear existing children
    while (lv_obj_get_child_count(row) > 0) {
        lv_obj_delete(lv_obj_get_child(row, 0));
    }

    for (int i = 0; i < count; ++i) {
        auto* backend = ams.get_backend(i);
        if (!backend) {
            continue;
        }

        std::string label = ams_type_to_string(backend->get_type());

        // Create a button-like segment for each backend
        lv_obj_t* btn = lv_obj_create(row);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(btn, 8, 0);
        lv_obj_set_style_pad_left(btn, 12, 0);
        lv_obj_set_style_pad_right(btn, 12, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

        if (i == active_backend_idx_) {
            lv_obj_set_style_bg_color(btn, theme_manager_get_color("primary"), 0);
        } else {
            lv_obj_set_style_bg_color(btn, theme_manager_get_color("elevated_bg"), 0);
        }

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, theme_manager_get_font("font_small"), 0);

        // Store index in user_data for click handler (dynamic buttons are a documented exception)
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto* btn_obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
                int idx =
                    static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn_obj)));
                auto* self = static_cast<AmsPanel*>(lv_event_get_user_data(e));
                if (self) {
                    self->on_backend_segment_selected(idx);
                }
            },
            LV_EVENT_CLICKED, this);
    }

    spdlog::debug("[AmsPanel] Backend selector rebuilt with {} segments (active={})", count,
                  active_backend_idx_);
}

void AmsPanel::on_backend_segment_selected(int index) {
    if (index == active_backend_idx_) {
        return;
    }

    active_backend_idx_ = index;
    AmsState::instance().set_active_backend(index);

    // Rebuild selector to update visual highlight
    rebuild_backend_selector();

    // Sync the selected backend and recreate slots
    AmsState::instance().sync_backend(index);

    auto* backend = AmsState::instance().get_backend(index);
    if (backend) {
        auto info = backend->get_system_info();
        create_slots(info.total_slots);

        // Update system header (logo + name)
        setup_system_header();

        // Update path visualization for this backend
        update_path_canvas_from_backend();
    }

    spdlog::info("[AmsPanel] Switched to backend {} ({})", index,
                 backend ? ams_type_to_string(backend->get_type()) : "null");
}

void AmsPanel::setup_slots() {
    lv_obj_t* unit_detail = lv_obj_find_by_name(panel_, "unit_detail");
    if (!unit_detail) {
        spdlog::warn("[{}] unit_detail not found in XML", get_name());
        return;
    }

    detail_widgets_ = ams_detail_find_widgets(unit_detail);
    slot_grid_ = detail_widgets_.slot_grid; // Keep for path canvas sync

    spdlog::debug("[{}] setup_slots: widgets resolved, slot creation deferred to on_activate()",
                  get_name());
}

void AmsPanel::create_slots(int count) {
    (void)count; // Slot count determined by shared helper from backend

    // Destroy existing
    ams_detail_destroy_slots(detail_widgets_, slot_widgets_, current_slot_count_);

    // Determine unit index for scoped views
    int unit_index = scoped_unit_index_;

    // Create new slots
    auto result = ams_detail_create_slots(detail_widgets_, slot_widgets_, MAX_VISIBLE_SLOTS,
                                          unit_index, on_slot_clicked, this);

    current_slot_count_ = result.slot_count;

    // Labels overlay for 5+ slots
    ams_detail_update_labels(detail_widgets_, slot_widgets_, result.slot_count, result.layout);

    // Move badges to overlay layer (in front of tray)
    ams_detail_update_badges(detail_widgets_, slot_widgets_, result.slot_count, result.layout);

    // Update path canvas sizing
    if (path_canvas_) {
        ui_filament_path_canvas_set_slot_overlap(path_canvas_, result.layout.overlap);
        ui_filament_path_canvas_set_slot_width(path_canvas_, result.layout.slot_width);
    }

    spdlog::info("[{}] Created {} slot widgets via shared helpers", get_name(), result.slot_count);

    // Update tray
    ams_detail_update_tray(detail_widgets_);
}

// on_slot_count_changed migrated to lambda in init_subjects()

void AmsPanel::setup_path_canvas() {
    path_canvas_ = lv_obj_find_by_name(panel_, "path_canvas");
    if (!path_canvas_) {
        spdlog::warn("[{}] path_canvas not found in XML", get_name());
        return;
    }

    // Set slot click callback (panel-specific)
    ui_filament_path_canvas_set_slot_callback(path_canvas_, on_path_slot_clicked, this);

    // Set bypass spool click callback (opens edit modal for external spool)
    ui_filament_path_canvas_set_bypass_callback(path_canvas_, on_bypass_spool_clicked, this);

    // Configure from backend using shared helper
    ams_detail_setup_path_canvas(path_canvas_, slot_grid_, scoped_unit_index_, false);

    spdlog::debug("[{}] Path canvas setup complete", get_name());
}

void AmsPanel::update_path_canvas_from_backend() {
    ams_detail_setup_path_canvas(path_canvas_, slot_grid_, scoped_unit_index_, false);
}

void AmsPanel::setup_bypass_spool() {
    if (!path_canvas_) {
        spdlog::debug("[{}] No path canvas — skipping bypass spool setup", get_name());
        return;
    }

    // Check if bypass is supported
    auto* backend = AmsState::instance().get_backend();
    if (!backend || !backend->get_system_info().supports_bypass) {
        spdlog::debug("[{}] Bypass not supported — skipping spool holder", get_name());
        return;
    }

    // Create a small card container as child of path_container (sibling of path_canvas)
    // with absolute positioning to overlay at the bypass endpoint location
    lv_obj_t* path_container = lv_obj_get_parent(path_canvas_);
    if (!path_container) {
        return;
    }

    bypass_spool_box_ = lv_obj_create(path_container);
    lv_obj_set_size(bypass_spool_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bypass_spool_box_, 4, 0);
    lv_obj_set_style_bg_opa(bypass_spool_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(bypass_spool_box_, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_radius(bypass_spool_box_, theme_manager_get_spacing("border_radius"), 0);
    lv_obj_set_style_border_width(bypass_spool_box_, 0, 0);
    lv_obj_remove_flag(bypass_spool_box_, LV_OBJ_FLAG_SCROLLABLE);

    // Create spool_canvas inside the card
    static constexpr int32_t BYPASS_SPOOL_SIZE = 48;
    bypass_spool_ = ui_spool_canvas_create(bypass_spool_box_, BYPASS_SPOOL_SIZE);
    if (!bypass_spool_) {
        spdlog::warn("[{}] Failed to create bypass spool canvas", get_name());
        lv_obj_delete(bypass_spool_box_);
        bypass_spool_box_ = nullptr;
        return;
    }

    // Position will be set after layout in update_bypass_spool_position()
    // For now, use flag-based flow positioning (will be corrected on first layout update)
    lv_obj_add_flag(bypass_spool_box_, LV_OBJ_FLAG_FLOATING);

    // Make clickable → opens edit modal for external spool (slot -2)
    lv_obj_add_flag(bypass_spool_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        bypass_spool_box_,
        [](lv_event_t* e) {
            auto* self = static_cast<AmsPanel*>(lv_event_get_user_data(e));
            if (self) {
                self->show_edit_modal(-2);
            }
        },
        LV_EVENT_CLICKED, this);

    // Set initial color/fill
    update_bypass_spool_from_state();

    // Position spool widget relative to bypass merge point in path canvas.
    // Layout: "Bypass" label → horizontal line → spool widget (top to bottom)
    lv_obj_update_layout(path_canvas_);
    int32_t canvas_w = lv_obj_get_width(path_canvas_);
    int32_t canvas_h = lv_obj_get_height(path_canvas_);
    int32_t canvas_x = lv_obj_get_x(path_canvas_);
    int32_t canvas_y = lv_obj_get_y(path_canvas_);

    // Match the path canvas bypass ratios — spool aligns with bypass merge sensor
    static constexpr float BYPASS_X_RATIO = 0.85f;
    static constexpr float BYPASS_MERGE_Y_RATIO = 0.44f;
    int32_t bypass_x = canvas_x + (int32_t)(canvas_w * BYPASS_X_RATIO);
    int32_t bypass_merge_y = canvas_y + (int32_t)(canvas_h * BYPASS_MERGE_Y_RATIO);

    // Center spool vertically on the bypass merge line (horizontal filament path)
    lv_obj_update_layout(bypass_spool_box_);
    int32_t box_w = lv_obj_get_width(bypass_spool_box_);
    int32_t box_h = lv_obj_get_height(bypass_spool_box_);
    lv_obj_set_pos(bypass_spool_box_, bypass_x - box_w / 2, bypass_merge_y - box_h / 2);

    spdlog::debug("[{}] Bypass spool: {}x{} at ({},{}), merge_y={}", get_name(), box_w, box_h,
                  bypass_x - box_w / 2, bypass_merge_y - box_h / 2, bypass_merge_y);
}

void AmsPanel::update_bypass_spool_from_state() {
    if (!bypass_spool_) {
        return;
    }

    auto ext = AmsState::instance().get_external_spool_info();
    if (ext.has_value()) {
        ui_spool_canvas_set_color(bypass_spool_, lv_color_hex(ext->color_rgb));
        float fill =
            (ext->total_weight_g > 0) ? ext->remaining_weight_g / ext->total_weight_g : 0.75f;
        ui_spool_canvas_set_fill_level(bypass_spool_, fill);
    } else {
        // No spool assigned — show muted empty spool
        ui_spool_canvas_set_color(bypass_spool_, lv_color_hex(0x505050));
        ui_spool_canvas_set_fill_level(bypass_spool_, 0.0f);
    }
    ui_spool_canvas_redraw(bypass_spool_);
}

void AmsPanel::setup_endless_arrows() {
    endless_arrows_ = lv_obj_find_by_name(panel_, "endless_arrows");
    if (!endless_arrows_) {
        spdlog::warn("[{}] endless_arrows not found in XML - skipping", get_name());
        return;
    }

    spdlog::info("[{}] Found endless_arrows widget", get_name());

    // Initial configuration from backend
    update_endless_arrows_from_backend();

    spdlog::info("[{}] Endless spool arrows setup complete", get_name());
}

void AmsPanel::update_endless_arrows_from_backend() {
    if (!endless_arrows_) {
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        ui_endless_spool_arrows_clear(endless_arrows_);
        lv_obj_add_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Check if endless spool is supported
    auto capabilities = backend->get_endless_spool_capabilities();
    if (!capabilities.supported) {
        ui_endless_spool_arrows_clear(endless_arrows_);
        lv_obj_add_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Get the endless spool configuration
    auto configs = backend->get_endless_spool_config();
    if (configs.empty()) {
        ui_endless_spool_arrows_clear(endless_arrows_);
        lv_obj_add_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Check if any backup is configured
    bool has_any_backup = false;
    for (const auto& config : configs) {
        if (config.backup_slot >= 0) {
            has_any_backup = true;
            break;
        }
    }

    if (!has_any_backup) {
        spdlog::trace("[{}] No endless spool backups configured - hiding arrows", get_name());
        ui_endless_spool_arrows_clear(endless_arrows_);
        lv_obj_add_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    spdlog::trace("[{}] Endless spool has {} configs with backups", get_name(), configs.size());

    // Build backup slots array
    int backup_slots[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int slot_count = 0;
    for (const auto& config : configs) {
        if (config.slot_index >= 0 && config.slot_index < 16) {
            backup_slots[config.slot_index] = config.backup_slot;
            slot_count = std::max(slot_count, config.slot_index + 1);
        }
    }

    // Get slot width and overlap from current layout
    int32_t slot_width = DEFAULT_SLOT_WIDTH;
    int32_t overlap = 0;
    if (slot_grid_) {
        lv_obj_t* slot_area = lv_obj_get_parent(slot_grid_);
        if (slot_area) {
            lv_obj_update_layout(slot_area);
            int32_t available_width = lv_obj_get_content_width(slot_area);
            auto layout = calculate_ams_slot_layout(available_width, slot_count);
            slot_width = layout.slot_width > 0 ? layout.slot_width : DEFAULT_SLOT_WIDTH;
            overlap = layout.overlap;
        }
    }

    // Update canvas
    ui_endless_spool_arrows_set_slot_count(endless_arrows_, slot_count);
    ui_endless_spool_arrows_set_slot_width(endless_arrows_, slot_width);
    ui_endless_spool_arrows_set_slot_overlap(endless_arrows_, overlap);
    ui_endless_spool_arrows_set_config(endless_arrows_, backup_slots, slot_count);

    // Show the canvas
    lv_obj_remove_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);

    spdlog::debug("[{}] Endless arrows updated with {} slots", get_name(), slot_count);
}

// Step progress, start_operation, preheat methods moved to AmsOperationSidebar

// ============================================================================
// Public API
// ============================================================================

void AmsPanel::refresh_slots() {
    if (!panel_ || !subjects_initialized_) {
        return;
    }

    update_slot_colors();

    // Update current slot highlight
    int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());
    update_current_slot_highlight(current_slot);

    // Update endless spool arrows (config may have changed)
    update_endless_arrows_from_backend();
}

// ============================================================================
// UI Update Handlers
// ============================================================================

void AmsPanel::update_slot_colors() {
    int slot_count = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    int backend_idx = AmsState::instance().active_backend_index();
    AmsBackend* backend = AmsState::instance().get_backend(backend_idx);

    for (int i = 0; i < MAX_VISIBLE_SLOTS; ++i) {
        if (!slot_widgets_[i]) {
            continue;
        }

        if (i >= slot_count) {
            // Hide slots beyond configured count
            lv_obj_add_flag(slot_widgets_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_remove_flag(slot_widgets_[i], LV_OBJ_FLAG_HIDDEN);

        // Get slot color from AmsState subject (using active backend)
        lv_subject_t* color_subject = AmsState::instance().get_slot_color_subject(backend_idx, i);
        if (color_subject) {
            uint32_t rgb = static_cast<uint32_t>(lv_subject_get_int(color_subject));
            lv_color_t color = lv_color_hex(rgb);

            // Find color swatch within slot
            lv_obj_t* swatch = lv_obj_find_by_name(slot_widgets_[i], "color_swatch");
            if (swatch) {
                lv_obj_set_style_bg_color(swatch, color, 0);
            }
        }

        // Update material label and fill level from backend slot info
        if (backend) {
            SlotInfo slot_info = backend->get_slot_info(i);

            // Update slot-internal material label
            // Truncate long material names when many slots to prevent overlap
            lv_obj_t* material_label = lv_obj_find_by_name(slot_widgets_[i], "material_label");
            if (material_label) {
                if (!slot_info.material.empty()) {
                    std::string material = slot_info.material;
                    // Truncate to 4 chars when overlapping (5+ slots)
                    if (slot_count > 4 && material.length() > 4) {
                        material = material.substr(0, 4);
                    }
                    lv_label_set_text(material_label, material.c_str());
                } else {
                    lv_label_set_text(material_label, "---");
                }
            }

            // Set fill level from Spoolman weight data
            if (slot_info.total_weight_g > 0.0f) {
                float fill_level = slot_info.remaining_weight_g / slot_info.total_weight_g;
                ui_ams_slot_set_fill_level(slot_widgets_[i], fill_level);
            } else if (slot_info.has_filament_info()) {
                // Weight data unknown — show 75% rather than defaulting to full
                ui_ams_slot_set_fill_level(slot_widgets_[i], 0.75f);
            }

            // Refresh slot to update tool badge and other dynamic state
            ui_ams_slot_refresh(slot_widgets_[i]);
        }

        // Update status indicator
        update_slot_status(i);
    }
}

void AmsPanel::update_slot_status(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_VISIBLE_SLOTS || !slot_widgets_[slot_index]) {
        return;
    }

    int backend_idx = AmsState::instance().active_backend_index();
    lv_subject_t* status_subject =
        AmsState::instance().get_slot_status_subject(backend_idx, slot_index);
    if (!status_subject) {
        return;
    }

    auto status = static_cast<SlotStatus>(lv_subject_get_int(status_subject));

    // Find status indicator icon within slot
    lv_obj_t* status_icon = lv_obj_find_by_name(slot_widgets_[slot_index], "status_icon");
    if (!status_icon) {
        return;
    }

    // Update icon based on status
    switch (status) {
    case SlotStatus::EMPTY:
        // Show empty indicator
        lv_obj_remove_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(status_icon, LV_OPA_30, 0);
        break;

    case SlotStatus::AVAILABLE:
    case SlotStatus::FROM_BUFFER:
        // Show filament available
        lv_obj_remove_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(status_icon, LV_OPA_100, 0);
        break;

    case SlotStatus::LOADED:
        // Show loaded (highlighted)
        lv_obj_remove_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(status_icon, LV_OPA_100, 0);
        break;

    case SlotStatus::BLOCKED:
        // Show error state
        lv_obj_remove_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(status_icon, LV_OPA_100, 0);
        break;

    case SlotStatus::UNKNOWN:
    default:
        lv_obj_add_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

void AmsPanel::update_current_slot_highlight(int slot_index) {
    // NOTE: Visual highlight (border + glow) on spool_container is fully handled
    // by slot-level observers in ui_ams_slot.cpp (apply_current_slot_highlight).
    // Loaded card display is handled by sidebar's own current_slot observer.

    // Update bypass-related state for path canvas visualization
    AmsBackend* backend = AmsState::instance().get_backend();
    bool bypass_active = (slot_index == -2 && backend && backend->is_bypass_active());

    if (path_canvas_) {
        ui_filament_path_canvas_set_bypass_active(path_canvas_, bypass_active);
    }
}

// ============================================================================
// Event Callbacks
// ============================================================================

void AmsPanel::on_bypass_spool_clicked(void* user_data) {
    auto* self = static_cast<AmsPanel*>(user_data);
    if (self) {
        self->handle_bypass_spool_click();
    }
}

void AmsPanel::handle_bypass_spool_click() {
    if (!parent_screen_ || !path_canvas_) {
        return;
    }

    // Capture click point from input device for menu positioning
    lv_point_t click_pt = {0, 0};
    lv_indev_t* indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &click_pt);
    }

    // Create context menu on first use
    if (!context_menu_) {
        context_menu_ = std::make_unique<helix::ui::AmsContextMenu>();
    }

    // Set callback to handle menu actions for external spool
    context_menu_->set_action_callback(
        [this](helix::ui::AmsContextMenu::MenuAction action, int /*slot*/) {
            switch (action) {
            case helix::ui::AmsContextMenu::MenuAction::EDIT:
                show_edit_modal(-2);
                break;

            case helix::ui::AmsContextMenu::MenuAction::SPOOLMAN:
                show_edit_modal(-2);
                break;

            case helix::ui::AmsContextMenu::MenuAction::CLEAR_SPOOL:
                AmsState::instance().clear_external_spool_info();
                // bypass display update handled reactively by external_spool_observer_
                NOTIFY_INFO("External spool cleared");
                break;

            case helix::ui::AmsContextMenu::MenuAction::CANCELLED:
            default:
                break;
            }
        });

    // Position menu at click point, show for external spool
    context_menu_->set_click_point(click_pt);
    context_menu_->show_for_external_spool(parent_screen_, path_canvas_);
}

void AmsPanel::on_path_slot_clicked(int slot_index, void* user_data) {
    auto* self = static_cast<AmsPanel*>(user_data);
    if (!self) {
        return;
    }

    spdlog::info("[AmsPanel] Path slot {} clicked - triggering load", slot_index);

    // Trigger filament load for the clicked slot
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("AMS not available");
        return;
    }

    // Check if backend is busy (lockout during in-flight tool changes)
    AmsSystemInfo info = backend->get_system_info();
    if (info.action != AmsAction::IDLE && info.action != AmsAction::ERROR) {
        spdlog::debug("[AmsPanel] Ignoring path click - backend busy: {}",
                      ams_action_to_string(info.action));
        return;
    }

    // Ignore click on the already-active tool
    if (info.current_slot >= 0 && info.current_slot == slot_index) {
        spdlog::debug("[AmsPanel] Ignoring path click - tool {} already active", slot_index);
        return;
    }

    AmsError error;

    // If filament is already loaded from a DIFFERENT slot, use tool change (unload-then-load)
    if (info.current_slot >= 0 && info.current_slot != slot_index) {
        // Get the tool number for the target slot
        const SlotInfo* slot_info = info.get_slot_global(slot_index);
        if (slot_info && slot_info->mapped_tool >= 0) {
            spdlog::info(
                "[AmsPanel] Slot {} already loaded, swapping to slot {} via tool change T{}",
                info.current_slot, slot_index, slot_info->mapped_tool);
            // Set up step progress BEFORE backend call
            if (self->sidebar_) {
                self->sidebar_->start_operation(StepOperationType::LOAD_SWAP, slot_index);
            }
            error = backend->change_tool(slot_info->mapped_tool);
        } else {
            // Fallback: unload first, then load
            spdlog::info("[AmsPanel] Slot {} already loaded, unloading first then loading {}",
                         info.current_slot, slot_index);
            if (self->sidebar_) {
                self->sidebar_->start_operation(StepOperationType::UNLOAD, info.current_slot);
            }
            error = backend->unload_filament();
            if (error.result == AmsResult::SUCCESS) {
                // Note: The actual load will be triggered after unload completes
                // For now, we'll rely on the user clicking again or the backend auto-loading
                NOTIFY_INFO("Unloading... click again to load slot {}", slot_index + 1);
            }
        }
    } else {
        // Fresh load - no tool currently loaded
        if (self->sidebar_) {
            self->sidebar_->start_operation(StepOperationType::LOAD_FRESH, slot_index);
        }
        error = backend->load_filament(slot_index);
    }

    if (error.result != AmsResult::SUCCESS) {
        NOTIFY_ERROR("Load failed: {}", error.user_msg);
    }
}

void AmsPanel::on_slot_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsPanel] on_slot_clicked");
    auto* self = static_cast<AmsPanel*>(lv_event_get_user_data(e));
    if (self) {
        // Capture click point from the input device while event is still active
        lv_point_t click_pt = {0, 0};
        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_indev_get_point(indev, &click_pt);
        }

        // Use current_target (widget callback was registered on) not target (originally clicked
        // child)
        lv_obj_t* slot = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        auto slot_index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(slot)));
        self->handle_slot_tap(slot_index, click_pt);
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Observer Callbacks
// ============================================================================

// on_slots_version_changed, on_action_changed, on_current_slot_changed, on_path_state_changed
// all migrated to lambdas in init_subjects()

// ============================================================================
// Action Handlers
// ============================================================================

void AmsPanel::handle_slot_tap(int slot_index, lv_point_t click_pt) {
    spdlog::info("[{}] Slot {} tapped", get_name(), slot_index);

    // Validate slot index against configured slot count
    int slot_count = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    if (slot_index < 0 || slot_index >= slot_count) {
        spdlog::warn("[{}] Invalid slot index {} (slot_count={})", get_name(), slot_index,
                     slot_count);
        return;
    }

    // Show context menu near the tapped slot
    if (slot_index >= 0 && slot_index < MAX_VISIBLE_SLOTS && slot_widgets_[slot_index]) {
        show_context_menu(slot_index, slot_widgets_[slot_index], click_pt);
    }
}

// ============================================================================
// Context Menu Management (delegates to helix::ui::AmsContextMenu)
// ============================================================================

void AmsPanel::show_context_menu(int slot_index, lv_obj_t* near_widget, lv_point_t click_pt) {
    if (!parent_screen_ || !near_widget) {
        return;
    }

    // Create context menu on first use
    if (!context_menu_) {
        context_menu_ = std::make_unique<helix::ui::AmsContextMenu>();
    }

    // Set callback to handle menu actions
    context_menu_->set_action_callback(
        [this](helix::ui::AmsContextMenu::MenuAction action, int slot) {
            AmsBackend* backend = AmsState::instance().get_backend();

            switch (action) {
            case helix::ui::AmsContextMenu::MenuAction::LOAD:
                if (!backend) {
                    NOTIFY_WARNING("AMS not available");
                    return;
                }
                // Check if backend is busy
                {
                    AmsSystemInfo info = backend->get_system_info();
                    if (info.action != AmsAction::IDLE && info.action != AmsAction::ERROR) {
                        NOTIFY_WARNING("AMS is busy: {}", ams_action_to_string(info.action));
                        return;
                    }
                }
                // Use preheat-aware load via sidebar instead of direct load
                if (this->sidebar_) {
                    this->sidebar_->handle_load_with_preheat(slot);
                }
                break;

            case helix::ui::AmsContextMenu::MenuAction::UNLOAD:
                if (!backend) {
                    NOTIFY_WARNING("AMS not available");
                    return;
                }
                {
                    AmsError error = backend->unload_filament();
                    if (error.result != AmsResult::SUCCESS) {
                        NOTIFY_ERROR("Unload failed: {}", error.user_msg);
                    }
                }
                break;

            case helix::ui::AmsContextMenu::MenuAction::EJECT:
                if (!backend) {
                    NOTIFY_WARNING("AMS not available");
                    return;
                }
                {
                    AmsError error = backend->eject_lane(slot);
                    if (error.result != AmsResult::SUCCESS) {
                        NOTIFY_ERROR("Eject failed: {}", error.user_msg);
                    }
                }
                break;

            case helix::ui::AmsContextMenu::MenuAction::RESET_LANE:
                if (!backend) {
                    NOTIFY_WARNING("AMS not available");
                    return;
                }
                {
                    AmsError error = backend->reset_lane(slot);
                    if (error.result != AmsResult::SUCCESS) {
                        NOTIFY_ERROR("Reset failed: {}", error.user_msg);
                    }
                }
                break;

            case helix::ui::AmsContextMenu::MenuAction::EDIT:
            case helix::ui::AmsContextMenu::MenuAction::SPOOLMAN:
                show_edit_modal(slot);
                break;

            case helix::ui::AmsContextMenu::MenuAction::CLEAR_SPOOL:
                if (!backend) {
                    NOTIFY_WARNING("AMS not available");
                    return;
                }
                {
                    // Clear spool assignment: reset material/color/spool data, keep slot status
                    SlotInfo cleared = backend->get_slot_info(slot);
                    cleared.material.clear();
                    cleared.color_rgb = AMS_DEFAULT_SLOT_COLOR;
                    cleared.color_name.clear();
                    cleared.multi_color_hexes.clear();
                    cleared.brand.clear();
                    cleared.spool_name.clear();
                    cleared.spoolman_id = 0;
                    cleared.remaining_weight_g = -1;
                    cleared.total_weight_g = -1;
                    auto error = backend->set_slot_info(slot, cleared);
                    if (error.success()) {
                        AmsState::instance().sync_from_backend();
                        refresh_slots();
                        NOTIFY_INFO("Slot {} spool cleared", slot + 1);
                    } else {
                        NOTIFY_ERROR("Clear failed: {}", error.user_msg);
                    }
                }
                break;

            case helix::ui::AmsContextMenu::MenuAction::CANCELLED:
            default:
                break;
            }
        });

    // Determine if the slot is loaded (filament in extruder)
    bool is_loaded = false;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        SlotInfo slot_info = backend->get_slot_info(slot_index);
        is_loaded = (slot_info.status == SlotStatus::LOADED);
    }

    // Position menu near the click point, then show
    context_menu_->set_click_point(click_pt);
    context_menu_->show_near_widget(parent_screen_, slot_index, near_widget, is_loaded, backend);
}

// ============================================================================
// Edit Modal (delegated to helix::ui::AmsEditModal)
// ============================================================================

void AmsPanel::show_edit_modal(int slot_index) {
    if (!parent_screen_) {
        spdlog::warn("[{}] Cannot show edit modal - no parent screen", get_name());
        return;
    }

    // Create modal on first use (lazy initialization)
    if (!edit_modal_) {
        edit_modal_ = std::make_unique<helix::ui::AmsEditModal>();
    }

    // External spool (bypass/direct) — not managed by backend
    if (slot_index == -2) {
        auto ext = AmsState::instance().get_external_spool_info();
        SlotInfo initial_info = ext.value_or(SlotInfo{});
        initial_info.slot_index = -2;
        initial_info.global_index = -2;

        edit_modal_->set_completion_callback([](const helix::ui::AmsEditModal::EditResult& result) {
            if (result.saved) {
                AmsState::instance().set_external_spool_info(result.slot_info);
                // bypass display update handled reactively by external_spool_observer_
                NOTIFY_INFO("External spool updated");
            }
        });
        edit_modal_->show_for_slot(parent_screen_, -2, initial_info, api_);
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("AMS not available");
        return;
    }

    // Get current slot info
    SlotInfo initial_info = backend->get_slot_info(slot_index);

    // Set completion callback to handle save result
    edit_modal_->set_completion_callback([this](const helix::ui::AmsEditModal::EditResult& result) {
        if (result.saved && result.slot_index >= 0) {
            // Apply the edited slot info to the backend
            AmsBackend* backend = AmsState::instance().get_backend();
            if (backend) {
                backend->set_slot_info(result.slot_index, result.slot_info);

                // Update the slot display
                AmsState::instance().sync_from_backend();
                refresh_slots();

                NOTIFY_INFO("Slot {} updated", result.slot_index + 1);
            }
        }
    });

    // Show the modal
    edit_modal_->show_for_slot(parent_screen_, slot_index, initial_info, api_);
}

void AmsPanel::show_loading_error_modal() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Cannot show error modal - no parent screen", get_name());
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }

    // Create modal on first use (lazy initialization)
    if (!error_modal_) {
        error_modal_ = std::make_unique<helix::ui::AmsLoadingErrorModal>();
    }

    // Get error details from backend
    AmsSystemInfo info = backend->get_system_info();
    std::string error_message = info.operation_detail;
    if (error_message.empty()) {
        error_message = "An error occurred during filament loading.";
    }

    // Store slot for retry
    int retry_slot = info.current_slot;

    // Show the error modal with retry callback
    error_modal_->show(parent_screen_, error_message, [this, retry_slot]() {
        // Retry load operation for the same slot
        if (retry_slot >= 0) {
            AmsBackend* backend = AmsState::instance().get_backend();
            if (backend) {
                spdlog::info("[AmsPanel] Retrying load for slot {}", retry_slot);
                // Reset the AMS first, then load
                backend->reset();
                if (this->sidebar_) {
                    this->sidebar_->handle_load_with_preheat(retry_slot);
                }
            }
        }
    });
}

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<AmsPanel> g_ams_panel;
static lv_obj_t* s_ams_panel_obj = nullptr;

void destroy_ams_panel_ui() {
    if (s_ams_panel_obj) {
        spdlog::info("[AMS Panel] Destroying panel UI to free memory");

        // Unregister close callback BEFORE deleting to prevent double-invocation
        // (e.g., if destroy called manually while panel is in overlay stack)
        NavigationManager::instance().unregister_overlay_close_callback(s_ams_panel_obj);

        // Clear the panel_ reference in AmsPanel before deleting
        if (g_ams_panel) {
            g_ams_panel->clear_panel_reference();
        }

        helix::ui::safe_delete(s_ams_panel_obj);

        // Note: Widget registrations remain (LVGL doesn't support unregistration)
        // Note: g_ams_panel C++ object stays for state preservation
    }
}

AmsPanel& get_global_ams_panel() {
    if (!g_ams_panel) {
        g_ams_panel = std::make_unique<AmsPanel>(get_printer_state(), get_moonraker_api());
        StaticPanelRegistry::instance().register_destroy("AmsPanel", []() { g_ams_panel.reset(); });
    }

    // Lazy create the panel UI if not yet created
    if (!s_ams_panel_obj && g_ams_panel) {
        // Ensure widgets and XML are registered
        ensure_ams_widgets_registered();

        // Initialize AmsState subjects BEFORE XML creation so bindings work
        AmsState::instance().init_subjects(true);

        // Create the panel on the active screen
        lv_obj_t* screen = lv_scr_act();
        s_ams_panel_obj = static_cast<lv_obj_t*>(lv_xml_create(screen, "ams_panel", nullptr));

        if (s_ams_panel_obj) {
            // Initialize panel observers (AmsState already initialized above)
            if (!g_ams_panel->are_subjects_initialized()) {
                g_ams_panel->init_subjects();
            }

            // Setup the panel
            g_ams_panel->setup(s_ams_panel_obj, screen);
            lv_obj_add_flag(s_ams_panel_obj, LV_OBJ_FLAG_HIDDEN); // Hidden by default

            // Register overlay instance for lifecycle management
            NavigationManager::instance().register_overlay_instance(s_ams_panel_obj,
                                                                    g_ams_panel.get());

            // Register close callback to clear scope when overlay is closed
            // Panel stays alive for instant re-open (no lazy-load penalty)
            NavigationManager::instance().register_overlay_close_callback(s_ams_panel_obj, []() {
                if (g_ams_panel) {
                    g_ams_panel->clear_unit_scope();
                }
            });

            spdlog::info("[AMS Panel] Lazy-created panel UI with close callback");
        } else {
            spdlog::error("[AMS Panel] Failed to create panel from XML");
        }
    }

    return *g_ams_panel;
}
