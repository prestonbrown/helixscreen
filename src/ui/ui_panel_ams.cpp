// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_ams.h"

#include "ui_afc_fault_path.h"
#include "ui_ams_detail.h"
#include "ui_ams_device_operations_overlay.h"
#include "ui_ams_environment_overlay.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_ams_slot_layout.h"
#include "ui_endless_spool_arrows.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_external_spool_menu.h"
#include "ui_filament_path_canvas.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_overlay_qr_scanner.h"
#include "ui_panel_common.h"
#include "ui_spool_canvas.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "data_root_resolver.h"
#if HELIX_HAS_CFS
#include "ams_backend_cfs.h"
#endif
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "buffer_status_modal.h"
#include "color_utils.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "system/crash_handler.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <any>
#include <cmath>
#include <memory>
#include <sstream>
#include <unordered_map>

using namespace helix;

// Default slot width for endless arrows canvas (when layout not yet computed)
static constexpr int32_t DEFAULT_SLOT_WIDTH = 80;

// Unit index handed to the shared ams_detail_* helpers, which take a unit to
// scope to or -1 for "every unit". This panel is always the whole-system view:
// the AMS Overview panel owns the per-unit presentation, via its own inline
// detail view rather than through here.
static constexpr int ALL_UNITS = -1;

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

    // Register the environment-indicator badge (component + click callback).
    // Shared with the multi-unit overview via one process-lifetime guard; must
    // run before ams_panel.xml is parsed since it nests <ams_environment_indicator>.
    helix::ui::ensure_ams_env_indicator_registered();

    // Tool text observers are initialized in ui_ams_current_tool_init() at
    // app startup so the print status panel's lane label works before any
    // AMS panel has been opened.

    // Register AMS overlay callbacks BEFORE XML parsing
    helix::ui::get_ams_device_operations_overlay().register_callbacks();
    helix::ui::get_ams_environment_overlay().register_callbacks();

    // Context menu callbacks registered by helix::ui::AmsContextMenu class
    // Slot editor and color picker callbacks registered by helix::ui::AmsEditOverlay class

    // Register XML components
    // NOTE: Old AMS settings panels removed - Device Operations overlay is registered in
    // xml_registration.cpp
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/ams_unit_detail.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/ams_loaded_card.xml").c_str());
    // ams_environment_indicator registered above via ensure_ams_env_indicator_registered()
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/ams_sidebar.xml").c_str());
    lv_xml_register_component_from_file(helix::asset_component_uri("ui_xml/ams_panel.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_context_menu.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_selector_menu.xml").c_str());
    // NOTE: spoolman_spool_item.xml and ams_edit_overlay.xml are registered
    // globally in xml_registration.cpp (needed by FilamentPanel without AMS lazy init)
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_loading_error_modal.xml").c_str());
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_environment_overlay.xml").c_str());
    // NOTE: color_picker.xml is registered at startup in xml_registration.cpp

    s_ams_widgets_registered = true;
    spdlog::debug("[AMS Panel] Widget and XML registration complete");
}

// XML event callbacks now handled by helix::ui::AmsOperationSidebar class
// Context menu callbacks handled by helix::ui::AmsContextMenu class
// Slot editor callbacks handled by helix::ui::AmsEditOverlay class

// ============================================================================
// Construction
// ============================================================================

AmsPanel::AmsPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
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
        AmsState::instance().get_slots_version_subject(), this,
        [](AmsPanel* self, int) {
            if (!self->subjects_initialized_ || !self->panel_)
                return;
            spdlog::trace("[AmsPanel] Gates version changed - refreshing slots");
            self->refresh_slots();
        },
        AmsState::instance().get_subjects_lifetime());

    // Simplified action observer - only handles panel-specific concerns
    // (path canvas heat glow and error modal). Step progress is handled by sidebar_.
    action_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_ams_action_subject(), this,
        [](AmsPanel* self, int action_int) {
            // Record the previous value before any early return so the
            // ERROR-exit edge below stays accurate across ticks that bail out.
            const int prev_action = self->prev_ams_action_;
            self->prev_ams_action_ = action_int;

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
                    // Cooldown: don't re-show within 3s of user dismissal
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - self->error_modal_dismiss_time_);
                    if (elapsed.count() >= 3) {
                        self->show_loading_error_modal();
                    }
                }
            } else {
                // Error cleared — reset cooldown so next error shows immediately
                self->error_modal_dismiss_time_ = {};

                // The fault resolved without the dialog being touched (recovered
                // from the console, another client, or a macro), so its
                // Resume/Eject/Recover buttons no longer describe anything real
                // (#1185). Edge only: prev_ams_action_ starts at -1, which never
                // equals ERROR, so the observer's first tick can't trigger this.
                if (prev_action == static_cast<int>(AmsAction::ERROR)) {
                    self->dismiss_error_modal_silently("AMS action left ERROR");
                }
            }
        },
        AmsState::instance().get_subjects_lifetime());

    // A resumed print is the second signal that a filament fault is over. The
    // user may have recovered by a route this panel never observes, leaving the
    // dialog up for the rest of the job (#1185).
    //
    // Edge INTO PRINTING, never a level: an error raised while the print is
    // already running must stay on screen. helix::is_active_print_state() and
    // print_start_nav_should_navigate() are deliberately not used here — they
    // count PAUSED as active, so a PAUSED -> PRINTING resume is not an edge
    // under them, and an AFC fault normally pauses the print, which makes that
    // resume the exact transition this needs to catch.
    //
    // The lifetime token is mandatory: PrinterState is a separate singleton and
    // its subjects can be torn down while this guard is still alive (#705).
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - paired with the keep-raw
    // comparison below; see the marker there for the full reason.
    print_state_observer_ = observe_int_sync<AmsPanel>(
        printer_state_.get_print_state_enum_subject(), this,
        [](AmsPanel* self, int print_state) {
            // Record before the teardown guard so the edge stays accurate
            // across ticks that bail out, matching the action observer above.
            const int prev_state = self->prev_print_state_;
            self->prev_print_state_ = print_state;

            if (!self->subjects_initialized_)
                return;

            // First tick is the observer syncing the current value, not a change.
            if (prev_state < 0)
                return;

            // RAW_PRINT_STATE_OK: a value question about what the printer
            // reports, not the capability question job_holds_machine() answers.
            // print_lifecycle stays Preparing for the whole of a firmware-side
            // PRINT_START, so reading it here would move this dismissal to the
            // END of PRINT_START. Pinned by the keep-raw case in
            // tests/unit/test_ams_error_modal_autodismiss.cpp.
            constexpr int printing = static_cast<int>(helix::PrintJobState::PRINTING);
            if (print_state != printing || prev_state == printing)
                return;

            self->dismiss_error_modal_silently("print resumed");
        },
        printer_state_.get_static_print_subjects_lifetime());

    current_slot_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_current_slot_subject(), this,
        [](AmsPanel* self, int slot) {
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
        },
        AmsState::instance().get_subjects_lifetime());

    // Slot count observer for dynamic slot creation (non-scoped mode only).
    // Deferred via lifetime_ to avoid deleting children during LVGL layout refresh (#563).
    slot_count_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_slot_count_subject(), this, [](AmsPanel* self, int new_count) {
            if (!self->panel_)
                return;
            if (!self->slot_creation_pending_) {
                self->slot_creation_pending_ = true;
                self->lifetime_.defer("AmsPanel::create_slots", [self, new_count]() {
                    self->slot_creation_pending_ = false;
                    spdlog::debug("[AmsPanel] Slot count changed to {}", new_count);
                    self->create_slots(new_count);
                });
            }
        });

    // Path state observers for filament path visualization.
    // Deferred via lifetime_ to avoid modifying widgets during LVGL layout refresh (#563).
    auto path_handler = [](AmsPanel* self, int) {
        if (!self->subjects_initialized_ || !self->panel_)
            return;
        if (!self->path_update_pending_) {
            self->path_update_pending_ = true;
            self->lifetime_.defer("AmsPanel::update_path_canvas", [self]() {
                self->path_update_pending_ = false;
                spdlog::debug("[AmsPanel] Path state changed - updating path canvas");
                self->update_path_canvas_from_backend();
            });
        }
    };
    path_segment_observer_ =
        observe_int_sync<AmsPanel>(AmsState::instance().get_path_filament_segment_subject(), this,
                                   path_handler, AmsState::instance().get_subjects_lifetime());
    path_topology_observer_ =
        observe_int_sync<AmsPanel>(AmsState::instance().get_path_topology_subject(), this,
                                   path_handler, AmsState::instance().get_subjects_lifetime());

    // Backend count observer for multi-backend selector
    backend_count_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_backend_count_subject(), this, [](AmsPanel* self, int /*count*/) {
            if (!self->backend_rebuild_pending_) {
                self->backend_rebuild_pending_ = true;
                self->lifetime_.defer("AmsPanel::rebuild_backend_selector", [self]() {
                    self->backend_rebuild_pending_ = false;
                    self->rebuild_backend_selector();
                });
            }
        });

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
                // …and only while the bypass node is on the path at all (#1229).
                bool has_spool = ext_spool.has_value() && helix::ui::bypass_node_visible_for(
                                                              AmsState::instance().get_backend());
                ui_filament_path_canvas_set_bypass_has_spool(self->path_canvas_, has_spool);
                if (has_spool) {
                    ui_filament_path_canvas_set_bypass_color(
                        self->path_canvas_, static_cast<uint32_t>(ext_spool->color_rgb));
                }
            }

            // Update bypass spool holder (3D spool in left column)
            self->update_bypass_spool_from_state();
        },
        AmsState::instance().get_subjects_lifetime());

    // Bypass support is not fixed at setup time. Happy Hare publishes has_bypass
    // from the live mmu object, and our own default stands in as true until that
    // first status lands, so the node has to be able to appear and disappear
    // after the panel is built.
    supports_bypass_observer_ = observe_int_sync<AmsPanel>(
        AmsState::instance().get_supports_bypass_subject(), this,
        [](AmsPanel* self, int /*supported*/) { self->update_bypass_spool_from_state(); });

    // UI module subjects are now encapsulated in their respective classes:
    // - helix::ui::AmsEditOverlay
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

    // Setup UI components. The system header needs no setup call: its logo and
    // name are bound to the ams_system_logo / ams_system_name subjects.
    setup_slots();
    setup_path_canvas();

    // Setup bypass spool holder (left column, below path canvas)
    setup_bypass_spool();

    // Setup endless spool arrows
    setup_endless_arrows();

    // Setup shared sidebar component
    sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(printer_state_);
    sidebar_->setup(panel_);
    sidebar_->init_observers();

    spdlog::debug("[{}] Setup complete!", get_name());
}

void AmsPanel::on_activate() {
    spdlog::debug("[{}] Activated - syncing from backend", get_name());

    // Sync state when panel becomes visible
    AmsState::instance().sync_from_backend();

    int slot_count = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    if (slot_count != current_slot_count_) {
        create_slots(slot_count);
    }

    // path_container is shown unconditionally; bypass_row visibility is managed
    // by bind_flag_if_eq on the ams_supports_bypass subject.
    lv_obj_t* path_container = lv_obj_find_by_name(panel_, "path_container");
    if (path_container)
        lv_obj_remove_flag(path_container, LV_OBJ_FLAG_HIDDEN);

    // Re-read non-subject slot fields on reactivation. The MATERIAL label has no
    // backing subject (unlike color, which self-refreshes via sync_from_backend()
    // above), so it is only re-read by refresh_slots(). Without this call the
    // material label stays stale until the next slots_version bump (#981).
    // refresh_slots() guards panel_ && subjects_initialized_ internally.
    refresh_slots();

    update_endless_arrows_from_backend();

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

    // Start Spoolman polling for slot weight updates. Guarded: on_activate()
    // can fire more than once per visit, and each unguarded call took another
    // reference that on_deactivate() never gave back.
    if (!holds_poll_ref_) {
        SpoolmanManager::instance().start_spoolman_polling();
        holds_poll_ref_ = true;
    }
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
    if (holds_poll_ref_) {
        SpoolmanManager::instance().stop_spoolman_polling();
        holds_poll_ref_ = false;
    }

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
    selector_menu_.reset();
    error_modal_.reset();

    // Nullify widget pointers BEFORE resetting observers — any cascading
    // observer callbacks during teardown will see null and bail out.
    panel_ = nullptr;
    parent_screen_ = nullptr;
    slot_grid_ = nullptr;
    detail_widgets_ = AmsDetailWidgets{};
    path_canvas_ = nullptr;
    bypass_widgets_ = {};
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
    slot_path_observers_.clear();
    print_state_observer_.reset();
    backend_count_observer_.reset();
    external_spool_observer_.reset();

    // Re-arm the edge sentinels: a rebuilt panel has observed nothing yet.
    prev_ams_action_ = -1;
    prev_print_state_ = -1;

    spdlog::debug("[AMS Panel] Cleared all widget references");
}

// ============================================================================
// Setup Helpers
// ============================================================================

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

    // Clear existing children. rebuild_backend_selector runs from
    // backend_count_observer_ via lifetime_.defer (UpdateQueue batch). [L081]
    helix::ui::safe_clean_children(row);

    for (int i = 0; i < count; ++i) {
        auto* backend = ams.get_backend(i);
        if (!backend) {
            continue;
        }

        std::string label = ams_type_to_string(backend->get_type());

        // Create a button-like segment for each backend
        lv_obj_t* btn = lv_obj_create(row);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        int32_t btn_pad_hor = theme_manager_get_spacing("space_md");
        lv_obj_set_style_pad_all(btn, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_style_pad_left(btn, btn_pad_hor, 0);
        lv_obj_set_style_pad_right(btn, btn_pad_hor, 0);
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

    // Pre-show environment indicator so flex layout accounts for its width
    // when calculating slot sizes. Must happen BEFORE slot creation.
    ams_detail_pre_show_env_indicator(detail_widgets_, ALL_UNITS);

    // Create new slots
    auto result = ams_detail_create_slots(detail_widgets_, slot_widgets_, MAX_VISIBLE_SLOTS,
                                          ALL_UNITS, on_slot_clicked, this);

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

    // Wire per-slot LIVE path observers so pushing/pulling filament redraws the
    // path in real time (Task 1). Re-built here because the slot count is known.
    setup_slot_path_observers(result.slot_count);

    // Update tray
    ams_detail_update_tray(detail_widgets_);
}

void AmsPanel::setup_slot_path_observers(int slot_count) {
    // Rebuild from scratch — slot_count may have changed.
    slot_path_observers_.clear();
    if (slot_count <= 0)
        return;

    auto& state = AmsState::instance();
    // This panel always shows every unit's slots, so global slot indices start at 0.
    const int slot_offset = 0;

    // Coalesced redraw handler (same pattern as path_segment_observer_): re-runs
    // the full path setup (which re-reads every slot's live segment + color and
    // calls ui_filament_path_canvas_refresh) on the next deferred tick.
    auto on_slot_path_change = [](AmsPanel* self, int) {
        if (!self->subjects_initialized_ || !self->panel_)
            return;
        spdlog::debug("[AmsPanel] Per-slot path subject fired — scheduling path redraw");
        if (!self->path_update_pending_) {
            self->path_update_pending_ = true;
            self->lifetime_.defer("AmsPanel::update_path_canvas(slot)", [self]() {
                self->path_update_pending_ = false;
                self->update_path_canvas_from_backend();
            });
        }
    };

    for (int i = 0; i < slot_count; ++i) {
        int global_idx = i + slot_offset;
        // Segment subject — how far filament extends along this lane's path.
        if (auto* seg_subj = state.get_slot_segment_subject(global_idx)) {
            slot_path_observers_.push_back(
                helix::ui::observe_int_sync<AmsPanel>(seg_subj, this, on_slot_path_change));
        }
        // Toolhead-present subject — live per-slot motion/switch sensor.
        if (auto* th_subj = state.get_slot_toolhead_present_subject(global_idx)) {
            slot_path_observers_.push_back(
                helix::ui::observe_int_sync<AmsPanel>(th_subj, this, on_slot_path_change));
        }
    }
    spdlog::debug("[AmsPanel] Wired {} per-slot path observers (offset={})",
                  slot_path_observers_.size(), slot_offset);
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

    // Set selector/hub click callback (opens Happy Hare selector context menu)
    ui_filament_path_canvas_set_hub_callback(path_canvas_, &AmsPanel::on_path_hub_clicked_thunk,
                                             this);

    // Set bypass spool click callback (opens edit modal for external spool)
    ui_filament_path_canvas_set_bypass_callback(path_canvas_, on_bypass_spool_clicked, this);
    ui_filament_path_canvas_set_buffer_callback(path_canvas_, on_buffer_clicked, this);

    // Configure from backend using shared helper
    ams_detail_setup_path_canvas(path_canvas_, slot_grid_, ALL_UNITS, false);

    spdlog::debug("[{}] Path canvas setup complete", get_name());
}

void AmsPanel::update_path_canvas_from_backend() {
    ams_detail_setup_path_canvas(path_canvas_, slot_grid_, ALL_UNITS, false);
}

void AmsPanel::setup_bypass_spool() {
    if (!path_canvas_) {
        spdlog::debug("[{}] No path canvas — skipping bypass spool setup", get_name());
        return;
    }

    // Built unconditionally, then shown or hidden by update_bypass_spool_from_state()
    // via bypass_node_visible_for(). Skipping creation here instead would make the
    // node's presence a one-shot decision taken before the backend has published
    // its first status: a system whose bypass only becomes visible later (Happy
    // Hare gates has_bypass on selector calibration) would keep the sidebar toggle
    // and the Device Operations section, which are subject-bound, but never grow
    // the spool back onto the path. AmsOverviewPanel already builds it this way.
    lv_obj_t* path_container = lv_obj_get_parent(path_canvas_);
    if (!path_container) {
        return;
    }

    bypass_widgets_ = helix::ui::bypass_spool_create(
        path_container,
        [](lv_event_t* e) {
            if (auto* self = static_cast<AmsPanel*>(lv_event_get_user_data(e))) {
                self->handle_bypass_spool_click();
            }
        },
        this);

    if (!bypass_widgets_.valid()) {
        spdlog::warn("[{}] Failed to create bypass spool widgets", get_name());
        return;
    }

    // Seed color/fill/material from current state
    update_bypass_spool_from_state();

    // Reposition whenever the path canvas resizes — the canvas size at the
    // time setup_bypass_spool() runs is not its final size; the surrounding
    // flex layout adjusts it later (we observed 251→283px height growth, which
    // shifted the rendered tube ~13px and left the spool stranded above it).
    lv_obj_add_event_cb(
        path_canvas_,
        [](lv_event_t* e) {
            auto* self = static_cast<AmsPanel*>(lv_event_get_user_data(e));
            if (self)
                self->update_bypass_spool_position();
        },
        LV_EVENT_SIZE_CHANGED, this);

    update_bypass_spool_position();
}

void AmsPanel::update_bypass_spool_position() {
    if (!bypass_widgets_.valid() || !path_canvas_)
        return;

    int32_t abs_cx = 0;
    int32_t abs_cy = 0;
    if (!ui_filament_path_canvas_get_bypass_merge_pos(path_canvas_, &abs_cx, &abs_cy)) {
        return;
    }
    lv_obj_t* parent = lv_obj_get_parent(bypass_widgets_.box);
    lv_area_t parent_abs;
    lv_obj_get_content_coords(parent, &parent_abs);
    helix::ui::bypass_spool_set_position(bypass_widgets_, abs_cx - parent_abs.x1,
                                         abs_cy - parent_abs.y1);
}

void AmsPanel::update_bypass_spool_from_state() {
    if (!bypass_widgets_.valid()) {
        return;
    }

    // On AFC the node is removed entirely while bypass is disengaged (#1229).
    const bool show_bypass = helix::ui::bypass_node_visible_for(AmsState::instance().get_backend());
    helix::ui::bypass_spool_set_visible(bypass_widgets_, show_bypass);
    if (!show_bypass) {
        return;
    }

    auto ext = AmsState::instance().get_external_spool_info();
    if (ext.has_value()) {
        helix::ui::bypass_spool_set_color(bypass_widgets_, ext->color_rgb);
        helix::ui::bypass_spool_set_has_spool(bypass_widgets_, true);
        helix::ui::bypass_spool_set_material(bypass_widgets_, ext->material.c_str());
    } else {
        helix::ui::bypass_spool_set_color(bypass_widgets_, 0x505050);
        helix::ui::bypass_spool_set_has_spool(bypass_widgets_, false);
        helix::ui::bypass_spool_set_material(bypass_widgets_, "");
    }
    // Reposition because the material label visibility may have changed,
    // which affects the layout above the spool box.
    update_bypass_spool_position();
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
    if (!capabilities.available()) {
        ui_endless_spool_arrows_clear(endless_arrows_);
        lv_obj_add_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Get the endless spool relation and flatten it to one arrow per slot. The
    // group-to-edge projection lives in helix::printer, shared with the context
    // menu's dropdown, so a Happy Hare group and an AFC chain cannot disagree
    // about which arrow to draw.
    static constexpr int MAX_ARROW_SLOTS = 16;
    const auto config = backend->get_endless_spool_config();
    int slot_count = std::min(backend->get_system_info().total_slots, MAX_ARROW_SLOTS);
    if (config.empty() || slot_count <= 0) {
        ui_endless_spool_arrows_clear(endless_arrows_);
        lv_obj_add_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const std::vector<int> edges = helix::printer::endless_spool_backup_edges(config, slot_count);
    if (std::none_of(edges.begin(), edges.end(), [](int backup) { return backup >= 0; })) {
        spdlog::trace("[{}] No endless spool backups configured - hiding arrows", get_name());
        ui_endless_spool_arrows_clear(endless_arrows_);
        lv_obj_add_flag(endless_arrows_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int backup_slots[MAX_ARROW_SLOTS];
    for (int i = 0; i < slot_count; ++i) {
        backup_slots[i] = edges[static_cast<size_t>(i)];
    }
    spdlog::trace("[{}] Endless spool relation has {} group(s) over {} slots", get_name(),
                  config.groups.size(), slot_count);

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

    int current_slot = lv_subject_get_int(AmsState::instance().get_current_slot_subject());
    update_current_slot_highlight(current_slot);

    // P2 (#1045): a per-slot status change (e.g. idle-lane unload) must also
    // re-read the backend's per-slot filament segments so a now-empty lane's
    // stale lane→hub tube is cleared. update_path_canvas_from_backend() reads
    // get_slot_filament_segment() live and repaints the canvas. It internally
    // guards against a missing canvas (ams_detail_setup_path_canvas() returns
    // early when canvas/backend is null) and never writes slot subjects, so it
    // can't recurse back into a slots_version bump.
    update_path_canvas_from_backend();
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

            // Find color swatch within slot. Multi-color spools render as
            // diagonal chunks; single-color falls back to a solid fill.
            lv_obj_t* swatch = lv_obj_find_by_name(slot_widgets_[i], "color_swatch");
            if (swatch) {
                std::string multi =
                    backend ? backend->get_slot_info(i).multi_color_hexes : std::string();
                helix::ui::apply_swatch_color(swatch, rgb, multi);
            }
        }

        // Material and fill are no longer pushed here: the ams_slot widget
        // observes its own per-slot subjects (AmsState::get_slot_material_subject
        // / get_slot_fill_subject), written by sync_from_backend. Observing in
        // the widget means every panel that hosts an ams_slot stays reactive —
        // the AmsOverviewPanel "100% full" fill bug (structural) and the #1065
        // "material stuck, color updates" bug are both fixed at the widget, so no
        // container re-reads them imperatively. Only non-subject state (tool
        // badge, error indicator) is refreshed from the backend here.
        ui_ams_slot_refresh(slot_widgets_[i]);

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
    helix::ui::show_external_spool_menu(
        parent_screen_, path_canvas_, context_menu_,
        [this](bool open_on_picker) { show_edit_modal(-2, open_on_picker); });
}

void AmsPanel::on_buffer_clicked(void* user_data) {
    auto* self = static_cast<AmsPanel*>(user_data);
    if (self) {
        self->handle_buffer_click();
    }
}

void AmsPanel::handle_buffer_click() {
    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    auto info = backend->get_system_info();
    BufferStatusModal::show_for(info, 0);
}

void AmsPanel::on_path_slot_clicked(int slot_index, void* user_data) {
    auto* self = static_cast<AmsPanel*>(user_data);
    if (!self) {
        return;
    }

    spdlog::info("[AmsPanel] Path slot {} clicked - opening context menu", slot_index);

    // Tapping the filament tube/line opens the per-slot context menu (Load /
    // Unload / Eject / Edit / …) — the same AmsContextMenu the spool box opens
    // — rather than firing an unconfirmed load/swap directly. This prevents
    // accidental tool changes from a stray tap on the path canvas. The canvas
    // slot callback only carries a slot index (no click point), so anchor the
    // menu to the canvas widget and let it position itself.
    int slot_count = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    if (slot_index < 0 || slot_index >= slot_count) {
        spdlog::warn("[AmsPanel] Ignoring path click - invalid slot {} (slot_count={})", slot_index,
                     slot_count);
        return;
    }

    if (!self->path_canvas_) {
        return;
    }

    // Capture the live touch point so the menu pops up at the tap location.
    // This runs synchronously inside the canvas's input event handler, so the
    // active indev still reports the press coordinates.
    lv_point_t click_pt = {0, 0};
    if (lv_indev_t* indev = lv_indev_active()) {
        lv_indev_get_point(indev, &click_pt);
    }

    self->show_context_menu(slot_index, self->path_canvas_, click_pt);
}

void AmsPanel::on_path_hub_clicked_thunk(lv_point_t click_pt, void* user_data) {
    auto* self = static_cast<AmsPanel*>(user_data);
    if (!self) {
        return;
    }
    self->on_path_hub_clicked(click_pt);
}

void AmsPanel::on_path_hub_clicked(lv_point_t click_pt) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || !backend->supports_gate_select()) {
        return; // Selector/Happy Hare backends only
    }
    selector_menu_ = std::make_unique<helix::ui::AmsSelectorMenu>();
    selector_menu_->set_action_callback(
        [this](helix::ui::AmsSelectorMenu::SelectorAction a) { dispatch_selector_action(a); });
    selector_menu_->show_at(parent_screen_, path_canvas_, click_pt, backend);
}

void AmsPanel::dispatch_selector_action(helix::ui::AmsSelectorMenu::SelectorAction a) {
    using SA = helix::ui::AmsSelectorMenu::SelectorAction;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }
    AmsError err{};
    // Feedback for these quick selector commands flows through the AMS status
    // display (the ams_action_detail subject) — the backend sets a transient
    // action/operation_detail and the UI observes it, matching how real Happy
    // Hare reports "Checking"/"Selecting"/etc. automatically. No toasts here.
    switch (a) {
    case SA::HOME:
        err = backend->reset(); // reset()==MMU_HOME for HH; reads as "Homing selector"
        break;
    case SA::CHECK_SLOTS:
        err = backend->check_all_gates();
        break;
    case SA::SERVO_UP:
        err = backend->execute_device_action("servo_up");
        break;
    case SA::SERVO_MOVE:
        err = backend->execute_device_action("servo_move");
        break;
    case SA::SERVO_DOWN:
        err = backend->execute_device_action("servo_down");
        break;
    case SA::JOG_PREV:
        err = backend->move_selector(-1);
        break;
    case SA::JOG_NEXT:
        err = backend->move_selector(+1);
        break;
    case SA::GEAR_SYNC_ON:
        err = backend->execute_device_action("gear_sync", std::any(true));
        break;
    case SA::GEAR_SYNC_OFF:
        err = backend->execute_device_action("gear_sync", std::any(false));
        break;
    case SA::RECOVER:
        helix::ui::modal_show_confirmation(
            lv_tr("Recover MMU state?"),
            lv_tr("Re-syncs Happy Hare's tracked state with the hardware."), ModalSeverity::Warning,
            lv_tr("Recover"),
            // A custom on_confirm REPLACES the dialog's default close handler, so
            // it must dismiss the dialog itself. Re-fetch the backend inside the
            // callback so it cannot dangle if the panel/backend changed while the
            // dialog was open. Feedback comes from the backend action state.
            +[](lv_event_t* /*e*/) {
                AmsBackend* b = AmsState::instance().get_backend();
                if (b) {
                    b->recover();
                }
                helix::ui::modal_hide(helix::ui::modal_get_top());
            },
            /*on_cancel*/ nullptr, /*user_data*/ nullptr);
        return;
    case SA::CANCELLED:
        return;
    }
    if (err.result != AmsResult::SUCCESS) {
        helix::ui::notify_ams_error(err, lv_tr("MMU command failed"));
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
    context_menu_->set_action_callback([this](helix::ui::AmsContextMenu::MenuAction action,
                                              int slot) {
        AmsBackend* backend = AmsState::instance().get_backend();

        // EJECT / RECOVER_POSITION / SELECT_GATE / CHECK_GATE / CLEAR_SPOOL are
        // identical in both AMS panels — see ams_dispatch_backend_action().
        if (helix::ui::ams_dispatch_backend_action(action, slot, path_canvas_)) {
            return;
        }

        switch (action) {
        case helix::ui::AmsContextMenu::MenuAction::LOAD:
            if (!backend) {
                NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
                return;
            }
            // Check if backend is busy
            {
                AmsSystemInfo info = backend->get_system_info();
                if (info.action != AmsAction::IDLE && info.action != AmsAction::ERROR) {
                    NOTIFY_WARNING(lv_tr("Multi-Filament System is busy: {}"),
                                   ams_action_to_string(info.action));
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
                NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
                return;
            }
            // Route through the sidebar so start_operation(UNLOAD) builds the
            // correct stepper (mirrors how LOAD routes via handle_load_with_preheat).
            if (this->sidebar_) {
                this->sidebar_->handle_unload(slot);
            } else {
                AmsError error = backend->unload_filament(slot);
                if (error.result != AmsResult::SUCCESS) {
                    helix::ui::notify_ams_error(error);
                }
            }
            break;

        case helix::ui::AmsContextMenu::MenuAction::EDIT:
            show_edit_modal(slot);
            break;

        case helix::ui::AmsContextMenu::MenuAction::SPOOLMAN:
            show_edit_modal(slot, /*open_on_picker=*/true);
            break;

        case helix::ui::AmsContextMenu::MenuAction::SCAN_QR: {
#if HELIX_HAS_CAMERA
            auto& scanner = helix::ui::get_qr_scanner_overlay();
            scanner.show(parent_screen_, slot, [slot](const SpoolInfo& spool) {
                AmsBackend* be = AmsState::instance().get_backend();
                if (!be)
                    return;

                // Capture the pre-edit slot BEFORE the commit — its unlink arm
                // (clear the server active spool) needs the old link.
                SlotInfo original = be->get_slot_info(slot);
                SlotInfo applied = original;
                apply_spool_to_slot(applied, spool);
                AmsError err = AmsState::instance().commit_slot_edit(slot, original, applied);
                if (!err.success()) {
                    helix::ui::notify_ams_error(err);
                    return;
                }
                spdlog::info("[AmsPanel] QR scan assigned spool #{} to slot {}", spool.id, slot);
            });
#endif // HELIX_HAS_CAMERA
            break;
        }

        case helix::ui::AmsContextMenu::MenuAction::CANCELLED:
        default:
            break;
        }
    });

    // Determine whether to offer Unload for this slot. Decoupled from the
    // display LOADED status so a runout that clears the head sensor doesn't
    // disable Unload on the firmware's active slot (#995).
    bool is_loaded = false;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        is_loaded = backend->can_unload_from_toolhead(slot_index);
    }

    // Position menu near the click point, then show
    context_menu_->set_click_point(click_pt);
    context_menu_->show_near_widget(parent_screen_, slot_index, near_widget, is_loaded, backend);
}

// ============================================================================
// Slot Editor (delegated to helix::ui::AmsEditOverlay)
// ============================================================================

void AmsPanel::show_edit_modal(int slot_index, bool open_on_picker) {
    if (!parent_screen_) {
        spdlog::warn("[{}] Cannot show slot editor - no parent screen", get_name());
        return;
    }

    auto& editor = helix::ui::get_ams_edit_overlay();

    // External spool (bypass/direct) — not managed by backend
    if (slot_index == -2) {
        auto ext = AmsState::instance().get_external_spool_info();
        SlotInfo initial_info = ext.value_or(SlotInfo{});
        initial_info.slot_index = -2;
        initial_info.global_index = -2;

        editor.show_for_slot(
            parent_screen_, -2, initial_info, api_,
            [](const helix::ui::AmsEditOverlay::EditResult& result) {
                if (result.saved) {
                    AmsState::instance().commit_external_spool_edit(result.slot_info);
                    // bypass display update handled reactively by external_spool_observer_
                    NOTIFY_INFO(lv_tr("External spool updated"));
                }
            },
            open_on_picker);
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    // Get current slot info
    SlotInfo initial_info = backend->get_slot_info(slot_index);

    editor.show_for_slot(
        parent_screen_, slot_index, initial_info, api_,
        [this](const helix::ui::AmsEditOverlay::EditResult& result) {
            if (result.saved && result.slot_index >= 0) {
                AmsBackend* backend = AmsState::instance().get_backend();
                if (backend) {
                    // Capture the pre-edit slot BEFORE the commit — its unlink
                    // arm (clear the server active spool) needs the old link.
                    SlotInfo original = backend->get_slot_info(result.slot_index);
                    AmsError err = AmsState::instance().commit_slot_edit(
                        result.slot_index, original, result.slot_info);
                    if (!err.success()) {
                        helix::ui::notify_ams_error(err);
                        return;
                    }
                    NOTIFY_INFO(lv_tr("Slot {} updated"), result.slot_index + 1);
                }
            }
        },
        open_on_picker);
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
        error_message = lv_tr("An error occurred during filament loading.");
    }

    // AFC welds a monospace position diagram onto its lane faults, which our
    // proportional font renders as noise (#1184). Publish the stop point to the
    // modal's <afc_fault_path> graphic and drop the art rows from the text. An
    // unrecognised message sets the subject to 0 (graphic hidden) and comes back
    // unchanged — including the non-AFC backends, which must clear a previous
    // AFC fault's marker rather than inherit it.
    error_message = helix::ui::afc_fault_path_apply(error_message);

    // Store slot for retry
    int retry_slot = info.current_slot;

    // Clear backend error state when user dismisses (Close/X/Cancel/backdrop).
    // AFC maintains a persistent message_queue and error_state that won't clear
    // until RESET_FAILURE + AFC_CLEAR_MESSAGE are sent. Without this, the error
    // dialog reappears immediately because AFC keeps reporting ERROR. (#497)
    error_modal_->set_dismiss_callback([this, retry_slot]() {
        error_modal_dismiss_time_ = std::chrono::steady_clock::now();
        AmsBackend* backend = AmsState::instance().get_backend();
        if (!backend) {
            return;
        }

        // Backends with a persistent message/error queue (AFC) must clear it on
        // dismiss or the error dialog re-fires immediately (#497). clear_fault()
        // owns that sequence for every backend.
        spdlog::info("[AmsPanel] Clearing error state on dismiss (type={})",
                     ams_type_to_string(backend->get_type()));
        backend->clear_fault(retry_slot);
    });

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

void AmsPanel::dismiss_error_modal_silently(const char* reason) {
    if (!error_modal_ || !error_modal_->is_visible()) {
        return;
    }

    spdlog::info("[{}] Auto-dismissing filament error dialog: {}", get_name(), reason);
    // dismiss_silently() skips the dismiss callback, which would send
    // fault-clearing gcode for a fault that is already gone and arm the 3s
    // re-show cooldown against a genuinely new error (#1185).
    error_modal_->dismiss_silently();
}

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<AmsPanel> g_ams_panel;
static lv_obj_t* s_ams_panel_obj = nullptr;

// NOTE: this deliberately does NOT call OverlayBase::destroy_overlay_ui().
// AmsPanel derives from PanelBase, not OverlayBase — the two are sibling
// IPanelLifecycle implementations, so the helper is not reachable from here
// (there is no overlay_root_ and no on_ui_destroyed() on this hierarchy).
// The sequence below mirrors the helper's, with two required deviations:
//   1. clear_panel_reference() runs BEFORE deletion (the helper's
//      on_ui_destroyed() runs after), because it destroys the sidebar /
//      context-menu / modal sub-objects that own widgets in this subtree.
//   2. safe_delete_subtree() instead of safe_delete_deferred() — see #983.
void destroy_ams_panel_ui() {
    if (s_ams_panel_obj) {
        spdlog::info("[AMS Panel] Destroying panel UI to free memory");

        // Drain deferred observer callbacks while pointers are still valid.
        // observe_int_sync queues lambdas via queue_update() that capture raw
        // panel/sidebar pointers; processing them here prevents use-after-free.
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();

        // Unregister close callback BEFORE deleting to prevent double-invocation
        // (e.g., if destroy called manually while panel is in overlay stack)
        NavigationManager::instance().unregister_overlay_close_callback(s_ams_panel_obj);
        NavigationManager::instance().unregister_overlay_instance(s_ams_panel_obj);

        // Breadcrumb the destroy so crashes in the close path can be pinned to
        // which overlay was being torn down. Pairs with the "overlay+" crumb on
        // push, and matches OverlayBase::destroy_overlay_ui().
        crash_handler::breadcrumb::note("ovrl_dst", "AmsPanel");

        // Clear the panel_ reference in AmsPanel before deleting
        if (g_ams_panel) {
            g_ams_panel->clear_panel_reference();
        }

        // safe_delete_subtree (not safe_delete) because the AMS panel root
        // contains grid/flex layouts whose grid_update/flex_update could race
        // teardown if an ancestor relayouts during this drain. Detaching the
        // subtree off-tree before deferred deletion makes that race
        // structurally impossible (#983). Null the pointer ourselves — the
        // subtree helper takes a raw pointer and does not null its argument.
        helix::ui::safe_delete_subtree(s_ams_panel_obj);
        s_ams_panel_obj = nullptr;

        // Note: Widget registrations remain (LVGL doesn't support unregistration)
        // Note: g_ams_panel C++ object stays for state preservation
    }
}

AmsPanel& get_global_ams_panel() {
    if (!g_ams_panel) {
        g_ams_panel = std::make_unique<AmsPanel>(get_printer_state(), get_moonraker_api());
        StaticPanelRegistry::instance().register_destroy("AmsPanel", []() {
            destroy_ams_panel_ui();
            g_ams_panel.reset();
        });
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

            NavigationManager::instance().register_overlay_instance(s_ams_panel_obj,
                                                                    g_ams_panel.get());

            // Destroy on overlay close to free memory on tight devices (AD5M/AD5X
            // ~107MB RAM). The C++ instance survives via g_ams_panel for state
            // preservation; widgets are recreated on next open.
            NavigationManager::instance().register_overlay_close_callback(
                s_ams_panel_obj, []() { destroy_ams_panel_ui(); });

            spdlog::info("[AMS Panel] Lazy-created panel UI with close callback");
        } else {
            spdlog::error("[AMS Panel] Failed to create panel from XML");
        }
    }

    return *g_ams_panel;
}

AmsPanel* get_existing_ams_panel() {
    return g_ams_panel.get();
}
