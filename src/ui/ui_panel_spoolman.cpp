// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_spoolman.h"

#include "ui_callback_helpers.h"
#include "ui_global_panel_helper.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_spool_canvas.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_globals.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ipp_print_modal.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#endif
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_types.h" // apply_spool_to_slot
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
        {"on_spoolman_location_filter_changed", on_location_filter_changed},
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

    spdlog::debug("[{}] on_activate()", get_name());

    // Clear search and location filter on activation
    search_query_.clear();
    selected_location_.clear();
    lv_obj_t* search_box = lv_obj_find_by_name(overlay_root_, "search_box");
    if (search_box) {
        lv_textarea_set_text(search_box, "");
    }

    // Refresh spool list when panel becomes visible
    refresh_spools();

    // Start Spoolman polling for weight updates
    SpoolmanManager::instance().start_spoolman_polling();
}

void SpoolmanPanel::on_deactivate() {
    SpoolmanManager::instance().stop_spoolman_polling();

    // Clean up debounce timer
    if (search_debounce_timer_) {
        lv_timer_delete(search_debounce_timer_);
        search_debounce_timer_ = nullptr;
    }

    // Reset visible state but keep pool intact for reactivation
    list_view_.reset();

    spdlog::debug("[{}] on_deactivate()", get_name());

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Data Loading
// ============================================================================

void SpoolmanPanel::refresh_spools() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API available, cannot refresh", get_name());
        show_empty_state();
        return;
    }

    show_loading_state();

    auto tok = lifetime_.token();

    // Shared handler: update cached spools and active ID, then repopulate
    auto apply_spools = [this, tok](std::vector<SpoolInfo> spools, int active_id) {
        if (tok.expired())
            return;
        tok.defer([this, spools = std::move(spools), active_id]() {
            cached_spools_ = spools;
            active_spool_id_ = active_id;
            populate_spool_list();
        });
    };

    std::string name = get_name();

    api->spoolman().get_spoolman_spools(
        [name, apply_spools, tok](const std::vector<SpoolInfo>& spools) {
            if (tok.expired())
                return;
            spdlog::info("[{}] Received {} spools from Spoolman", name, spools.size());

            // Also get active spool ID before updating UI
            IMoonrakerAPI* api_inner = get_moonraker_api();
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
        [this, name, tok](const MoonrakerError& err) {
            if (tok.expired())
                return;
            spdlog::error("[{}] Failed to fetch spools: {}", name, err.message);
            tok.defer([this]() {
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
    } else if (filtered_spools_.size() != cached_spools_.size()) {
        // Show filtered count: "Spoolman: 5/19 Spools"
        char buf[64];
        snprintf(buf, sizeof(buf), lv_tr("Spoolman: %zu/%zu Spool%s"), filtered_spools_.size(),
                 cached_spools_.size(), cached_spools_.size() == 1 ? "" : "s");
        lv_subject_copy_string(&header_title_subject_, buf);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), lv_tr("Spoolman: %zu Spool%s"), cached_spools_.size(),
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

    // Sync search_query_ from the actual search box text so the filter always
    // matches what the user sees (handles refresh after wizard completion, etc.)
    lv_obj_t* search_box = lv_obj_find_by_name(overlay_root_, "search_box");
    if (search_box) {
        const char* text = lv_textarea_get_text(search_box);
        search_query_ = text ? text : "";
    }

    // Apply current search filter
    apply_filter();
    update_location_filter_dropdown();

    if (filtered_spools_.empty()) {
        show_empty_state();
        return;
    }

    // Delegate to virtualized list view
    list_view_.populate(filtered_spools_, active_spool_id_, preserve_scroll_);
    preserve_scroll_ = false; // Reset after use
    show_spool_list();

    spdlog::debug("[{}] Populated {} spool rows (filtered from {})", get_name(),
                  filtered_spools_.size(), cached_spools_.size());
}

void SpoolmanPanel::apply_filter() {
    auto location_filtered = filter_by_location(cached_spools_);
    filtered_spools_ = filter_spools(location_filtered, search_query_);
    update_spool_count();
}

void SpoolmanPanel::update_location_filter_dropdown() {
    lv_obj_t* dropdown = lv_obj_find_by_name(overlay_root_, "location_filter");
    if (!dropdown) {
        return;
    }

    // Defensive guard against potential re-entry. LVGL 9.5 does not fire
    // value_changed from lv_dropdown_set_options(), but we guard anyway
    // in case future versions change that behavior.
    if (updating_location_dropdown_) {
        return;
    }
    updating_location_dropdown_ = true;

    // Collect unique non-empty locations
    std::vector<std::string> locations;
    for (const auto& spool : cached_spools_) {
        if (!spool.location.empty()) {
            if (std::find(locations.begin(), locations.end(), spool.location) == locations.end()) {
                locations.push_back(spool.location);
            }
        }
    }

    // Hide dropdown if no locations exist.
    // Imperative visibility exception: dropdown content is fully dynamic (options
    // set from C++), so subject-based binding adds no value here.
    if (locations.empty()) {
        lv_obj_add_flag(dropdown, LV_OBJ_FLAG_HIDDEN);
        selected_location_.clear();
        updating_location_dropdown_ = false;
        return;
    }

    // Sort alphabetically
    std::sort(locations.begin(), locations.end());

    // Build options string: "All\nLocation1\nLocation2\n..."
    std::string options = lv_tr("All");
    for (const auto& loc : locations) {
        options += "\n" + loc;
    }
    lv_dropdown_set_options(dropdown, options.c_str());

    // Restore or reset selection
    if (!selected_location_.empty()) {
        auto it = std::find(locations.begin(), locations.end(), selected_location_);
        if (it != locations.end()) {
            uint32_t idx = static_cast<uint32_t>(std::distance(locations.begin(), it)) + 1;
            lv_dropdown_set_selected(dropdown, idx);
        } else {
            // Previously selected location no longer exists
            selected_location_.clear();
            lv_dropdown_set_selected(dropdown, 0);
        }
    } else {
        // Ensure "All" is selected (lv_dropdown_set_options resets internally,
        // but be explicit for clarity)
        lv_dropdown_set_selected(dropdown, 0);
    }

    lv_obj_remove_flag(dropdown, LV_OBJ_FLAG_HIDDEN);
    updating_location_dropdown_ = false;
}

std::vector<SpoolInfo>
SpoolmanPanel::filter_by_location(const std::vector<SpoolInfo>& spools) const {
    if (selected_location_.empty()) {
        return spools;
    }
    std::vector<SpoolInfo> result;
    for (const auto& spool : spools) {
        if (spool.location == selected_location_) {
            result.push_back(spool);
        }
    }
    return result;
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

#if HELIX_HAS_LABEL_PRINTER
    case MenuAction::PRINT_LABEL:
        print_label_for_spool(spool_id);
        break;
#endif

    case MenuAction::DUPLICATE:
        duplicate_spool(spool_id);
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
    const SpoolInfo* found = find_cached_spool(spool_id);
    if (!found) {
        spdlog::warn("[{}] Spool {} not found in cache, cannot set active", get_name(), spool_id);
        return;
    }

    std::string name = get_name();
    auto tok = lifetime_.token();
    std::string spool_name = found->display_name();

    // External-spool slot (sentinel -2), identity transferred by the shared
    // helper. The commit is the single authority for spool assignment writes:
    // S1 server set gated on the round-trip, S5 store subset with the
    // emptiness predicate, S6 identity-cache invalidation of the replaced
    // link (prestonbrown/helixscreen#1283).
    SlotInfo slot;
    slot.slot_index = -2;
    slot.global_index = -2;
    apply_spool_to_slot(slot, *found);

    AmsState::instance().commit_external_spool_edit(
        slot,
        [this, spool_id, spool_name, name, tok]() {
            // Main-thread-only: AmsState::commit_external_spool_edit marshals
            // on_committed through queue_update before invoking it.
            if (tok.expired())
                return; // L081_OK: see comment above
            spdlog::info("[{}] Set active spool to {}", name, spool_id);

            active_spool_id_ = spool_id;
            update_active_indicators();

            std::string msg = std::string(lv_tr("Active")) + ": " + spool_name;
            ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str(), 2000);
        },
        [name, spool_id, tok](const MoonrakerError& err) {
            // Main-thread-only: on_error is marshalled through queue_update
            // by commit_external_spool_edit before invocation.
            if (tok.expired())
                return; // L081_OK: see comment above
            spdlog::error("[{}] Failed to set active spool {}: {}", name, spool_id, err.message);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to set active spool"),
                                          3000);
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

    IMoonrakerAPI* api = get_moonraker_api();

    edit_modal_.set_completion_callback([this](bool saved) {
        if (saved) {
            preserve_scroll_ = true;
            refresh_spools();
        }
    });

    edit_modal_.show_for_spool(lv_screen_active(), *spool, api);
}

// ============================================================================
// Delete Spool
// ============================================================================

void SpoolmanPanel::duplicate_spool(int spool_id) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API, cannot duplicate spool", get_name());
        return;
    }

    const SpoolInfo* spool = find_cached_spool(spool_id);
    if (!spool) {
        spdlog::warn("[{}] Spool {} not found in cache", get_name(), spool_id);
        return;
    }

    // Build create payload: same filament, full weight, blank lot/notes
    nlohmann::json body;
    body["filament_id"] = spool->filament_id;
    if (spool->initial_weight_g > 0) {
        body["remaining_weight"] = spool->initial_weight_g;
    }
    if (spool->spool_weight_g > 0) {
        body["spool_weight"] = spool->spool_weight_g;
    }

    std::string display = spool->display_name();
    auto token = lifetime_.token();

    api->spoolman().create_spoolman_spool(
        body,
        [token, display](const SpoolInfo& created) {
            spdlog::info("[Spoolman] Duplicated '{}' as spool #{}", display, created.id);
            helix::ui::queue_update([token, created]() {
                std::string msg =
                    std::string(lv_tr("Duplicated")) + ": #" + std::to_string(created.id);
                ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str(), 3000);
                if (token.expired())
                    return;
                auto& panel = get_global_spoolman_panel();
                panel.preserve_scroll_ = true;
                panel.refresh_spools();
            });
        },
        [display](const MoonrakerError& err) {
            spdlog::error("[Spoolman] Failed to duplicate '{}': {}", display, err.message);
            helix::ui::queue_update([]() {
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Failed to duplicate spool"), 3000);
            });
        });
}

void SpoolmanPanel::delete_spool(int spool_id) {
    // Build confirmation message with spool info
    const SpoolInfo* spool = find_cached_spool(spool_id);
    std::string spool_desc;
    if (spool) {
        spool_desc = spool->display_name() + " (#" + std::to_string(spool_id) + ")";
    } else {
        spool_desc = std::string(lv_tr("Spool #")) + std::to_string(spool_id);
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

            IMoonrakerAPI* api = get_moonraker_api();
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
                        auto& panel = get_global_spoolman_panel();
                        panel.preserve_scroll_ = true;
                        panel.refresh_spools();
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

void SpoolmanPanel::on_location_filter_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!dropdown) {
        return;
    }

    auto& panel = get_global_spoolman_panel();

    // Guard: if we're programmatically updating the dropdown, ignore the event
    if (panel.updating_location_dropdown_) {
        return;
    }

    uint32_t selected = lv_dropdown_get_selected(dropdown);
    if (selected == 0) {
        // "All" selected
        panel.selected_location_.clear();
    } else {
        char buf[128];
        lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
        panel.selected_location_ = buf;
    }

    spdlog::debug("[Spoolman] Location filter: '{}'", panel.selected_location_);
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

#if HELIX_HAS_LABEL_PRINTER
void SpoolmanPanel::print_label_for_spool(int spool_id) {
    auto& settings = helix::LabelPrinterSettingsManager::instance();

    if (!settings.is_configured()) {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Set up your label printer in Settings"), 3000);
        return;
    }

    const SpoolInfo* spool = find_cached_spool(spool_id);
    if (!spool) {
        spdlog::warn("[{}] Cannot print label: spool {} not found", get_name(), spool_id);
        return;
    }

    auto print_cb = [](bool success, const std::string& error) {
        if (success) {
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Label printed"), 2000);
        } else {
            spdlog::error("[SpoolmanPanel] Print failed: {}", error);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          helix::friendly_label_printer_error(error).c_str(), 5000);
        }
    };

    if (!helix::maybe_show_ipp_print_modal(*spool, print_cb)) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing label..."), 2000);
        helix::print_spool_label(*spool, print_cb);
    }
}
#endif
