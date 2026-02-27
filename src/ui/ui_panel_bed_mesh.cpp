// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_panel_bed_mesh.cpp
 * @brief Bed mesh visualization panel with 3D preview and profile management
 *
 * @pattern GOLD STANDARD - declarative XML + subject bindings, no imperative widget manipulation
 * @threading Destruction flag guards async callbacks
 *
 * @see Referenced in CLAUDE.md as exemplar
 */

#include "ui_panel_bed_mesh.h"

#include "ui_bed_mesh.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_global_panel_helper.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

using namespace helix;

#include <cstdio>
#include <cstring>
#include <limits>

// ============================================================================
// Forward declarations for static event callbacks
// ============================================================================
static void on_profile_clicked_cb(lv_event_t* e);
static void on_profile_rename_cb(lv_event_t* e);
static void on_profile_delete_cb(lv_event_t* e);
static void on_calibrate_header_clicked_cb(lv_event_t* e);
static void on_calibrate_cancel_cb(lv_event_t* e);
static void on_calibrate_start_cb(lv_event_t* e);
static void on_rename_cancel_cb(lv_event_t* e);
static void on_rename_confirm_cb(lv_event_t* e);
static void on_delete_cancel_cb(lv_event_t* e);
static void on_delete_confirm_cb(lv_event_t* e);
static void on_save_config_no_cb(lv_event_t* e);
static void on_save_config_yes_cb(lv_event_t* e);
static void on_emergency_stop_cb(lv_event_t* e);
static void on_save_profile_cb(lv_event_t* e);

// ============================================================================
// Constructor / Destructor
// ============================================================================

BedMeshPanel::BedMeshPanel() {
    // Initialize buffer contents
    std::memset(profile_name_buf_, 0, sizeof(profile_name_buf_));
    std::strncpy(dimensions_buf_, "No mesh data", sizeof(dimensions_buf_) - 1);
    std::memset(max_label_buf_, 0, sizeof(max_label_buf_));
    std::memset(max_value_buf_, 0, sizeof(max_value_buf_));
    std::memset(min_label_buf_, 0, sizeof(min_label_buf_));
    std::memset(min_value_buf_, 0, sizeof(min_value_buf_));
    std::memset(variance_buf_, 0, sizeof(variance_buf_));
    std::memset(rename_old_name_buf_, 0, sizeof(rename_old_name_buf_));
    std::memset(probe_text_buf_, 0, sizeof(probe_text_buf_));
    std::memset(error_message_buf_, 0, sizeof(error_message_buf_));

    // Initialize profile buffers
    for (int i = 0; i < BED_MESH_MAX_PROFILES; i++) {
        std::memset(profile_name_bufs_[static_cast<size_t>(i)].data(), 0, 64);
        std::memset(profile_range_bufs_[static_cast<size_t>(i)].data(), 0, 32);
    }

    spdlog::trace("[BedMeshPanel] Instance created");
}

BedMeshPanel::~BedMeshPanel() {
    deinit_subjects();

    // Signal to async callbacks that this panel is being destroyed [L012]
    // Must happen BEFORE any cleanup that callbacks might reference
    alive_->store(false);

    // During shutdown, MoonrakerClient may already be destroyed - release subscription
    // guard WITHOUT trying to unsubscribe (matches pattern in ams_backend_*.cpp)
    subscription_.release();

    // CRITICAL: Check if LVGL is still initialized before calling LVGL functions.
    // During static destruction, LVGL may already be torn down.
    if (lv_is_initialized()) {
        // Modal dialogs: use helix::ui::modal_hide() - NOT lv_obj_del()!
        // See docs/DEVELOPER_QUICK_REFERENCE.md "Modal Dialog Lifecycle"
        if (calibrate_modal_widget_) {
            helix::ui::modal_hide(calibrate_modal_widget_);
            calibrate_modal_widget_ = nullptr;
        }
        if (rename_modal_widget_) {
            helix::ui::modal_hide(rename_modal_widget_);
            rename_modal_widget_ = nullptr;
        }
        if (save_config_modal_widget_) {
            helix::ui::modal_hide(save_config_modal_widget_);
            save_config_modal_widget_ = nullptr;
        }
        if (delete_modal_widget_) {
            helix::ui::modal_hide(delete_modal_widget_);
            delete_modal_widget_ = nullptr;
        }
    }

    // Clear widget pointers (LVGL owns the objects)
    canvas_ = nullptr;
    calibrate_name_input_ = nullptr;
    rename_name_input_ = nullptr;
}

// ============================================================================
// Subject Initialization
// ============================================================================

void BedMeshPanel::init_subjects() {
    init_subjects_guarded([this]() {
        // Current mesh stats subjects
        UI_MANAGED_SUBJECT_INT(bed_mesh_available_, 0, "bed_mesh_available", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_profile_name_, profile_name_buf_, "",
                                  "bed_mesh_profile_name", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_dimensions_, dimensions_buf_, "No mesh data",
                                  "bed_mesh_dimensions", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_max_label_, max_label_buf_, "Max", "bed_mesh_max_label",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_max_value_, max_value_buf_, "--", "bed_mesh_max_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_min_label_, min_label_buf_, "Min", "bed_mesh_min_label",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_min_value_, min_value_buf_, "--", "bed_mesh_min_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_variance_, variance_buf_, "", "bed_mesh_variance",
                                  subjects_);

        // Profile count
        UI_MANAGED_SUBJECT_INT(bed_mesh_profile_count_, 0, "bed_mesh_profile_count", subjects_);

        // Profile list subjects (5 profiles)
        for (int i = 0; i < BED_MESH_MAX_PROFILES; i++) {
            std::string name_key = "bed_mesh_profile_" + std::to_string(i) + "_name";
            std::string range_key = "bed_mesh_profile_" + std::to_string(i) + "_range";
            std::string active_key = "bed_mesh_profile_" + std::to_string(i) + "_active";

            auto idx = static_cast<size_t>(i);
            char* name_buf = profile_name_bufs_[idx].data();
            char* range_buf = profile_range_bufs_[idx].data();

            // Initialize with 5-arg form: (subject, buf, prev_buf, size, initial_value)
            lv_subject_init_string(&profile_name_subjects_[idx], name_buf, nullptr,
                                   profile_name_bufs_[idx].size(), "");
            lv_xml_register_subject(nullptr, name_key.c_str(), &profile_name_subjects_[idx]);
            subjects_.register_subject(&profile_name_subjects_[idx]);

            lv_subject_init_string(&profile_range_subjects_[idx], range_buf, nullptr,
                                   profile_range_bufs_[idx].size(), "");
            lv_xml_register_subject(nullptr, range_key.c_str(), &profile_range_subjects_[idx]);
            subjects_.register_subject(&profile_range_subjects_[idx]);

            lv_subject_init_int(&profile_active_subjects_[idx], 0);
            lv_xml_register_subject(nullptr, active_key.c_str(), &profile_active_subjects_[idx]);
            subjects_.register_subject(&profile_active_subjects_[idx]);
        }

        // Modal state subjects (NOT visibility - internal state only)
        UI_MANAGED_SUBJECT_INT(bed_mesh_calibrating_, 0, "bed_mesh_calibrating", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_rename_old_name_, rename_old_name_buf_, "",
                                  "bed_mesh_rename_old_name", subjects_);
        // Note: All modals now use helix::ui::modal_show() pattern instead of visibility subjects

        // Calibration state machine subjects
        UI_MANAGED_SUBJECT_INT(bed_mesh_calibrate_state_, 0, "bed_mesh_calibrate_state", subjects_);
        UI_MANAGED_SUBJECT_INT(bed_mesh_probe_progress_, 0, "bed_mesh_probe_progress", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_probe_text_, probe_text_buf_, "Preparing...",
                                  "bed_mesh_probe_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_error_message_, error_message_buf_, "",
                                  "bed_mesh_error_message", subjects_);

        // Self-register cleanup — ensures deinit runs before lv_deinit()
        StaticPanelRegistry::instance().register_destroy(
            "BedMeshPanelSubjects", []() { get_global_bed_mesh_panel().deinit_subjects(); });

        spdlog::debug("[{}] Subjects registered", get_name());
    });
}

void BedMeshPanel::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* BedMeshPanel::create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Creating overlay from XML", get_name());

    parent_screen_ = parent;

    // Reset cleanup flag when (re)creating
    cleanup_called_ = false;

    // Create overlay from XML
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "bed_mesh_panel", nullptr));

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    // Use standard overlay panel setup
    // Note: Back button is wired via header_bar.xml default callback (on_header_back_clicked)
    ui_overlay_panel_setup_standard(overlay_root_, parent_screen_, "overlay_header",
                                    "overlay_content");

    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] overlay_content not found!", get_name());
        return overlay_root_;
    }

    // Find canvas widget
    canvas_ = lv_obj_find_by_name(overlay_content, "bed_mesh_canvas");
    if (!canvas_) {
        spdlog::error("[{}] Canvas widget 'bed_mesh_canvas' not found in XML", get_name());
        return overlay_root_;
    }
    spdlog::debug("[{}] Found canvas widget - rotation controlled by touch drag", get_name());

    // Setup Moonraker subscription for mesh updates
    setup_moonraker_subscription();

    // Setup observer for build_volume changes (to refresh bounds when stepper config loads)
    setup_build_volume_observer();

    // Load initial mesh data from MoonrakerAPI
    MoonrakerAPI* api = get_moonraker_api();
    if (api && api->advanced().has_bed_mesh()) {
        const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
        if (mesh) {
            spdlog::info("[{}] Active mesh: profile='{}', size={}x{}", get_name(), mesh->name,
                         mesh->x_count, mesh->y_count);
            on_mesh_update_internal(*mesh);
        }
    } else {
        spdlog::info("[{}] No mesh data available from Moonraker", get_name());
    }

    // Always update profile list — saved profiles exist even without an active mesh
    if (api) {
        update_profile_list_subjects();
    }

    // Apply saved render mode preference from settings
    int saved_mode = DisplaySettingsManager::instance().get_bed_mesh_render_mode();
    auto render_mode = static_cast<BedMeshRenderMode>(saved_mode);
    ui_bed_mesh_set_render_mode(canvas_, render_mode);
    spdlog::debug("[{}] Render mode set from settings: {} ({})", get_name(), saved_mode,
                  saved_mode == 0 ? "Auto" : (saved_mode == 1 ? "3D" : "2D"));

    // Apply zero plane visibility from settings
    bool show_zero_plane = DisplaySettingsManager::instance().get_bed_mesh_show_zero_plane();
    ui_bed_mesh_set_zero_plane_visible(canvas_, show_zero_plane);
    spdlog::debug("[{}] Zero plane visibility set from settings: {}", get_name(), show_zero_plane);

    // Evaluate render mode based on FPS history from previous sessions
    // This decides whether to use 3D or 2D fallback mode for AUTO mode
    ui_bed_mesh_evaluate_render_mode(canvas_);

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Callback Registration
// ============================================================================

void BedMeshPanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    register_xml_callbacks({
        // Header calibrate button
        {"on_bed_mesh_calibrate_clicked", on_calibrate_header_clicked_cb},

        // Profile row callbacks (5 profiles)
        {"on_profile_0_clicked", on_profile_clicked_cb},
        {"on_profile_1_clicked", on_profile_clicked_cb},
        {"on_profile_2_clicked", on_profile_clicked_cb},
        {"on_profile_3_clicked", on_profile_clicked_cb},
        {"on_profile_4_clicked", on_profile_clicked_cb},

        {"on_profile_0_rename", on_profile_rename_cb},
        {"on_profile_1_rename", on_profile_rename_cb},
        {"on_profile_2_rename", on_profile_rename_cb},
        {"on_profile_3_rename", on_profile_rename_cb},
        {"on_profile_4_rename", on_profile_rename_cb},

        {"on_profile_0_delete", on_profile_delete_cb},
        {"on_profile_1_delete", on_profile_delete_cb},
        {"on_profile_2_delete", on_profile_delete_cb},
        {"on_profile_3_delete", on_profile_delete_cb},
        {"on_profile_4_delete", on_profile_delete_cb},

        // Calibrate modal
        {"on_bed_mesh_calibrate_cancel", on_calibrate_cancel_cb},
        {"on_bed_mesh_calibrate_start", on_calibrate_start_cb},

        // Rename modal
        {"on_bed_mesh_rename_cancel", on_rename_cancel_cb},
        {"on_bed_mesh_rename_confirm", on_rename_confirm_cb},

        // Delete modal
        {"on_bed_mesh_delete_cancel", on_delete_cancel_cb},
        {"on_bed_mesh_delete_confirm", on_delete_confirm_cb},

        // Save config modal
        {"on_bed_mesh_save_config_no", on_save_config_no_cb},
        {"on_bed_mesh_save_config_yes", on_save_config_yes_cb},

        // Calibration modal - emergency stop and save profile
        {"on_bed_mesh_emergency_stop", on_emergency_stop_cb},
        {"on_bed_mesh_save_profile", on_save_profile_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void BedMeshPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Refresh mesh data when panel becomes visible
    MoonrakerAPI* api = get_moonraker_api();
    if (api && api->advanced().has_bed_mesh()) {
        const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
        if (mesh) {
            on_mesh_update_internal(*mesh);
        }
    }

    // Always refresh profile list — saved profiles exist even without an active mesh
    if (api) {
        update_profile_list_subjects();
    }
}

void BedMeshPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Profile List Update
// ============================================================================

void BedMeshPanel::update_profile_list_subjects() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        lv_subject_set_int(&bed_mesh_profile_count_, 0);
        return;
    }

    const auto profiles = api->advanced().get_bed_mesh_profiles();
    const BedMeshProfile* active_mesh = api->advanced().get_active_bed_mesh();
    std::string active_name = active_mesh ? active_mesh->name : "";

    spdlog::debug("[{}] update_profile_list_subjects: {} profiles, active='{}'", get_name(),
                  profiles.size(), active_name);

    int count = std::min(static_cast<int>(profiles.size()), BED_MESH_MAX_PROFILES);
    lv_subject_set_int(&bed_mesh_profile_count_, count);

    for (int i = 0; i < BED_MESH_MAX_PROFILES; i++) {
        size_t idx = static_cast<size_t>(i);
        if (i < count) {
            profile_names_[idx] = profiles[idx];

            // Set name
            lv_subject_copy_string(&profile_name_subjects_[idx], profiles[idx].c_str());

            // Calculate and set range (mm without suffix for profile lists)
            float range = calculate_profile_range(profiles[idx]);
            std::snprintf(profile_range_bufs_[idx].data(), 32, "%.3f", range);
            lv_subject_copy_string(&profile_range_subjects_[idx], profile_range_bufs_[idx].data());

            // Set active state
            int is_active = (profiles[idx] == active_name) ? 1 : 0;
            lv_subject_set_int(&profile_active_subjects_[idx], is_active);
        } else {
            profile_names_[idx].clear();
            lv_subject_copy_string(&profile_name_subjects_[idx], "");
            lv_subject_copy_string(&profile_range_subjects_[idx], "");
            lv_subject_set_int(&profile_active_subjects_[idx], 0);
        }
    }

    spdlog::debug("[{}] Profile list updated: {} profiles, active='{}'", get_name(), count,
                  active_name);
}

float BedMeshPanel::calculate_profile_range(const std::string& profile_name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return 0.0f;

    // Get mesh data for this profile (supports both active and stored profiles)
    const BedMeshProfile* mesh = api->advanced().get_bed_mesh_profile(profile_name);
    if (!mesh || mesh->probed_matrix.empty()) {
        return 0.0f;
    }

    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();

    for (const auto& row : mesh->probed_matrix) {
        for (float z : row) {
            min_z = std::min(min_z, z);
            max_z = std::max(max_z, z);
        }
    }

    return max_z - min_z;
}

// ============================================================================
// Mesh Data Update
// ============================================================================

void BedMeshPanel::set_mesh_data(const std::vector<std::vector<float>>& mesh_data) {
    if (!canvas_) {
        spdlog::error("[{}] Cannot set mesh data - canvas not initialized", get_name());
        return;
    }

    if (mesh_data.empty() || mesh_data[0].empty()) {
        spdlog::error("[{}] Invalid mesh data - empty rows or columns", get_name());
        return;
    }

    int rows = static_cast<int>(mesh_data.size());
    int cols = static_cast<int>(mesh_data[0].size());

    std::vector<const float*> row_pointers(static_cast<size_t>(rows));
    for (int i = 0; i < rows; i++) {
        row_pointers[static_cast<size_t>(i)] = mesh_data[static_cast<size_t>(i)].data();
    }

    if (!ui_bed_mesh_set_data(canvas_, row_pointers.data(), rows, cols)) {
        spdlog::error("[{}] Failed to set mesh data in widget", get_name());
        return;
    }

    update_info_subjects(mesh_data, cols, rows);
}

void BedMeshPanel::redraw() {
    if (!canvas_) {
        spdlog::warn("[{}] Cannot redraw - canvas not initialized", get_name());
        return;
    }
    ui_bed_mesh_redraw(canvas_);
}

void BedMeshPanel::setup_moonraker_subscription() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] Cannot subscribe to Moonraker - API is null", get_name());
        return;
    }

    auto alive = alive_; // Capture shared_ptr by value for destruction detection [L012]

    SubscriptionId id =
        api->subscribe_notifications([this, api, alive](nlohmann::json notification) {
            // Check destruction flag FIRST - panel may have been deleted
            if (!alive->load()) {
                return;
            }

            // Check if this notification contains bed_mesh data BEFORE deferring to main thread
            // This avoids unnecessary context switches for unrelated notifications
            if (!notification.contains("params") || !notification["params"].is_array() ||
                notification["params"].empty()) {
                return;
            }
            const nlohmann::json& params = notification["params"][0];
            if (!params.contains("bed_mesh") || !params["bed_mesh"].is_object()) {
                return;
            }

            // CRITICAL: Defer LVGL modifications to main thread via ui_queue_update [L012]
            // WebSocket callbacks run on libhv thread - direct lv_subject_* calls cause crashes
            struct Ctx {
                BedMeshPanel* panel;
                MoonrakerAPI* api;
                std::shared_ptr<std::atomic<bool>> alive;
            };
            auto ctx = std::make_unique<Ctx>(Ctx{this, api, alive});
            helix::ui::queue_update<Ctx>(std::move(ctx), [](Ctx* c) {
                // Check again on main thread - panel could be destroyed between queue and exec
                if (!c->alive->load()) {
                    return;
                }
                const BedMeshProfile* mesh = c->api->advanced().get_active_bed_mesh();
                if (mesh) {
                    c->panel->on_mesh_update_internal(*mesh);
                }
                c->panel->update_profile_list_subjects();
            });
        });

    // Store in RAII guard for automatic cleanup on destruction
    subscription_ = SubscriptionGuard(api, id);
    spdlog::debug("[{}] Registered Moonraker callback for mesh updates", get_name());
}

void BedMeshPanel::setup_build_volume_observer() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] Cannot observe build_volume - API is null", get_name());
        return;
    }

    // Observe build_volume_version subject to refresh bounds when stepper config loads
    build_volume_observer_ = helix::ui::observe_int_sync<BedMeshPanel>(
        api->get_build_volume_version_subject(), this, [](BedMeshPanel* self, int /*version*/) {
            spdlog::debug("[{}] build_volume changed, refreshing bed bounds", self->get_name());
            self->refresh_bed_bounds();
        });
}

void BedMeshPanel::refresh_bed_bounds() {
    if (!canvas_ || !has_cached_mesh_bounds_) {
        return;
    }

    MoonrakerAPI* api = get_moonraker_api();
    const auto& bed = api ? api->hardware().build_volume() : BuildVolume{};
    double bed_x_min = bed.x_min;
    double bed_x_max = bed.x_max;
    double bed_y_min = bed.y_min;
    double bed_y_max = bed.y_max;

    // Wait for valid build_volume - do NOT use fallback to avoid flash
    if (bed_x_max <= bed_x_min || bed_y_max <= bed_y_min) {
        spdlog::debug("[{}] Deferring render until build_volume is available", get_name());
        return;
    }

    spdlog::debug("[{}] Using build_volume for bed bounds: X[{:.0f},{:.0f}] Y[{:.0f},{:.0f}]",
                  get_name(), bed_x_min, bed_x_max, bed_y_min, bed_y_max);

    ui_bed_mesh_set_bounds(canvas_, bed_x_min, bed_x_max, bed_y_min, bed_y_max, cached_mesh_min_x_,
                           cached_mesh_max_x_, cached_mesh_min_y_, cached_mesh_max_y_);

    // If we have pending mesh data, render it now that bounds are valid
    if (has_pending_mesh_data_) {
        spdlog::debug("[{}] Rendering deferred mesh data", get_name());
        set_mesh_data(pending_mesh_data_);
        pending_mesh_data_.clear();
        has_pending_mesh_data_ = false;
    }
}

void BedMeshPanel::on_mesh_update_internal(const BedMeshProfile& mesh) {
    spdlog::debug("[{}] on_mesh_update_internal called, probed_matrix.size={}", get_name(),
                  mesh.probed_matrix.size());

    if (mesh.probed_matrix.empty()) {
        lv_subject_set_int(&bed_mesh_available_, 0);
        lv_subject_copy_string(&bed_mesh_dimensions_, lv_tr("No mesh data"));
        lv_subject_copy_string(&bed_mesh_max_label_, lv_tr("Max"));
        lv_subject_copy_string(&bed_mesh_max_value_, "--");
        lv_subject_copy_string(&bed_mesh_min_label_, lv_tr("Min"));
        lv_subject_copy_string(&bed_mesh_min_value_, "--");
        lv_subject_copy_string(&bed_mesh_variance_, "");
        spdlog::warn("[{}] No mesh data available", get_name());
        return;
    }

    lv_subject_set_int(&bed_mesh_available_, 1);
    lv_subject_copy_string(&bed_mesh_profile_name_, mesh.name.c_str());

    std::snprintf(dimensions_buf_, sizeof(dimensions_buf_), "%dx%d", mesh.x_count, mesh.y_count);
    lv_subject_copy_string(&bed_mesh_dimensions_, dimensions_buf_);

    // Calculate Z range, mean, and coordinates of min/max points
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    double z_sum = 0.0;
    int z_count = 0;
    int min_row = 0, min_col = 0;
    int max_row = 0, max_col = 0;

    for (size_t row = 0; row < mesh.probed_matrix.size(); row++) {
        for (size_t col = 0; col < mesh.probed_matrix[row].size(); col++) {
            float z = mesh.probed_matrix[row][col];
            z_sum += z;
            z_count++;
            if (z < min_z) {
                min_z = z;
                min_row = static_cast<int>(row);
                min_col = static_cast<int>(col);
            }
            if (z > max_z) {
                max_z = z;
                max_row = static_cast<int>(row);
                max_col = static_cast<int>(col);
            }
        }
    }

    // Normalize mesh data: subtract mean so deviations are centered around zero.
    // This makes the 3D visualization show bed flatness (what users care about)
    // rather than absolute probe heights (which depend on Z calibration).
    float z_mean = (z_count > 0) ? static_cast<float>(z_sum / z_count) : 0.0f;
    float norm_min_z = min_z - z_mean;
    float norm_max_z = max_z - z_mean;

    std::vector<std::vector<float>> normalized_matrix;
    normalized_matrix.reserve(mesh.probed_matrix.size());
    for (const auto& row : mesh.probed_matrix) {
        std::vector<float> norm_row;
        norm_row.reserve(row.size());
        for (float z : row) {
            norm_row.push_back(z - z_mean);
        }
        normalized_matrix.push_back(std::move(norm_row));
    }

    spdlog::debug(
        "[{}] Normalized mesh: mean={:.4f}, raw range [{:.3f}, {:.3f}] -> [{:.3f}, {:.3f}]",
        get_name(), z_mean, min_z, max_z, norm_min_z, norm_max_z);

    // Convert mesh indices to actual printer coordinates using mesh_min/mesh_max
    // Klipper's probed_matrix: row 0 = mesh_min[1], row N-1 = mesh_max[1]
    float x_step =
        (mesh.x_count > 1) ? (mesh.mesh_max[0] - mesh.mesh_min[0]) / (mesh.x_count - 1) : 0.0f;
    float y_step =
        (mesh.y_count > 1) ? (mesh.mesh_max[1] - mesh.mesh_min[1]) / (mesh.y_count - 1) : 0.0f;
    float min_x = mesh.mesh_min[0] + min_col * x_step;
    float min_y = mesh.mesh_min[1] + min_row * y_step;
    float max_x = mesh.mesh_min[0] + max_col * x_step;
    float max_y = mesh.mesh_min[1] + max_row * y_step;

    // Display raw Z values in stats (what Klipper actually measured)
    std::snprintf(max_label_buf_, sizeof(max_label_buf_), "Max [%.1f, %.1f]", max_x, max_y);
    lv_subject_copy_string(&bed_mesh_max_label_, max_label_buf_);
    helix::format::format_distance_mm(max_z, 3, max_value_buf_, sizeof(max_value_buf_));
    lv_subject_copy_string(&bed_mesh_max_value_, max_value_buf_);

    std::snprintf(min_label_buf_, sizeof(min_label_buf_), "Min [%.1f, %.1f]", min_x, min_y);
    lv_subject_copy_string(&bed_mesh_min_label_, min_label_buf_);
    helix::format::format_distance_mm(min_z, 3, min_value_buf_, sizeof(min_value_buf_));
    lv_subject_copy_string(&bed_mesh_min_value_, min_value_buf_);

    // Variance (range) is the same whether normalized or not
    float variance = max_z - min_z;
    helix::format::format_distance_mm(variance, 3, variance_buf_, sizeof(variance_buf_));
    lv_subject_copy_string(&bed_mesh_variance_, variance_buf_);

    // Cache mesh bounds
    if ((mesh.mesh_max[0] > mesh.mesh_min[0]) && (mesh.mesh_max[1] > mesh.mesh_min[1])) {
        cached_mesh_min_x_ = mesh.mesh_min[0];
        cached_mesh_max_x_ = mesh.mesh_max[0];
        cached_mesh_min_y_ = mesh.mesh_min[1];
        cached_mesh_max_y_ = mesh.mesh_max[1];
        has_cached_mesh_bounds_ = true;
    }

    // Tell the renderer to add back the mean when displaying Z values
    // so axis labels and tooltips show original probe heights
    if (canvas_) {
        ui_bed_mesh_set_z_display_offset(canvas_, static_cast<double>(z_mean));
    }

    // Check if build_volume is available
    MoonrakerAPI* api = get_moonraker_api();
    const auto& bed = api ? api->hardware().build_volume() : BuildVolume{};
    bool has_valid_build_volume = (bed.x_max > bed.x_min && bed.y_max > bed.y_min);

    spdlog::debug("[{}] BuildVolume check: x=[{:.0f},{:.0f}] y=[{:.0f},{:.0f}] valid={}, "
                  "mesh_bounds_cached={}",
                  get_name(), bed.x_min, bed.x_max, bed.y_min, bed.y_max, has_valid_build_volume,
                  has_cached_mesh_bounds_);

    if (has_valid_build_volume) {
        // Build volume available - set bounds and render immediately
        refresh_bed_bounds();
        set_mesh_data(normalized_matrix);
    } else {
        // Build volume not yet available - defer rendering until it arrives
        pending_mesh_data_ = std::move(normalized_matrix);
        has_pending_mesh_data_ = true;
        spdlog::debug("[{}] Deferring mesh render until build_volume is available", get_name());
    }

    spdlog::info("[{}] Mesh updated: {} ({}x{}, raw Z: [{:.3f}, {:.3f}], normalized: [{:.3f}, "
                 "{:.3f}])",
                 get_name(), mesh.name, mesh.x_count, mesh.y_count, min_z, max_z, norm_min_z,
                 norm_max_z);
}

void BedMeshPanel::update_info_subjects(const std::vector<std::vector<float>>& mesh_data, int cols,
                                        int rows) {
    std::snprintf(dimensions_buf_, sizeof(dimensions_buf_), "%dx%d points", cols, rows);
    lv_subject_copy_string(&bed_mesh_dimensions_, dimensions_buf_);

    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    for (const auto& row : mesh_data) {
        for (float val : row) {
            min_z = std::min(min_z, val);
            max_z = std::max(max_z, val);
        }
    }

    float variance = max_z - min_z;
    std::snprintf(variance_buf_, sizeof(variance_buf_), "%.3f mm", variance);
    lv_subject_copy_string(&bed_mesh_variance_, variance_buf_);
}

// ============================================================================
// Profile Operations
// ============================================================================

void BedMeshPanel::load_profile(int index) {
    if (index < 0 || index >= BED_MESH_MAX_PROFILES)
        return;
    if (profile_names_[static_cast<size_t>(index)].empty())
        return;
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING("Operation already in progress");
        return;
    }

    const std::string& name = profile_names_[static_cast<size_t>(index)];
    spdlog::info("[{}] Loading profile: {}", get_name(), name);

    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        operation_guard_.begin(SLOW_OPERATION_TIMEOUT_MS, [this] {
            hide_all_modals();
            pending_operation_ = PendingOperation::None;
            NOTIFY_WARNING("Bed mesh operation timed out");
        });
        std::string cmd = "BED_MESH_PROFILE LOAD=" + name;
        api->execute_gcode(
            cmd,
            [this, name]() {
                operation_guard_.end();
                spdlog::debug("[{}] Profile loaded: {}", get_name(), name);
            },
            [this](const MoonrakerError& err) {
                operation_guard_.end();
                spdlog::error("[{}] Failed to load profile: {}", get_name(), err.message);
                NOTIFY_ERROR("Failed to load profile");
            });
    }
}

void BedMeshPanel::delete_profile(int index) {
    if (index < 0 || index >= BED_MESH_MAX_PROFILES)
        return;
    if (profile_names_[static_cast<size_t>(index)].empty())
        return;

    const std::string& name = profile_names_[static_cast<size_t>(index)];
    show_delete_confirm_modal(name);
}

void BedMeshPanel::rename_profile(int index) {
    if (index < 0 || index >= BED_MESH_MAX_PROFILES)
        return;
    if (profile_names_[static_cast<size_t>(index)].empty())
        return;

    const std::string& name = profile_names_[static_cast<size_t>(index)];
    show_rename_modal(name);
}

void BedMeshPanel::start_calibration() {
    // Reset state to PROBING
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::PROBING));
    lv_subject_set_int(&bed_mesh_probe_progress_, 0);
    lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Preparing..."));

    // Show modal immediately
    calibrate_modal_widget_ = helix::ui::modal_show("bed_mesh_calibrate_modal");
    spdlog::debug("[BedMeshPanel] Starting calibration, modal shown");

    // Get API
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_calibration_error("API not available");
        return;
    }

    // Capture alive flag for callback safety
    auto alive = alive_;

    // Start calibration with progress tracking
    api->advanced().start_bed_mesh_calibrate(
        // Progress callback (from WebSocket thread)
        [this, alive](int current, int total) {
            if (!alive->load())
                return;
            // Must use ui_queue_update for thread safety [L012]
            struct ProgressCtx {
                BedMeshPanel* panel;
                int current;
                int total;
            };
            auto ctx = std::make_unique<ProgressCtx>(ProgressCtx{this, current, total});
            helix::ui::queue_update<ProgressCtx>(std::move(ctx), [](ProgressCtx* c) {
                c->panel->on_probe_progress(c->current, c->total);
            });
        },
        // Complete callback (from WebSocket thread)
        [this, alive]() {
            if (!alive->load())
                return;
            helix::ui::async_call(
                [](void* data) { static_cast<BedMeshPanel*>(data)->on_calibration_complete(); },
                this);
        },
        // Error callback (from WebSocket thread)
        [this, alive](const MoonrakerError& err) {
            if (!alive->load())
                return;
            struct ErrorCtx {
                BedMeshPanel* panel;
                std::string message;
            };
            auto ctx = std::make_unique<ErrorCtx>(ErrorCtx{this, err.message});
            helix::ui::queue_update<ErrorCtx>(
                std::move(ctx), [](ErrorCtx* c) { c->panel->on_calibration_error(c->message); });
        });
}

// ============================================================================
// Modal Management
// ============================================================================

void BedMeshPanel::show_calibrate_modal() {
    lv_subject_set_int(&bed_mesh_calibrating_, 0);

    calibrate_modal_widget_ = helix::ui::modal_show("bed_mesh_calibrate_modal");
    spdlog::debug("[{}] Showing calibrate modal", get_name());
}

void BedMeshPanel::show_rename_modal(const std::string& profile_name) {
    pending_rename_old_ = profile_name;
    lv_subject_copy_string(&bed_mesh_rename_old_name_, profile_name.c_str());

    rename_modal_widget_ = helix::ui::modal_show("bed_mesh_rename_modal");
    spdlog::debug("[{}] Showing rename modal for: {}", get_name(), profile_name);
}

void BedMeshPanel::show_delete_confirm_modal(const std::string& profile_name) {
    pending_delete_profile_ = profile_name;

    // Create message with profile name
    char msg_buf[256];
    snprintf(msg_buf, sizeof(msg_buf), "Delete profile '%s'? This action cannot be undone.",
             profile_name.c_str());

    delete_modal_widget_ = helix::ui::modal_show_confirmation(
        lv_tr("Delete Profile?"), msg_buf, ModalSeverity::Warning, lv_tr("Delete"),
        on_delete_confirm_cb, on_delete_cancel_cb,
        nullptr); // Uses global panel reference

    if (!delete_modal_widget_) {
        spdlog::error("[{}] Failed to create delete confirmation modal", get_name());
        return;
    }

    spdlog::debug("[{}] Showing delete confirm modal for: {}", get_name(), profile_name);
}

void BedMeshPanel::show_save_config_modal() {
    save_config_modal_widget_ = helix::ui::modal_show("bed_mesh_save_config_modal");
    spdlog::debug("[{}] Showing save config modal", get_name());
}

void BedMeshPanel::hide_all_modals() {
    // Cancel any pending operation timeout
    operation_guard_.end();

    // Reset calibration state machine
    lv_subject_set_int(&bed_mesh_calibrating_, 0);
    lv_subject_set_int(&bed_mesh_calibrate_state_, static_cast<int>(BedMeshCalibrationState::IDLE));

    // Hide all modals (all use ui_modal_hide pattern now)
    if (calibrate_modal_widget_) {
        helix::ui::modal_hide(calibrate_modal_widget_);
        calibrate_modal_widget_ = nullptr;
    }
    if (rename_modal_widget_) {
        helix::ui::modal_hide(rename_modal_widget_);
        rename_modal_widget_ = nullptr;
    }
    if (save_config_modal_widget_) {
        helix::ui::modal_hide(save_config_modal_widget_);
        save_config_modal_widget_ = nullptr;
    }
    if (delete_modal_widget_) {
        helix::ui::modal_hide(delete_modal_widget_);
        delete_modal_widget_ = nullptr;
    }
}

void BedMeshPanel::confirm_delete_profile() {
    std::string name = pending_delete_profile_;
    hide_all_modals();
    execute_delete_profile(name);
}

void BedMeshPanel::decline_save_config() {
    hide_all_modals();
    pending_operation_ = PendingOperation::None;
}

void BedMeshPanel::confirm_save_config() {
    hide_all_modals();
    execute_save_config();
    pending_operation_ = PendingOperation::None;
}

void BedMeshPanel::start_calibration_with_name(const std::string& profile_name) {
    hide_all_modals();
    execute_calibration(profile_name);
}

void BedMeshPanel::confirm_rename(const std::string& new_name) {
    std::string old_name = pending_rename_old_;
    hide_all_modals();
    execute_rename_profile(old_name, new_name);
}

// ============================================================================
// Profile Operation Implementations
// ============================================================================

void BedMeshPanel::execute_delete_profile(const std::string& name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Deleting profile: {}", get_name(), name);

    operation_guard_.begin(OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING("Bed mesh operation timed out");
    });

    std::string cmd = "BED_MESH_PROFILE REMOVE=" + name;
    api->execute_gcode(
        cmd,
        [this, name]() {
            operation_guard_.end();
            spdlog::info("[{}] Profile deleted: {}", get_name(), name);
            NOTIFY_SUCCESS("Profile deleted");
            pending_operation_ = PendingOperation::Delete;
            show_save_config_modal();
        },
        [this](const MoonrakerError& err) {
            operation_guard_.end();
            spdlog::error("[{}] Failed to delete profile: {}", get_name(), err.message);
            NOTIFY_ERROR("Failed to delete profile");
        });
}

void BedMeshPanel::execute_rename_profile(const std::string& old_name,
                                          const std::string& new_name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Renaming profile: {} -> {}", get_name(), old_name, new_name);

    operation_guard_.begin(OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING("Bed mesh operation timed out");
    });

    // Step 1: Load the profile
    std::string load_cmd = "BED_MESH_PROFILE LOAD=" + old_name;
    api->execute_gcode(
        load_cmd,
        [this, old_name, new_name]() {
            // Step 2: Save with new name
            MoonrakerAPI* api2 = get_moonraker_api();
            if (!api2) {
                operation_guard_.end();
                return;
            }
            std::string save_cmd = "BED_MESH_PROFILE SAVE=" + new_name;
            api2->execute_gcode(
                save_cmd,
                [this, old_name, new_name]() {
                    // Step 3: Remove old name
                    MoonrakerAPI* api3 = get_moonraker_api();
                    if (!api3) {
                        operation_guard_.end();
                        return;
                    }
                    std::string remove_cmd = "BED_MESH_PROFILE REMOVE=" + old_name;
                    api3->execute_gcode(
                        remove_cmd,
                        [this, old_name, new_name]() {
                            operation_guard_.end();
                            spdlog::info("[{}] Profile renamed: {} -> {}", get_name(), old_name,
                                         new_name);
                            NOTIFY_SUCCESS("Profile renamed");
                            pending_operation_ = PendingOperation::Rename;
                            show_save_config_modal();
                        },
                        [this](const MoonrakerError& err) {
                            operation_guard_.end();
                            spdlog::error("[{}] Failed to remove old profile: {}", get_name(),
                                          err.message);
                            NOTIFY_ERROR("Rename failed at remove step");
                        });
                },
                [this](const MoonrakerError& err) {
                    operation_guard_.end();
                    spdlog::error("[{}] Failed to save new profile: {}", get_name(), err.message);
                    NOTIFY_ERROR("Rename failed at save step");
                });
        },
        [this](const MoonrakerError& err) {
            operation_guard_.end();
            spdlog::error("[{}] Failed to load profile for rename: {}", get_name(), err.message);
            NOTIFY_ERROR("Rename failed at load step");
        });
}

void BedMeshPanel::execute_calibration(const std::string& profile_name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Starting calibration for profile: {}", get_name(), profile_name);
    lv_subject_set_int(&bed_mesh_calibrating_, 1);

    std::string cmd = "BED_MESH_CALIBRATE PROFILE=" + profile_name;
    api->execute_gcode(
        cmd,
        [this, profile_name]() {
            spdlog::info("[{}] Calibration started for: {}", get_name(), profile_name);
            NOTIFY_INFO("Calibration started");
            // Modal will close when mesh update notification arrives
        },
        [this](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[{}] Calibration response timed out (may still be running)",
                             get_name());
                NOTIFY_WARNING("Calibration may still be running — response timed out");
            } else {
                spdlog::error("[{}] Failed to start calibration: {}", get_name(), err.message);
                NOTIFY_ERROR("Failed to start calibration");
                lv_subject_set_int(&bed_mesh_calibrating_, 0);
            }
        },
        CALIBRATION_TIMEOUT_MS);
}

void BedMeshPanel::execute_save_config() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Saving config (will restart Klipper)", get_name());

    operation_guard_.begin(SLOW_OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING("Bed mesh operation timed out");
    });

    api->execute_gcode(
        "SAVE_CONFIG",
        [this]() {
            operation_guard_.end();
            spdlog::info("[{}] SAVE_CONFIG sent - Klipper will restart", get_name());
            NOTIFY_INFO("Configuration saved - restarting");
        },
        [this](const MoonrakerError& err) {
            operation_guard_.end();
            spdlog::error("[{}] Failed to save config: {}", get_name(), err.message);
            NOTIFY_ERROR("Failed to save configuration");
        });
}

// ============================================================================
// Calibration Progress Handlers
// ============================================================================

void BedMeshPanel::on_probe_progress(int current, int total) {
    int progress = (total > 0) ? (current * 100 / total) : 0;
    lv_subject_set_int(&bed_mesh_probe_progress_, progress);

    std::snprintf(probe_text_buf_, sizeof(probe_text_buf_), "Probing point %d of %d", current,
                  total);
    lv_subject_copy_string(&bed_mesh_probe_text_, probe_text_buf_);

    spdlog::debug("[BedMeshPanel] Probe progress: {}/{} ({}%)", current, total, progress);
}

void BedMeshPanel::on_calibration_complete() {
    spdlog::info("[BedMeshPanel] Calibration complete, transitioning to naming state");
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::NAMING));
}

void BedMeshPanel::on_calibration_error(const std::string& message) {
    spdlog::error("[BedMeshPanel] Calibration error: {}", message);
    std::strncpy(error_message_buf_, message.c_str(), sizeof(error_message_buf_) - 1);
    error_message_buf_[sizeof(error_message_buf_) - 1] = '\0';
    lv_subject_copy_string(&bed_mesh_error_message_, error_message_buf_);
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::ERROR));
}

void BedMeshPanel::handle_emergency_stop() {
    spdlog::warn("[BedMeshPanel] Emergency stop during bed mesh calibration");

    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        api->emergency_stop([]() { spdlog::info("[BedMeshPanel] Emergency stop sent"); },
                            [](const MoonrakerError& err) {
                                spdlog::error("[BedMeshPanel] Emergency stop failed: {}",
                                              err.message);
                            });
    }

    // Close modal and reset state
    hide_all_modals();
    lv_subject_set_int(&bed_mesh_calibrate_state_, static_cast<int>(BedMeshCalibrationState::IDLE));
}

void BedMeshPanel::save_profile_with_name(const std::string& name) {
    spdlog::info("[BedMeshPanel] Saving mesh profile: {}", name);

    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        hide_all_modals();
        return;
    }

    std::string cmd = "BED_MESH_PROFILE SAVE=" + name;
    api->execute_gcode(
        cmd,
        [this, name]() {
            spdlog::info("[BedMeshPanel] Profile saved: {}", name);
            NOTIFY_SUCCESS("Mesh saved as '" + name + "'");
            hide_all_modals();
            lv_subject_set_int(&bed_mesh_calibrate_state_,
                               static_cast<int>(BedMeshCalibrationState::IDLE));
            // Prompt to save config
            pending_operation_ = PendingOperation::Calibrate;
            show_save_config_modal();
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[BedMeshPanel] Failed to save profile: {}", err.message);
            NOTIFY_ERROR("Failed to save profile");
            hide_all_modals();
        });
}

// ============================================================================
// Static Event Callbacks
// ============================================================================

// Helper to extract profile index from callback name
static int get_profile_index_from_event(lv_event_t* e) {
    // The callback name contains the index (e.g., "on_profile_2_clicked")
    // We use user_data to pass the panel, not the index
    // Instead, look at the target object's name
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!target)
        return -1;

    // Walk from target upward looking for profile_row_N
    // Start from target itself (handles clicking the row card directly)
    lv_obj_t* obj = target;
    while (obj) {
        const char* name = lv_obj_get_name(obj);
        if (name && std::strncmp(name, "profile_row_", 12) == 0) {
            return name[12] - '0'; // Extract digit
        }
        obj = lv_obj_get_parent(obj);
    }
    return -1;
}

static void on_profile_clicked_cb(lv_event_t* e) {
    int index = get_profile_index_from_event(e);
    if (index >= 0) {
        get_global_bed_mesh_panel().load_profile(index);
    }
}

static void on_profile_rename_cb(lv_event_t* e) {
    int index = get_profile_index_from_event(e);
    if (index >= 0) {
        get_global_bed_mesh_panel().rename_profile(index);
    }
}

static void on_profile_delete_cb(lv_event_t* e) {
    int index = get_profile_index_from_event(e);
    if (index >= 0) {
        get_global_bed_mesh_panel().delete_profile(index);
    }
}

static void on_calibrate_header_clicked_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().start_calibration();
}

static void on_calibrate_cancel_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().hide_all_modals();
}

static void on_calibrate_start_cb(lv_event_t* /*e*/) {
    // Find the textarea
    lv_obj_t* input = lv_obj_find_by_name(lv_layer_top(), "calibrate_profile_name_input");
    if (!input) {
        // Try from parent screen
        input = lv_obj_find_by_name(lv_screen_active(), "calibrate_profile_name_input");
    }

    std::string profile_name = "default";
    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && std::strlen(text) > 0) {
            profile_name = text;
        }
    }

    get_global_bed_mesh_panel().start_calibration_with_name(profile_name);
}

static void on_rename_cancel_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().hide_all_modals();
}

static void on_rename_confirm_cb(lv_event_t* /*e*/) {
    // Get the new name from the input field
    lv_obj_t* input = lv_obj_find_by_name(lv_layer_top(), "rename_new_name_input");
    if (!input) {
        input = lv_obj_find_by_name(lv_screen_active(), "rename_new_name_input");
    }

    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && std::strlen(text) > 0) {
            get_global_bed_mesh_panel().confirm_rename(std::string(text));
        }
    }
}

static void on_delete_cancel_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().hide_all_modals();
}

static void on_delete_confirm_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().confirm_delete_profile();
}

static void on_save_config_no_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().decline_save_config();
}

static void on_save_config_yes_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().confirm_save_config();
}

static void on_emergency_stop_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().handle_emergency_stop();
}

static void on_save_profile_cb(lv_event_t* /*e*/) {
    // Find the input field in the modal
    lv_obj_t* input = lv_obj_find_by_name(lv_layer_top(), "calibrate_profile_name_input");
    if (!input) {
        input = lv_obj_find_by_name(lv_screen_active(), "calibrate_profile_name_input");
    }

    std::string profile_name = "default";
    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && std::strlen(text) > 0) {
            profile_name = text;
        }
    }

    get_global_bed_mesh_panel().save_profile_with_name(profile_name);
}

// ============================================================================
// Global Instance
// ============================================================================

DEFINE_GLOBAL_PANEL(BedMeshPanel, g_bed_mesh_panel, get_global_bed_mesh_panel)
