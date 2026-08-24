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
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_global_panel_helper.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "bed_mesh_portrait_layout.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "layout_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "temperature_controller.h"
#include "theme_manager.h"
#include "toolhead_homing.h"

#include <spdlog/fmt/fmt.h>
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
    std::memset(max_value_buf_, 0, sizeof(max_value_buf_));
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

        // wire_canvas_and_content() registers on_canvas_deleted_cb (canvas_)
        // plus on_content_size_changed and on_content_deleted_cb (content_),
        // all with user_data=this.
        // StaticPanelRegistry::destroy_all() runs BEFORE lv_deinit() and
        // before a soft restart's explicit widget-tree deletion — in both
        // paths `this` is about to be freed while canvas_/overlay_content
        // are still live widgets. Without removing these here, the later
        // real deletion would fire on_canvas_deleted_cb with user_data
        // pointing at freed memory (rule 5's hazard class, DELETE variant —
        // worse than the analogous SIZE_CHANGED case because DELETE is
        // GUARANTEED to fire during teardown). lv_obj_remove_event_cb()
        // removes every registration of the given callback function
        // regardless of how many accumulated, so this is correct even if
        // wire_canvas_and_content() were ever called more than once against
        // the same still-live widget.
        if (canvas_) {
            lv_obj_remove_event_cb(canvas_, on_canvas_deleted_cb);
        }
        if (content_) {
            lv_obj_remove_event_cb(content_, on_content_size_changed);
            lv_obj_remove_event_cb(content_, on_content_deleted_cb);
        }
    }

    // Clear widget pointers (LVGL owns the objects)
    canvas_ = nullptr;
    content_ = nullptr;
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
        UI_MANAGED_SUBJECT_STRING(bed_mesh_max_value_, max_value_buf_, "--", "bed_mesh_max_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_max_coord_, max_coord_buf_, "", "bed_mesh_max_coord",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_min_value_, min_value_buf_, "--", "bed_mesh_min_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_min_coord_, min_coord_buf_, "", "bed_mesh_min_coord",
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
        UI_MANAGED_SUBJECT_INT(bed_mesh_probe_indeterminate_, 0, "bed_mesh_probe_indeterminate",
                               subjects_);
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

    // Find canvas widget, guard it against dangling, and wire SIZE_CHANGED —
    // see wire_canvas_and_content() for what this covers and why.
    if (!wire_canvas_and_content(overlay_content)) {
        return overlay_root_;
    }
    spdlog::debug("[{}] Found canvas widget - rotation controlled by touch drag", get_name());
    spdlog::debug("[{}] Registered SIZE_CHANGED on overlay_content", get_name());

    // bed_mesh_panel.xml's top-level <if cond="ui_is_portrait ..."> rebuilds
    // overlay_content (and therefore bed_mesh_canvas) in place whenever the
    // display rotates across the portrait/landscape boundary — real hardware
    // never does this, but the SDL dev window does on a resize. Must be
    // registered AFTER lv_xml_create() above so it observes ui_is_portrait
    // strictly after that <if>'s own observer — see the method for why.
    setup_orientation_rewire_observer();

    // Setup Moonraker subscription for mesh updates
    setup_moonraker_subscription();

    // Setup observer for build_volume changes (to refresh bounds when stepper config loads)
    setup_build_volume_observer();

    // Load initial mesh data from IMoonrakerAPI
    IMoonrakerAPI* api = get_moonraker_api();
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

    apply_canvas_render_settings();

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Canvas / content wiring — initial create(), and surviving a rebuild
// ============================================================================
//
// bed_mesh_panel.xml's top-level view branches on <if cond="ui_is_portrait eq
// 1"> to pick between the portrait and landscape arrangements. Because that
// cond references a subject, LVGL's XML engine treats it as REACTIVE: it
// binds an observer to ui_is_portrait and, on every change, tears down the
// current expansion and rebuilds the other branch in place
// (xml_frag_rebuild -> xml_frag_teardown, lib/helix-xml/src/xml/lv_xml.c).
// overlay_content — and therefore bed_mesh_canvas underneath it — IS that
// expansion's root: teardown reparents it into an off-tree, hidden,
// zero-size "condemned" sink and lv_obj_delete_async()s the sink, so the old
// widgets are gone (or about to be) the moment the rebuild runs.
//
// Nothing else in this file ever learned that canvas_ or the SIZE_CHANGED
// registration on the old overlay_content had gone stale — on_ui_destroyed()
// (which nulls canvas_) is only called on full panel teardown, never on an
// in-place XML rebuild. Every canvas_ dereference between a rotation and the
// panel's eventual destruction — including ensure_async_rendering()'s
// ui_bed_mesh_request_async_render(canvas_) reachable from the resize path
// in src/application/application.cpp — was a potential use-after-free.
//
// Real hardware never resizes or rotates at runtime, so this only matters
// for the SDL dev window — but "don't crash" applies there too. The fix is
// two independent, minimal pieces:
//   1. wire_canvas_and_content() installs an LV_EVENT_DELETE guard directly
//      on canvas_ that nulls it the instant the widget is actually deleted,
//      from WHICHEVER path deletes it. canvas_ can therefore never dangle —
//      every existing call site already checks `if (canvas_)` first.
//   2. setup_orientation_rewire_observer() + rewire_after_orientation_flip()
//      re-run the same wiring against the freshly rebuilt tree, so a flip
//      leaves the panel fully functional instead of merely non-crashing.

bool BedMeshPanel::wire_canvas_and_content(lv_obj_t* overlay_content) {
    if (!overlay_content) {
        canvas_ = nullptr;
        return false;
    }

    canvas_ = lv_obj_find_by_name(overlay_content, "bed_mesh_canvas");
    if (!canvas_) {
        spdlog::error("[{}] Canvas widget 'bed_mesh_canvas' not found in XML", get_name());
        return false;
    }

    // Belt-and-suspenders against on_ui_destroyed() (full teardown) AND the
    // <if> rebuild path above — whichever deletes this widget, canvas_ ends
    // up null rather than dangling.
    // Remove first for the same reason as the SIZE_CHANGED registration below:
    // create()'s immediate observer fire re-enters against the same canvas.
    lv_obj_remove_event_cb(canvas_, on_canvas_deleted_cb);
    lv_obj_add_event_cb(canvas_, on_canvas_deleted_cb, LV_EVENT_DELETE, this);

    // Wire LV_EVENT_SIZE_CHANGED on overlay_content so the portrait canvas
    // height recomputes whenever the column resizes (rotation, or the
    // initial layout pass). Direct lv_obj_add_event_cb is correct here —
    // SIZE_CHANGED has no XML binding equivalent (same rationale as
    // ui_panel_print_status.cpp:941).
    //
    // Usually overlay_content is fresh — from lv_xml_create() the first time,
    // from the <if> rebuild after each orientation flip. The exception is
    // create(): observe_int_immediate fires at registration, so
    // rewire_after_orientation_flip() runs once against the SAME
    // overlay_content that was just wired here. Remove before adding so that
    // path leaves one registration rather than two.
    lv_obj_remove_event_cb(overlay_content, on_content_size_changed);
    lv_obj_remove_event_cb(overlay_content, on_content_deleted_cb);
    lv_obj_add_event_cb(overlay_content, on_content_size_changed, LV_EVENT_SIZE_CHANGED, this);
    lv_obj_add_event_cb(overlay_content, on_content_deleted_cb, LV_EVENT_DELETE, this);
    content_ = overlay_content;

    return true;
}

void BedMeshPanel::on_canvas_deleted_cb(lv_event_t* e) {
    auto* self = static_cast<BedMeshPanel*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    // Guard against a STALE canvas's delete clobbering a already-rewired
    // canvas_: xml_frag_teardown reparents the old canvas into an off-tree
    // sink and lv_obj_delete_async()s it, so its LV_EVENT_DELETE fires on a
    // LATER tick — by which time rewire_after_orientation_flip() has
    // already, synchronously, pointed canvas_ at the brand-new widget from
    // the rebuild. Without this check, that late DELETE would null out the
    // CURRENT valid canvas_ instead of the dead one that actually triggered
    // it, since every canvas this panel has ever owned shares the same
    // user_data (`this`) on this same callback.
    if (lv_event_get_target(e) == self->canvas_) {
        self->canvas_ = nullptr;
    }
}

void BedMeshPanel::on_content_deleted_cb(lv_event_t* e) {
    auto* self = static_cast<BedMeshPanel*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    // Same stale-widget check as on_canvas_deleted_cb, for the same reason:
    // the condemned overlay_content's async delete lands after
    // rewire_after_orientation_flip() has already pointed content_ at the
    // rebuilt one, and every overlay_content this panel has ever wired shares
    // user_data (`this`) on this callback.
    if (lv_event_get_target(e) == self->content_) {
        self->content_ = nullptr;
    }
}

void BedMeshPanel::apply_canvas_render_settings() {
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
}

void BedMeshPanel::setup_orientation_rewire_observer() {
    lv_subject_t* portrait_subject = lv_xml_get_subject(nullptr, "ui_is_portrait");
    if (!portrait_subject) {
        spdlog::warn("[{}] ui_is_portrait subject not found; portrait <if> rebuild rewiring "
                     "disabled (canvas_ would dangle after a real orientation flip)",
                     get_name());
        return;
    }

    // observe_int_IMMEDIATE, deliberately not the usual observe_int_sync:
    // bed_mesh_panel.xml's own <if cond="ui_is_portrait eq 1"> is ALSO bound
    // to this subject, and its rebuild (xml_frag_rebuild) runs SYNCHRONOUSLY
    // inside lv_subject_set_int() — not deferred. LVGL appends observers to
    // a subject's list with lv_ll_ins_tail and notifies head-to-tail
    // (lv_subject_notify, lv_observer.c), so registration order is
    // notification order: the <if>'s observer was added during the
    // lv_xml_create() call in create(), strictly before this one, so it
    // always fires — and finishes its rebuild — first. observe_int_sync's
    // deferral (helix::ui::queue_update()) would be exactly wrong here: it
    // would leave this rewire running on a LATER tick, after the old
    // overlay_content/canvas_ were already condemned and possibly freed.
    // observe_int_immediate's stated precondition (no observer-lifecycle
    // mutation inside the callback) holds — rewire_after_orientation_flip()
    // only re-finds widgets and adds plain lv_obj_add_event_cb hooks, it
    // never touches an ObserverGuard or a ui_is_portrait subscription.
    portrait_rewire_observer_ = helix::ui::observe_int_immediate<BedMeshPanel>(
        portrait_subject, this,
        [](BedMeshPanel* self, int /*is_portrait*/) { self->rewire_after_orientation_flip(); });
}

void BedMeshPanel::rewire_after_orientation_flip() {
    if (!overlay_root_) {
        return;
    }

    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!wire_canvas_and_content(overlay_content)) {
        spdlog::warn("[{}] Orientation flip: could not re-wire canvas/content", get_name());
        return;
    }
    spdlog::debug("[{}] Orientation flip: re-wired canvas + SIZE_CHANGED on rebuilt content",
                  get_name());

    apply_canvas_render_settings();

    // The rebuild replaced bed_mesh_canvas with a brand-new custom widget
    // instance carrying no mesh data — reload the current one so the panel
    // does not go blank after a flip. Mirrors on_activate()'s deferred
    // reload lambda.
    IMoonrakerAPI* api = get_moonraker_api();
    if (api && api->advanced().has_bed_mesh()) {
        const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
        if (mesh) {
            on_mesh_update_internal(*mesh);
        }
    }

    ensure_async_rendering();

    // The rebuilt tree's canvas_wrapper starts at its XML default height
    // (45%); re-run the portrait sizing decision immediately rather than
    // waiting for a SIZE_CHANGED that may or may not still fire on this
    // exact transition.
    apply_portrait_canvas_height();
}

// ============================================================================
// Portrait canvas sizing
// ============================================================================

void BedMeshPanel::on_content_size_changed(lv_event_t* e) {
    auto* self = static_cast<BedMeshPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->apply_portrait_canvas_height();
    }
}

// Portrait sizes the canvas explicitly rather than flexing it — see
// include/bed_mesh_portrait_layout.h. Below the canvas, current_mesh_card
// and profiles_card sit SIDE BY SIDE in portrait_cards_row (see
// bed_mesh_panel.xml), sharing that row's height — profiles_card fills it
// via height="100%" rather than being a separate stacked block competing
// for its own slice of the column, so only current_mesh_card's own content
// height needs to be reserved here, not a second, independent minimum for
// profiles_card.
void BedMeshPanel::apply_portrait_canvas_height() {
    // Re-entrancy guard: lv_obj_set_height() below can itself emit another
    // SIZE_CHANGED on overlay_content before this call returns. Bail rather
    // than recurse — an unguarded loop here presents as a hung UI, not a
    // crash, since nothing throws or asserts.
    if (!overlay_root_ || applying_portrait_canvas_height_) {
        return;
    }

    // Visual decision: canvas height follows the effective layout so a
    // --layout override reshapes this the same way it reshapes the XML. #1255.
    if (!helix::is_portrait_layout(helix::LayoutManager::instance().type())) {
        return;
    }

    lv_obj_t* wrapper = lv_obj_find_by_name(overlay_root_, "canvas_wrapper");
    lv_obj_t* content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    lv_obj_t* info = lv_obj_find_by_name(overlay_root_, "current_mesh_card");
    if (!wrapper || !content || !info) {
        return;
    }

    // lv_obj_get_height(info) below reads the COMPUTED coord. This call does
    // NOT force that computation here: lv_obj_update_layout() early-returns
    // via its own re-entrancy guard (update_layout_mutex, lv_obj_pos.c:387)
    // whenever a layout pass is already running — and SIZE_CHANGED fires
    // from exactly inside one, so this is always a no-op in this handler's
    // own context. It is harmless rather than wrong: the read below settles
    // correctly on a later pass regardless (LVGL fires SIZE_CHANGED again
    // once the triggering pass finishes propagating), so nothing here
    // depends on this call actually doing anything. current_mesh_card is
    // "content"-sized (not flexed) in portrait_cards_row specifically so
    // that eventual read reflects its true minimum regardless of the row's
    // assigned height — the Max/Min stat rows hide themselves at cramped
    // tiers (bed_mesh_hide_stat_rows in bed_mesh_current_mesh_card.xml), so
    // this height already accounts for that.
    lv_obj_update_layout(content);

    // avail_h is what is left for the canvas AFTER current_mesh_card's own
    // content height and the one gap between the canvas and the row below it
    // — NOT the column's whole content height, which still includes the
    // canvas and would make this circular. column_h IS the whole content
    // height, and the two are deliberately kept distinct:
    // bed_mesh_portrait_canvas_height's floor is a share of column_h, and
    // passing avail_h for both silently zeroes that floor out (see the
    // BED_MESH_PORTRAIT_CANVAS_MIN_PCT comment in bed_mesh_portrait_layout.h).
    //
    // Only current_mesh_card is subtracted, not a separate profiles_card
    // minimum: profiles_card shares this ROW's height (height="100%" at the
    // instantiation site in bed_mesh_panel.xml) rather than stacking below
    // current_mesh_card as its own block, so it never competes for a
    // separate slice of the column — it gets exactly what the row gets,
    // which is never less than current_mesh_card's own content need.
    const int32_t gap = theme_manager_get_spacing("space_md");
    const int32_t band_w = lv_obj_get_width(wrapper);
    const int32_t column_h = lv_obj_get_content_height(content);
    const int32_t avail_h = column_h - lv_obj_get_height(info) - gap;

    const int32_t fitted = helix::bed_mesh_portrait_canvas_height(band_w, avail_h, column_h);

    // Arbitrate against bed_mesh_portrait_canvas_height's own 35%-of-column_h
    // floor, which is a PREFERENCE for a taller, more readable canvas and
    // knows nothing about current_mesh_card/profiles_card sharing the
    // column — on a short enough column it can return more than avail_h,
    // and handing the wrapper that overflows the column by however much it
    // exceeds avail_h. Not overflowing overlay_content (checked via its
    // scroll.bottom, which must stay 0) is the invariant; a flatter canvas
    // is merely degraded. Done here, not by weakening
    // bed_mesh_portrait_canvas_height itself — that function's floor/ceiling
    // contract is correct and independently unit-tested; only this caller
    // knows about the other block sharing the column.
    const int32_t h = (avail_h > 0 && fitted > avail_h) ? avail_h : fitted;

    spdlog::debug("[{}] apply_portrait_canvas_height: band_w={} column_h={} info_h={} gap={} "
                  "avail_h={} fitted={} -> h={} (wrapper currently {})",
                  get_name(), band_w, column_h, lv_obj_get_height(info), gap, avail_h, fitted, h,
                  lv_obj_get_height(wrapper));
    // Skip when the computed height already matches — besides being a no-op,
    // lv_obj_set_height() short-circuits internally when the value is
    // unchanged and would not re-fire SIZE_CHANGED anyway, but checking here
    // keeps the guard explicit rather than relying on that LVGL behavior.
    if (h > 0 && h != lv_obj_get_height(wrapper)) {
        applying_portrait_canvas_height_ = true;
        lv_obj_set_height(wrapper, h);
        applying_portrait_canvas_height_ = false;
    }
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

    // Defer layout computation and data loading to after the current timer cycle.
    // on_activate() runs inside push_overlay()'s queue_update lambda, which fires
    // during lv_timer_handler(). Calling lv_obj_update_layout() here can crash in
    // layout_update_core() because lv_obj_move_foreground() just modified the
    // children array in the same cycle (#417, #419).
    lifetime_.defer("BedMeshPanel::activate_layout", [this]() {
        if (canvas_) {
            lv_obj_update_layout(canvas_);
        }

        IMoonrakerAPI* api = get_moonraker_api();
        if (api && api->advanced().has_bed_mesh()) {
            const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
            if (mesh) {
                on_mesh_update_internal(*mesh);
            }
        }

        if (api) {
            update_profile_list_subjects();
        }

        ensure_async_rendering();
    });
}

void BedMeshPanel::ensure_async_rendering() {
    if (!canvas_) {
        return;
    }

    // Don't start the render thread if there's no mesh data — it would just
    // fail and show "Rendering..." on top of the "No mesh loaded" overlay.
    if (!ui_bed_mesh_has_data(canvas_)) {
        return;
    }

    if (!ui_bed_mesh_is_async_mode(canvas_)) {
        lv_obj_update_layout(canvas_);
        ui_bed_mesh_set_async_mode(canvas_, true);
    }

    lv_obj_invalidate(canvas_);
    ui_bed_mesh_request_async_render(canvas_);
}

void BedMeshPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Stop the render thread when panel is not visible to avoid wasting CPU
    if (canvas_) {
        ui_bed_mesh_set_async_mode(canvas_, false);
    }

    // Call base class
    OverlayBase::on_deactivate();
}

void BedMeshPanel::on_ui_destroyed() {
    canvas_ = nullptr;
    profile_dropdown_ = nullptr;
    calibrate_name_input_ = nullptr;
    rename_name_input_ = nullptr;
    calibrate_modal_widget_ = nullptr;
    rename_modal_widget_ = nullptr;
    save_config_modal_widget_ = nullptr;
    delete_modal_widget_ = nullptr;
    build_volume_observer_.reset();
    portrait_rewire_observer_.reset();
}

// ============================================================================
// Profile List Update
// ============================================================================

void BedMeshPanel::update_profile_list_subjects() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        lv_subject_set_int(&bed_mesh_profile_count_, 0);
        return;
    }

    const auto raw_profiles = api->advanced().get_bed_mesh_profiles();
    const BedMeshProfile* active_mesh = api->advanced().get_active_bed_mesh();
    std::string active_name = active_mesh ? active_mesh->name : "";

    // Filter out the temporary calibration profile used internally
    std::vector<std::string> profiles;
    profiles.reserve(raw_profiles.size());
    for (const auto& p : raw_profiles) {
        if (p != "_hs_temp")
            profiles.push_back(p);
    }

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
    IMoonrakerAPI* api = get_moonraker_api();
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
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] Cannot subscribe to Moonraker - API is null", get_name());
        return;
    }

    auto token = lifetime_.token();

    SubscriptionId id =
        api->subscribe_notifications([this, api, token](nlohmann::json notification) {
            // Check if this notification contains bed_mesh data BEFORE deferring to main thread
            if (!notification.contains("params") || !notification["params"].is_array() ||
                notification["params"].empty()) {
                return;
            }
            const nlohmann::json& params = notification["params"][0];
            if (!params.contains("bed_mesh") || !params["bed_mesh"].is_object()) {
                return;
            }

            // Defer LVGL modifications to main thread
            token.defer("BedMeshPanel::mesh_update", [this, api]() {
                const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
                if (mesh) {
                    on_mesh_update_internal(*mesh);
                }
                update_profile_list_subjects();
            });
        });

    // Store in RAII guard for automatic cleanup on destruction
    subscription_ = SubscriptionGuard(api, id);
    spdlog::debug("[{}] Registered Moonraker callback for mesh updates", get_name());
}

void BedMeshPanel::setup_build_volume_observer() {
    IMoonrakerAPI* api = get_moonraker_api();
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

    IMoonrakerAPI* api = get_moonraker_api();
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
        // Start async rendering now that data is available
        ensure_async_rendering();
    }
}

void BedMeshPanel::on_mesh_update_internal(const BedMeshProfile& mesh) {
    spdlog::debug("[{}] on_mesh_update_internal called, probed_matrix.size={}", get_name(),
                  mesh.probed_matrix.size());

    if (mesh.probed_matrix.empty()) {
        lv_subject_set_int(&bed_mesh_available_, 0);
        lv_subject_copy_string(&bed_mesh_dimensions_, lv_tr("No mesh data"));
        lv_subject_copy_string(&bed_mesh_max_value_, "--");
        // Empty, not "[]" - no coordinates exist without a probed mesh.
        lv_subject_copy_string(&bed_mesh_max_coord_, "");
        lv_subject_copy_string(&bed_mesh_min_value_, "--");
        lv_subject_copy_string(&bed_mesh_min_coord_, "");
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

    // Display raw Z values in stats (what Klipper actually measured). The
    // label itself stays a static "Max"/"Min" (set at init and in the
    // no-mesh fallback above) - only the coordinate sub-line and the
    // measured value change per update.
    //
    // Coordinates are rounded to whole mm (%.0f, not %.1f): with a 7x7 mesh
    // over a ~110mm span, probe points land on fractional coordinates like
    // [33.3, 51.7], and 0.1mm precision on WHERE the high/low spot sits is
    // not actionable for a human looking at a bed. The measured Z VALUE
    // keeps its 3-decimal precision below - only the location rounds.
    std::snprintf(max_coord_buf_, sizeof(max_coord_buf_), "[%.0f, %.0f]", max_x, max_y);
    lv_subject_copy_string(&bed_mesh_max_coord_, max_coord_buf_);
    helix::format::format_distance_mm(max_z, 3, max_value_buf_, sizeof(max_value_buf_));
    lv_subject_copy_string(&bed_mesh_max_value_, max_value_buf_);

    std::snprintf(min_coord_buf_, sizeof(min_coord_buf_), "[%.0f, %.0f]", min_x, min_y);
    lv_subject_copy_string(&bed_mesh_min_coord_, min_coord_buf_);
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
    IMoonrakerAPI* api = get_moonraker_api();
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
        // Start async rendering now that renderer has data (may be first data arrival
        // while panel is already visible — e.g. profile load or WebSocket update)
        ensure_async_rendering();
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
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    const std::string& name = profile_names_[static_cast<size_t>(index)];
    spdlog::info("[{}] Loading profile: {}", get_name(), name);

    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        operation_guard_.begin(SLOW_OPERATION_TIMEOUT_MS, [this] {
            hide_all_modals();
            pending_operation_ = PendingOperation::None;
            NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
        });
        std::string cmd = "BED_MESH_PROFILE LOAD=" + name;
        api->execute_gcode(
            cmd,
            lifetime_.bg_cb("BedMeshPanel::load_refresh",
                            [this, api]() {
                                operation_guard_.end();
                                spdlog::debug("[{}] Profile loaded", get_name());
                                const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
                                if (mesh) {
                                    on_mesh_update_internal(*mesh);
                                }
                                update_profile_list_subjects();
                            }),
            lifetime_.bg_cb("BedMeshPanel::load_error", [this](const MoonrakerError& err) {
                operation_guard_.end();
                spdlog::error("[{}] Failed to load profile: {}", get_name(), err.message);
                NOTIFY_ERROR(lv_tr("Failed to load profile"));
            }));
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

void BedMeshPanel::preheat_for_probing() {
    preheat_turned_on_nozzle_ = false;
    preheat_turned_on_bed_ = false;

    auto& state = get_printer_state();

    // Subject values are decidegrees (value * 10) — target of 0 means heater is off
    int nozzle_target = lv_subject_get_int(state.get_active_extruder_target_subject());
    int bed_target = lv_subject_get_int(state.get_bed_target_subject());

    auto set_temp = [](const std::string& heater, double temp, const char* label) {
        if (auto* c = get_temperature_controller()) {
            c->set_target(heater, temp,
                          {.toast = false, .on_error = [label](const MoonrakerError& err) {
                               spdlog::warn("[BedMeshPanel] Failed to preheat {}: {}", label,
                                            err.message);
                           }});
        }
    };

    if (nozzle_target == 0) {
        preheat_turned_on_nozzle_ = true;
        spdlog::info("[BedMeshPanel] Preheating nozzle to {}°C for probing", PROBE_NOZZLE_TEMP);
        set_temp(state.active_extruder_name(), PROBE_NOZZLE_TEMP, "nozzle");
    } else {
        spdlog::info("[BedMeshPanel] Nozzle already heating (target={}°C), skipping",
                     helix::units::from_decidegrees(nozzle_target));
    }

    if (bed_target == 0) {
        preheat_turned_on_bed_ = true;
        spdlog::info("[BedMeshPanel] Preheating bed to {}°C for probing", PROBE_BED_TEMP);
        set_temp("heater_bed", PROBE_BED_TEMP, "bed");
    } else {
        spdlog::info("[BedMeshPanel] Bed already heating (target={}°C), skipping",
                     helix::units::from_decidegrees(bed_target));
    }
}

void BedMeshPanel::cooldown_after_probing() {
    if (!preheat_turned_on_nozzle_ && !preheat_turned_on_bed_)
        return;

    auto turn_off = [](const std::string& heater, const char* label) {
        spdlog::info("[BedMeshPanel] Turning off {} (was off before probing)", label);
        if (auto* c = get_temperature_controller()) {
            c->set_target(heater, 0.0,
                          {.toast = false, .on_error = [label](const MoonrakerError& err) {
                               spdlog::warn("[BedMeshPanel] Failed to turn off {}: {}", label,
                                            err.message);
                           }});
        }
    };

    if (preheat_turned_on_nozzle_) {
        turn_off(get_printer_state().active_extruder_name(), "nozzle");
        preheat_turned_on_nozzle_ = false;
    }

    if (preheat_turned_on_bed_) {
        turn_off("heater_bed", "bed");
        preheat_turned_on_bed_ = false;
    }
}

void BedMeshPanel::start_calibration() {
    // Prevent double-tap while already probing
    auto current_state =
        static_cast<BedMeshCalibrationState>(lv_subject_get_int(&bed_mesh_calibrate_state_));
    if (current_state == BedMeshCalibrationState::PROBING) {
        spdlog::debug("[BedMeshPanel] Calibration already in progress, ignoring");
        return;
    }

    // Reset state to PROBING
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::PROBING));
    lv_subject_set_int(&bed_mesh_probe_progress_, 0);
    lv_subject_set_int(&bed_mesh_probe_indeterminate_, 0);
    lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Preparing..."));

    // Show modal immediately
    calibrate_modal_widget_ = helix::ui::modal_show("bed_mesh_calibrate_modal");
    spdlog::debug("[BedMeshPanel] Starting calibration, modal shown");

    // Get API
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_calibration_error("API not available");
        return;
    }

    // Preheat nozzle and bed for probing (respects existing targets)
    preheat_for_probing();

    // If we turned on any heaters, wait for them to reach target before proceeding
    if (preheat_turned_on_nozzle_ || preheat_turned_on_bed_) {
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 1);
        lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Heating..."));

        // Build TEMPERATURE_WAIT command for heaters we turned on
        std::string wait_cmd;
        if (preheat_turned_on_nozzle_) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "TEMPERATURE_WAIT SENSOR=%s MINIMUM=%.0f\n",
                          get_printer_state().active_extruder_name().c_str(), PROBE_NOZZLE_TEMP);
            wait_cmd += buf;
        }
        if (preheat_turned_on_bed_) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "TEMPERATURE_WAIT SENSOR=heater_bed MINIMUM=%.0f",
                          PROBE_BED_TEMP);
            wait_cmd += buf;
        }

        spdlog::info("[BedMeshPanel] Waiting for preheat: {}", wait_cmd);

        api->execute_gcode(
            wait_cmd,
            lifetime_.bg_cb("BedMeshPanel::preheat_done",
                            [this]() {
                                spdlog::info(
                                    "[BedMeshPanel] Preheat complete, proceeding to home/probe");
                                start_home_and_probe();
                            }),
            lifetime_.bg_cb("BedMeshPanel::preheat_error",
                            [this](const MoonrakerError& err) {
                                on_calibration_error("Preheat failed: " + err.message);
                            }),
            CALIBRATION_TIMEOUT_MS);
    } else {
        // Heaters already on — go straight to home/probe
        start_home_and_probe();
    }
}

void BedMeshPanel::start_home_and_probe() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_calibration_error("API not available");
        return;
    }

    // Check homing state — auto-home if needed before probing. The
    // indeterminate spinner + "Homing..." text only apply while we are
    // actually about to home, so gate them on the same predicate
    // ensure_homed_then() uses internally.
    if (!toolhead_is_homed(get_printer_state())) {
        spdlog::info("[BedMeshPanel] Not fully homed, sending G28 first");
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 1);
        lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Homing..."));
    }

    ensure_homed_then(
        api, lifetime_,
        [this]() {
            spdlog::info("[BedMeshPanel] Proceeding to calibration");
            start_calibration_probing();
        },
        [this](const MoonrakerError& err) {
            on_calibration_error(err.type == MoonrakerErrorType::TIMEOUT
                                     ? std::string("Homing timed out — printer may still be homing")
                                     : "Homing failed: " + err.message);
        });
}

void BedMeshPanel::start_calibration_probing() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_calibration_error("API not available");
        return;
    }

    lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Preparing..."));

    auto token = lifetime_.token();

    // Query configfile for bed_mesh probe_count before starting calibration.
    // This gives us the expected total for firmware that only emits
    // "probe at X,Y" lines without a "Probing point X/Y" progress line.
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};
    api->get_client().send_jsonrpc(
        "printer.objects.query", params,
        [this, api, token](json response) {
            // BG: parse JSON without touching `this`. Member call (launch_calibration)
            // happens inside the defer below.
            int expected = 0;
            int samples = 1;
            try {
                const auto& settings = response["result"]["status"]["configfile"]["settings"];
                if (settings.contains("bed_mesh") && settings["bed_mesh"].contains("probe_count")) {
                    const auto& pc = settings["bed_mesh"]["probe_count"];
                    if (pc.is_array() && pc.size() >= 2) {
                        expected = pc[0].template get<int>() * pc[1].template get<int>();
                    } else if (pc.is_number_integer()) {
                        int n = pc.template get<int>();
                        expected = n * n; // square grid
                    }
                }
                // Probe samples: each mesh point may be probed multiple times.
                // Check known probe sections — bltouch inherits from probe;
                // load_cell_probe (Centauri Carbon's mainline-Klipper strain
                // gauge module) has its own samples key.
                for (const char* section : {"probe", "bltouch", "load_cell_probe"}) {
                    if (settings.contains(section) && settings[section].contains("samples")) {
                        int s = settings[section]["samples"].template get<int>();
                        if (s > 1) {
                            samples = s;
                            break;
                        }
                    }
                }
            } catch (...) {
                // Non-fatal — proceed with 0 (indeterminate)
            }

            spdlog::info("[BedMeshPanel] Expected probe count from config: {} (samples={})",
                         expected, samples);
            // L081 Mechanism C: launch_calibration is a member fn; marshal to main.
            token.defer("BedMeshPanel::launch_after_query", [this, api, expected, samples]() {
                launch_calibration(api, expected, samples);
            });
        },
        [this, api, token](const MoonrakerError& /*err*/) {
            // L081 Mechanism C: launch_calibration is a member fn; marshal to main.
            token.defer("BedMeshPanel::launch_after_query_err", [this, api]() {
                spdlog::warn("[BedMeshPanel] Failed to query bed_mesh config, "
                             "proceeding without expected probe count");
                launch_calibration(api, 0);
            });
        });
}

void BedMeshPanel::launch_calibration(IMoonrakerAPI* api, int expected_probes, int probe_samples) {
    // Start calibration with progress tracking. All three callbacks fire on the
    // WebSocket thread; bg_cb defers the body to main and re-checks the lifetime
    // generation atomically before invoking.
    api->advanced().start_bed_mesh_calibrate(
        lifetime_.bg_cb("BedMeshPanel::probe_progress",
                        [this](int current, int total) { on_probe_progress(current, total); }),
        lifetime_.bg_cb("BedMeshPanel::calibrate_done", [this]() { on_calibration_complete(); }),
        lifetime_.bg_cb("BedMeshPanel::calibrate_error",
                        [this](const MoonrakerError& err) { on_calibration_error(err.message); }),
        expected_probes, probe_samples);
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
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Deleting profile: {}", get_name(), name);

    operation_guard_.begin(OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
    });

    std::string cmd = "BED_MESH_PROFILE REMOVE=" + name;
    api->execute_gcode(
        cmd,
        lifetime_.bg_cb("BedMeshPanel::delete_done",
                        [this, name]() {
                            operation_guard_.end();
                            spdlog::info("[{}] Profile deleted: {}", get_name(), name);
                            NOTIFY_SUCCESS(lv_tr("Profile deleted"));
                            pending_operation_ = PendingOperation::Delete;
                            show_save_config_modal();
                        }),
        lifetime_.bg_cb("BedMeshPanel::delete_error", [this](const MoonrakerError& err) {
            operation_guard_.end();
            spdlog::error("[{}] Failed to delete profile: {}", get_name(), err.message);
            NOTIFY_ERROR(lv_tr("Failed to delete profile"));
        }));
}

void BedMeshPanel::execute_rename_profile(const std::string& old_name,
                                          const std::string& new_name) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Renaming profile: {} -> {}", get_name(), old_name, new_name);

    operation_guard_.begin(OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
    });

    // Three-step chain: LOAD → SAVE (new name) → REMOVE (old name). Each success
    // body runs on the main thread (via bg_cb), so it's safe to call member
    // helpers like get_moonraker_api() and queue the next gcode from there.
    std::string load_cmd = "BED_MESH_PROFILE LOAD=" + old_name;
    api->execute_gcode(
        load_cmd,
        lifetime_.bg_cb(
            "BedMeshPanel::rename_load_done",
            [this, old_name, new_name]() {
                // Step 2: Save with new name
                IMoonrakerAPI* api2 = get_moonraker_api();
                if (!api2) {
                    operation_guard_.end();
                    return;
                }
                std::string save_cmd = "BED_MESH_PROFILE SAVE=" + new_name;
                api2->execute_gcode(
                    save_cmd,
                    lifetime_.bg_cb(
                        "BedMeshPanel::rename_save_done",
                        [this, old_name, new_name]() {
                            // Step 3: Remove old name
                            IMoonrakerAPI* api3 = get_moonraker_api();
                            if (!api3) {
                                operation_guard_.end();
                                return;
                            }
                            std::string remove_cmd = "BED_MESH_PROFILE REMOVE=" + old_name;
                            api3->execute_gcode(
                                remove_cmd,
                                lifetime_.bg_cb("BedMeshPanel::rename_done",
                                                [this, old_name, new_name]() {
                                                    operation_guard_.end();
                                                    spdlog::info("[{}] Profile renamed: {} -> {}",
                                                                 get_name(), old_name, new_name);
                                                    NOTIFY_SUCCESS(lv_tr("Profile renamed"));
                                                    pending_operation_ = PendingOperation::Rename;
                                                    show_save_config_modal();
                                                }),
                                lifetime_.bg_cb(
                                    "BedMeshPanel::rename_remove_error",
                                    [this](const MoonrakerError& err) {
                                        operation_guard_.end();
                                        spdlog::error("[{}] Failed to remove old profile: {}",
                                                      get_name(), err.message);
                                        NOTIFY_ERROR(lv_tr("Rename failed at remove step"));
                                    }));
                        }),
                    lifetime_.bg_cb("BedMeshPanel::rename_save_error",
                                    [this](const MoonrakerError& err) {
                                        operation_guard_.end();
                                        spdlog::error("[{}] Failed to save new profile: {}",
                                                      get_name(), err.message);
                                        NOTIFY_ERROR(lv_tr("Rename failed at save step"));
                                    }));
            }),
        lifetime_.bg_cb("BedMeshPanel::rename_load_error", [this](const MoonrakerError& err) {
            operation_guard_.end();
            spdlog::error("[{}] Failed to load profile for rename: {}", get_name(), err.message);
            NOTIFY_ERROR(lv_tr("Rename failed at load step"));
        }));
}

void BedMeshPanel::execute_calibration(const std::string& /*profile_name*/) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    // Probe into a temporary profile so we don't clobber an existing "default"
    // mesh. save_profile_with_name() renames this to the user's chosen name
    // after probing completes.
    static constexpr const char* TEMP_PROFILE = "_hs_temp";

    // A printer may declare a multi-line gcode template in the database
    // (calibration.bed_mesh_gcode). Used today by the Elegoo Centauri Carbon,
    // whose mainline-Klipper [load_cell_probe] aborts on stale tare; the
    // template tares first, runs the vendor's wipe wrapper, then saves the
    // current mesh under {profile}.
    const std::string& printer_name = get_printer_state().get_printer_type();
    std::string cmd = PrinterDetector::get_bed_mesh_calibrate_gcode(printer_name);

    std::string macro_name;
    if (!cmd.empty()) {
        const std::string placeholder = "{profile}";
        for (size_t pos = cmd.find(placeholder); pos != std::string::npos;
             pos = cmd.find(placeholder, pos + 1)) {
            cmd.replace(pos, placeholder.size(), TEMP_PROFILE);
        }
        macro_name = "<calibration.bed_mesh_gcode>";
    } else {
        // Resolve the calibration macro via StandardMacros (user-configurable,
        // defaults to auto-detected BED_MESH_CALIBRATE or G29)
        const auto& macro_info = StandardMacros::instance().get(StandardMacroSlot::BedMesh);
        macro_name = macro_info.get_macro();
        if (macro_name.empty()) {
            macro_name = "BED_MESH_CALIBRATE"; // Fallback if nothing configured/detected
        }
        cmd = macro_name + " PROFILE=" + std::string(TEMP_PROFILE);
    }

    spdlog::info("[{}] Starting calibration (temp profile: {}, macro: {})", get_name(),
                 TEMP_PROFILE, macro_name);
    lv_subject_set_int(&bed_mesh_calibrating_, 1);

    api->execute_gcode(
        cmd,
        lifetime_.bg_cb("BedMeshPanel::calibrate_gcode_done",
                        [this]() {
                            spdlog::info("[{}] Calibration started", get_name());
                            NOTIFY_INFO(lv_tr("Calibration started"));
                        }),
        lifetime_.bg_cb(
            "BedMeshPanel::calibrate_gcode_error",
            [this](const MoonrakerError& err) {
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    spdlog::warn("[{}] Calibration response timed out (may still be running)",
                                 get_name());
                    NOTIFY_WARNING(lv_tr("Calibration may still be running — response timed out"));
                } else {
                    spdlog::error("[{}] Failed to start calibration: {}", get_name(), err.message);
                    NOTIFY_ERROR(lv_tr("Failed to start calibration"));
                    lv_subject_set_int(&bed_mesh_calibrating_, 0);
                }
            }),
        CALIBRATION_TIMEOUT_MS);
}

void BedMeshPanel::execute_save_config() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Saving config (will restart Klipper)", get_name());

    // SAVE_CONFIG triggers an expected Klipper restart
    helix::ui::begin_expected_klippy_restart("Configuration saved - restarting");

    operation_guard_.begin(SLOW_OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
    });

    api->execute_gcode(
        "SAVE_CONFIG",
        lifetime_.bg_cb("BedMeshPanel::save_config_done",
                        [this]() {
                            operation_guard_.end();
                            spdlog::info("[{}] SAVE_CONFIG sent - Klipper will restart",
                                         get_name());
                        }),
        lifetime_.bg_cb("BedMeshPanel::save_config_error", [this](const MoonrakerError& err) {
            operation_guard_.end();
            spdlog::error("[{}] Failed to save config: {}", get_name(), err.message);
            NOTIFY_ERROR(lv_tr("Failed to save configuration"));
        }));
}

// ============================================================================
// Calibration Progress Handlers
// ============================================================================

void BedMeshPanel::on_probe_progress(int current, int total) {
    if (total > 0) {
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 0);
        // Cap at 99% — completion signal handles the final transition.
        // Adaptive mesh may produce more probes than the config hint.
        int progress = std::min(current * 100 / total, 99);
        lv_subject_set_int(&bed_mesh_probe_progress_, progress);

        const std::string text = fmt::format(lv_tr("Probing point {} of {}"), current, total);
        std::snprintf(probe_text_buf_, sizeof(probe_text_buf_), "%s", text.c_str());
        spdlog::debug("[BedMeshPanel] Probe progress: {}/{} ({}%)", current, total, progress);
    } else {
        // Fallback: firmware didn't report total — show spinner instead of bar
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 1);
        const std::string text = fmt::format(lv_tr("Probing... ({} points)"), current);
        std::snprintf(probe_text_buf_, sizeof(probe_text_buf_), "%s", text.c_str());
        spdlog::debug("[BedMeshPanel] Probe progress: {} points (total unknown)", current);
    }
    lv_subject_copy_string(&bed_mesh_probe_text_, probe_text_buf_);
}

void BedMeshPanel::on_calibration_complete() {
    spdlog::info("[BedMeshPanel] Calibration complete, transitioning to naming state");
    cooldown_after_probing();
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::NAMING));
}

/// Strip Klipper error prefixes and make messages user-friendly
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
            // Find the value after "message":
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

void BedMeshPanel::on_calibration_error(const std::string& message) {
    cooldown_after_probing();
    spdlog::error("[BedMeshPanel] Calibration error: {}", message);
    std::string clean = sanitize_error_message(message);
    std::strncpy(error_message_buf_, clean.c_str(), sizeof(error_message_buf_) - 1);
    error_message_buf_[sizeof(error_message_buf_) - 1] = '\0';
    lv_subject_copy_string(&bed_mesh_error_message_, error_message_buf_);
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::ERROR));
}

void BedMeshPanel::handle_emergency_stop() {
    spdlog::warn("[BedMeshPanel] Emergency stop during bed mesh calibration");

    // E-stop kills all heaters — just reset preheat tracking flags
    preheat_turned_on_nozzle_ = false;
    preheat_turned_on_bed_ = false;

    // Suppress recovery dialog — user intentionally triggered E-Stop from this modal
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);

    IMoonrakerAPI* api = get_moonraker_api();
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

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        hide_all_modals();
        return;
    }

    // The calibration probed into "_hs_temp" to avoid clobbering existing
    // profiles. Now save the mesh under the user's chosen name and remove
    // the temporary profile.
    static constexpr const char* TEMP_PROFILE = "_hs_temp";

    std::string cmd = "BED_MESH_PROFILE SAVE=" + name;
    api->execute_gcode(
        cmd,
        lifetime_.bg_cb("BedMeshPanel::save_profile_done",
                        [this, name, api]() {
                            // Clean up the temporary calibration profile
                            api->execute_gcode(std::string("BED_MESH_PROFILE REMOVE=") +
                                                   TEMP_PROFILE,
                                               nullptr, nullptr);
                            spdlog::info("[BedMeshPanel] Profile saved: {}", name);
                            NOTIFY_SUCCESS(lv_tr("Mesh saved as '{}'"), name);
                            hide_all_modals();
                            lv_subject_set_int(&bed_mesh_calibrate_state_,
                                               static_cast<int>(BedMeshCalibrationState::IDLE));
                            pending_operation_ = PendingOperation::Calibrate;
                            show_save_config_modal();
                        }),
        lifetime_.bg_cb("BedMeshPanel::save_profile_error", [this](const MoonrakerError& err) {
            spdlog::error("[BedMeshPanel] Failed to save profile: {}", err.message);
            NOTIFY_ERROR(lv_tr("Failed to save profile"));
            hide_all_modals();
        }));
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
