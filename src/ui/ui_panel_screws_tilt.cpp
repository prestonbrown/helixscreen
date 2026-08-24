// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_screws_tilt.h"

#include "ui_callback_helpers.h"
#include "ui_fonts.h"
#include "ui_nav_manager.h"
#include "ui_screws_tilt_share_modal.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "toolhead_homing.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <memory>

using namespace helix;

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

static std::unique_ptr<ScrewsTiltPanel> s_screws_tilt_panel;

// State subject (0=IDLE, 1=PROBING, 2=RESULTS, 3=ERROR)
static lv_subject_t s_screws_tilt_state;

// Forward declarations
static void on_screws_tilt_row_clicked(lv_event_t* e);
IMoonrakerClient* get_moonraker_client();
IMoonrakerAPI* get_moonraker_api();

ScrewsTiltPanel& get_global_screws_tilt_panel() {
    if (!s_screws_tilt_panel) {
        s_screws_tilt_panel = std::make_unique<ScrewsTiltPanel>();
        // Delegate to destroy_screws_tilt_panel() rather than resetting the
        // pointer directly: it runs ScrewsTiltPanel::cleanup() first, which
        // unregisters the overlay instance and sets the cleanup_called_ flag
        // that the panel's deferred probe callbacks check before touching
        // widgets.
        StaticPanelRegistry::instance().register_destroy("ScrewsTiltPanel",
                                                         []() { destroy_screws_tilt_panel(); });
    }
    return *s_screws_tilt_panel;
}

void destroy_screws_tilt_panel() {
    if (s_screws_tilt_panel) {
        s_screws_tilt_panel->cleanup();
        s_screws_tilt_panel.reset();
    }
}

void init_screws_tilt_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_screws_tilt_row_clicked", on_screws_tilt_row_clicked);
    spdlog::trace("[ScrewsTilt] Row click callback registered");
}

/**
 * @brief Row click handler for opening screws tilt from Advanced panel
 *
 * Registered via init_screws_tilt_row_handler().
 * Lazy-creates the screws tilt panel on first click.
 */
static void on_screws_tilt_row_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[ScrewsTilt] Bed leveling row clicked");

    auto& panel = get_global_screws_tilt_panel();

    // Lazy-create the screws tilt panel
    if (!panel.get_root()) {
        spdlog::debug("[ScrewsTilt] Creating screws tilt panel...");

        // Initialize subjects (must be before XML creation)
        if (!panel.are_subjects_initialized()) {
            panel.init_subjects();
        }

        // Set client and API before creating UI
        IMoonrakerClient* client = get_moonraker_client();
        IMoonrakerAPI* api = get_moonraker_api();
        panel.set_client(client, api);

        // Create the overlay UI
        lv_obj_t* overlay = panel.create(lv_display_get_screen_active(nullptr));

        if (!overlay) {
            spdlog::error("[ScrewsTilt] Failed to create screws_tilt_panel");
            return;
        }

        spdlog::info("[ScrewsTilt] Panel created and setup complete");
    }

    // Show the overlay (registers and pushes)
    panel.show();
}

// ============================================================================
// XML EVENT CALLBACK REGISTRATION
// ============================================================================

void ui_panel_screws_tilt_register_callbacks() {
    // Register event callbacks
    register_xml_callbacks({
        {"screws_tilt_start_cb",
         [](lv_event_t* /*e*/) { get_global_screws_tilt_panel().handle_start_clicked(); }},
        {"screws_tilt_cancel_cb",
         [](lv_event_t* /*e*/) { get_global_screws_tilt_panel().handle_cancel_clicked(); }},
        {"screws_tilt_done_cb",
         [](lv_event_t* /*e*/) { get_global_screws_tilt_panel().handle_done_clicked(); }},
        {"screws_tilt_reprobe_cb",
         [](lv_event_t* /*e*/) { get_global_screws_tilt_panel().handle_reprobe_clicked(); }},
        {"screws_tilt_retry_cb",
         [](lv_event_t* /*e*/) { get_global_screws_tilt_panel().handle_retry_clicked(); }},
        {"screws_tilt_share_cb",
         [](lv_event_t* /*e*/) { get_global_screws_tilt_panel().handle_share_clicked(); }},
    });

    // Initialize subjects BEFORE XML creation (bindings resolve at parse time)
    auto& panel = get_global_screws_tilt_panel();
    panel.init_subjects();

    spdlog::debug("[ScrewsTilt] Registered XML event callbacks");
}

// ============================================================================
// SUBJECT INITIALIZATION (must run BEFORE XML creation)
// ============================================================================

void ScrewsTiltPanel::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize state subject for state machine visibility
    // Note: s_screws_tilt_state is file-static, managed separately
    UI_MANAGED_SUBJECT_INT(s_screws_tilt_state, 0, "screws_tilt_state", subjects_);

    // Initialize subjects for reactive list rows (4 slots max)
    for (size_t i = 0; i < MAX_SCREWS; i++) {
        // Initialize char buffers to empty
        screw_name_bufs_[i][0] = '\0';
        screw_adj_bufs_[i][0] = '\0';

        // Build registration names
        char visible_name[32];
        char name_name[32];
        char adj_name[32];
        snprintf(visible_name, sizeof(visible_name), "screw_%zu_visible", i);
        snprintf(name_name, sizeof(name_name), "screw_%zu_name", i);
        snprintf(adj_name, sizeof(adj_name), "screw_%zu_adjustment", i);

        // Init subjects using managed macros - visible defaults to 0 (hidden)
        UI_MANAGED_SUBJECT_INT(screw_visible_subjects_[i], 0, visible_name, subjects_);
        UI_MANAGED_SUBJECT_STRING_N(screw_name_subjects_[i], screw_name_bufs_[i],
                                    SCREW_NAME_BUF_SIZE, "", name_name, subjects_);
        UI_MANAGED_SUBJECT_STRING_N(screw_adjustment_subjects_[i], screw_adj_bufs_[i],
                                    SCREW_ADJ_BUF_SIZE, "", adj_name, subjects_);
    }

    // Initialize status label subjects
    UI_MANAGED_SUBJECT_STRING(probe_count_subject_, probe_count_buf_, "", "probe_count_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(error_message_subject_, error_message_buf_, "", "error_message_text",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(results_is_leveled_subject_, 0, "results_is_leveled", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[ScrewsTilt] Subjects initialized and registered");
}

void ScrewsTiltPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // SubjectManager handles all subject cleanup via RAII
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[ScrewsTiltPanel] Subjects deinitialized");
}

// ============================================================================
// DESTRUCTOR
// ============================================================================

ScrewsTiltPanel::~ScrewsTiltPanel() {
    // Deinitialize subjects to disconnect observers before we're destroyed
    deinit_subjects();

    // Guard against static destruction order fiasco (spdlog may be gone)
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[ScrewsTilt] Destroyed");
    }
}

// ============================================================================
// OVERLAYBASE INTERFACE
// ============================================================================

lv_obj_t* ScrewsTiltPanel::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::debug("[ScrewsTilt] Overlay already created, reusing");
        return overlay_root_;
    }

    parent_screen_ = parent;

    // Create UI from XML
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "screws_tilt_panel", nullptr));

    if (!overlay_root_) {
        spdlog::error("[ScrewsTilt] Failed to create screws_tilt_panel XML");
        return nullptr;
    }

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Setup widget references
    setup_widgets();

    spdlog::info("[ScrewsTilt] Overlay created");
    return overlay_root_;
}

void ScrewsTiltPanel::setup_widgets() {
    if (!overlay_root_) {
        return;
    }

    // Find display elements
    bed_diagram_container_ = lv_obj_find_by_name(overlay_root_, "bed_diagram_container");
    results_instruction_ = lv_obj_find_by_name(overlay_root_, "results_instruction");

    // Find screw dot widgets for color updates
    screw_dots_[0] = lv_obj_find_by_name(overlay_root_, "screw_dot_0");
    screw_dots_[1] = lv_obj_find_by_name(overlay_root_, "screw_dot_1");
    screw_dots_[2] = lv_obj_find_by_name(overlay_root_, "screw_dot_2");
    screw_dots_[3] = lv_obj_find_by_name(overlay_root_, "screw_dot_3");
}

void ScrewsTiltPanel::show() {
    if (!overlay_root_) {
        spdlog::error("[ScrewsTilt] Cannot show - overlay not created");
        return;
    }

    spdlog::debug("[ScrewsTilt] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);
}

void ScrewsTiltPanel::on_activate() {
    OverlayBase::on_activate();

    // Reset for fresh session
    probe_count_ = 0;
    lv_subject_set_int(&results_is_leveled_subject_, 0);
    set_state(State::IDLE);
    clear_results();

    // Refresh the thread pitch each time: it decides how wide the level window
    // is, and the answer arrives well before the first probe completes.
    query_screw_thread();

    spdlog::info("[ScrewsTilt] Activated (probe count reset)");

    // Auto-start probing for testing (env var)
    if (std::getenv("SCREWS_AUTO_START")) {
        spdlog::info("[ScrewsTilt] Auto-starting probe (SCREWS_AUTO_START set)");
        start_probing();
    }
}

void ScrewsTiltPanel::on_deactivate() {
    if (state_ == State::PROBING) {
        // Cancel ongoing probe via Moonraker
        if (api_) {
            spdlog::info("[ScrewsTilt] Aborting probe on deactivate");
            api_->execute_gcode("ABORT", nullptr, nullptr);
        }
    }

    // Clean up dynamic indicators
    clear_results();

    OverlayBase::on_deactivate();
    spdlog::debug("[ScrewsTilt] Deactivated");
}

void ScrewsTiltPanel::cleanup() {
    spdlog::debug("[ScrewsTilt] Cleanup called");

    // Unregister from NavigationManager
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    OverlayBase::cleanup();
}

// ============================================================================
// STATE MANAGEMENT
// ============================================================================

void ScrewsTiltPanel::set_state(State new_state) {
    spdlog::debug("[ScrewsTilt] State change: {} -> {}", static_cast<int>(state_),
                  static_cast<int>(new_state));
    state_ = new_state;

    // Update subject - XML bindings handle visibility automatically
    // State mapping: 0=IDLE, 1=PROBING, 2=RESULTS, 3=ERROR
    lv_subject_set_int(&s_screws_tilt_state, static_cast<int>(new_state));
}

// ============================================================================
// ERROR MESSAGE SANITIZATION
// ============================================================================

static std::string sanitize_error_message(const std::string& raw) {
    std::string msg = raw;

    // Strip Klipper emergency/error prefixes
    if (msg.rfind("!! ", 0) == 0)
        msg = msg.substr(3);
    if (msg.rfind("Error:", 0) == 0)
        msg = msg.substr(6);
    if (msg.rfind("error:", 0) == 0)
        msg = msg.substr(6);

    // Trim leading whitespace
    auto start = msg.find_first_not_of(" \t");
    if (start != std::string::npos && start > 0)
        msg = msg.substr(start);

    // Strip JSON-looking content (e.g. {"message": "Must home axis..."} → extract message)
    if (!msg.empty() && msg.front() == '{') {
        auto mpos = msg.find("\"message\"");
        if (mpos != std::string::npos) {
            auto colon = msg.find(':', mpos + 9);
            if (colon != std::string::npos) {
                auto qstart = msg.find('"', colon + 1);
                auto qend =
                    (qstart != std::string::npos) ? msg.find('"', qstart + 1) : std::string::npos;
                if (qstart != std::string::npos && qend != std::string::npos) {
                    msg = msg.substr(qstart + 1, qend - qstart - 1);
                }
            }
        }
    }

    return msg;
}

// ============================================================================
// COMMAND HELPERS
// ============================================================================

void ScrewsTiltPanel::start_probing() {
    if (!api_) {
        spdlog::error("[ScrewsTilt] No API - cannot probe");
        on_screws_tilt_error(lv_tr("Internal error: API not available"));
        return;
    }

    probe_count_++;
    set_state(State::PROBING);

    spdlog::info("[ScrewsTilt] Starting probe #{}", probe_count_);

    ensure_homed_then(
        api_, lifetime_,
        [this]() {
            if (cleanup_called())
                return;
            if (state_ != State::PROBING)
                return;
            spdlog::info("[ScrewsTilt] Proceeding to screws tilt");
            start_screws_tilt_command();
        },
        [this](const MoonrakerError& err) {
            if (cleanup_called())
                return;
            if (state_ != State::PROBING)
                return;
            std::string msg = (err.type == MoonrakerErrorType::TIMEOUT)
                                  ? lv_tr("Homing timed out — printer may still be homing")
                                  : std::string(lv_tr("Homing failed: ")) + err.message;
            on_screws_tilt_error(msg);
        });
}

void ScrewsTiltPanel::start_screws_tilt_command() {
    auto token = lifetime_.token();
    api_->advanced().calculate_screws_tilt(
        [this, token](const std::vector<ScrewTiltResult>& results) {
            if (token.expired())
                return;
            token.defer("ScrewsTilt::results", [this, results]() {
                if (cleanup_called())
                    return;
                if (state_ != State::PROBING)
                    return;
                on_screws_tilt_results(results);
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("ScrewsTilt::error", [this, msg]() {
                if (cleanup_called())
                    return;
                if (state_ != State::PROBING)
                    return;
                on_screws_tilt_error(msg);
            });
        });
}

void ScrewsTiltPanel::cancel_probing() {
    spdlog::info("[ScrewsTilt] Probing cancelled by user");
    set_state(State::IDLE);
}

// ============================================================================
// RESULT CALLBACKS
// ============================================================================

void ScrewsTiltPanel::on_screws_tilt_results(const std::vector<ScrewTiltResult>& results) {
    spdlog::info("[ScrewsTilt] Received {} screw results", results.size());

    screw_results_ = results;
    level_report_ = evaluate_screw_level(results, screw_pitch_mm_);

    // An unreadable adjustment used to skip the screw, which read as a level
    // bed. Surface it instead — we cannot judge a bed we cannot parse.
    if (level_report_.verdict == ScrewLevelVerdict::PARSE_ERROR) {
        spdlog::error("[ScrewsTilt] Unreadable screw result ({}) — cannot judge level",
                      level_report_.parse_error);
        on_screws_tilt_error(lv_tr("Could not read the screw adjustments from the printer"));
        return;
    }

    spdlog::info("[ScrewsTilt] Spread {} min vs {} min tolerance (pitch {:.1f}mm/turn)",
                 level_report_.spread_minutes, level_report_.tolerance_minutes, screw_pitch_mm_);

    populate_results(results);

    // Always show RESULTS state — success banner shown via results_is_leveled subject
    bool all_level = level_report_.is_level();
    lv_subject_set_int(&results_is_leveled_subject_, all_level ? 1 : 0);

    if (all_level) {
        char buf[64];
        snprintf(buf, sizeof(buf), lv_tr("Completed in %d probe%s"), probe_count_,
                 probe_count_ == 1 ? "" : "s");
        lv_subject_copy_string(&probe_count_subject_, buf);
    }

    set_state(State::RESULTS);
}

void ScrewsTiltPanel::on_screws_tilt_error(const std::string& message) {
    spdlog::error("[ScrewsTilt] Error: {}", message);

    std::string clean = sanitize_error_message(message);
    lv_subject_copy_string(&error_message_subject_, clean.c_str());
    set_state(State::ERROR);
}

// ============================================================================
// UI UPDATE HELPERS
// ============================================================================

void ScrewsTiltPanel::populate_results(const std::vector<ScrewTiltResult>& results) {
    clear_results();

    // Store results first so the level report lines up with them
    screw_results_ = results;

    // Update subjects for reactive list rows (XML handles the UI)
    for (size_t i = 0; i < MAX_SCREWS; i++) {
        if (i < results.size()) {
            const auto& screw = results[i];
            bool in_spec = i < level_report_.in_spec.size() && level_report_.in_spec[i];
            bool is_worst = (i == level_report_.worst_index && !screw.is_reference && !in_spec);

            // Copy strings into fixed buffers (LVGL string subjects require stable storage)
            snprintf(screw_name_bufs_[i], SCREW_NAME_BUF_SIZE, "%s", screw.display_name().c_str());
            // Use friendly adjustment text (e.g., "Tighten 1/4 turn" instead of "CW 00:18")
            snprintf(screw_adj_bufs_[i], SCREW_ADJ_BUF_SIZE, "%s",
                     screw.friendly_adjustment(in_spec).c_str());

            // Update subjects - this triggers XML binding updates
            lv_subject_set_int(&screw_visible_subjects_[i], 1); // Show row
            lv_subject_copy_string(&screw_name_subjects_[i], screw_name_bufs_[i]);
            lv_subject_copy_string(&screw_adjustment_subjects_[i], screw_adj_bufs_[i]);

            // Update dot color (not bindable via subject, so do directly)
            if (screw_dots_[i]) {
                lv_obj_set_style_bg_color(screw_dots_[i],
                                          get_adjustment_color(screw, in_spec, is_worst), 0);
            }

            // Create bed diagram indicator (position varies, so still dynamic)
            create_screw_indicator(i, screw, in_spec, is_worst);
        } else {
            // Hide unused rows
            lv_subject_set_int(&screw_visible_subjects_[i], 0);
        }
    }

    update_screw_diagram();
}

void ScrewsTiltPanel::clear_results() {
    // Clear bed diagram indicators (dynamically positioned)
    for (auto* indicator : screw_indicators_) {
        helix::ui::safe_delete(indicator);
    }
    screw_indicators_.clear();

    // Hide all list rows via subjects (reactive pattern)
    for (size_t i = 0; i < MAX_SCREWS; i++) {
        lv_subject_set_int(&screw_visible_subjects_[i], 0);
    }
}

/**
 * @brief Create a screw indicator widget for the bed diagram
 *
 * Uses LVGL alignment to position indicators at corners rather than
 * complex coordinate math. This is more robust and works regardless
 * of container size.
 */
// Animation callback for rotating icons
static void rotation_anim_cb(void* var, int32_t value) {
    lv_obj_set_style_transform_rotation(static_cast<lv_obj_t*>(var), value, 0);
}

void ScrewsTiltPanel::create_screw_indicator(size_t index, const ScrewTiltResult& screw,
                                             bool in_spec, bool is_worst) {
    if (!bed_diagram_container_) {
        return;
    }

    // Circular screw indicators - size based on icon
    constexpr int INDICATOR_SIZE = 52; // Square for circle

    // Create circular indicator
    lv_obj_t* indicator = lv_obj_create(bed_diagram_container_);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, INDICATOR_SIZE, INDICATOR_SIZE);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0); // Fully round
    lv_obj_set_style_border_width(indicator, is_worst ? 3 : 2, 0);
    lv_obj_set_style_border_color(indicator, theme_manager_get_color("text"), 0);

    // Color based on adjustment severity (worst screw gets highlighted)
    lv_color_t bg_color = get_adjustment_color(screw, in_spec, is_worst);
    lv_obj_set_style_bg_color(indicator, bg_color, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0); // Must be AFTER bg_color

    spdlog::debug("[ScrewsTilt] Indicator {} ({}): color=0x{:06X}, is_worst={}", index,
                  screw.screw_name, (bg_color.red << 16) | (bg_color.green << 8) | bg_color.blue,
                  is_worst);

    // Create centered icon/text label
    lv_obj_t* label = lv_label_create(indicator);
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
    lv_obj_center(label);

    if (in_spec) {
        // Inside the level window (reference screws always are): show a static
        // checkmark, no rotation animation
        lv_obj_set_style_text_font(label, &mdi_icons_32, 0);
        // check icon (F012C)
        lv_label_set_text(label, "\xF3\xB0\x84\xAC");
    } else {
        // Adjustment needed - show animated rotation icon
        bool is_clockwise = screw.adjustment.find("CW") == 0 && screw.adjustment.find("CCW") != 0;

        lv_obj_set_style_text_font(label, &mdi_icons_32, 0);
        // rotate-right (F0467) = clockwise/tighten, rotate-left (F0465) = CCW/loosen
        const char* dir_icon = is_clockwise ? "\xF3\xB0\x91\xA7" : "\xF3\xB0\x91\xA5";
        lv_label_set_text(label, dir_icon);

        // Set transform pivot to center for rotation
        lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(label, LV_PCT(50), 0);

        // Animate rotation continuously with linear easing for smooth motion
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, label);
        lv_anim_set_exec_cb(&anim, rotation_anim_cb);
        lv_anim_set_path_cb(&anim, lv_anim_path_linear);
        lv_anim_set_duration(&anim, 2000); // 2 seconds per rotation
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);

        if (is_clockwise) {
            lv_anim_set_values(&anim, 0, 3600); // 0 to 360 degrees (LVGL uses 0.1 degree units)
        } else {
            lv_anim_set_values(&anim, 3600, 0); // 360 to 0 degrees (reverse)
        }

        lv_anim_start(&anim);
    }

    screw_indicators_.push_back(indicator);

    (void)index;
}

/**
 * @brief Position screw indicators using LVGL alignment
 *
 * Maps screw positions to corner alignments based on their relative positions
 * on the bed. This is simpler and more reliable than coordinate math.
 */
void ScrewsTiltPanel::update_screw_diagram() {
    if (!bed_diagram_container_ || screw_results_.empty()) {
        return;
    }

    // Force layout calculation
    lv_obj_update_layout(bed_diagram_container_);

    // Find bed bounds
    float min_x = screw_results_[0].x_pos;
    float max_x = screw_results_[0].x_pos;
    float min_y = screw_results_[0].y_pos;
    float max_y = screw_results_[0].y_pos;

    for (const auto& screw : screw_results_) {
        min_x = std::min(min_x, screw.x_pos);
        max_x = std::max(max_x, screw.x_pos);
        min_y = std::min(min_y, screw.y_pos);
        max_y = std::max(max_y, screw.y_pos);
    }

    float center_x = (min_x + max_x) / 2.0f;
    float center_y = (min_y + max_y) / 2.0f;

    // Position indicators using alignment based on quadrant
    for (size_t i = 0; i < screw_results_.size() && i < screw_indicators_.size(); i++) {
        const auto& screw = screw_results_[i];
        lv_obj_t* indicator = screw_indicators_[i];

        // Determine quadrant and set alignment
        bool is_left = screw.x_pos < center_x;
        bool is_front = screw.y_pos < center_y; // Front = lower Y in bed coords

        lv_align_t align;
        if (is_left && is_front) {
            align = LV_ALIGN_BOTTOM_LEFT;
        } else if (!is_left && is_front) {
            align = LV_ALIGN_BOTTOM_RIGHT;
        } else if (is_left && !is_front) {
            align = LV_ALIGN_TOP_LEFT;
        } else {
            align = LV_ALIGN_TOP_RIGHT;
        }

        // Apply alignment with small offset from edges
        lv_obj_align(indicator, align, 0, 0);

        spdlog::debug("[ScrewsTilt] {} -> {} (x:{:.0f}, y:{:.0f})", screw.screw_name,
                      is_left ? (is_front ? "bottom_left" : "top_left")
                              : (is_front ? "bottom_right" : "top_right"),
                      screw.x_pos, screw.y_pos);
    }
}

lv_color_t ScrewsTiltPanel::get_adjustment_color(const ScrewTiltResult& screw, bool in_spec,
                                                 bool is_worst_screw) const {
    // Helper to get color from globals.xml constant
    auto get_theme_color = [](const char* const_name) -> lv_color_t {
        const char* hex = lv_xml_get_const(nullptr, const_name);
        if (hex) {
            return theme_manager_parse_hex_color(hex);
        }
        return theme_manager_get_color(const_name); // Fallback to direct token lookup
    };

    if (screw.is_reference || in_spec) {
        return get_theme_color("success");
    }

    if (is_worst_screw) {
        // Highlight the worst screw with primary color (bright, attention-grabbing)
        return get_theme_color("primary");
    }

    // Both thresholds derive from the same pitch-aware helper, so a coarse
    // thread never gets a wider window than a fine one.
    if (screw.adjustment_minutes() <= screw_severe_adjustment_minutes(screw_pitch_mm_)) {
        return get_theme_color("warning");
    }

    return get_theme_color("danger");
}

// ============================================================================
// CONFIG QUERY
// ============================================================================

void ScrewsTiltPanel::query_screw_thread() {
    if (!client_) {
        return;
    }

    // Query configfile.settings.screws_tilt_adjust for the bed screw thread.
    // `settings` (not `config`) so Klipper's own default is present even when
    // the user never spelled screw_thread out in printer.cfg.
    json params = {{"objects", json::object({{"configfile", {"settings"}}})}};

    auto token = lifetime_.token();
    client_->send_jsonrpc("printer.objects.query", params, [this, token](json response) {
        // L081 Mechanism C: hop to the main thread before touching members.
        token.defer("ScrewsTilt::screw_thread", [this, response = std::move(response)]() {
            if (cleanup_called()) {
                return;
            }
            // const operator[] on a missing key asserts rather than throws,
            // so the chain has to be guarded level by level.
            if (!response.contains("result") || !response["result"].contains("status") ||
                !response["result"]["status"].contains("configfile") ||
                !response["result"]["status"]["configfile"].contains("settings") ||
                !response["result"]["status"]["configfile"]["settings"].is_object()) {
                spdlog::debug("[ScrewsTilt] No configfile settings — assuming {:.1f}mm/turn",
                              screw_pitch_mm_);
                return;
            }

            const auto& settings = response["result"]["status"]["configfile"]["settings"];
            if (!settings.contains("screws_tilt_adjust") ||
                !settings["screws_tilt_adjust"].is_object()) {
                spdlog::debug("[ScrewsTilt] No screws_tilt_adjust section — assuming "
                              "{:.1f}mm/turn",
                              screw_pitch_mm_);
                return;
            }

            const auto& section = settings["screws_tilt_adjust"];
            if (!section.contains("screw_thread") || !section["screw_thread"].is_string()) {
                spdlog::debug("[ScrewsTilt] No screw_thread key — assuming {:.1f}mm/turn",
                              screw_pitch_mm_);
                return;
            }

            std::string thread = section["screw_thread"].get<std::string>();
            screw_pitch_mm_ = screw_thread_pitch_mm(thread);
            spdlog::info("[ScrewsTilt] screw_thread={} -> {:.1f}mm/turn, level window {} min",
                         thread, screw_pitch_mm_, screw_level_tolerance_minutes(screw_pitch_mm_));
        });
    });
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void ScrewsTiltPanel::handle_start_clicked() {
    spdlog::debug("[ScrewsTilt] Start clicked");
    start_probing();
}

void ScrewsTiltPanel::handle_cancel_clicked() {
    spdlog::debug("[ScrewsTilt] Cancel clicked");
    cancel_probing();
}

void ScrewsTiltPanel::handle_reprobe_clicked() {
    spdlog::debug("[ScrewsTilt] Re-probe clicked");
    start_probing();
}

void ScrewsTiltPanel::handle_done_clicked() {
    spdlog::debug("[ScrewsTilt] Done clicked");
    probe_count_ = 0;
    clear_results();
    set_state(State::IDLE);
    NavigationManager::instance().go_back();
}

void ScrewsTiltPanel::handle_retry_clicked() {
    spdlog::debug("[ScrewsTilt] Retry clicked");
    start_probing();
}

void ScrewsTiltPanel::handle_share_clicked() {
    spdlog::debug("[ScrewsTilt] Share clicked ({} results)", screw_results_.size());
    if (screw_results_.empty()) {
        return;
    }
    // Self-deleting modal — it removes itself on hide.
    auto* modal = new helix::ui::ScrewsTiltShareModal(screw_results_);
    modal->show_modal(nullptr);
}
