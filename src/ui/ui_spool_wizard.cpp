// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_wizard.h"

#include "ui_callback_helpers.h"
#include "ui_color_picker.h"
#include "ui_global_panel_helper.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <unordered_map>

#include "hv/json.hpp"

namespace {

/// Maximum input lengths for sanity checking
constexpr size_t MAX_VENDOR_NAME_LEN = 256;
constexpr size_t MAX_VENDOR_URL_LEN = 2048;

/// Return a lowercased copy of the input string
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// Return a whitespace-trimmed copy of the input string
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos)
        return "";
    auto end = s.find_last_not_of(" \t\n\r\f\v");
    return s.substr(start, end - start + 1);
}

/// Set a JSON temperature range object, only including fields with positive values
void set_temp_range(nlohmann::json& data, const char* key, int min_val, int max_val) {
    if (min_val > 0 || max_val > 0) {
        data[key] = nlohmann::json::object();
        if (min_val > 0)
            data[key]["min"] = min_val;
        if (max_val > 0)
            data[key]["max"] = max_val;
    }
}

} // namespace

// ============================================================================
// Global Instance
// ============================================================================

DEFINE_GLOBAL_PANEL(SpoolWizardOverlay, g_spool_wizard, get_global_spool_wizard)

// ============================================================================
// Constructor / Destructor
// ============================================================================

SpoolWizardOverlay::SpoolWizardOverlay() {
    spdlog::trace("[{}] Constructor", get_name());
}

SpoolWizardOverlay::~SpoolWizardOverlay() {
    deinit_subjects();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void SpoolWizardOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Step subject — drives step visibility in XML via bind_flag_if_not_eq
        UI_MANAGED_SUBJECT_INT(step_subject_, static_cast<int32_t>(Step::VENDOR),
                               "spool_wizard_step", subjects_);

        // Can proceed — drives Next/Create button disabled state
        UI_MANAGED_SUBJECT_INT(can_proceed_subject_, 0, "spool_wizard_can_proceed", subjects_);

        // Step label string — "Step 1 of 3"
        std::snprintf(step_label_buf_, sizeof(step_label_buf_), "%s", lv_tr("Step 1 of 3"));
        UI_MANAGED_SUBJECT_STRING(step_label_subject_, step_label_buf_, step_label_buf_,
                                  "spool_wizard_step_label", subjects_);

        // Creating spinner state
        UI_MANAGED_SUBJECT_INT(creating_subject_, 0, "wizard_creating", subjects_);

        // Selected vendor name display (step 1 header)
        UI_MANAGED_SUBJECT_STRING(selected_vendor_name_subject_, selected_vendor_name_buf_, "",
                                  "wizard_selected_vendor_name", subjects_);

        // Summary fields (step 2)
        UI_MANAGED_SUBJECT_STRING(summary_vendor_subject_, summary_vendor_buf_, "",
                                  "wizard_summary_vendor", subjects_);
        UI_MANAGED_SUBJECT_STRING(summary_filament_subject_, summary_filament_buf_, "",
                                  "wizard_summary_filament", subjects_);

        // Create vendor/filament form visibility toggles
        UI_MANAGED_SUBJECT_INT(show_create_vendor_subject_, 0, "spool_wizard_show_create_vendor",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(show_create_filament_subject_, 0,
                               "spool_wizard_show_create_filament", subjects_);

        // List state subjects
        UI_MANAGED_SUBJECT_INT(vendor_count_subject_, -1, "spool_wizard_vendor_count", subjects_);
        UI_MANAGED_SUBJECT_INT(filament_count_subject_, -1, "spool_wizard_filament_count",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(vendors_loading_subject_, 0, "spool_wizard_vendors_loading",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(filaments_loading_subject_, 0, "spool_wizard_filaments_loading",
                               subjects_);

        // Can create vendor (form validation)
        UI_MANAGED_SUBJECT_INT(can_create_vendor_subject_, 0, "spool_wizard_can_create_vendor",
                               subjects_);
    });
}

void SpoolWizardOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// Callback Registration
// ============================================================================

void SpoolWizardOverlay::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    register_xml_callbacks({
        // Navigation
        {"on_wizard_back", on_wizard_back},
        {"on_wizard_next", on_wizard_next},
        {"on_wizard_create", on_wizard_create},

        // Vendor step
        {"on_wizard_vendor_selected", on_wizard_vendor_selected},
        {"on_wizard_show_create_vendor_modal", on_wizard_show_create_vendor_modal},
        {"on_wizard_cancel_create_vendor", on_wizard_cancel_create_vendor},
        {"on_wizard_vendor_search_changed", on_wizard_vendor_search_changed},
        {"on_wizard_new_vendor_name_changed", on_wizard_new_vendor_name_changed},
        {"on_wizard_new_vendor_url_changed", on_wizard_new_vendor_url_changed},
        {"on_wizard_confirm_create_vendor", on_wizard_confirm_create_vendor},

        // Filament step
        {"on_wizard_filament_selected", on_wizard_filament_selected},
        {"on_wizard_show_create_filament_modal", on_wizard_show_create_filament_modal},
        {"on_wizard_cancel_create_filament", on_wizard_cancel_create_filament},
        {"on_wizard_material_changed", on_wizard_material_changed},
        {"on_wizard_new_filament_name_changed", on_wizard_new_filament_name_changed},
        {"on_wizard_pick_filament_color", on_wizard_pick_filament_color},
        {"on_wizard_nozzle_temp_changed", on_wizard_nozzle_temp_changed},
        {"on_wizard_bed_temp_changed", on_wizard_bed_temp_changed},
        {"on_wizard_filament_weight_changed", on_wizard_filament_weight_changed},
        {"on_wizard_spool_weight_changed", on_wizard_spool_weight_changed},
        {"on_wizard_confirm_create_filament", on_wizard_confirm_create_filament},

        // Spool details step
        {"on_wizard_remaining_weight_changed", on_wizard_remaining_weight_changed},
        {"on_wizard_price_changed", on_wizard_price_changed},
        {"on_wizard_lot_changed", on_wizard_lot_changed},
        {"on_wizard_notes_changed", on_wizard_notes_changed},
    });

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* SpoolWizardOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, "spool_wizard")) {
        return nullptr;
    }

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void SpoolWizardOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] on_activate()", get_name());

    // Reset ALL wizard state for a fresh session
    reset_state();

    // Reset wizard to step 0
    navigate_to_step(Step::VENDOR);

    // Load vendors for step 0
    load_vendors();
}

void SpoolWizardOverlay::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Close create vendor modal if open
    if (create_vendor_dialog_) {
        Modal::hide(create_vendor_dialog_);
        create_vendor_dialog_ = nullptr;
    }

    // Close create filament modal if open
    if (create_filament_dialog_) {
        Modal::hide(create_filament_dialog_);
        create_filament_dialog_ = nullptr;
    }

    OverlayBase::on_deactivate();
}

void SpoolWizardOverlay::reset_state() {
    // Vendor state
    all_vendors_.clear();
    filtered_vendors_.clear();
    selected_vendor_ = {};
    new_vendor_name_.clear();
    new_vendor_url_.clear();
    vendor_search_query_.clear();

    // Filament state
    all_filaments_.clear();
    selected_filament_ = {};
    creating_new_filament_ = false;
    new_filament_name_.clear();
    new_filament_material_.clear();
    new_filament_color_hex_.clear();
    new_filament_color_name_.clear();
    new_filament_nozzle_min_ = 0;
    new_filament_nozzle_max_ = 0;
    new_filament_bed_min_ = 0;
    new_filament_bed_max_ = 0;
    new_filament_density_ = 0;
    new_filament_weight_ = 0;
    new_filament_spool_weight_ = 0;

    // Spool details state
    spool_remaining_weight_ = 0;
    spool_price_ = 0;
    spool_lot_nr_.clear();
    spool_notes_.clear();

    // Creation flow tracking
    created_vendor_id_ = -1;
    created_filament_id_ = -1;

    // Navigation
    can_proceed_ = false;

    // Reset subjects
    if (subjects_initialized_) {
        lv_subject_set_int(&can_proceed_subject_, 0);
        lv_subject_set_int(&creating_subject_, 0);
        lv_subject_set_int(&show_create_vendor_subject_, 0);
        lv_subject_set_int(&show_create_filament_subject_, 0);
        lv_subject_set_int(&vendor_count_subject_, -1);
        lv_subject_set_int(&filament_count_subject_, -1);
        lv_subject_set_int(&can_create_vendor_subject_, 0);
    }

    spdlog::debug("[{}] State reset for new wizard session", get_name());
}

// ============================================================================
// Step Navigation (pure logic — testable without LVGL)
// ============================================================================

void SpoolWizardOverlay::navigate_next() {
    if (!can_proceed_) {
        spdlog::debug("[{}] navigate_next blocked: can_proceed=false", get_name());
        return;
    }

    int next = static_cast<int>(current_step_) + 1;
    if (next >= STEP_COUNT) {
        spdlog::debug("[{}] Already at final step", get_name());
        return;
    }

    auto next_step = static_cast<Step>(next);
    navigate_to_step(next_step);

    // Load data for the new step
    if (next_step == Step::FILAMENT) {
        load_filaments();
    } else if (next_step == Step::SPOOL_DETAILS) {
        // Pre-fill remaining weight from selected filament's net weight
        spool_remaining_weight_ = selected_filament_.weight;

        // Update UI fields if overlay is active
        if (overlay_root_) {
            lv_obj_t* weight_input = lv_obj_find_by_name(overlay_root_, "remaining_weight");
            if (weight_input && spool_remaining_weight_ > 0) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.0f", spool_remaining_weight_);
                lv_textarea_set_text(weight_input, buf);
            }

            // Update summary color swatch from selected filament
            lv_obj_t* swatch = lv_obj_find_by_name(overlay_root_, "summary_color_swatch");
            if (swatch && !selected_filament_.color_hex.empty()) {
                uint32_t color_val =
                    std::strtoul(selected_filament_.color_hex.c_str(), nullptr, 16);
                lv_obj_set_style_bg_color(swatch, lv_color_hex(color_val), 0);
            }
        }

        // Enable proceed if weight is pre-filled
        if (spool_remaining_weight_ > 0) {
            set_can_proceed(true);
        }
    }
}

void SpoolWizardOverlay::navigate_back() {
    int prev = static_cast<int>(current_step_) - 1;
    if (prev < 0) {
        // At first step — close the overlay
        spdlog::debug("[{}] navigate_back at step 0 — closing overlay", get_name());
        if (close_callback_) {
            close_callback_();
        }
        return;
    }

    navigate_to_step(static_cast<Step>(prev));
}

void SpoolWizardOverlay::set_can_proceed(bool val) {
    can_proceed_ = val;
    sync_subjects();
}

std::string SpoolWizardOverlay::step_label() const {
    int step_num = static_cast<int>(current_step_) + 1;
    return fmt::format(lv_tr("New Spool: Step {} of {}"), step_num, STEP_COUNT);
}

void SpoolWizardOverlay::on_create_requested() {
    spdlog::info("[{}] Create spool requested", get_name());

    // Reset tracking for rollback
    created_vendor_id_ = -1;
    created_filament_id_ = -1;

    set_creating(true);

    if (selected_vendor_.server_id < 0) {
        // Vendor is new — create it first, then filament, then spool
        create_vendor_then_filament_then_spool();
    } else if (selected_filament_.server_id < 0) {
        // Vendor exists, filament is new — create filament, then spool
        create_filament_then_spool(selected_vendor_.server_id);
    } else {
        // Both exist — create spool directly
        create_spool(selected_filament_.server_id);
    }
}

// ============================================================================
// Navigation Helpers
// ============================================================================

void SpoolWizardOverlay::navigate_to_step(Step step) {
    current_step_ = step;
    can_proceed_ = false;
    update_step_label();
    sync_subjects();

    spdlog::debug("[{}] Navigated to step {}", get_name(), static_cast<int>(step));
}

void SpoolWizardOverlay::update_step_label() {
    std::string label = step_label();
    std::snprintf(step_label_buf_, sizeof(step_label_buf_), "%s", label.c_str());

    // Update subject if initialized
    if (subjects_initialized_) {
        lv_subject_copy_string(&step_label_subject_, step_label_buf_);
    }

    // Update header title directly
    if (overlay_root_) {
        lv_obj_t* title = lv_obj_find_by_name(overlay_root_, "header_title");
        if (title) {
            lv_label_set_text(title, step_label_buf_);
        }
    }
}

void SpoolWizardOverlay::sync_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    lv_subject_set_int(&step_subject_, static_cast<int32_t>(current_step_));
    lv_subject_set_int(&can_proceed_subject_, can_proceed_ ? 1 : 0);
}

// ============================================================================
// Creation Flow
// ============================================================================

void SpoolWizardOverlay::set_creating(bool val) {
    if (subjects_initialized_) {
        lv_subject_set_int(&creating_subject_, val ? 1 : 0);
    }
}

void SpoolWizardOverlay::create_vendor_then_filament_then_spool() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_creation_error(lv_tr("No API connection"));
        return;
    }

    nlohmann::json data;
    data["name"] = selected_vendor_.name;
    if (!new_vendor_url_.empty()) {
        data["url"] = new_vendor_url_;
    }

    api->spoolman().create_spoolman_vendor(
        data,
        lifetime_.bg_cb("SpoolWizard::create_vendor_ok",
                        [this](const VendorInfo& vendor) {
                            if (!is_visible()) {
                                spdlog::warn("[{}] Vendor created but overlay no longer visible",
                                             get_name());
                                return;
                            }
                            selected_vendor_.server_id = vendor.id;
                            created_vendor_id_ = vendor.id;
                            spdlog::info("[{}] Created vendor id={} name='{}'", get_name(),
                                         vendor.id, vendor.name);

                            if (selected_filament_.server_id < 0) {
                                create_filament_then_spool(vendor.id);
                            } else {
                                create_spool(selected_filament_.server_id);
                            }
                        }),
        lifetime_.bg_cb("SpoolWizard::create_vendor_error", [this](const MoonrakerError& err) {
            on_creation_error(fmt::format("{} {}", lv_tr("Failed to create vendor:"), err.message));
        }));
}

void SpoolWizardOverlay::create_filament_then_spool(int vendor_id) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_creation_error(lv_tr("No API connection"), created_vendor_id_);
        return;
    }

    nlohmann::json data;
    data["vendor_id"] = vendor_id;
    data["name"] = selected_filament_.name.empty()
                       ? selected_filament_.material + " " + selected_filament_.color_name
                       : selected_filament_.name;
    data["material"] = selected_filament_.material;
    if (!selected_filament_.color_hex.empty()) {
        data["color_hex"] = selected_filament_.color_hex;
    }
    // density and diameter are REQUIRED by Spoolman (no defaults in their API)
    data["density"] = selected_filament_.density > 0 ? selected_filament_.density : 1.24;
    data["diameter"] = selected_filament_.diameter > 0 ? selected_filament_.diameter : 1.75;
    if (selected_filament_.weight > 0) {
        data["weight"] = selected_filament_.weight;
    }
    if (selected_filament_.spool_weight > 0) {
        data["spool_weight"] = selected_filament_.spool_weight;
    }
    set_temp_range(data, "settings_extruder_temp", selected_filament_.nozzle_temp_min,
                   selected_filament_.nozzle_temp_max);
    set_temp_range(data, "settings_bed_temp", selected_filament_.bed_temp_min,
                   selected_filament_.bed_temp_max);

    api->spoolman().create_spoolman_filament(
        data,
        lifetime_.bg_cb("SpoolWizard::create_filament_ok",
                        [this](const FilamentInfo& filament) {
                            if (!is_visible()) {
                                spdlog::warn("[{}] Filament created but overlay no longer visible",
                                             get_name());
                                return;
                            }
                            selected_filament_.server_id = filament.id;
                            created_filament_id_ = filament.id;
                            spdlog::info("[{}] Created filament id={} name='{}'", get_name(),
                                         filament.id, filament.display_name());
                            create_spool(filament.id);
                        }),
        lifetime_.bg_cb("SpoolWizard::create_filament_error", [this](const MoonrakerError& err) {
            on_creation_error(
                fmt::format("{} {}", lv_tr("Failed to create filament:"), err.message),
                created_vendor_id_);
        }));
}

void SpoolWizardOverlay::create_spool(int filament_id) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_creation_error(lv_tr("No API connection"), created_vendor_id_, created_filament_id_);
        return;
    }

    nlohmann::json data;
    data["filament_id"] = filament_id;
    if (spool_remaining_weight_ > 0) {
        data["remaining_weight"] = spool_remaining_weight_;
    }
    if (spool_price_ > 0) {
        data["price"] = spool_price_;
    }
    if (!spool_lot_nr_.empty()) {
        data["lot_nr"] = spool_lot_nr_;
    }
    if (!spool_notes_.empty()) {
        data["comment"] = spool_notes_;
    }

    api->spoolman().create_spoolman_spool(
        data,
        lifetime_.bg_cb("SpoolWizard::create_spool_ok",
                        [this](const SpoolInfo& spool) {
                            if (!is_visible()) {
                                spdlog::warn("[{}] Spool created but overlay no longer visible",
                                             get_name());
                                return;
                            }
                            on_creation_success(spool);
                        }),
        lifetime_.bg_cb("SpoolWizard::create_spool_error", [this](const MoonrakerError& err) {
            on_creation_error(fmt::format("{} {}", lv_tr("Failed to create spool:"), err.message),
                              created_vendor_id_, created_filament_id_);
        }));
}

void SpoolWizardOverlay::on_creation_success(const SpoolInfo& spool) {
    spdlog::info("[{}] Spool created successfully (id={})", get_name(), spool.id);
    set_creating(false);

    // Show success toast
    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Spool created successfully"));

    // Refresh the spool list in SpoolmanPanel
    if (completion_callback_) {
        completion_callback_();
    }

    // Close the wizard overlay
    NavigationManager::instance().go_back();
}

void SpoolWizardOverlay::on_creation_error(const std::string& message, int rollback_vendor_id,
                                           int rollback_filament_id) {
    spdlog::error("[{}] Creation failed: {}", get_name(), message);

    // Show error toast so user knows what happened
    ToastManager::instance().show(ToastSeverity::ERROR, message.c_str());

    // Best-effort rollback — delete filament first (references vendor), then vendor
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        auto delete_vendor = [api, rollback_vendor_id]() {
            if (rollback_vendor_id >= 0) {
                api->spoolman().delete_spoolman_vendor(
                    rollback_vendor_id,
                    [rollback_vendor_id]() {
                        spdlog::info("Rollback: deleted vendor {}", rollback_vendor_id);
                    },
                    [](const MoonrakerError& e) {
                        spdlog::warn("Rollback vendor failed: {}", e.message);
                    });
            }
        };

        if (rollback_filament_id >= 0) {
            // Delete filament first, then vendor (respects FK ordering)
            api->spoolman().delete_spoolman_filament(
                rollback_filament_id,
                [rollback_filament_id, delete_vendor]() {
                    spdlog::info("Rollback: deleted filament {}", rollback_filament_id);
                    delete_vendor();
                },
                [delete_vendor](const MoonrakerError& e) {
                    spdlog::warn("Rollback filament failed: {}", e.message);
                    delete_vendor(); // Still try vendor cleanup
                });
        } else {
            delete_vendor();
        }
    }

    set_creating(false);
}

// ============================================================================
// Static Event Callbacks
// ============================================================================

void SpoolWizardOverlay::on_wizard_back(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Back clicked");
    get_global_spool_wizard().navigate_back();
}

void SpoolWizardOverlay::on_wizard_next(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Next clicked");
    get_global_spool_wizard().navigate_next();
}

void SpoolWizardOverlay::on_wizard_create(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Create clicked");
    get_global_spool_wizard().on_create_requested();
}

// ============================================================================
// Vendor Step Logic
// ============================================================================

std::vector<SpoolWizardOverlay::VendorEntry>
SpoolWizardOverlay::merge_vendors(const std::vector<VendorEntry>& external_vendors,
                                  const std::vector<VendorEntry>& server_vendors) {
    // Build a map keyed by lowercased name for deduplication
    std::unordered_map<std::string, VendorEntry> by_name;

    // Server vendors first (they have IDs, so they take priority)
    for (const auto& sv : server_vendors) {
        by_name[to_lower(sv.name)] = sv;
    }

    // Merge in external DB vendors -- mark from_database, keep server ID if already present
    for (const auto& ext : external_vendors) {
        auto [it, inserted] =
            by_name.try_emplace(to_lower(ext.name), VendorEntry{ext.name, -1, false, true});
        if (!inserted) {
            it->second.from_database = true;
        }
    }

    // Collect and sort alphabetically by name (case-insensitive)
    std::vector<VendorEntry> result;
    result.reserve(by_name.size());
    for (auto& [_, entry] : by_name) {
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const VendorEntry& a, const VendorEntry& b) {
        return to_lower(a.name) < to_lower(b.name);
    });

    return result;
}

std::vector<SpoolWizardOverlay::VendorEntry>
SpoolWizardOverlay::filter_vendor_list(const std::vector<VendorEntry>& vendors,
                                       const std::string& query) {
    if (query.empty()) {
        return vendors;
    }

    std::string lower_query = to_lower(query);

    std::vector<VendorEntry> result;
    for (const auto& v : vendors) {
        if (to_lower(v.name).find(lower_query) != std::string::npos) {
            result.push_back(v);
        }
    }
    return result;
}

void SpoolWizardOverlay::load_vendors() {
    spdlog::debug("[{}] Loading vendors", get_name());

    // Reset vendor state
    all_vendors_.clear();
    filtered_vendors_.clear();
    selected_vendor_ = {};
    new_vendor_name_.clear();
    new_vendor_url_.clear();
    vendor_search_query_.clear();

    // Show loading state
    if (subjects_initialized_) {
        lv_subject_set_int(&vendors_loading_subject_, 1);
        lv_subject_set_int(&vendor_count_subject_, -1);
        lv_subject_set_int(&show_create_vendor_subject_, 0);
    }

    // Get server + external vendors (both async via IMoonrakerAPI)
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API available, showing empty vendors", get_name());
        if (subjects_initialized_) {
            lv_subject_set_int(&vendors_loading_subject_, 0);
            lv_subject_set_int(&vendor_count_subject_, 0);
        }
        populate_vendor_list();
        return;
    }

    // Shared context to coordinate two async calls (atomic counter for thread safety)
    struct VendorLoadContext {
        std::vector<VendorEntry> server_vendors;
        std::vector<VendorEntry> external_vendors;
        std::atomic<int> completed{0};
    };
    auto ctx = std::make_shared<VendorLoadContext>();

    // Helper lambda — called by whichever callback completes second, on whichever
    // background thread got there. Captures a lifetime token rather than touching
    // `this->lifetime_` off-thread (#707 TOCTOU); the deferred body is skipped
    // outright if the overlay was deactivated in the meantime.
    auto finish = [this, ctx, tok = lifetime_.token()]() {
        tok.defer("SpoolWizard::load_vendors_apply", [this, ctx]() {
            all_vendors_ = merge_vendors(ctx->external_vendors, ctx->server_vendors);
            filtered_vendors_ = filter_vendor_list(all_vendors_, vendor_search_query_);

            if (subjects_initialized_) {
                lv_subject_set_int(&vendors_loading_subject_, 0);
                lv_subject_set_int(&vendor_count_subject_,
                                   static_cast<int32_t>(filtered_vendors_.size()));
            }

            populate_vendor_list();
            spdlog::info("[SpoolWizard] Loaded {} vendors total ({} server + {} external)",
                         all_vendors_.size(), ctx->server_vendors.size(),
                         ctx->external_vendors.size());
        });
    };

    // Fetch server vendors
    // Fetch server vendors
    api->spoolman().get_spoolman_vendors(
        [ctx, finish](const std::vector<VendorInfo>& server_list) {
            ctx->server_vendors.reserve(server_list.size());
            for (const auto& vi : server_list) {
                VendorEntry entry;
                entry.name = vi.name;
                entry.server_id = vi.id;
                entry.from_server = true;
                entry.from_database = false;
                ctx->server_vendors.push_back(std::move(entry));
            }
            spdlog::debug("[SpoolWizard] Got {} vendors from server", ctx->server_vendors.size());
            if (ctx->completed.fetch_add(1) == 1) {
                finish();
            }
        },
        [ctx, finish](const MoonrakerError& err) {
            spdlog::warn("[SpoolWizard] Failed to fetch server vendors: {}", err.message);
            if (ctx->completed.fetch_add(1) == 1) {
                finish();
            }
        });

    // Fetch external DB vendors
    api->spoolman().get_spoolman_external_vendors(
        [ctx, finish](const std::vector<VendorInfo>& ext_list) {
            ctx->external_vendors.reserve(ext_list.size());
            for (const auto& vi : ext_list) {
                VendorEntry entry;
                entry.name = vi.name;
                entry.server_id = -1;
                entry.from_server = false;
                entry.from_database = true;
                ctx->external_vendors.push_back(std::move(entry));
            }
            spdlog::debug("[SpoolWizard] Got {} vendors from external DB",
                          ctx->external_vendors.size());
            if (ctx->completed.fetch_add(1) == 1) {
                finish();
            }
        },
        [ctx, finish](const MoonrakerError& err) {
            spdlog::warn("[SpoolWizard] Failed to fetch external vendors: {}", err.message);
            if (ctx->completed.fetch_add(1) == 1) {
                finish();
            }
        });
}

void SpoolWizardOverlay::filter_vendors(const std::string& query) {
    vendor_search_query_ = query;
    filtered_vendors_ = filter_vendor_list(all_vendors_, query);

    if (subjects_initialized_) {
        lv_subject_set_int(&vendor_count_subject_, static_cast<int32_t>(filtered_vendors_.size()));
    }

    populate_vendor_list();
    spdlog::debug("[{}] Filtered vendors: {} match '{}'", get_name(), filtered_vendors_.size(),
                  query);
}

void SpoolWizardOverlay::select_vendor(int index) {
    if (index < 0 || index >= static_cast<int>(filtered_vendors_.size())) {
        spdlog::warn("[{}] Invalid vendor index: {}", get_name(), index);
        return;
    }

    selected_vendor_ = filtered_vendors_[static_cast<size_t>(index)];
    new_vendor_name_.clear();
    new_vendor_url_.clear();

    spdlog::info("[{}] Selected vendor: '{}' (server_id={})", get_name(), selected_vendor_.name,
                 selected_vendor_.server_id);

    // Update checked state on vendor rows
    if (overlay_root_) {
        lv_obj_t* vendor_list = lv_obj_find_by_name(overlay_root_, "vendor_list");
        if (vendor_list) {
            uint32_t count = lv_obj_get_child_count(vendor_list);
            for (uint32_t i = 0; i < count; i++) {
                lv_obj_t* row = lv_obj_get_child(vendor_list, static_cast<int32_t>(i));
                lv_obj_set_state(row, LV_STATE_CHECKED, static_cast<int>(i) == index);
            }
        }
    }

    // Update subjects for display on filament step
    if (subjects_initialized_) {
        std::snprintf(selected_vendor_name_buf_, sizeof(selected_vendor_name_buf_), "%s",
                      selected_vendor_.name.c_str());
        lv_subject_copy_string(&selected_vendor_name_subject_, selected_vendor_name_buf_);

        std::snprintf(summary_vendor_buf_, sizeof(summary_vendor_buf_), "%s",
                      selected_vendor_.name.c_str());
        lv_subject_copy_string(&summary_vendor_subject_, summary_vendor_buf_);
    }

    set_can_proceed(true);
}

void SpoolWizardOverlay::set_new_vendor(const std::string& name, const std::string& url) {
    new_vendor_name_ = name.substr(0, MAX_VENDOR_NAME_LEN);
    new_vendor_url_ = url.substr(0, MAX_VENDOR_URL_LEN);

    bool valid = !trim(new_vendor_name_).empty();

    if (subjects_initialized_) {
        lv_subject_set_int(&can_create_vendor_subject_, valid ? 1 : 0);
    }

    spdlog::debug("[{}] New vendor name='{}' url='{}' valid={}", get_name(), name, url, valid);
}

void SpoolWizardOverlay::populate_vendor_list() {
    if (!overlay_root_) {
        spdlog::trace("[{}] populate_vendor_list: no overlay_root_, skipping UI", get_name());
        return;
    }

    lv_obj_t* vendor_list = lv_obj_find_by_name(overlay_root_, "vendor_list");
    if (!vendor_list) {
        spdlog::error("[{}] vendor_list widget not found", get_name());
        return;
    }

    // Clear existing rows
    helix::ui::safe_clean_children(vendor_list);

    for (size_t i = 0; i < filtered_vendors_.size(); i++) {
        const auto& vendor = filtered_vendors_[i];

        // Create row from XML component
        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(vendor_list, "wizard_vendor_row", nullptr));
        if (!row) {
            spdlog::error("[{}] Failed to create vendor row for '{}'", get_name(), vendor.name);
            continue;
        }

        // Store index in user_data for click handling
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        // Set vendor name
        lv_obj_t* name_label = lv_obj_find_by_name(row, "vendor_name");
        if (name_label) {
            lv_label_set_text(name_label, vendor.name.c_str());
        }

        // Set source badge
        lv_obj_t* source_label = lv_obj_find_by_name(row, "vendor_source");
        if (source_label) {
            if (vendor.from_server && vendor.from_database) {
                lv_label_set_text(source_label, lv_tr("Both"));
            } else if (vendor.from_server) {
                lv_label_set_text(source_label, "Spoolman"); // i18n: product name, do not translate
            } else {
                lv_label_set_text(source_label, lv_tr("Database"));
            }
        }
    }

    spdlog::debug("[{}] Populated {} vendor rows", get_name(), filtered_vendors_.size());
}

// ============================================================================
// Vendor Step Event Callbacks
// ============================================================================

void SpoolWizardOverlay::on_wizard_vendor_selected(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    spdlog::debug("[SpoolWizard] Vendor selected, index={}", index);
    get_global_spool_wizard().select_vendor(index);
}

void SpoolWizardOverlay::on_wizard_show_create_vendor_modal(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Show create vendor modal");
    auto& wiz = get_global_spool_wizard();

    // Clear previous input state
    wiz.new_vendor_name_.clear();
    wiz.new_vendor_url_.clear();
    if (wiz.subjects_initialized_) {
        lv_subject_set_int(&wiz.can_create_vendor_subject_, 0);
    }

    // Show the modal
    wiz.create_vendor_dialog_ = Modal::show("create_vendor_modal");

    if (wiz.create_vendor_dialog_) {
        // Register keyboards for text inputs
        lv_obj_t* name_input = lv_obj_find_by_name(wiz.create_vendor_dialog_, "new_vendor_name");
        if (name_input) {
            helix::ui::modal_register_keyboard(wiz.create_vendor_dialog_, name_input);
        }
        lv_obj_t* url_input = lv_obj_find_by_name(wiz.create_vendor_dialog_, "new_vendor_url");
        if (url_input) {
            helix::ui::modal_register_keyboard(wiz.create_vendor_dialog_, url_input);
        }
    }
}

void SpoolWizardOverlay::on_wizard_cancel_create_vendor(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Cancel create vendor");
    auto& wiz = get_global_spool_wizard();
    if (wiz.create_vendor_dialog_) {
        Modal::hide(wiz.create_vendor_dialog_);
        wiz.create_vendor_dialog_ = nullptr;
    }
}

void SpoolWizardOverlay::on_wizard_vendor_search_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    spdlog::debug("[SpoolWizard] Vendor search: '{}'", text ? text : "");
    get_global_spool_wizard().filter_vendors(text ? text : "");
}

void SpoolWizardOverlay::on_wizard_new_vendor_name_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    spdlog::debug("[SpoolWizard] New vendor name: '{}'", text ? text : "");
    auto& wiz = get_global_spool_wizard();
    wiz.set_new_vendor(text ? text : "", wiz.new_vendor_url_);
}

void SpoolWizardOverlay::on_wizard_new_vendor_url_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    spdlog::debug("[SpoolWizard] New vendor URL: '{}'", text ? text : "");
    auto& wiz = get_global_spool_wizard();
    wiz.set_new_vendor(wiz.new_vendor_name_, text ? text : "");
}

void SpoolWizardOverlay::on_wizard_confirm_create_vendor(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Confirm create vendor");
    auto& wiz = get_global_spool_wizard();

    std::string name = trim(wiz.new_vendor_name_);
    if (name.empty()) {
        spdlog::warn("[SpoolWizard] Cannot create vendor with empty name");
        return;
    }

    // Check for duplicate vendor name (case-insensitive)
    std::string name_lower = to_lower(name);
    for (const auto& v : wiz.all_vendors_) {
        if (to_lower(v.name) == name_lower) {
            spdlog::warn("[SpoolWizard] Duplicate vendor name: '{}'", name);
            ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Vendor already exists"));
            return;
        }
    }

    // Close the modal first (before touching the list, to avoid focus/scroll side effects)
    if (wiz.create_vendor_dialog_) {
        Modal::hide(wiz.create_vendor_dialog_);
        wiz.create_vendor_dialog_ = nullptr;
    }

    // Set as selected vendor with server_id = -1 (will be created on final submit)
    VendorEntry new_vendor = {name, -1, false, false};
    wiz.selected_vendor_ = new_vendor;

    // Add to vendor lists and re-sort alphabetically
    wiz.all_vendors_.push_back(new_vendor);
    std::sort(wiz.all_vendors_.begin(), wiz.all_vendors_.end(),
              [](const VendorEntry& a, const VendorEntry& b) {
                  return to_lower(a.name) < to_lower(b.name);
              });
    wiz.filtered_vendors_ = filter_vendor_list(wiz.all_vendors_, wiz.vendor_search_query_);

    // Update display subjects
    if (wiz.subjects_initialized_) {
        std::snprintf(wiz.selected_vendor_name_buf_, sizeof(wiz.selected_vendor_name_buf_), "%s",
                      name.c_str());
        lv_subject_copy_string(&wiz.selected_vendor_name_subject_, wiz.selected_vendor_name_buf_);

        std::snprintf(wiz.summary_vendor_buf_, sizeof(wiz.summary_vendor_buf_), "%s", name.c_str());
        lv_subject_copy_string(&wiz.summary_vendor_subject_, wiz.summary_vendor_buf_);

        lv_subject_set_int(&wiz.vendor_count_subject_,
                           static_cast<int32_t>(wiz.filtered_vendors_.size()));
    }

    // Repopulate the list and select the new vendor
    wiz.populate_vendor_list();

    // Find the new vendor's index in filtered list and highlight it
    for (size_t i = 0; i < wiz.filtered_vendors_.size(); i++) {
        if (to_lower(wiz.filtered_vendors_[i].name) == to_lower(name)) {
            // Set checked state on the matching row
            if (wiz.overlay_root_) {
                lv_obj_t* vendor_list = lv_obj_find_by_name(wiz.overlay_root_, "vendor_list");
                if (vendor_list) {
                    uint32_t count = lv_obj_get_child_count(vendor_list);
                    for (uint32_t j = 0; j < count; j++) {
                        lv_obj_t* row = lv_obj_get_child(vendor_list, static_cast<int32_t>(j));
                        lv_obj_set_state(row, LV_STATE_CHECKED, j == i);
                    }
                    // Scroll to show the selected row
                    lv_obj_t* selected_row = lv_obj_get_child(vendor_list, static_cast<int32_t>(i));
                    if (selected_row) {
                        lv_obj_scroll_to_view(selected_row, LV_ANIM_ON);
                    }
                }
            }
            break;
        }
    }

    wiz.set_can_proceed(true);
    spdlog::info("[SpoolWizard] New vendor '{}' confirmed (will be created on submit)", name);
}

// ============================================================================
// Filament Step Logic
// ============================================================================

std::vector<SpoolWizardOverlay::FilamentEntry>
SpoolWizardOverlay::merge_filaments(const std::vector<FilamentInfo>& server_filaments,
                                    const std::vector<FilamentInfo>& external_filaments) {
    // Build a dedup set keyed by lowercase(material) + "|" + lowercase(color_hex)
    std::unordered_map<std::string, FilamentEntry> by_key;

    auto make_key = [](const std::string& material, const std::string& color_hex) {
        return to_lower(material) + "|" + to_lower(color_hex);
    };

    // Helper to create a FilamentEntry from a FilamentInfo
    auto to_entry = [](const FilamentInfo& fi, bool is_server) -> FilamentEntry {
        FilamentEntry entry;
        entry.name = fi.display_name();
        entry.material = fi.material;
        entry.color_hex = fi.color_hex;
        // FilamentEntry::color_name means a colour word ("Red") — it is what the
        // colour picker fills on the create-new path. FilamentInfo::filament_name
        // is the filament's own name and already reaches the entry through
        // display_name() above, so it must not be copied here.
        entry.server_id = is_server ? fi.id : -1;
        entry.vendor_id = is_server ? fi.vendor_id : -1;
        entry.density = fi.density;
        entry.diameter = fi.diameter;
        entry.weight = fi.weight;
        entry.spool_weight = fi.spool_weight;
        entry.nozzle_temp_min = fi.nozzle_temp_min;
        entry.nozzle_temp_max = fi.nozzle_temp_max;
        entry.bed_temp_min = fi.bed_temp_min;
        entry.bed_temp_max = fi.bed_temp_max;
        entry.from_server = is_server;
        entry.from_database = !is_server;
        return entry;
    };

    // Server filaments first (they have real IDs, so they take priority)
    for (const auto& sf : server_filaments) {
        std::string key = make_key(sf.material, sf.color_hex);
        by_key[key] = to_entry(sf, true);
    }

    // Merge in external DB filaments — fill in extras, mark from_database
    for (const auto& ext : external_filaments) {
        std::string key = make_key(ext.material, ext.color_hex);
        auto it = by_key.find(key);
        if (it != by_key.end()) {
            // Already have this from server — mark as also from external DB
            it->second.from_database = true;
            // Fill in missing temperature data from external if server has none
            if (it->second.nozzle_temp_min == 0 && ext.nozzle_temp_min > 0) {
                it->second.nozzle_temp_min = ext.nozzle_temp_min;
            }
            if (it->second.nozzle_temp_max == 0 && ext.nozzle_temp_max > 0) {
                it->second.nozzle_temp_max = ext.nozzle_temp_max;
            }
            if (it->second.bed_temp_min == 0 && ext.bed_temp_min > 0) {
                it->second.bed_temp_min = ext.bed_temp_min;
            }
            if (it->second.bed_temp_max == 0 && ext.bed_temp_max > 0) {
                it->second.bed_temp_max = ext.bed_temp_max;
            }
            if (it->second.density == 0 && ext.density > 0) {
                it->second.density = ext.density;
            }
            if (it->second.weight == 0 && ext.weight > 0) {
                it->second.weight = ext.weight;
            }
            if (it->second.spool_weight == 0 && ext.spool_weight > 0) {
                it->second.spool_weight = ext.spool_weight;
            }
        } else {
            // External DB-only entry
            by_key[key] = to_entry(ext, false);
        }
    }

    // Collect and sort by material then name
    std::vector<FilamentEntry> result;
    result.reserve(by_key.size());
    for (auto& [_, entry] : by_key) {
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const FilamentEntry& a, const FilamentEntry& b) {
        std::string a_mat = to_lower(a.material);
        std::string b_mat = to_lower(b.material);
        if (a_mat != b_mat)
            return a_mat < b_mat;
        return a.name < b.name;
    });

    return result;
}

void SpoolWizardOverlay::load_filaments() {
    spdlog::debug("[{}] Loading filaments for vendor '{}' (server_id={})", get_name(),
                  selected_vendor_.name, selected_vendor_.server_id);

    // Reset filament state
    all_filaments_.clear();
    selected_filament_ = {};
    creating_new_filament_ = false;
    new_filament_name_.clear();
    new_filament_material_.clear();
    new_filament_color_hex_.clear();
    new_filament_color_name_.clear();
    new_filament_nozzle_min_ = 0;
    new_filament_nozzle_max_ = 0;
    new_filament_bed_min_ = 0;
    new_filament_bed_max_ = 0;
    new_filament_density_ = 0;
    new_filament_weight_ = 0;
    new_filament_spool_weight_ = 0;

    if (subjects_initialized_) {
        lv_subject_set_int(&filament_count_subject_, -1);
        lv_subject_set_int(&show_create_filament_subject_, 0);
        lv_subject_set_int(&filaments_loading_subject_, 1);
    }

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API available, showing empty filaments", get_name());
        if (subjects_initialized_) {
            lv_subject_set_int(&filament_count_subject_, 0);
            lv_subject_set_int(&filaments_loading_subject_, 0);
        }
        populate_filament_list();
        return;
    }

    // DB-only vendor (not yet created on server) — no filaments to fetch.
    // User must use "+ New" to create filaments for this vendor.
    if (selected_vendor_.server_id < 0) {
        spdlog::debug("[{}] DB-only vendor '{}', no server filaments to fetch", get_name(),
                      selected_vendor_.name);
        if (subjects_initialized_) {
            lv_subject_set_int(&filaments_loading_subject_, 0);
            lv_subject_set_int(&filament_count_subject_, 0);
        }
        populate_filament_list();
        return;
    }

    // Fetch filaments from Spoolman server, filtered by vendor.id.
    // NOTE: We intentionally do NOT call the external DB endpoint here —
    // /v1/external/filament has no vendor filtering and returns the entire
    // SpoolmanDB (~thousands of entries), which is too heavy for embedded.
    // Users can create filaments via "+ New" if the server list is empty.
    int vendor_id = selected_vendor_.server_id;
    api->spoolman().get_spoolman_filaments(
        vendor_id,
        lifetime_.bg_cb("SpoolWizard::load_filaments_apply",
                        [this, vendor_id](const std::vector<FilamentInfo>& server_list) {
                            // Convert FilamentInfo -> FilamentEntry
                            for (const auto& fi : server_list) {
                                FilamentEntry entry;
                                entry.name = fi.display_name();
                                entry.material = fi.material;
                                entry.color_hex = fi.color_hex;
                                // See to_entry(): a filament name is not a colour name.
                                entry.server_id = fi.id;
                                entry.vendor_id = fi.vendor_id;
                                entry.density = fi.density;
                                entry.diameter = fi.diameter;
                                entry.weight = fi.weight;
                                entry.spool_weight = fi.spool_weight;
                                entry.nozzle_temp_min = fi.nozzle_temp_min;
                                entry.nozzle_temp_max = fi.nozzle_temp_max;
                                entry.bed_temp_min = fi.bed_temp_min;
                                entry.bed_temp_max = fi.bed_temp_max;
                                entry.from_server = true;
                                all_filaments_.push_back(entry);
                            }

                            // Sort by material then name
                            std::sort(all_filaments_.begin(), all_filaments_.end(),
                                      [](const FilamentEntry& a, const FilamentEntry& b) {
                                          std::string a_mat = to_lower(a.material);
                                          std::string b_mat = to_lower(b.material);
                                          if (a_mat != b_mat)
                                              return a_mat < b_mat;
                                          return to_lower(a.name) < to_lower(b.name);
                                      });

                            if (subjects_initialized_) {
                                lv_subject_set_int(&filaments_loading_subject_, 0);
                                lv_subject_set_int(&filament_count_subject_,
                                                   static_cast<int32_t>(all_filaments_.size()));
                            }

                            populate_filament_list();
                            spdlog::info("[SpoolWizard] Loaded {} filaments for vendor_id {}",
                                         all_filaments_.size(), vendor_id);
                        }),
        lifetime_.bg_cb("SpoolWizard::load_filaments_error", [this](const MoonrakerError& err) {
            spdlog::warn("[SpoolWizard] Failed to fetch filaments: {}", err.message);
            if (cleanup_called())
                return;
            if (subjects_initialized_) {
                lv_subject_set_int(&filaments_loading_subject_, 0);
                lv_subject_set_int(&filament_count_subject_, 0);
            }
            populate_filament_list();
        }));
}

void SpoolWizardOverlay::select_filament(int index) {
    if (index < 0 || index >= static_cast<int>(all_filaments_.size())) {
        spdlog::warn("[{}] Invalid filament index: {}", get_name(), index);
        return;
    }

    selected_filament_ = all_filaments_[static_cast<size_t>(index)];
    creating_new_filament_ = false;

    spdlog::info("[{}] Selected filament: '{}' {} (server_id={})", get_name(),
                 selected_filament_.name, selected_filament_.material,
                 selected_filament_.server_id);

    // Update checked state on filament rows
    if (overlay_root_) {
        lv_obj_t* filament_list = lv_obj_find_by_name(overlay_root_, "filament_list");
        if (filament_list) {
            uint32_t count = lv_obj_get_child_count(filament_list);
            for (uint32_t i = 0; i < count; i++) {
                lv_obj_t* row = lv_obj_get_child(filament_list, static_cast<int32_t>(i));
                lv_obj_set_state(row, LV_STATE_CHECKED, static_cast<int>(i) == index);
            }
        }
    }

    // Update summary subject
    if (subjects_initialized_) {
        std::string summary = selected_filament_.material;
        if (!selected_filament_.name.empty()) {
            summary += " - " + selected_filament_.name;
        }
        std::snprintf(summary_filament_buf_, sizeof(summary_filament_buf_), "%s", summary.c_str());
        lv_subject_copy_string(&summary_filament_subject_, summary_filament_buf_);
    }

    set_can_proceed(true);
}

void SpoolWizardOverlay::set_new_filament_material(const std::string& material) {
    new_filament_material_ = material;

    // Look up material in the static filament database for auto-fill
    auto mat_info = filament::find_material(material);
    if (mat_info.has_value()) {
        new_filament_nozzle_min_ = mat_info->nozzle_min;
        new_filament_nozzle_max_ = mat_info->nozzle_max;
        new_filament_bed_min_ = mat_info->bed_temp;
        new_filament_bed_max_ = mat_info->bed_temp;
        new_filament_density_ = static_cast<double>(mat_info->density_g_cm3);

        spdlog::debug("[{}] Auto-filled temps for {}: nozzle {}-{}, bed {}, density {:.2f}",
                      get_name(), material, new_filament_nozzle_min_, new_filament_nozzle_max_,
                      new_filament_bed_min_, new_filament_density_);

        // Update UI text inputs in the modal dialog
        lv_obj_t* search_root = create_filament_dialog_ ? create_filament_dialog_ : overlay_root_;
        if (search_root) {
            lv_obj_t* nozzle_min = lv_obj_find_by_name(search_root, "nozzle_temp_min");
            lv_obj_t* nozzle_max = lv_obj_find_by_name(search_root, "nozzle_temp_max");
            lv_obj_t* bed_min = lv_obj_find_by_name(search_root, "bed_temp_min");
            lv_obj_t* bed_max = lv_obj_find_by_name(search_root, "bed_temp_max");

            char buf[16];
            if (nozzle_min) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_nozzle_min_);
                lv_textarea_set_text(nozzle_min, buf);
            }
            if (nozzle_max) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_nozzle_max_);
                lv_textarea_set_text(nozzle_max, buf);
            }
            if (bed_min) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_bed_min_);
                lv_textarea_set_text(bed_min, buf);
            }
            if (bed_max) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_bed_max_);
                lv_textarea_set_text(bed_max, buf);
            }
        }
    } else {
        spdlog::debug("[{}] Material '{}' not found in database, no auto-fill", get_name(),
                      material);
    }

    update_new_filament_can_proceed();
}

void SpoolWizardOverlay::set_new_filament_color(const std::string& hex, const std::string& name) {
    new_filament_color_hex_ = hex;
    new_filament_color_name_ = name;

    spdlog::debug("[{}] New filament color: #{} ({})", get_name(), hex, name);

    // Update the color swatch in the modal dialog
    lv_obj_t* search_root = create_filament_dialog_ ? create_filament_dialog_ : overlay_root_;
    if (search_root && !hex.empty()) {
        lv_obj_t* swatch = lv_obj_find_by_name(search_root, "filament_color_swatch");
        if (swatch) {
            uint32_t color_val = std::strtoul(hex.c_str(), nullptr, 16);
            lv_obj_set_style_bg_color(swatch, lv_color_hex(color_val), 0);
        }
    }

    update_new_filament_can_proceed();
}

void SpoolWizardOverlay::populate_filament_list() {
    if (!overlay_root_) {
        spdlog::trace("[{}] populate_filament_list: no overlay_root_, skipping UI", get_name());
        return;
    }

    lv_obj_t* filament_list = lv_obj_find_by_name(overlay_root_, "filament_list");
    if (!filament_list) {
        spdlog::error("[{}] filament_list widget not found", get_name());
        return;
    }

    // Clear existing rows
    helix::ui::safe_clean_children(filament_list);

    for (size_t i = 0; i < all_filaments_.size(); i++) {
        const auto& fil = all_filaments_[i];

        // Create row from XML component
        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(filament_list, "wizard_filament_row", nullptr));
        if (!row) {
            spdlog::error("[{}] Failed to create filament row for '{}'", get_name(), fil.name);
            continue;
        }

        // Store index in user_data for click handling
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        // Set color swatch
        lv_obj_t* swatch = lv_obj_find_by_name(row, "color_swatch");
        if (swatch && !fil.color_hex.empty()) {
            uint32_t color_val = std::strtoul(fil.color_hex.c_str(), nullptr, 16);
            lv_obj_set_style_bg_color(swatch, lv_color_hex(color_val), 0);
        }

        // Set combined material - name label
        lv_obj_t* material_label = lv_obj_find_by_name(row, "filament_material");
        if (material_label) {
            std::string display = fil.material;
            if (!fil.name.empty()) {
                display += " - " + fil.name;
            }
            lv_label_set_text(material_label, display.c_str());
        }

        // Set temps label
        lv_obj_t* temps_label = lv_obj_find_by_name(row, "filament_temps");
        if (temps_label) {
            char temp_buf[32] = {};
            if (fil.nozzle_temp_min > 0 && fil.nozzle_temp_max > 0) {
                std::snprintf(temp_buf, sizeof(temp_buf), "%d-%d\u00B0C", fil.nozzle_temp_min,
                              fil.nozzle_temp_max);
            } else if (fil.nozzle_temp_max > 0) {
                std::snprintf(temp_buf, sizeof(temp_buf), "%d\u00B0C", fil.nozzle_temp_max);
            }
            lv_label_set_text(temps_label, temp_buf);
        }
    }

    spdlog::debug("[{}] Populated {} filament rows", get_name(), all_filaments_.size());
}

void SpoolWizardOverlay::update_new_filament_can_proceed() {
    // Material + color are required for a new filament
    bool valid = !new_filament_material_.empty() && !new_filament_color_hex_.empty();

    if (valid && creating_new_filament_) {
        set_can_proceed(true);
    }

    spdlog::debug("[{}] New filament can_proceed: material='{}' color='{}' valid={}", get_name(),
                  new_filament_material_, new_filament_color_hex_, valid);
}

// ============================================================================
// Filament Step Event Callbacks
// ============================================================================

void SpoolWizardOverlay::on_wizard_filament_selected(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    spdlog::debug("[SpoolWizard] Filament selected, index={}", index);
    get_global_spool_wizard().select_filament(index);
}

void SpoolWizardOverlay::on_wizard_show_create_filament_modal(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Show create filament modal");
    auto& wiz = get_global_spool_wizard();

    // Clear previous filament input state, default material to first in database
    wiz.new_filament_name_.clear();
    wiz.new_filament_material_ = filament::MATERIALS[0].name; // "PLA"
    wiz.new_filament_color_hex_.clear();
    wiz.new_filament_color_name_.clear();
    wiz.new_filament_nozzle_min_ = 0;
    wiz.new_filament_nozzle_max_ = 0;
    wiz.new_filament_bed_min_ = 0;
    wiz.new_filament_bed_max_ = 0;
    wiz.new_filament_density_ = 0;
    wiz.new_filament_weight_ = 0;
    wiz.new_filament_spool_weight_ = 0;
    wiz.creating_new_filament_ = true;

    // Clear previous selection so can_proceed is false until form is confirmed
    wiz.selected_filament_ = {};
    wiz.set_can_proceed(false);

    // Show the modal
    wiz.create_filament_dialog_ = Modal::show("create_filament_modal");

    if (wiz.create_filament_dialog_) {
        // Register keyboards for text inputs
        lv_obj_t* name_input =
            lv_obj_find_by_name(wiz.create_filament_dialog_, "new_filament_name");
        if (name_input) {
            helix::ui::modal_register_keyboard(wiz.create_filament_dialog_, name_input);
        }
        lv_obj_t* nozzle_min = lv_obj_find_by_name(wiz.create_filament_dialog_, "nozzle_temp_min");
        if (nozzle_min) {
            helix::ui::modal_register_keyboard(wiz.create_filament_dialog_, nozzle_min);
        }
        lv_obj_t* nozzle_max = lv_obj_find_by_name(wiz.create_filament_dialog_, "nozzle_temp_max");
        if (nozzle_max) {
            helix::ui::modal_register_keyboard(wiz.create_filament_dialog_, nozzle_max);
        }
        lv_obj_t* bed_min = lv_obj_find_by_name(wiz.create_filament_dialog_, "bed_temp_min");
        if (bed_min) {
            helix::ui::modal_register_keyboard(wiz.create_filament_dialog_, bed_min);
        }
        lv_obj_t* bed_max = lv_obj_find_by_name(wiz.create_filament_dialog_, "bed_temp_max");
        if (bed_max) {
            helix::ui::modal_register_keyboard(wiz.create_filament_dialog_, bed_max);
        }
        lv_obj_t* weight = lv_obj_find_by_name(wiz.create_filament_dialog_, "filament_weight");
        if (weight) {
            helix::ui::modal_register_keyboard(wiz.create_filament_dialog_, weight);
        }
        lv_obj_t* spool_weight =
            lv_obj_find_by_name(wiz.create_filament_dialog_, "filament_spool_weight");
        if (spool_weight) {
            helix::ui::modal_register_keyboard(wiz.create_filament_dialog_, spool_weight);
        }

        // Populate material dropdown from filament database
        lv_obj_t* dropdown = lv_obj_find_by_name(wiz.create_filament_dialog_, "material_dropdown");
        if (dropdown) {
            auto names = filament::get_all_material_names();
            std::string options;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0)
                    options += '\n';
                options += names[i];
            }
            lv_dropdown_set_options(dropdown, options.c_str());

            // Default to first material (PLA) and trigger auto-fill
            lv_dropdown_set_selected(dropdown, 0);
            wiz.set_new_filament_material(names[0]);
        }
    }
}

void SpoolWizardOverlay::on_wizard_cancel_create_filament(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Cancel create filament");
    auto& wiz = get_global_spool_wizard();
    wiz.creating_new_filament_ = false;
    if (wiz.create_filament_dialog_) {
        Modal::hide(wiz.create_filament_dialog_);
        wiz.create_filament_dialog_ = nullptr;
    }
}

void SpoolWizardOverlay::on_wizard_material_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    char buf[64] = {};
    lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
    spdlog::debug("[SpoolWizard] Material changed: '{}'", buf);
    get_global_spool_wizard().set_new_filament_material(buf);
}

void SpoolWizardOverlay::on_wizard_new_filament_name_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    spdlog::debug("[SpoolWizard] New filament name: '{}'", text ? text : "");
    auto& wiz = get_global_spool_wizard();
    wiz.new_filament_name_ = text ? text : "";
}

void SpoolWizardOverlay::on_wizard_pick_filament_color(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Pick filament color");
    auto& wiz = get_global_spool_wizard();

    // Create picker on first use (lazy initialization)
    if (!wiz.color_picker_) {
        wiz.color_picker_ = std::make_unique<helix::ui::ColorPicker>();
    }

    // Set callback for when color is selected (access global, don't capture reference)
    wiz.color_picker_->set_color_callback([](uint32_t color_rgb, const std::string& color_name) {
        char hex_buf[8];
        std::snprintf(hex_buf, sizeof(hex_buf), "%06X", color_rgb);
        get_global_spool_wizard().set_new_filament_color(hex_buf, color_name);
    });

    // Parse current color for initial value
    uint32_t initial_color = 0x808080;
    if (!wiz.new_filament_color_hex_.empty()) {
        initial_color = std::strtoul(wiz.new_filament_color_hex_.c_str(), nullptr, 16);
    }

    // Show color picker on the screen (it creates its own modal)
    lv_obj_t* parent = wiz.create_filament_dialog_
                           ? lv_obj_get_parent(wiz.create_filament_dialog_)
                           : (wiz.overlay_root_ ? lv_obj_get_parent(wiz.overlay_root_) : nullptr);
    if (parent) {
        wiz.color_picker_->show_with_color(parent, initial_color);
    }
}

void SpoolWizardOverlay::on_wizard_nozzle_temp_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    int val = text ? std::atoi(text) : 0;
    auto& wiz = get_global_spool_wizard();

    // Determine if this is min or max based on widget name
    const char* name = lv_obj_get_name(ta);
    if (name && std::string_view(name) == "nozzle_temp_min") {
        wiz.new_filament_nozzle_min_ = val;
    } else {
        wiz.new_filament_nozzle_max_ = val;
    }

    spdlog::debug("[SpoolWizard] Nozzle temp changed: {}-{}", wiz.new_filament_nozzle_min_,
                  wiz.new_filament_nozzle_max_);
}

void SpoolWizardOverlay::on_wizard_bed_temp_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    int val = text ? std::atoi(text) : 0;
    auto& wiz = get_global_spool_wizard();

    const char* name = lv_obj_get_name(ta);
    if (name && std::string_view(name) == "bed_temp_min") {
        wiz.new_filament_bed_min_ = val;
    } else {
        wiz.new_filament_bed_max_ = val;
    }

    spdlog::debug("[SpoolWizard] Bed temp changed: {}-{}", wiz.new_filament_bed_min_,
                  wiz.new_filament_bed_max_);
}

void SpoolWizardOverlay::on_wizard_filament_weight_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    auto& wiz = get_global_spool_wizard();
    wiz.new_filament_weight_ = text ? std::atof(text) : 0;
    spdlog::debug("[SpoolWizard] Filament weight: {:.0f}g", wiz.new_filament_weight_);
}

void SpoolWizardOverlay::on_wizard_spool_weight_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    auto& wiz = get_global_spool_wizard();
    wiz.new_filament_spool_weight_ = text ? std::atof(text) : 0;
    spdlog::debug("[SpoolWizard] Spool weight: {:.0f}g", wiz.new_filament_spool_weight_);
}

void SpoolWizardOverlay::on_wizard_confirm_create_filament(lv_event_t* /*e*/) {
    spdlog::debug("[SpoolWizard] Confirm create filament");
    auto& wiz = get_global_spool_wizard();

    // Helper to set/clear error highlighting on a named label within the modal
    lv_obj_t* dialog = wiz.create_filament_dialog_;
    auto& theme = ThemeManager::instance();
    auto set_field_error = [dialog, &theme](const char* label_name, bool error) {
        if (!dialog)
            return;
        lv_obj_t* label = lv_obj_find_by_name(dialog, label_name);
        if (!label)
            return;
        lv_color_t color = error ? theme.get_color("danger") : theme.get_color("text_muted");
        lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    };

    // Validate required fields
    bool has_error = false;

    bool material_missing = wiz.new_filament_material_.empty();
    set_field_error("material_label", material_missing);
    if (material_missing) {
        has_error = true;
    }

    bool color_missing = wiz.new_filament_color_hex_.empty();
    set_field_error("color_label", color_missing);
    if (color_missing) {
        has_error = true;
    }

    if (has_error) {
        spdlog::warn("[SpoolWizard] Cannot create filament — missing required fields");
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("Please fill in the highlighted fields"));
        return;
    }

    // Check for duplicate (case-insensitive material + name match)
    std::string mat_lower = to_lower(wiz.new_filament_material_);
    std::string name_lower = to_lower(trim(wiz.new_filament_name_));
    for (const auto& f : wiz.all_filaments_) {
        if (to_lower(f.material) == mat_lower && to_lower(f.name) == name_lower) {
            spdlog::warn("[SpoolWizard] Duplicate filament: {} '{}'", wiz.new_filament_material_,
                         wiz.new_filament_name_);
            ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Filament already exists"));
            return;
        }
    }

    // Close the modal first
    if (wiz.create_filament_dialog_) {
        Modal::hide(wiz.create_filament_dialog_);
        wiz.create_filament_dialog_ = nullptr;
    }

    // Build a display summary for the filament
    std::string summary = wiz.new_filament_material_;
    if (!wiz.new_filament_name_.empty()) {
        summary += " - " + wiz.new_filament_name_;
    } else if (!wiz.new_filament_color_name_.empty()) {
        summary += " " + wiz.new_filament_color_name_;
    }

    // Build the new filament entry
    FilamentEntry new_fil;
    new_fil.name = wiz.new_filament_name_;
    new_fil.material = wiz.new_filament_material_;
    new_fil.color_hex = wiz.new_filament_color_hex_;
    new_fil.color_name = wiz.new_filament_color_name_;
    new_fil.server_id = -1;
    new_fil.vendor_id = wiz.selected_vendor_.server_id;
    new_fil.density = wiz.new_filament_density_;
    new_fil.weight = wiz.new_filament_weight_;
    new_fil.spool_weight = wiz.new_filament_spool_weight_;
    new_fil.nozzle_temp_min = wiz.new_filament_nozzle_min_;
    new_fil.nozzle_temp_max = wiz.new_filament_nozzle_max_;
    new_fil.bed_temp_min = wiz.new_filament_bed_min_;
    new_fil.bed_temp_max = wiz.new_filament_bed_max_;

    // Set as selected filament
    wiz.selected_filament_ = new_fil;

    // Add to filament list and re-sort by material then name
    wiz.all_filaments_.push_back(new_fil);
    std::sort(wiz.all_filaments_.begin(), wiz.all_filaments_.end(),
              [](const FilamentEntry& a, const FilamentEntry& b) {
                  std::string a_mat = to_lower(a.material);
                  std::string b_mat = to_lower(b.material);
                  if (a_mat != b_mat)
                      return a_mat < b_mat;
                  return a.name < b.name;
              });

    // Update filament count subject
    if (wiz.subjects_initialized_) {
        lv_subject_set_int(&wiz.filament_count_subject_,
                           static_cast<int32_t>(wiz.all_filaments_.size()));

        // Update summary display
        std::snprintf(wiz.summary_filament_buf_, sizeof(wiz.summary_filament_buf_), "%s",
                      summary.c_str());
        lv_subject_copy_string(&wiz.summary_filament_subject_, wiz.summary_filament_buf_);
    }

    // Repopulate the list and highlight the new entry
    wiz.populate_filament_list();

    // Find the new filament's index and set checked state
    for (size_t i = 0; i < wiz.all_filaments_.size(); i++) {
        if (to_lower(wiz.all_filaments_[i].material) == mat_lower &&
            to_lower(wiz.all_filaments_[i].name) == name_lower) {
            if (wiz.overlay_root_) {
                lv_obj_t* filament_list = lv_obj_find_by_name(wiz.overlay_root_, "filament_list");
                if (filament_list) {
                    uint32_t count = lv_obj_get_child_count(filament_list);
                    for (uint32_t j = 0; j < count; j++) {
                        lv_obj_t* row = lv_obj_get_child(filament_list, static_cast<int32_t>(j));
                        lv_obj_set_state(row, LV_STATE_CHECKED, j == i);
                    }
                    // Scroll to show the selected row
                    lv_obj_t* selected_row =
                        lv_obj_get_child(filament_list, static_cast<int32_t>(i));
                    if (selected_row) {
                        lv_obj_scroll_to_view(selected_row, LV_ANIM_ON);
                    }
                }
            }
            break;
        }
    }

    wiz.creating_new_filament_ = false;
    wiz.set_can_proceed(true);
    spdlog::info("[SpoolWizard] New filament '{}' confirmed (will be created on submit)", summary);
}

// ============================================================================
// Spool Details Event Callbacks
// ============================================================================

void SpoolWizardOverlay::on_wizard_remaining_weight_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    auto& wiz = get_global_spool_wizard();
    wiz.spool_remaining_weight_ = text ? std::atof(text) : 0;
    wiz.set_can_proceed(wiz.spool_remaining_weight_ > 0);
    spdlog::debug("[SpoolWizard] Remaining weight: {:.0f}g", wiz.spool_remaining_weight_);
}

void SpoolWizardOverlay::on_wizard_price_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    auto& wiz = get_global_spool_wizard();
    wiz.spool_price_ = text ? std::atof(text) : 0;
    spdlog::debug("[SpoolWizard] Price: {:.2f}", wiz.spool_price_);
}

void SpoolWizardOverlay::on_wizard_lot_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    auto& wiz = get_global_spool_wizard();
    wiz.spool_lot_nr_ = text ? text : "";
    spdlog::debug("[SpoolWizard] Lot: '{}'", wiz.spool_lot_nr_);
}

void SpoolWizardOverlay::on_wizard_notes_changed(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    auto& wiz = get_global_spool_wizard();
    wiz.spool_notes_ = text ? text : "";
    spdlog::debug("[SpoolWizard] Notes: '{}'", wiz.spool_notes_);
}
