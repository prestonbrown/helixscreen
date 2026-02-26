// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_spoolman.h"

#include "ui_callback_helpers.h"
#include "ui_global_panel_helper.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_spool_canvas.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_globals.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "ui/ui_lazy_panel_helper.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// ============================================================================
// Global Instance
// ============================================================================

DEFINE_GLOBAL_PANEL(SpoolmanPanel, g_spoolman_panel, get_global_spoolman_panel)

// ============================================================================
// Constructor
// ============================================================================

SpoolmanPanel::SpoolmanPanel() {
    spdlog::trace("[{}] Constructor", get_name());
    std::memset(header_title_buf_, 0, sizeof(header_title_buf_));
}

SpoolmanPanel::~SpoolmanPanel() {
    callback_guard_.reset();
    if (search_debounce_timer_) {
        lv_timer_delete(search_debounce_timer_);
        search_debounce_timer_ = nullptr;
    }
    deinit_subjects();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void SpoolmanPanel::init_subjects() {
    init_subjects_guarded([this]() {
        // Initialize panel state subject (starts in LOADING state)
        UI_MANAGED_SUBJECT_INT(panel_state_subject_,
                               static_cast<int32_t>(SpoolmanPanelState::LOADING),
                               "spoolman_panel_state", subjects_);

        UI_MANAGED_SUBJECT_STRING(header_title_subject_, header_title_buf_, "Spoolman",
                                  "spoolman_header_title", subjects_);
    });
}

void SpoolmanPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[SpoolmanPanel] Subjects deinitialized");
}

// ============================================================================
// Callback Registration
// ============================================================================

void SpoolmanPanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    // Register XML event callbacks
    register_xml_callbacks({
        {"on_spoolman_spool_row_clicked", on_spool_row_clicked},
        {"on_spoolman_refresh_clicked", on_refresh_clicked},
        {"on_spoolman_add_spool_clicked", on_add_spool_clicked},
        {"on_spoolman_search_changed", on_search_changed},
        {"on_spoolman_search_clear", on_search_clear},
    });

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* SpoolmanPanel::create(lv_obj_t* parent) {
    register_callbacks();

    if (!create_overlay_from_xml(parent, "spoolman_panel")) {
        return nullptr;
    }

    // Find widget references
    lv_obj_t* content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (content) {
        spool_list_ = lv_obj_find_by_name(content, "spool_list");
    }

    if (!spool_list_) {
        spdlog::error("[{}] spool_list not found!", get_name());
        return nullptr;
    }

    // Setup virtualized list view
    list_view_.setup(spool_list_);

    // Add scroll handler for virtualization
    lv_obj_add_event_cb(spool_list_, on_scroll, LV_EVENT_SCROLL, this);

    // Bind header title to subject for dynamic "Spoolman: XX Spools" text
    lv_obj_t* header = lv_obj_find_by_name(overlay_root_, "overlay_header");
    if (header) {
        lv_obj_t* title = lv_obj_find_by_name(header, "header_title");
        if (title) {
            lv_label_bind_text(title, &header_title_subject_, nullptr);
        }

        // Gate "+" (add spool) button behind beta features
        lv_obj_t* add_btn = lv_obj_find_by_name(header, "action_button_2");
        lv_subject_t* beta_subject = lv_xml_get_subject(nullptr, "show_beta_features");
        if (add_btn && beta_subject) {
            lv_obj_bind_flag_if_eq(add_btn, beta_subject, LV_OBJ_FLAG_HIDDEN, 0);
        }
    }

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void SpoolmanPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    // Create callback guard for async operations
    callback_guard_ = std::make_shared<bool>(true);

    spdlog::debug("[{}] on_activate()", get_name());

    // Clear search on activation (text_input handles clear button visibility internally)
    search_query_.clear();
    lv_obj_t* search_box = lv_obj_find_by_name(overlay_root_, "search_box");
    if (search_box) {
        lv_textarea_set_text(search_box, "");
        KeyboardManager::instance().register_textarea(search_box);
    }

    // Refresh spool list when panel becomes visible
    refresh_spools();

    // Start Spoolman polling for weight updates
    AmsState::instance().start_spoolman_polling();
}

void SpoolmanPanel::on_deactivate() {
    // Invalidate callback guard to prevent async callbacks from using stale 'this'
    callback_guard_.reset();

    AmsState::instance().stop_spoolman_polling();

    // Clean up debounce timer
    if (search_debounce_timer_) {
        lv_timer_delete(search_debounce_timer_);
        search_debounce_timer_ = nullptr;
    }

    // Clean up list view (pool widgets are children of spool_list_, cleaned by LVGL)
    list_view_.cleanup();

    spdlog::debug("[{}] on_deactivate()", get_name());

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Data Loading
// ============================================================================

void SpoolmanPanel::refresh_spools() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API available, cannot refresh", get_name());
        show_empty_state();
        return;
    }

    show_loading_state();

    std::weak_ptr<bool> weak_guard = callback_guard_;

    // Shared handler: update cached spools and active ID, then repopulate
    auto apply_spools = [this, weak_guard](std::vector<SpoolInfo> spools, int active_id) {
        helix::ui::queue_update([this, weak_guard, spools = std::move(spools), active_id]() {
            if (weak_guard.expired())
                return;
            cached_spools_ = spools;
            active_spool_id_ = active_id;
            populate_spool_list();
        });
    };

    std::string name = get_name();

    api->spoolman().get_spoolman_spools(
        [name, apply_spools](const std::vector<SpoolInfo>& spools) {
            spdlog::info("[{}] Received {} spools from Spoolman", name, spools.size());

            // Also get active spool ID before updating UI
            MoonrakerAPI* api_inner = get_moonraker_api();
            if (!api_inner) {
                spdlog::warn("[{}] API unavailable for status check", name);
                apply_spools(spools, -1);
                return;
            }

            api_inner->spoolman().get_spoolman_status(
                [name, apply_spools, spools](bool /*connected*/, int active_id) {
                    spdlog::debug("[{}] Active spool ID: {}", name, active_id);
                    apply_spools(spools, active_id);
                },
                [name, apply_spools, spools](const MoonrakerError& err) {
                    spdlog::warn("[{}] Failed to get active spool: {}", name, err.message);
                    apply_spools(spools, -1);
                });
        },
        [this, weak_guard, name](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to fetch spools: {}", name, err.message);
            helix::ui::queue_update([this, weak_guard]() {
                if (weak_guard.expired())
                    return;
                cached_spools_.clear();
                show_empty_state();
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to load spools"),
                                              3000);
            });
        });
}

// ============================================================================
// UI State Management
// ============================================================================

void SpoolmanPanel::show_loading_state() {
    lv_subject_set_int(&panel_state_subject_, static_cast<int32_t>(SpoolmanPanelState::LOADING));
}

void SpoolmanPanel::show_empty_state() {
    lv_subject_set_int(&panel_state_subject_, static_cast<int32_t>(SpoolmanPanelState::EMPTY));
    update_spool_count();
}

void SpoolmanPanel::show_spool_list() {
    lv_subject_set_int(&panel_state_subject_, static_cast<int32_t>(SpoolmanPanelState::SPOOLS));
    update_spool_count();
}

void SpoolmanPanel::update_spool_count() {
    if (cached_spools_.empty()) {
        lv_subject_copy_string(&header_title_subject_, "Spoolman");
    } else if (!search_query_.empty() && filtered_spools_.size() != cached_spools_.size()) {
        // Show filtered count: "Spoolman: 5/19 Spools"
        char buf[64];
        snprintf(buf, sizeof(buf), "Spoolman: %zu/%zu Spool%s", filtered_spools_.size(),
                 cached_spools_.size(), cached_spools_.size() == 1 ? "" : "s");
        lv_subject_copy_string(&header_title_subject_, buf);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Spoolman: %zu Spool%s", cached_spools_.size(),
                 cached_spools_.size() == 1 ? "" : "s");
        lv_subject_copy_string(&header_title_subject_, buf);
    }
}

// ============================================================================
// Cache Lookup
// ============================================================================

const SpoolInfo* SpoolmanPanel::find_cached_spool(int spool_id) const {
    auto it = std::find_if(cached_spools_.begin(), cached_spools_.end(),
                           [spool_id](const SpoolInfo& s) { return s.id == spool_id; });
    return it != cached_spools_.end() ? &(*it) : nullptr;
}

// ============================================================================
// Spool List Population
// ============================================================================

void SpoolmanPanel::populate_spool_list() {
    if (!spool_list_) {
        spdlog::error("[{}] spool_list_ is null", get_name());
        return;
    }

    if (cached_spools_.empty()) {
        show_empty_state();
        return;
    }

    // Apply current search filter
    apply_filter();

    if (filtered_spools_.empty()) {
        show_empty_state();
        return;
    }

    // Delegate to virtualized list view
    list_view_.populate(filtered_spools_, active_spool_id_);
    show_spool_list();

    spdlog::debug("[{}] Populated {} spool rows (filtered from {})", get_name(),
                  filtered_spools_.size(), cached_spools_.size());
}

void SpoolmanPanel::apply_filter() {
    filtered_spools_ = filter_spools(cached_spools_, search_query_);
    update_spool_count();
}

void SpoolmanPanel::update_active_indicators() {
    list_view_.update_active_indicators(filtered_spools_, active_spool_id_);
}

// ============================================================================
// Spool Selection
// ============================================================================

void SpoolmanPanel::handle_spool_clicked(lv_obj_t* row, lv_point_t click_pt) {
    if (!row)
        return;

    // Get spool ID from user_data
    void* user_data = lv_obj_get_user_data(row);
    int spool_id = static_cast<int>(reinterpret_cast<intptr_t>(user_data));

    spdlog::info("[{}] Spool {} clicked", get_name(), spool_id);

    const SpoolInfo* spool = find_cached_spool(spool_id);
    if (!spool) {
        spdlog::warn("[{}] Spool {} not found in cache", get_name(), spool_id);
        return;
    }

    // Set up context menu action handler
    context_menu_.set_action_callback([this](helix::ui::SpoolmanContextMenu::MenuAction action,
                                             int id) { handle_context_action(action, id); });

    // Show context menu near the click point
    context_menu_.set_click_point(click_pt);
    context_menu_.show_for_spool(lv_screen_active(), *spool, row);
}

void SpoolmanPanel::handle_context_action(helix::ui::SpoolmanContextMenu::MenuAction action,
                                          int spool_id) {
    using MenuAction = helix::ui::SpoolmanContextMenu::MenuAction;

    switch (action) {
    case MenuAction::SET_ACTIVE:
        set_active_spool(spool_id);
        break;

    case MenuAction::EDIT:
        show_edit_modal(spool_id);
        break;

    case MenuAction::PRINT_LABEL:
        // TODO: Print label (Phase 4)
        spdlog::info("[{}] Print label for spool {} (not yet implemented)", get_name(), spool_id);
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Label printing coming soon"),
                                      2000);
        break;

    case MenuAction::DELETE:
        delete_spool(spool_id);
        break;

    case MenuAction::CANCELLED:
        spdlog::debug("[{}] Context menu cancelled", get_name());
        break;
    }
}

void SpoolmanPanel::set_active_spool(int spool_id) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API, cannot set active spool", get_name());
        return;
    }

    std::weak_ptr<bool> weak_guard = callback_guard_;

    std::string name = get_name();

    api->spoolman().set_active_spool(
        spool_id,
        [this, spool_id, weak_guard, name]() {
            spdlog::info("[{}] Set active spool to {}", name, spool_id);

            helix::ui::queue_update([this, spool_id, weak_guard]() {
                if (weak_guard.expired())
                    return;

                const SpoolInfo* found = find_cached_spool(spool_id);
                std::string spool_name =
                    found ? found->display_name() : "Spool " + std::to_string(spool_id);

                active_spool_id_ = spool_id;
                update_active_indicators();
                std::string msg = std::string(lv_tr("Active")) + ": " + spool_name;
                ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str(), 2000);
            });
        },
        [name, spool_id](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to set active spool {}: {}", name, spool_id, err.message);
            helix::ui::queue_update([]() {
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Failed to set active spool"), 3000);
            });
        });
}

// ============================================================================
// Edit Spool Modal
// ============================================================================

void SpoolmanPanel::show_edit_modal(int spool_id) {
    const SpoolInfo* spool = find_cached_spool(spool_id);
    if (!spool) {
        spdlog::warn("[{}] Cannot edit - spool {} not in cache", get_name(), spool_id);
        return;
    }

    MoonrakerAPI* api = get_moonraker_api();

    edit_modal_.set_completion_callback([this](bool saved) {
        if (saved) {
            // Refresh the spool list to show updated values
            refresh_spools();
        }
    });

    edit_modal_.show_for_spool(lv_screen_active(), *spool, api);
}

// ============================================================================
// Delete Spool
// ============================================================================

void SpoolmanPanel::delete_spool(int spool_id) {
    // Build confirmation message with spool info
    const SpoolInfo* spool = find_cached_spool(spool_id);
    std::string spool_desc;
    if (spool) {
        spool_desc = spool->display_name() + " (#" + std::to_string(spool_id) + ")";
    } else {
        spool_desc = "Spool #" + std::to_string(spool_id);
    }

    std::string message = spool_desc + "\n" + lv_tr("This cannot be undone.");

    // Store spool_id for the confirmation callback via a static (only one delete at a time)
    static int s_pending_delete_id = 0;
    s_pending_delete_id = spool_id;

    helix::ui::modal_show_confirmation(
        lv_tr("Delete Spool?"), message.c_str(), ModalSeverity::Warning, lv_tr("Delete"),
        [](lv_event_t* /*e*/) {
            // Close the confirmation dialog immediately
            lv_obj_t* top = Modal::get_top();
            if (top) {
                Modal::hide(top);
            }

            int id = s_pending_delete_id;
            spdlog::info("[Spoolman] Confirmed delete of spool {}", id);

            MoonrakerAPI* api = get_moonraker_api();
            if (!api) {
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("API not available"),
                                              3000);
                return;
            }

            api->spoolman().delete_spoolman_spool(
                id,
                [id]() {
                    spdlog::info("[Spoolman] Spool {} deleted successfully", id);
                    helix::ui::queue_update([id]() {
                        ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                      lv_tr("Spool deleted"), 2000);
                        get_global_spoolman_panel().refresh_spools();
                    });
                },
                [id](const MoonrakerError& err) {
                    spdlog::error("[Spoolman] Failed to delete spool {}: {}", id, err.message);
                    helix::ui::queue_update([]() {
                        ToastManager::instance().show(ToastSeverity::ERROR,
                                                      lv_tr("Failed to delete spool"), 3000);
                    });
                });
        },
        nullptr, // No cancel callback needed
        nullptr);
}

// ============================================================================
// Static Event Callbacks
// ============================================================================

void SpoolmanPanel::on_spool_row_clicked(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    // Capture click point from the input device while event is still active
    lv_point_t click_pt = {0, 0};
    lv_indev_t* indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &click_pt);
    }

    // The target might be a child of the row, walk up to find the row
    lv_obj_t* row = target;
    while (row && lv_obj_get_user_data(row) == nullptr) {
        row = lv_obj_get_parent(row);
    }

    if (row) {
        get_global_spoolman_panel().handle_spool_clicked(row, click_pt);
    }
}

void SpoolmanPanel::on_refresh_clicked(lv_event_t* /*e*/) {
    spdlog::debug("[Spoolman] Refresh clicked");
    get_global_spoolman_panel().refresh_spools();
}

void SpoolmanPanel::on_add_spool_clicked(lv_event_t* /*e*/) {
    spdlog::info("[SpoolmanPanel] Add spool clicked — launching wizard");
    auto& panel = get_global_spoolman_panel();

    // Set completion callback on the wizard to refresh spool list after creation
    auto& wizard = get_global_spool_wizard();
    wizard.set_completion_callback([]() { get_global_spoolman_panel().refresh_spools(); });

    helix::ui::lazy_create_and_push_overlay<SpoolWizardOverlay>(
        get_global_spool_wizard, panel.wizard_panel_, lv_display_get_screen_active(nullptr),
        "Spool Wizard", "SpoolmanPanel");
}

void SpoolmanPanel::on_scroll(lv_event_t* e) {
    auto* self = static_cast<SpoolmanPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->list_view_.update_visible(self->filtered_spools_, self->active_spool_id_);
    }
}

void SpoolmanPanel::on_search_changed(lv_event_t* e) {
    lv_obj_t* textarea = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!textarea) {
        return;
    }

    auto& panel = get_global_spoolman_panel();

    // Store the new query text
    const char* text = lv_textarea_get_text(textarea);
    panel.search_query_ = text ? text : "";

    // Debounce: cancel existing timer, start new one
    if (panel.search_debounce_timer_) {
        lv_timer_delete(panel.search_debounce_timer_);
        panel.search_debounce_timer_ = nullptr;
    }

    panel.search_debounce_timer_ = lv_timer_create(on_search_timer, SEARCH_DEBOUNCE_MS, &panel);
    lv_timer_set_repeat_count(panel.search_debounce_timer_, 1);
}

void SpoolmanPanel::on_search_clear(lv_event_t* /*e*/) {
    // Text is already cleared by text_input's internal clear button handler.
    // We just need to update the search state and repopulate immediately.
    auto& panel = get_global_spoolman_panel();
    panel.search_query_.clear();
    if (panel.search_debounce_timer_) {
        lv_timer_delete(panel.search_debounce_timer_);
        panel.search_debounce_timer_ = nullptr;
    }
    panel.populate_spool_list();
}

void SpoolmanPanel::on_search_timer(lv_timer_t* timer) {
    auto* self = static_cast<SpoolmanPanel*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }

    self->search_debounce_timer_ = nullptr;

    spdlog::debug("[Spoolman] Search query: '{}'", self->search_query_);

    // Re-filter and repopulate (populate_spool_list handles empty/non-empty states)
    self->populate_spool_list();
}
