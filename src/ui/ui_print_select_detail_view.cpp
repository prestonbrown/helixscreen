// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_detail_view.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_exclude_object_badges.h"
#include "ui_filename_utils.h"
#include "ui_gcode_viewer.h"
#include "ui_icon.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_print_preparation_manager.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_remap.h"
#include "ams_state.h"
#include "app_globals.h"
#include "bed_dimensions.h"
#include "color_utils.h"
#include "config.h"
#include "display_settings_manager.h"
#include "filament_chip_model.h"
#include "file_analysis.h"
#include "gcode_footer_summary.h"
#include "gcode_parser.h"
#include "gcode_preview_fetcher.h"
#include "gcode_preview_gate.h"
#include "gcode_preview_setup.h"
#include "gcode_temp_reclaim.h"
#include "host_identity.h"
#include "http_executor.h"
#include "i18n_plural.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_utils.h"
#include "moonraker_types.h"
#include "moonraker_validation.h"
#include "observer_factory.h"
#include "plugin_menu_context.h"
#include "pre_start_exclude.h"
#include "print_detail_layout.h"
#include "print_status_preview_decision.h"
#include "queued_job_start.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "text_io.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix::ui {

namespace tio = ::helix::text_io;

// ============================================================================
// Static instance pointer for callback access
// ============================================================================

// Static instance pointer for the helper functions in this TU (currently
// just update_prep_time_label) to reach the live detail view. Only one
// detail view exists at a time; set during init_subjects() / cleared in the
// destructor.
static PrintSelectDetailView* s_detail_view_instance = nullptr;

// ============================================================================
// Static callback declarations
// ============================================================================

// Forward decl for the prep-time estimate refresh that runs after every
// toggle. Defined later in this TU.
static void update_prep_time_label();

// ============================================================================
// Lifecycle
// ============================================================================

PrintSelectDetailView::~PrintSelectDetailView() {
    // Clear static instance pointer
    if (s_detail_view_instance == this) {
        s_detail_view_instance = nullptr;
    }

    // lifetime_ destructor auto-invalidates all outstanding tokens

    // Clean up temp gcode file
    if (!temp_gcode_path_.empty()) {
        reclaim_download(temp_gcode_path_);
        temp_gcode_path_.clear();
    }

    // CRITICAL: During static destruction (app exit), LVGL may already be gone.
    // We check if LVGL is still initialized before calling any LVGL functions.
    if (!lv_is_initialized()) {
        spdlog::trace("[DetailView] Destroyed (LVGL already deinit)");
        return;
    }

    spdlog::trace("[DetailView] Destroyed");

    // The tree can outlive this view; its delete event must not reach a freed `this`.
    uninstall_root_delete_hook();

    // Cancel the pre-flight readiness safety timer if still armed (LVGL is known
    // initialized here — checked above).
    if (preflight_ready_timeout_timer_) {
        lv_timer_delete(preflight_ready_timeout_timer_);
        preflight_ready_timeout_timer_ = nullptr;
    }

    // Same for the indexing-progress poll. StaticPanelRegistry::destroy_all()
    // runs BEFORE lv_deinit(), so a timer cancelled only in the deactivate path
    // stays armed on a freed `this` when teardown skips it (#1173).
    cancel_progress_timer();

    disarm_viewer_callbacks();

    // A tree deleted externally leaves overlay_root_ naming freed memory.
    if (tree_deleted_) {
        helix::nav::clear_on_close(overlay_root_);
        helix::nav::unregister_overlay(overlay_root_);
        overlay_root_ = nullptr;
    }

    // Unregister from NavigationManager (fallback if cleanup() wasn't called)
    if (overlay_root_) {
        helix::nav::unregister_overlay(overlay_root_);
    }

    // Deinitialize subjects to disconnect observers before widgets are deleted
    // This prevents dangling pointers and frees observer linked lists
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clean up confirmation dialog if open
    if (confirmation_dialog_widget_) {
        helix::ui::modal_hide(confirmation_dialog_widget_);
        confirmation_dialog_widget_ = nullptr;
    }

    // Destructors can run from inside a queue_update() batch (unique_ptr
    // reassignment, owner reset()); a sync lv_obj_delete() there corrupts
    // LVGL's event list if another sync delete lands in the same batch
    // (#776, #190, #80). Deferred delete is safe outside a batch too.
    helix::ui::safe_delete_deferred(overlay_root_);
}

// ============================================================================
// Setup
// ============================================================================

void PrintSelectDetailView::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[DetailView] Subjects already initialized, skipping");
        return;
    }

    // Set static instance pointer for callbacks (must be before callback registration)
    s_detail_view_instance = this;

    // Per-option toggle callbacks are wired imperatively in
    // populate_option_rows() once the dynamic rows are created. The previous
    // hardcoded XML <event_cb callback="on_preprint_*_toggled"/> bindings are
    // gone — there is nothing to register here.

    // G-code viewer visibility mode (0=thumbnail, 1=3D, 2=2D)
    UI_MANAGED_SUBJECT_INT(detail_gcode_viewer_mode_, 0, "detail_gcode_viewer_mode", subjects_);
    // G-code loading indicator (0=hidden, 1=visible)
    UI_MANAGED_SUBJECT_INT(detail_gcode_loading_, 0, "detail_gcode_loading", subjects_);

    // Indexing progress, 0-100. 0 means "no figure to show" — the XML falls
    // back to the indeterminate spinner there, matching wizard_input_shaper's
    // determinate/indeterminate pair. Only the streaming path can report: it
    // scans the whole file to build a layer index before anything renders, and
    // on a large print that IS the wait (69s for a 133MB / 5M-line file on a
    // K2 Plus, during which the panel showed a bare spinner).
    UI_MANAGED_SUBJECT_INT(detail_gcode_progress_, 0, "detail_gcode_progress", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_gcode_progress_text_, detail_gcode_progress_text_buf_, "",
                              "detail_gcode_progress_text", subjects_);
    // Whether the viewer has rendered its first frame (0=no, 1=yes). The
    // thumbnail stays on top until this flips to hide the gray gap.
    UI_MANAGED_SUBJECT_INT(detail_viewer_first_frame_, 0, "detail_viewer_first_frame", subjects_);
    UI_MANAGED_SUBJECT_INT(detail_viewer_hidden_, 1, "detail_viewer_hidden", subjects_);

    // Preview color mode: 0 = actual (loaded slot colors), 1 = sliced (slicer intent)
    UI_MANAGED_SUBJECT_INT(detail_prefer_sliced_colors_, 0, "detail_prefer_sliced_colors",
                           subjects_);

    // Mapping/swatch readiness (0 = skeleton, 1 = authoritative chips rendered).
    // Mirrors is_preflight_ready() — the same readiness the print-start gate
    // waits on. Cache hit => 1 immediately at show(); else flips when the tools
    // scan or viewer parse completes.
    UI_MANAGED_SUBJECT_INT(detail_mapping_ready_, 0, "detail_mapping_ready", subjects_);

    // The chips print_file_detail.xml repeats.
    FilamentMappingCard::init_subjects();

    // Filament mapping card visibility (0=hidden, 1=visible). Driven by
    // FilamentMappingCard::should_show() after each update(); XML binds
    // via bind_flag_if_eq in print_file_detail.xml.
    UI_MANAGED_SUBJECT_INT(filament_mapping_visible_, 0, "filament_mapping_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(gcode_rewrite_available_, -1, "detail_gcode_rewrite_available",
                           subjects_); // SUBJECT_OK: option rows bind it via the renderer lookup

    // Whether a tap on the filament card opens the remap picker (0=no,
    // 1=yes). Binds the card's chevron AND its clickable flag in
    // print_file_detail.xml, so the affordance and the behaviour move together.
    // Published alongside filament_mapping_visible_ by
    // publish_card_visibility(), against the same backend snapshot.
    UI_MANAGED_SUBJECT_INT(color_card_remappable_, 0, "color_card_remappable", subjects_);
    // The three that describe a refusal. Published from the same call as the
    // chevron so a card cannot be greyed while its chevron still invites a tap.
    UI_MANAGED_SUBJECT_INT(color_card_remap_needs_setup_, 0, "color_card_remap_needs_setup",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(color_card_remap_help_visible_, 0, "color_card_remap_help_visible",
                           subjects_);

    UI_MANAGED_SUBJECT_INT(detail_fit_severity_, 0, "detail_fit_check_severity", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_fit_text_, detail_fit_text_buf_, "", "detail_fit_check_text",
                              subjects_);

    // Options scroll-area overflow (0=everything visible, 1=content hidden
    // below the visible bottom edge). print_file_detail.xml shows the bottom
    // fade cue on this; update_options_more_below() is the only writer.
    UI_MANAGED_SUBJECT_INT(detail_options_more_below_, 0, "detail_options_more_below", subjects_);

    // Pre-print time estimate (formatted string for bind_text); the int twin
    // hides the whole line when no estimate exists, because an empty string
    // still occupies a row in the layout.
    UI_MANAGED_SUBJECT_STRING(prep_time_estimate_subject_, prep_time_estimate_buf_, "",
                              "preprint_estimate_text", subjects_);
    UI_MANAGED_SUBJECT_INT(preprint_estimate_visible_, 0, "preprint_estimate_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(print_detail_has_plugin_items_, 0, "print_detail_has_plugin_items",
                           subjects_);

    // Re-color the preview live when a slot's loaded color/presence changes
    // (filament reloaded). Static singleton subject -> plain ObserverGuard, no
    // lifetime token. Handler no-ops while the view is closed.
    slots_version_observer_ = observe<int>(
        AmsState::instance().get_slots_version_subject(), this,
        [](PrintSelectDetailView* self, int /*version*/) { self->on_ams_state_changed(); },
        AmsState::instance().get_subjects_lifetime());
    // The printer's tool table moving changes no slot, so it has its own
    // version. Un-picked mappings follow it.
    tool_map_version_observer_ = observe<int>(
        AmsState::instance().get_tool_map_version_subject(), this,
        [](PrintSelectDetailView* self, int /*version*/) { self->on_ams_state_changed(); },
        AmsState::instance().get_subjects_lifetime());
    // The loaded chips of a printer with no filament system compare against the
    // external spool, which every spool edit notifies through its colour.
    external_spool_observer_ = observe<int>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](PrintSelectDetailView* self, int /*rgb*/) { self->on_ams_state_changed(); },
        AmsState::instance().get_subjects_lifetime());
    // The auto-color-map setting picks the seed, so the un-picked rows follow it
    // as soon as it flips (the remap modal's toggle writes it, and its Cancel
    // writes it back).
    auto_color_map_observer_ = observe<int>(
        helix::SettingsManager::instance().subject_auto_color_map(), this,
        [](PrintSelectDetailView* self, int /*on*/) { self->on_ams_state_changed(); },
        helix::SettingsManager::instance().get_subjects_lifetime());

    // Both land after the card has already published: the plugin probe completes
    // after first paint, and the Moonraker version arrives with discovery.
    // Without these the card keeps whatever it decided before either was known.
    plugin_installed_observer_ = observe<int>(
        get_printer_state().plugin_status_state().get_helix_plugin_installed_subject(), this,
        [](PrintSelectDetailView* self, int /*state*/) {
            self->publish_rewrite_availability();
            self->publish_card_visibility();
        },
        get_printer_state().get_subjects_lifetime());
    moonraker_degraded_observer_ = observe<int>(
        get_printer_state().versions_state().get_moonraker_history_degraded_subject(), this,
        [](PrintSelectDetailView* self, int /*degraded*/) { self->publish_card_visibility(); },
        get_printer_state().get_subjects_lifetime());

    // Pre-start object picks: a private model and the skip button's subjects.
    exclude_objects_.init_subjects(false);
    UI_MANAGED_SUBJECT_INT(detail_exclude_available_, 0, "detail_exclude_available", subjects_);
    UI_MANAGED_SUBJECT_INT(detail_exclude_pick_count_, 0, "detail_exclude_pick_count", subjects_);
    UI_MANAGED_SUBJECT_STRING(detail_exclude_pick_count_text_, detail_exclude_pick_count_text_buf_,
                              "", "detail_exclude_pick_count_text", subjects_);
    exclude_picks_observer_ = observe<int>(
        exclude_objects_.get_excluded_objects_version_subject(), this,
        [](PrintSelectDetailView* self, int) { self->publish_exclude_picks(); },
        exclude_objects_.get_subjects_lifetime());
    // A printer switch can add or drop [exclude_object] while a file is open.
    exclude_capability_observer_ = observe<int>(
        get_printer_state().capabilities_state().subject(helix::Capability::HasExcludeObject), this,
        [](PrintSelectDetailView* self, int) { self->refresh_exclude_objects(); },
        get_printer_state().get_subjects_lifetime());

    subjects_initialized_ = true;
    publish_rewrite_availability();
    spdlog::debug("[DetailView] Initialized pre-print option subjects");
}

lv_obj_t* PrintSelectDetailView::create(lv_obj_t* parent_screen) {
    if (!parent_screen) {
        spdlog::error("[DetailView] Cannot create: parent_screen is null");
        return nullptr;
    }

    if (overlay_root_) {
        spdlog::warn("[DetailView] Detail view already exists");
        return overlay_root_;
    }

    parent_screen_ = parent_screen;

    overlay_root_ = helix::ui::create_xml_hidden(parent_screen_, "print_file_detail");

    if (!overlay_root_) {
        LOG_ERROR_INTERNAL("[DetailView] Failed to create detail view from XML");
        NOTIFY_ERROR(lv_tr("Failed to load file details"));
        return nullptr;
    }

    // A rebuilt view can reach here with the hook still on its previous root,
    // whose deletion is deferred; take it off before tracking the new one.
    uninstall_root_delete_hook();
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(overlay_root_, on_root_deleted, LV_EVENT_DELETE, this);
    delete_hook_root_ = overlay_root_;

    // Set responsive padding for content area
    lv_obj_t* content_container = find_required(overlay_root_, "content_container", get_name());
    if (content_container) {
        lv_coord_t padding = ui_get_header_content_padding();
        lv_obj_set_style_pad_all(content_container, padding, 0);
    }

    // Store reference to print button for enable/disable state management
    print_button_ = find_required(overlay_root_, "print_button", get_name());

    // Find and configure G-code viewer widget
    gcode_viewer_ = lv_obj_find_by_name(overlay_root_, "detail_gcode_viewer");
    if (gcode_viewer_) {
        spdlog::debug("[DetailView] G-code viewer widget found");
        helix::ui::gcode_viewer_disable_streaming(gcode_viewer_);

        helix::ui::apply_preview_render_mode(gcode_viewer_, "DetailView");

        // Here the strip IS an overlay over the preview's bottom, so this is a
        // real occlusion (~a third of the card) and the render shifts to clear it.
        helix::ui::set_preview_bottom_occluder(
            gcode_viewer_,
            helix::ui::find_required(overlay_root_, "detail_metadata_clip", get_name()));

        // Frame the render the way the thumbnail beside it is framed (lifted
        // square, model centre at 55%), so swapping thumbnail for render is
        // not a jump in size or position. Parity mode ignores the occluder for
        // the fit; the occluder stays set because its measurement still drives
        // the content-offset refresh plumbing.
        helix::ui::gcode_viewer_set_thumbnail_parity(gcode_viewer_, true);

        // Start paused — will resume in on_activate()
        helix::ui::gcode_viewer_set_paused(gcode_viewer_, true);

        // Memory-pressure responder calls helix::ui::gcode_viewer_clear_all_active();
        // flip the mode subject back to thumbnail so the user sees the slicer
        // preview rather than a transparent rectangle.
        helix::ui::gcode_viewer_set_clear_callback(
            gcode_viewer_,
            [](lv_obj_t*, void* ud) {
                auto* self = static_cast<PrintSelectDetailView*>(ud);
                // Its map and badges belong to the parse just freed; picks stay.
                self->exclude_mode_.hide();
                self->show_gcode_viewer(false);
                self->gcode_loaded_ = false;
                // gcode_loaded_ flipping false can drop readiness (when the
                // headless scan hasn't finished) — keep the skeleton latch in
                // sync with is_preflight_ready().
                self->publish_mapping_ready();
            },
            this);
    }

    // The pre-print option rows are populated dynamically from the active
    // printer's PrePrintOptionSet — see populate_option_rows(). The
    // hardcoded checkbox widgets that used to live in the XML are gone;
    // their pointers exist only as inert nullptr fields kept on the class
    // for backward compatibility with external callers (none today).
    bed_mesh_checkbox_ = nullptr;
    qgl_checkbox_ = nullptr;
    z_tilt_checkbox_ = nullptr;
    nozzle_clean_checkbox_ = nullptr;
    purge_line_checkbox_ = nullptr;
    timelapse_checkbox_ = nullptr;
    pre_print_options_container_ =
        find_required(overlay_root_, "pre_print_options_container", get_name());

    // The options scroll area feeds two measured rules: the more-below cue
    // (scroll bottom > 0) and the portrait preview height. Scroll, size and
    // layout events have no XML trigger; both handlers only measure and
    // publish, the cue's visibility stays bound in XML. LAYOUT_CHANGED on the
    // scroll area is the one event that also fires when the CONTENT grows
    // without any scrolling (option tiles or filament rows arriving after the
    // view is built), which scroll/size events alone would miss.
    options_scroll_ = find_required(overlay_root_, "detail_options_scroll", get_name());
    detail_card_ = find_required(overlay_root_, "detail_card", get_name());
    if (options_scroll_) {
        for (lv_event_code_t code : {LV_EVENT_SCROLL, LV_EVENT_SCROLL_END, LV_EVENT_SIZE_CHANGED,
                                     LV_EVENT_LAYOUT_CHANGED}) {
            lv_obj_add_event_cb(
                options_scroll_,
                [](lv_event_t* e) {
                    static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e))
                        ->defer_detail_fit();
                },
                code, this);
        }
    }

    // Portrait preview sizing is measured layout: it depends on runtime pixel
    // sizes of the scroll content, so it reacts to the container's layout
    // rather than any subject. The container was looked up above for its
    // responsive padding; the same object carries this event.
    if (content_container) {
        lv_obj_add_event_cb(
            content_container,
            [](lv_event_t* e) {
                static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e))->defer_detail_fit();
            },
            LV_EVENT_LAYOUT_CHANGED, this);
    }

    // Look up and initialize the one filament card. Its header and row
    // declare clickable=false + event_bubble=true in print_file_detail.xml
    // (L071), so the card receives every tap outside a chip; a chip takes its
    // own tap (filament_chip_clicked_cb). FilamentMappingCard
    // ::create() installs the CLICKED handler (allowed exception) — deliberately
    // NOT a second lv_obj_add_event_cb here, which would fire the remap opener
    // twice per tap. Whether the card is clickable at all, and whether it shows
    // the chevron that says so, both come from color_card_remappable.
    lv_obj_t* mapping_card = find_required(overlay_root_, "filament_mapping_card", get_name());
    lv_obj_t* mapping_rows = find_required(overlay_root_, "filament_mapping_rows", get_name());
    lv_obj_t* mapping_warning = lv_obj_find_by_name(overlay_root_, "filament_mapping_warning");
    filament_mapping_card_.create(mapping_card, mapping_rows, mapping_warning);
    // Route the card tap to the panel's single remap opener instead of the
    // card's own internal modal, so there is ONE opener and ONE modal instance
    // for every backend (AFC/CFS card tap, U1 chip tap, preflight "Map Filament…" all
    // reach PrintSelectPanel::open_remap_modal()).
    filament_mapping_card_.set_on_tap([this]() { on_color_card_clicked(); });
    // A chip takes its own tap: the same remap gate, then that tool's dropdown.
    filament_mapping_card_.set_on_chip_tap(
        [this](size_t chip, lv_obj_t* trigger) { on_chip_clicked(chip, trigger); });
    // The gate is asked again when a pick lands: the backend or the plugin may
    // have changed while the dropdown was open.
    filament_mapping_card_.set_pick_gate(
        [this]() { return remap_tap_allowed("chip pick") && print_carries_out_chip_pick(); });
    filament_mapping_card_.set_on_mappings_changed([this]() {
        // Fired by set_mappings() AFTER it repaints the chips, so this is only
        // the downstream work: re-colour the preview and re-gate pre-flight.
        // Synchronous, on the main thread, so a direct call is safe.
        refresh_preview_colors_and_mismatch();
    });

    // Look up history status display
    history_status_row_ = find_required(overlay_root_, "history_status_row", get_name());
    history_status_icon_ = find_required(overlay_root_, "history_status_icon", get_name());
    history_status_label_ = find_required(overlay_root_, "history_status_label", get_name());

    spdlog::debug("[DetailView] Detail view created");
    return overlay_root_;
}

void PrintSelectDetailView::set_dependencies(IMoonrakerAPI* api, PrinterState* printer_state) {
    set_analysis_dependencies(api, printer_state);

    // Ask once, well before the G-code is needed, and never block on the
    // answer. A printer that never replies simply leaves us on the HTTP path.
    resolve_local_gcodes_root();
}

void PrintSelectDetailView::set_analysis_dependencies(IMoonrakerAPI* api,
                                                      PrinterState* printer_state) {
    api_ = api;
    gcode_fetcher_.set_api(api);
    facts_loader_.set_api(api);
    printer_state_ = printer_state;
    publish_rewrite_availability(); // the transport is half the answer

    // Not tied to the widget tree: the panel wires dependencies at setup, before
    // any file opens, so the PRINT_START analysis starts on connect, and the
    // manager survives destroy-on-close with the callbacks PrintSelectPanel set.
    if (!prep_manager_) {
        prep_manager_ = std::make_unique<PrintPreparationManager>();
        // A scan answer can be what a deferred Print tap is waiting on.
        prep_manager_->set_on_scan_answered([this]() {
            refresh_exclude_objects();
            refresh_fit_check();
            notify_print_readiness_changed();
            fire_on_preflight_ready();
        });
        // Macro rows come and go with the analysis, so an open view rebuilds
        // them now; a hidden one rebuilds in on_activate().
        prep_manager_->set_macro_analysis_callback(
            [this](const helix::PrintStartAnalysis& analysis) {
                if (is_visible()) {
                    populate_option_rows();
                }
                if (on_macro_analysis_cb_) {
                    on_macro_analysis_cb_(analysis);
                }
            });
    }
    // Per-option toggle state flows through the OptionStateProvider that
    // populate_option_rows() registers with the prep manager.
    prep_manager_->set_dependencies(api_, printer_state_);
}

// ============================================================================
// Visibility
// ============================================================================

void PrintSelectDetailView::set_file_palette(const std::vector<std::string>& filament_colors,
                                             const std::vector<std::string>& filament_materials) {
    current_filament_colors_ = filament_colors;
    current_filament_materials_ = filament_materials;
    palette_settled_ = !current_filament_colors_.empty();
    filament_mapping_card_.update(filament_colors, filament_materials);
    publish_card_visibility();
    publish_material_text();
}

void PrintSelectDetailView::set_card_used_tools(std::optional<std::set<int>> used) {
    filament_mapping_card_.set_used_tools(std::move(used));
    publish_material_text();
}

void PrintSelectDetailView::publish_material_text() {
    if (!material_text_subject_) {
        return;
    }
    // The chips' own used-tool set, so the row and the chips name the same tools.
    // Empty reads as not known yet, as it does for the card.
    const auto& used = filament_mapping_card_.used_tools();
    const std::string text =
        helix::used_materials_text(current_filament_materials_,
                                   used && !used->empty() ? used : std::optional<std::set<int>>{});
    // A file naming no material keeps the panel's text (its filament name).
    if (!text.empty()) {
        lv_subject_copy_string(material_text_subject_, text.c_str());
    }
}

void PrintSelectDetailView::show(const std::string& filename, const std::string& current_path,
                                 const std::string& filament_type,
                                 const std::vector<std::string>& filament_colors,
                                 const std::vector<std::string>& filament_materials,
                                 size_t file_size_bytes, time_t modified_timestamp,
                                 uint64_t gcode_end_byte, const std::string& local_path) {
    reclaim_deleted_tree();

    // Lazy re-create widget tree if it was destroyed by destroy-on-close
    if (!overlay_root_ && parent_screen_) {
        spdlog::info("[DetailView] Re-creating widget tree (destroy-on-close recovery)");
        if (!create(parent_screen_)) {
            spdlog::error("[DetailView] Failed to re-create detail view");
            return;
        }
        // Re-wire dependencies (subjects need re-binding to new widgets)
        if (api_ || printer_state_) {
            set_dependencies(api_, printer_state_);
        }
    }

    if (!overlay_root_) {
        spdlog::warn("[DetailView] Cannot show: widget not created");
        return;
    }

    // No-op once resolved; retries an attempt that met a connection still coming up.
    resolve_local_gcodes_root();

    // Picks belong to one file; a hold for a print start keeps them for that file only.
    const bool same_file = filename == current_filename_ && current_path == current_path_;
    if (!same_file) {
        exclude_mode_.hide();
    }
    if (!(picks_held_for_start_ && same_file)) {
        exclude_objects_.clear_objects();
    }
    picks_held_for_start_ = false;

    // Cache parameters for on_activate() to use
    current_filename_ = filename;
    current_path_ = current_path;
    current_local_path_ = local_path;
    current_filament_type_ = filament_type;
    current_filament_colors_ = filament_colors;
    current_filament_materials_ = filament_materials;
    current_file_version_ = {static_cast<uint64_t>(file_size_bytes), modified_timestamp};
    current_gcode_end_byte_ = gcode_end_byte;

    // Clear cached metadata when file selection changes — the new async fetch will repopulate it
    cached_file_metadata_.reset();

    // Decided from declarations, not checks: the checks run when the more button is
    // tapped, against the metadata as it stands then.
    lv_subject_set_int(
        &print_detail_has_plugin_items_,
        helix::plugin_menu::any_declared(helix::plugin_menu::MenuKind::PrintFile) ? 1 : 0);

    // Clear any used-tools filter carried over from the previously-selected
    // file BEFORE the pre-parse update() below. The card is repopulated from
    // this file's full palette and must show all tools until this file's own
    // gcode parse pushes its real used set (viewer-parse or headless hook).
    // Without this reset a disjoint prior set would mis-filter — or blank — the
    // new file's card pre-parse.
    set_card_used_tools(std::nullopt);

    // Update filament mapping card (shown when AMS is available)
    filament_mapping_card_.update(filament_colors, filament_materials);

    // Publish the card's display subjects. Its visibility depends only on its
    // own state (AMS presence + slicer colors + bypass), so it can be decided
    // here, pre-parse; render_authoritative_chips() re-publishes both against
    // the precise used-tool set as soon as that set is known.
    publish_card_visibility();

    lv_subject_set_int(&detail_prefer_sliced_colors_, 0); // every open starts on actual colors
    fit_record_.reset();
    refresh_fit_check();

    // Drop any cached pre-flight result from a previously-selected file. The
    // validator re-runs in try_extract_gcode_colors() once this file's gcode is
    // parsed; clearing here prevents the gate/modal from reading stale checks.
    preflight_result_ = {};

    // Drop any pending run_when_loaded() callback from a previously-selected
    // file so a stale print-attempt can't fire against this file's parse.
    on_loaded_cb_ = nullptr;

    // Drop any pending preflight-ready attempt + timer so a stale attempt from
    // a previous file can't fire against this one. (The headless state itself
    // is reset in the cache block just below, BEFORE the lookup that may
    // re-seed it for this file.)
    on_preflight_ready_cb_ = nullptr;
    if (preflight_ready_timeout_timer_) {
        lv_timer_delete(preflight_ready_timeout_timer_);
        preflight_ready_timeout_timer_ = nullptr;
    }

    // --- Tools-used cache: instant authoritative chip state on re-prints ---
    // Reset + publish "not ready" FIRST so a miss shows the skeleton (subjects
    // settle before the first frame renders — LVGL batches within one show()
    // call). A hit then seeds the used-tool set and marks the scan done, so
    // on_activate()'s scan kicks nothing and ready=1 publishes before the
    // first frame.
    headless_tools_used_.reset();
    headless_tool_grams_.clear();
    headless_scan_done_ = false;
    headless_scan_settled_ = false;
    printer_stop_wait_timed_out_ = false;
    // Moonraker's metadata is the one palette source available before any read:
    // when it carried colors the question is already answered, and when it did
    // not (Creality's Moonraker reports filament_type and no filament_colors for
    // OrcaSlicer files) the footer read below is what settles it.
    palette_settled_ = !current_filament_colors_.empty();
    lv_subject_set_int(&detail_mapping_ready_, 0);

    if (auto cached = helix::tools_used_cache().lookup(current_file_key(), current_file_version_)) {
        headless_tools_used_ = *cached;
        headless_scan_done_ = true;    // readiness now true; the scan below is skipped
        headless_scan_settled_ = true; // the cached answer IS the settled answer
        spdlog::debug("[DetailView] Tools-used cache hit ({} tools)", cached->size());

        // Seed the authoritative render from the cached set — the same shape
        // as apply_scan_result()'s !is_gcode_loaded() branch, which the skipped scan
        // would have run. Without this, a reprint's first frame would show a
        // card built from the full slicer palette until the viewer parse lands
        // — and on Thumbnail-Only / parse-fallback opens, where the parse never
        // fires, it would never be corrected at all.
        render_authoritative_chips(tools_used_effective());
    }

    // Register with NavigationManager for lifecycle callbacks
    helix::nav::register_overlay(overlay_root_, this);

    // Register close callback to destroy widget tree when overlay closes.
    // Frees memory when detail view is dismissed. Subjects survive;
    // next show() call re-creates widgets via lazy creation above.
    helix::nav::on_close(overlay_root_, [this]() { destroy_overlay_ui(); });

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    helix::nav::push_overlay(overlay_root_);

    if (visible_subject_) {
        lv_subject_set_int(visible_subject_, 1);
    }

    // Publish readiness last: 1 when the cache seeded the chip state above,
    // 0 on a miss (skeleton until the scan/parse flips it). Still within this
    // show() call, so the first frame never sees a stale value.
    publish_mapping_ready();

    // The objects were cleared above; a view that is already open gets no
    // on_activate() to list them again.
    refresh_exclude_objects();

    spdlog::debug("[DetailView] Showing detail view for: {} ({} colors)", filename,
                  filament_colors.size());
}

void PrintSelectDetailView::hide() {
    reclaim_deleted_tree();
    if (!overlay_root_) {
        return;
    }

    // Pop from navigation stack - on_deactivate() will be called by NavigationManager
    helix::nav::go_back();

    if (visible_subject_) {
        lv_subject_set_int(visible_subject_, 0);
    }

    spdlog::debug("[DetailView] Detail view hidden");
}

// ============================================================================
// Shared G-code Download (one file + one download for scan + viewer preview)
// ============================================================================

std::string PrintSelectDetailView::canonical_gcode_path() const {
    // Keyed on the FULL relative path (not just the filename) so same-name files
    // in different directories never collide on one temp file.
    return get_helix_cache_dir("gcode_temp") + "/" +
           helix::ui::GcodePreviewFetcher::cache_file_name("detail_", current_file_key());
}

void PrintSelectDetailView::resolve_local_gcodes_root() {
    if (local_gcodes_root_resolved_ || !api_) {
        return;
    }
    // A request now fails before reaching Moonraker, and latching that answer
    // would send every file over HTTP for the rest of the session.
    if (api_->get_connection_state() != ConnectionState::CONNECTED) {
        return;
    }

    // Only worth asking when Moonraker is this machine. A remote printer's
    // absolute root path names a filesystem we cannot see, and reading a local
    // path that happens to match would be reading the wrong file entirely.
    std::string host;
    Config* cfg = Config::get_instance();
    host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");

    if (!helix::is_moonraker_on_same_host(host)) {
        local_gcodes_root_resolved_ = true;
        spdlog::debug("[DetailView] Moonraker is remote ('{}') — G-code comes over HTTP", host);
        return;
    }

    auto token = lifetime_.token();
    api_->files().get_file_roots(
        [this, token](const std::vector<FileRoot>& roots) {
            // === BG THREAD: pure lookup into a local, no `this` access ===
            const std::string root = helix::readable_root_path(roots, "gcodes");
            token.defer("DetailView::local_gcodes_root", [this, root]() {
                local_gcodes_root_ = root;
                local_gcodes_root_resolved_ = true;
                spdlog::info("[DetailView] Moonraker is local; gcodes root '{}'", root);
            });
        },
        [this, token](const MoonrakerError& err) {
            auto msg = err.message;
            token.defer("DetailView::local_gcodes_root_error", [this, msg]() {
                // Not fatal — older forks have no server.files.roots. Latch the
                // attempt so every subsequent open goes straight to HTTP instead
                // of paying for this round-trip again.
                local_gcodes_root_.clear();
                local_gcodes_root_resolved_ = true;
                spdlog::debug("[DetailView] server.files.roots unavailable ({}); "
                              "G-code comes over HTTP",
                              msg);
            });
        });
}

std::string PrintSelectDetailView::local_gcode_source() const {
    std::string candidate = current_local_path_;
    if (candidate.empty()) {
        if (local_gcodes_root_.empty()) {
            return {};
        }
        candidate = local_gcodes_root_ + "/" + current_file_key();
    }

    // Size is the same staleness check the cached-copy path uses. It also
    // doubles as the existence and readability probe: a Moonraker file we
    // cannot open here comes over HTTP instead, and a USB file is unreadable.
    if (!tio::open_file(candidate, "rb")) {
        spdlog::debug("[DetailView] No readable local G-code at '{}'", candidate);
        return {};
    }
    const auto on_disk_bytes = static_cast<size_t>(tio::file_size(candidate).value_or(0));
    if (on_disk_bytes == 0) {
        return {};
    }
    const uint64_t expected_bytes = current_file_version_.size_bytes;
    if (expected_bytes != 0 && on_disk_bytes != expected_bytes) {
        spdlog::warn("[DetailView] Local G-code size mismatch (disk={}, expected={}) — "
                     "falling back to HTTP",
                     on_disk_bytes, expected_bytes);
        return {};
    }
    return candidate;
}

void PrintSelectDetailView::reclaim_download(const std::string& path) {
    // Single gate for every reclaim in this file. is_reclaimable_download()
    // owns the rule; this owns the side effect and the refusal log, so a path
    // we do not own is a no-op rather than a deleted print file.
    if (path.empty()) {
        return;
    }
    if (!helix::ui::is_reclaimable_download(path, get_helix_cache_dir("gcode_temp"))) {
        spdlog::debug("[DetailView] Not reclaiming '{}' — not a file we downloaded", path);
        return;
    }
    std::remove(path.c_str());
}

bool PrintSelectDetailView::file_check_pending() const {
    return prep_manager_ && !current_filename_.empty() &&
           prep_manager_->print_waits_on_scan(current_file_key());
}

void PrintSelectDetailView::notify_print_readiness_changed() const {
    if (on_print_readiness_changed_cb_) {
        on_print_readiness_changed_cb_();
    }
}

std::string PrintSelectDetailView::current_file_key() const {
    return PrintPreparationManager::scan_key(current_filename_, current_path_, current_local_path_);
}

void PrintSelectDetailView::ensure_gcode_downloaded(
    helix::SharedCallback<void(bool ok, const std::string& path)> cb) {
    if (!api_ || get_helix_cache_dir("gcode_temp").empty()) {
        spdlog::warn("[DetailView] No API or cache dir for shared G-code download");
        cb(false, {});
        return;
    }
    // 0. The file is on this machine (a USB stick, or Moonraker's copy when
    //    Moonraker runs here): no transfer, no second copy on the same
    //    flash. For Moonraker's copy this only CONSULTS the answer; the resolve is
    //    kicked once from set_dependencies() and never awaited here. Awaiting it
    //    would hang this load outright on any Moonraker that does not answer
    //    server.files.roots — which includes older forks and our own mock
    //    client, neither of which invokes either callback. The path deliberately
    //    never becomes temp_gcode_path_: it is the user's print file, not
    //    something we may delete (reclaim_download() refuses it too).
    if (const std::string local = local_gcode_source(); !local.empty()) {
        spdlog::info("[DetailView] Using local G-code in place: {}", local);
        cb(true, local);
        return;
    }
    if (!current_local_path_.empty()) {
        // Moonraker has no copy of a USB file until it is printed.
        spdlog::warn("[DetailView] USB file unreadable: {}", current_local_path_);
        cb(false, {});
        return;
    }

    // One shared copy and one shared transfer per open: the fetcher joins a
    // transfer already running and keeps a copy only when its size matches the
    // file on the server, so a re-sliced or truncated copy is never scanned.
    const std::string path = canonical_gcode_path();
    gcode_fetcher_.ensure_local(
        "gcodes", current_file_key(), path, static_cast<size_t>(current_file_version_.size_bytes),
        [this, cb](const std::string& local, helix::ui::GcodePreviewFetcher::Source source) {
            if (source == helix::ui::GcodePreviewFetcher::Source::Download) {
                // Retire the previous file's temp copy and adopt this one for
                // teardown.
                if (!temp_gcode_path_.empty() && temp_gcode_path_ != local) {
                    reclaim_download(temp_gcode_path_);
                }
                temp_gcode_path_ = local;
            }
            cb(true, local);
        },
        [this, path, cb](helix::ui::GcodePreviewFetcher::Unavailable why) {
            if (why == helix::ui::GcodePreviewFetcher::Unavailable::NoLocalCopies) {
                spdlog::debug("[DetailView] No local G-code copy on this transport");
            } else {
                spdlog::warn("[DetailView] Shared G-code download failed");
            }
            // Drop any partial file the failed transfer left behind so a later
            // open doesn't mistake it for a complete cached copy.
            reclaim_download(path);
            cb(false, {});
        });
}

// ============================================================================
// Lifecycle Hooks (called by NavigationManager)
// ============================================================================

void PrintSelectDetailView::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[DetailView] on_activate() for file: {}", current_filename_);

    // (Re)build dynamic option rows from the active printer's option set.
    // Idempotent — only rebuilds when the printer or its option ids changed.
    populate_option_rows();

    // A queued job's saved states override the defaults for this render:
    // unknown ids drop, unmentioned ids keep their defaults.
    if (pending_option_seed_) {
        for (const auto& id : option_rows_renderer_.rendered_ids()) {
            const int state = helix::queue::saved_option_state(*pending_option_seed_, id);
            if (state >= 0) {
                option_rows_renderer_.set_state(id, state);
            }
        }
        pending_option_seed_.reset();
    }

    // Cache file size for safety checks (before modification attempts)
    if (prep_manager_ && current_file_version_.size_bytes > 0) {
        prep_manager_->set_cached_file_size(static_cast<size_t>(current_file_version_.size_bytes));
    }

    // The table or the lanes may have moved while the view was away.
    on_ams_state_changed();

    // Ask the active AMS backend to refresh its slot/state view. Lets users
    // self-recover from any drift between cached UI state and printer truth
    // by navigating away and back. AD5X IFS re-reads Adventurer5M.json +
    // GET_ZCOLOR and debounces that itself; a tool changer re-reads the shared
    // lane_data namespace into the lane source model; every other backend does
    // nothing here.
    //
    // The lane_data re-read is NOT coalesced: one database_get_namespace per
    // entry into this view, with out-of-order completions settled by whichever
    // lands last. A known gap, and a cheap one while nothing reads the result.
    if (auto* backend = AmsState::instance().get_backend()) {
        backend->request_resync();
    }

    // Trigger async scan for embedded G-code operations (for conflict detection)
    // The scan happens NOW after registration, so if user navigates away,
    // on_deactivate() will be called and we can check cleanup_called()
    if (!current_filename_.empty() && prep_manager_) {
        prep_manager_->scan_file_for_operations(current_filename_, current_path_,
                                                current_local_path_, current_file_version_);
    }
    notify_print_readiness_changed();
    // A cached scan answers without calling back, so read it here too.
    refresh_exclude_objects();
    refresh_fit_check();
    request_fit_record();

    // Headless tools_used scan — runs on ALL platforms (including 2D-only, where
    // the visual viewer below skips parsing). Provides tools_used + the pre-flight
    // readiness signal the print-start gate waits on, so prints never hang on
    // 2D-only devices. Result is typically ready by the time Print is tapped.
    kick_off_headless_tools_scan();

    // Invalidate predictor cache so we pick up any new timing data from completed prints
    if (prep_manager_) {
        prep_manager_->invalidate_predictor_cache();
    }

    // Calculate initial pre-print time estimate
    update_prep_time_label();

    // Load gcode for 3D/2D preview (viewer stays paused until load completes)
    load_gcode_for_preview();
}

void PrintSelectDetailView::on_deactivating(DeactivateReason reason) {
    spdlog::debug("[DetailView] on_deactivating({})", deactivate_reason_name(reason));

    // Exclude mode closes on every reason: the viewer is cleared below. Picks
    // live in exclude_objects_; a blanked screen keeps them, leaving the file
    // drops them, unless they are held for the print start that hid this view.
    exclude_mode_.hide();
    filament_mapping_card_.hide_slot_picker();
    if ((reason == DeactivateReason::NavigateAway && !picks_held_for_start_) ||
        reason == DeactivateReason::Shutdown) {
        exclude_objects_.clear_objects();
    }

    // The owning panel's close bookkeeping runs for every dismissal path —
    // this hook is the only one ESC/go_back() and a navbar switch reach.
    if (on_dismissed_cb_) {
        on_dismissed_cb_();
    }

    // Clear and pause gcode viewer immediately so the old model doesn't
    // linger when the user selects a different file
    if (gcode_viewer_) {
        helix::ui::gcode_viewer_clear(gcode_viewer_);
        helix::ui::gcode_viewer_set_paused(gcode_viewer_, true);
    }

    // Reset viewer mode to thumbnail so next open starts clean
    show_gcode_viewer(false);
    lv_subject_set_int(&detail_viewer_first_frame_, 0);
    refresh_viewer_visibility();
    gcode_loaded_ = false;
    // Readiness drops with the view — headless_scan_done_ goes false too, so
    // the publish below resolves to 0 and the skeleton latch re-arms. show()
    // re-seeds (cache) / re-runs (scan) it on the next open. The settled flag
    // follows: a late scan answer from a still-running worker is cancelled
    // with the view's lifetime anyway, and the next open must not treat
    // the previous open's settlement as authorization to delete files.
    headless_scan_done_ = false;
    headless_scan_settled_ = false;
    printer_stop_wait_timed_out_ = false;
    palette_settled_ = false;
    publish_mapping_ready();

    // Drop any pending run_when_loaded() callback. If the user tapped Print
    // before parse completed (deferring the attempt) and then navigated away,
    // a late load callback firing fire_on_loaded() would call start_print() on
    // a hidden panel → a ghost print with no UI. Clearing here prevents that.
    on_loaded_cb_ = nullptr;

    // Same protection for the preflight-ready deferral + its safety timer: a late
    // scan completion (or timeout) must not start a print on a hidden panel.
    on_preflight_ready_cb_ = nullptr;
    if (preflight_ready_timeout_timer_) {
        lv_timer_delete(preflight_ready_timeout_timer_);
        preflight_ready_timeout_timer_ = nullptr;
    }

    // Nothing left to report progress for.
    cancel_progress_timer();

    // Hide any open delete confirmation modal
    hide_delete_confirmation();

    // Note: We don't cancel scans here because PrintPreparationManager
    // has its own lifetime guard. Async callbacks in prep_manager_
    // will check cleanup_called() if needed.
}

void PrintSelectDetailView::disarm_viewer_callbacks() {
    // Every viewer callback carries `this` and touches subjects this view
    // deinitialises. The viewer outlives the view on teardown (its tree is
    // deleted on a later tick), and a parse finishing in that window would
    // otherwise call into a cleaned-up or freed view.
    if (!gcode_viewer_) {
        return;
    }
    helix::ui::gcode_viewer_set_first_frame_callback(gcode_viewer_, nullptr, nullptr);
    helix::ui::gcode_viewer_set_load_callback(gcode_viewer_, nullptr, nullptr);
    helix::ui::gcode_viewer_set_clear_callback(gcode_viewer_, nullptr, nullptr);
}

void PrintSelectDetailView::cleanup() {
    spdlog::debug("[DetailView] cleanup()");
    reclaim_deleted_tree();

    // Pause viewer before subject cleanup to avoid rendering with freed subjects.
    if (gcode_viewer_) {
        helix::ui::gcode_viewer_set_paused(gcode_viewer_, true);
    }
    disarm_viewer_callbacks();
    // The prep manager outlives cleanup() and keeps scanning; its callbacks
    // reach subjects deinitialised below.
    if (prep_manager_) {
        prep_manager_->set_on_scan_answered(nullptr);
        prep_manager_->set_macro_analysis_callback(nullptr);
        prep_manager_->set_option_state_provider(nullptr);
    }

    // Expire all outstanding async tokens
    lifetime_.invalidate();
    facts_loader_.cancel();
    // The scroll area, the card and the content container are children of
    // overlay_root_; their events can still fire between cleanup() and the
    // widgets' deletion, and a fresh lifetime_.token() is valid again, so
    // drop the pointers a deferred fit would dereference.
    options_scroll_ = nullptr;
    detail_card_ = nullptr;
    fit_pending_ = false;

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        helix::nav::unregister_overlay(overlay_root_);
    }

    exclude_mode_.hide();
    exclude_picks_observer_.reset();
    exclude_capability_observer_.reset();
    exclude_objects_.deinit_subjects();

    // Deinitialize subjects to disconnect observers
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Call base class to set cleanup_called_ flag
    OverlayBase::cleanup();
}

// ============================================================================
// Destroy-on-close support
// ============================================================================

void PrintSelectDetailView::on_root_deleted(lv_event_t* e) {
    auto* self = static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    // A replaced root is deleted after create() has pointed the view at its
    // successor; clearing the pointers then would blank a live tree.
    if (lv_event_get_current_target(e) != self->delete_hook_root_) {
        return;
    }
    self->delete_hook_root_ = nullptr;
    self->tree_deleted_ = true;
    self->forget_cached_widgets();
}

void PrintSelectDetailView::reclaim_deleted_tree() {
    if (!tree_deleted_) {
        return;
    }
    tree_deleted_ = false;
    spdlog::debug("[DetailView] Widget tree was deleted externally - releasing it");
    helix::nav::clear_on_close(overlay_root_);
    helix::nav::unregister_overlay(overlay_root_);
    overlay_root_ = nullptr;
    on_ui_destroyed();
}

void PrintSelectDetailView::forget_cached_widgets() {
    print_button_ = nullptr;
    gcode_viewer_ = nullptr;

    // Pre-print option checkboxes (kept as inert fields; see create()).
    bed_mesh_checkbox_ = nullptr;
    qgl_checkbox_ = nullptr;
    z_tilt_checkbox_ = nullptr;
    nozzle_clean_checkbox_ = nullptr;
    purge_line_checkbox_ = nullptr;
    timelapse_checkbox_ = nullptr;

    pre_print_options_container_ = nullptr;
    // The scroll area, the preview card and the content container (whose
    // LAYOUT_CHANGED feeds fit_portrait_preview) are children of overlay_root_;
    // their event callbacks die with them.
    options_scroll_ = nullptr;
    detail_card_ = nullptr;

    filament_mapping_card_.on_ui_destroyed();

    history_status_row_ = nullptr;
    history_status_icon_ = nullptr;
    history_status_label_ = nullptr;
}

void PrintSelectDetailView::uninstall_root_delete_hook() {
    helix::ui::remove_event_cb_if_alive(delete_hook_root_, on_root_deleted, this);
    delete_hook_root_ = nullptr;
}

void PrintSelectDetailView::on_ui_destroyed() {
    spdlog::debug("[DetailView] on_ui_destroyed() - nulling widget pointers");

    // The base class only condemns the tree; its delete event comes later and
    // must not reach a view that may be gone by then.
    uninstall_root_delete_hook();

    // Its widgets live in the tree being torn down.
    exclude_mode_.hide();

    // Invalidate outstanding tokens so in-flight async callbacks (gcode download,
    // metadata fetch, load callbacks) bail out — they captured pointers to
    // widgets that no longer exist (e.g. gcode_viewer_).
    // New tokens from lifetime_.token() will be valid for the next create() cycle.
    lifetime_.invalidate();
    facts_loader_.cancel();

    // Pause and clear gcode viewer state (widget is already deleted by base)
    gcode_loaded_ = false;
    // gcode_loaded_ flipping false can drop readiness (when the headless scan
    // hasn't finished), and detail_mapping_ready must ALWAYS equal
    // is_preflight_ready(). Every other site that flips readiness republishes —
    // show()'s reset/seed, on_deactivate(), the viewer clear callback — so the
    // teardown path does too. Ordinarily on_deactivate() ran first and this is
    // a no-op; it is not when the widget tree is torn down without a
    // deactivate, which is exactly when a stale ready=1 would survive.
    publish_mapping_ready();

    // Drop any pending run_when_loaded() callback so a late load callback can't
    // fire start_print() against a destroyed view (ghost-print guard).
    on_loaded_cb_ = nullptr;

    // Clean up temp gcode file so stale cached data doesn't persist
    if (!temp_gcode_path_.empty()) {
        reclaim_download(temp_gcode_path_);
        temp_gcode_path_.clear();
    }

    // The destroyed session's callers no longer want a shared download still in
    // flight: drop them. A running transfer finishes on its own and removes the
    // file when nobody claims it, and a fresh open joins it meanwhile, so the
    // canonical file is removed here only when no transfer is writing it.
    // temp_gcode_path_ above only tracks a COMPLETED download.
    gcode_fetcher_.cancel();
    if (!gcode_fetcher_.is_downloading(canonical_gcode_path())) {
        reclaim_download(canonical_gcode_path());
    }

    // Note: parent_screen_ is NOT nulled — it's the parent screen (not a child
    // widget) and is needed for lazy re-creation in show().
    confirmation_dialog_widget_ = nullptr;
    forget_cached_widgets();

    // The dynamic option rows were children of overlay_root_, which has been
    // destroyed by the base class. Drop the renderer's row state and force a
    // rebuild on next show(). Subjects inside the renderer are heap-owned —
    // their observers were attached to the now-deleted row widgets, so
    // dropping the subjects here is safe.
    // The invalidate above drops a queued fit without running it; the flag
    // must not survive into the next create() cycle.
    fit_pending_ = false;
    option_rows_renderer_.clear();
    last_rendered_rows_.clear();
    // A seed that never reached a render dies with the view it was meant
    // for; the next file to open starts from its own defaults.
    pending_option_seed_.reset();
    if (prep_manager_) {
        prep_manager_->set_option_state_provider(nullptr);
    }

    // Note: prep_manager_ is NOT reset — it holds no widget references and
    // retains its callbacks (scan_complete, macro_analysis) set by PrintSelectPanel.
    // It will be re-wired with dependencies in show() -> set_dependencies().
}

// ============================================================================
// Delete Confirmation
// ============================================================================

void PrintSelectDetailView::show_delete_confirmation(const std::string& filename) {
    // Create message with current filename
    char msg_buf[256];
    snprintf(msg_buf, sizeof(msg_buf),
             "Are you sure you want to delete '%s'? This action cannot be undone.",
             filename.c_str());

    // The modal closes itself on a button press and deletes itself after any
    // close, so every path here nulls the stored handle rather than hiding the
    // dialog; only confirm acts past that.
    auto drop_handle = [this] { confirmation_dialog_widget_ = nullptr; };
    helix::ui::ConfirmOptions opts;
    opts.on_cancel = drop_handle;
    opts.on_dismiss = drop_handle;
    opts.owner_token = lifetime_.token();
    confirmation_dialog_widget_ = helix::ui::modal_confirm(
        lv_tr("Delete File?"), msg_buf, ModalSeverity::Warning, lv_tr("Delete"),
        [this, drop_handle] {
            drop_handle();
            if (on_delete_confirmed_) {
                on_delete_confirmed_();
            }
        },
        opts);

    if (!confirmation_dialog_widget_) {
        spdlog::error("[DetailView] Failed to create confirmation dialog");
        return;
    }

    spdlog::info("[DetailView] Delete confirmation dialog shown for: {}", filename);
}

void PrintSelectDetailView::hide_delete_confirmation() {
    if (confirmation_dialog_widget_) {
        helix::ui::modal_hide(confirmation_dialog_widget_);
        confirmation_dialog_widget_ = nullptr;
    }
}

// ============================================================================
// Resize Handling
// ============================================================================

void PrintSelectDetailView::handle_resize(lv_obj_t* parent_screen) {
    if (!tree_alive() || !parent_screen) {
        return;
    }

    lv_obj_t* content_container =
        helix::ui::find_required(overlay_root_, "content_container", get_name());
    if (content_container) {
        lv_coord_t padding = ui_get_header_content_padding();
        lv_obj_set_style_pad_all(content_container, padding, 0);
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void PrintSelectDetailView::update_history_status(FileHistoryStatus status, int success_count) {
    if (!history_status_row_ || !history_status_icon_ || !history_status_label_) {
        return;
    }

    switch (status) {
    case FileHistoryStatus::NEVER_PRINTED:
        // Hide the row entirely for files with no history
        lv_obj_add_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        break;

    case FileHistoryStatus::CURRENTLY_PRINTING:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        helix::ui::icon::set_source(history_status_icon_, "clock");
        helix::ui::icon::set_variant(history_status_icon_, "accent");
        lv_label_set_text(history_status_label_, lv_tr("Currently printing"));
        break;

    case FileHistoryStatus::COMPLETED: {
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        helix::ui::icon::set_source(history_status_icon_, "check");
        helix::ui::icon::set_variant(history_status_icon_, "success");
        char buf[64];
        snprintf(
            buf, sizeof(buf),
            tr_plural("Printed %d time", "Printed %d times", static_cast<uint64_t>(success_count)),
            success_count);
        lv_label_set_text(history_status_label_, buf);
        break;
    }

    case FileHistoryStatus::FAILED:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        helix::ui::icon::set_source(history_status_icon_, "alert");
        helix::ui::icon::set_variant(history_status_icon_, "danger");
        lv_label_set_text(history_status_label_, lv_tr("Last print failed"));
        break;

    case FileHistoryStatus::CANCELLED:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        helix::ui::icon::set_source(history_status_icon_, "cancel");
        helix::ui::icon::set_variant(history_status_icon_, "warning");
        lv_label_set_text(history_status_label_, lv_tr("Last print canceled"));
        break;
    }
}

// ============================================================================
// G-code Viewer
// ============================================================================

void PrintSelectDetailView::cancel_progress_timer() {
    if (!progress_timer_) {
        return;
    }
    // cancel_safe self-guards on lv_is_initialized() and neuters rather than
    // unlinking, so this is safe from the destructor and from inside
    // lv_timer_handler (#750, #751).
    helix::ui::lv_timer_cancel_safe(progress_timer_);
    progress_timer_ = nullptr;
}

void PrintSelectDetailView::start_progress_timer() {
    cancel_progress_timer();
    if (!gcode_viewer_) {
        return;
    }
    // 250ms: the figure moves on a human scale over a minute-long scan, and the
    // poll is one relaxed atomic load. Deliberately a poll rather than a push
    // from the scan thread — the scan must never touch LVGL, and the subject
    // write has to happen on the main thread.
    progress_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<PrintSelectDetailView*>(lv_timer_get_user_data(t));
            self->poll_load_progress();
        },
        250, this);
}

void PrintSelectDetailView::refresh_viewer_visibility() {
    const int mode = lv_subject_get_int(&detail_gcode_viewer_mode_);
    const bool first_frame = lv_subject_get_int(&detail_viewer_first_frame_) != 0;
    lv_subject_set_int(&detail_viewer_hidden_,
                       helix::ui::preview_viewer_hidden(mode, first_frame) ? 1 : 0);
}

void PrintSelectDetailView::poll_load_progress() {
    if (!gcode_viewer_) {
        cancel_progress_timer();
        return;
    }

    const bool loading = lv_subject_get_int(&detail_gcode_loading_) != 0;
    const bool revealed = lv_subject_get_int(&detail_viewer_first_frame_) != 0;
    const bool is_2d = lv_subject_get_int(&detail_gcode_viewer_mode_) == helix::ui::kPreviewMode2d;

    // A hidden 2D viewer is not drawn, so nothing would drive its build. Pump
    // it here and reveal on readiness. 3D is untouched: it stays visible under
    // the thumbnail because it uploads during the draw pass, and reveals from
    // its own first-frame callback.
    if (is_2d && !revealed && helix::ui::gcode_viewer_pump_offscreen_2d(gcode_viewer_)) {
        spdlog::debug("[DetailView] Preview has content - revealing viewer");
        lv_subject_set_int(&detail_viewer_first_frame_, 1);
        refresh_viewer_visibility();
        cancel_progress_timer();
        return;
    }

    if (!loading) {
        // Indexing is done. Keep ticking only while a hidden 2D viewer still
        // needs pumping; otherwise there is nothing left for this timer to do.
        if (revealed || !is_2d) {
            cancel_progress_timer();
        }
        return;
    }

    const float fraction = helix::ui::gcode_viewer_get_load_progress(gcode_viewer_);
    // 0 is the XML's cue to keep showing the indeterminate spinner: the
    // full-load path reports nothing, and a streaming scan that has not reached
    // its first checkpoint has nothing honest to show either. Clamp the low end
    // to 1 once there IS progress so a real 0.4% does not read as "unknown".
    int percent = 0;
    if (fraction > 0.0f) {
        percent = static_cast<int>(fraction * 100.0f);
        percent = percent < 1 ? 1 : (percent > 100 ? 100 : percent);
    }

    if (percent == lv_subject_get_int(&detail_gcode_progress_)) {
        return;
    }
    lv_subject_set_int(&detail_gcode_progress_, percent);

    char buf[64];
    if (percent > 0) {
        snprintf(buf, sizeof(buf), "%s %d%%", lv_tr("Indexing G-code..."), percent);
    } else {
        snprintf(buf, sizeof(buf), "%s", lv_tr("Indexing G-code..."));
    }
    lv_subject_copy_string(&detail_gcode_progress_text_, buf);
}

void PrintSelectDetailView::show_gcode_viewer(bool show) {
    // detail_gcode_viewer_mode_ drives the XML visibility bindings in
    // print_file_detail.xml:
    //   0 = thumbnail  (viewer hidden, gradient + thumbnail shown)
    //   1 = 3D viewer  (viewer shown, gradient hidden for clean black bg,
    //                   rotate hint shown)
    //   2 = 2D viewer  (viewer shown over the gradient, rotate hint hidden —
    //                   there's no rotate affordance for the flat 2D layer view)
    // 2D-mode devices (no-GLES / GPU-blocked / budget-forced) now render the
    // real toolpath preview instead of falling back to the thumbnail.
    int mode = 0;
    if (show) {
        const bool is_2d = gcode_viewer_ && helix::ui::gcode_viewer_is_using_2d_mode(gcode_viewer_);
        mode = is_2d ? 2 : 1;
    }
    lv_subject_set_int(&detail_gcode_viewer_mode_, mode);
    refresh_viewer_visibility();

    // Returning to thumbnail mode must also drop the first-frame latch — that
    // latch is what keeps the thumbnail hidden once the viewer has painted
    // (print_file_detail.xml binds detail_viewer_first_frame). Leaving it set
    // while hiding the viewer (the memory-pressure clear callback does exactly
    // that, mid-view) hides BOTH layers and the preview goes blank.
    if (mode == 0) {
        lv_subject_set_int(&detail_viewer_first_frame_, 0);
        refresh_viewer_visibility();
    }

    // The 3D render is the preview when the viewer is active, so the
    // no-thumbnail placeholder glyph must not sit on top of it. (When the
    // viewer is inactive the print-select panel's has-thumbnail logic owns
    // whether the placeholder shows.)
    if (mode > 0 && tree_alive()) {
        lv_obj_t* no_thumb =
            helix::ui::find_required(overlay_root_, "detail_no_thumbnail_icon", get_name());
        if (no_thumb) {
            lv_obj_add_flag(no_thumb, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide loading spinner now that viewer state is resolved
    lv_subject_set_int(&detail_gcode_loading_, 0);
    lv_subject_set_int(&detail_gcode_progress_, 0);

    // A viewer that is not revealed yet is hidden, so no draw drives its build.
    // Keep the tick alive to pump it; poll_load_progress() stops itself on the
    // reveal.
    if (mode == helix::ui::kPreviewMode2d && lv_subject_get_int(&detail_viewer_first_frame_) == 0) {
        start_progress_timer();
    } else {
        cancel_progress_timer();
    }

    spdlog::trace("[DetailView] G-code viewer mode: {} ({})", mode, mode == 0 ? "thumbnail" : "3D");
}

void PrintSelectDetailView::apply_preview_colors() {
    if (!gcode_viewer_ || !gcode_loaded_) {
        return;
    }
    const auto tools = get_used_tool_info();
    if (tools.empty()) {
        return;
    }
    const auto slots = AmsState::instance().collect_available_slots();

    // Single color engine — the SAME FilamentMapper::effective_tool_colors the
    // print-file swatches and the live print-status render use. The sliced/actual
    // toggle only changes which MAPPINGS feed it:
    //   - Actual (loaded): effective_mappings() — card edits win on editable
    //     backends, auto color+type match otherwise. This is what colors each
    //     tool by its MATCHED lane instead of the identity physical-slot position.
    //   - Sliced: fully-default (unmapped) mappings, so resolve_display_colors
    //     falls back to each tool's own slicer color — the file's intended look.
    const std::vector<helix::ToolMapping> mappings =
        (lv_subject_get_int(&detail_prefer_sliced_colors_) == 1)
            ? std::vector<helix::ToolMapping>(tools.size())
            : effective_mappings();

    const auto colors = helix::FilamentMapper::effective_tool_colors(tools, mappings, slots);
    if (!colors.empty()) {
        helix::ui::gcode_viewer_set_tool_colors(gcode_viewer_, colors);
        lv_obj_invalidate(gcode_viewer_);
    }
}

void PrintSelectDetailView::set_prefer_sliced_colors(bool prefer_sliced) {
    lv_subject_set_int(&detail_prefer_sliced_colors_, prefer_sliced ? 1 : 0);
    apply_preview_colors();
    if (gcode_viewer_) {
        lv_obj_invalidate(gcode_viewer_);
    }
}

void PrintSelectDetailView::on_ams_state_changed() {
    if (!is_visible()) {
        return;
    }
    // Slots landing can change whether the card shows, not only its chips: the
    // multi-tool test reads the slot count, and the availability and bypass
    // gates read the backend. A shown card refreshes its slots and re-seeds
    // every mapping the user did not pick, so a pick survives and the rest
    // follow the printer; a hidden one has none to keep and is decided afresh
    // from the file's palette.
    if (filament_mapping_card_.should_show()) {
        filament_mapping_card_.refresh_slot_data();
    } else {
        filament_mapping_card_.update(current_filament_colors_, current_filament_materials_);
    }
    publish_card_visibility();
    // on_deactivate() clears gcode_loaded_ and on_ui_destroyed() nulls
    // gcode_viewer_, so the preview half waits for a loaded, live viewer.
    if (!gcode_loaded_ || !gcode_viewer_) {
        return;
    }
    refresh_preview_colors_and_mismatch();
}

void PrintSelectDetailView::refresh_preview_colors_and_mismatch() {
    apply_preview_colors();
    // Re-evaluate the pre-flight gate so a subsequent Print reflects the current
    // tool->slot mapping (native remap flow reads get_filament_mappings()).
    recompute_preflight();
    // The chips themselves are NOT repainted here: the card repaints from
    // set_mappings()/refresh_slot_data() before this runs, so everything left
    // in this function is downstream of that one render.
}

void PrintSelectDetailView::try_extract_gcode_colors(lv_obj_t* viewer) {
    // Backfill filament_colors when neither slicer metadata nor the footer read
    // provided them (Snapmaker and a few other Moonraker variants). Shared with
    // PrintStatusPanel, which needs the identical backfill for its cached-file
    // fast path — one helper so the two print surfaces cannot recover different
    // palettes for the same file.
    if (helix::ui::gcode_viewer_adopt_palette_if_empty(viewer, current_filament_colors_)) {
        // Rebuild the mapping card's internal tool/slot/mapping state with the
        // newly-extracted colors (the card uses them when AMS is present). The
        // pre-flight validator below is the single source of truth for the gate.
        filament_mapping_card_.refresh_palette(current_filament_colors_,
                                               current_filament_materials_);

        // Re-push the preview colors now that we finally know them. The earlier
        // apply_preview_colors() ran while current_filament_colors_ was still
        // empty, so build_tool_info() handed every tool its 0x808080 unknown
        // default and that grey went to the renderer as a per-tool override -
        // outranking the slicer palette the viewer had installed correctly. On a
        // printer with no AMS and no Spoolman that is every file, which is why an
        // AD5M drew a #000000 cube and a #F7F7F7 cube in the same grey.
        apply_preview_colors();
    }

    // The viewer has read the file: whatever it found (or did not) is the
    // answer. Releases the chip skeleton on any path where the footer read
    // could not, without it having to succeed first.
    mark_palette_settled();
    publish_mapping_ready();

    // Authoritative render from the precise tools_used set — not the slicer
    // palette size (which often over-counts).
    render_authoritative_chips(tools_used_effective());
    // The preview's model box is the fit check's bounds without the plugin.
    refresh_fit_check();

    // Write-through: persist the used set so the next open of this file renders
    // final chips instantly instead of the skeleton. Gated on the set actually
    // being this file's answer. In full-load mode the parse IS that answer. In
    // streaming mode there is no parsed tool set at all, so tools_used_effective()
    // is whatever the headless scan has so far — and persisting THAT before the
    // scan settles would cache an empty set as final truth for the file (the
    // Finding-1 poison apply_scan_result() guards against for the same reason).
    // Deliberately still keyed on the PARSED file, not on tools_used_effective()
    // having found something: the streaming index's set is a whole-file answer
    // too, but it is produced asynchronously and this runs on the load callback,
    // so treating "non-empty" as "final" would reintroduce the same provisional-
    // set poison from the other direction. The headless scan's settle flag stays
    // the one signal that a streamed file's answer is done.
    const bool set_is_authoritative =
        helix::ui::gcode_viewer_get_parsed_file(viewer) != nullptr || headless_scan_settled_;
    if (set_is_authoritative) {
        helix::tools_used_cache().store(current_file_key(), current_file_version_,
                                        tools_used_effective());
    } else {
        spdlog::debug("[DetailView] Streaming load with the tools scan still pending — "
                      "not caching a provisional used-tool set");
    }
}

std::vector<helix::GcodeToolInfo> PrintSelectDetailView::get_used_tool_info() const {
    // Source per-tool info DIRECTLY from the slicer palette (Moonraker metadata,
    // populated on ALL platforms) via the stateless assembler — NOT from the
    // mapping card INSTANCE, whose tool_info_ is empty on the U1/headless path.
    return FilamentMappingCard::build_used_tool_info(
        current_filament_colors_, current_filament_materials_, tools_used_effective());
}

bool PrintSelectDetailView::effective_auto_match() const {
    // The rule lives on AmsState because the print-status panel resolves the
    // same question for the same print: this view's swatches and that viewer's
    // per-tool colors must land on the same lane.
    return AmsState::instance().effective_auto_match();
}

std::vector<helix::ToolMapping> PrintSelectDetailView::effective_mappings() const {
    // The card's mappings (its seed, or the user's edit) when it holds any, else
    // the automatic seed: the rule a queued start with no file view shares.
    return helix::effective_file_mappings(filament_mapping_card_.get_mappings(),
                                          get_used_tool_info());
}

void PrintSelectDetailView::render_authoritative_chips(const std::set<int>& tools_used,
                                                       bool refresh_card_from_palette) {
    // Headless path only (see apply_scan_result()). FIRST, because should_show() and
    // the chips are both rebuilt by update(): publishing before it would
    // publish the answer the pre-parse palette gave, which is how a card that
    // should now appear stays hidden (the same ordering apply_footer_summary()
    // documents). Also BEFORE the gate, which must validate against the
    // rebuilt mappings.
    if (refresh_card_from_palette) {
        filament_mapping_card_.refresh_palette(current_filament_colors_,
                                               current_filament_materials_);
    }

    // Backend-agnostic pre-flight validation (single source of truth for the
    // print-start gate).
    recompute_preflight();

    // Restrict the mapping card to the tools this file actually uses — it was
    // populated from the full slicer palette. Empty/unknown => show all (safe
    // default). Order-independent with respect to the gate above: set_used_tools
    // only DROPS mapping entries whose tool_index is outside tools_used, and the
    // validator only ever looks mappings up by the tool_index of a tool in
    // get_used_tool_info(), which is filtered by the same set.
    set_card_used_tools(tools_used);

    // One chip surface for every backend: the card renders from its own store,
    // so all this has to decide is whether that surface is on screen. The
    // tool-count rule that used to live here as swatches_card_visible_for()
    // moved into the card — one predicate, so a state that hides the card can no
    // longer show a second surface drawing the same chips.
    //
    // LAST, after set_used_tools(). This function exists because the precise
    // used-tool set has just arrived, and that set is an INPUT to the rule:
    // publishing before it hands over would publish the answer the slicer
    // palette gave, which over-counts. A bypassed print of a 3-colour file that
    // uses one tool is the case that breaks — it wants one lane, the palette
    // says three, and the card would show the chips the bypass gate exists to
    // suppress.
    publish_card_visibility();
}

void PrintSelectDetailView::recompute_preflight() {
    // ------------------------------------------------------------------
    // Backend-agnostic pre-flight validation (single source of truth for the
    // print-start gate).
    //
    // Runs for ALL AMS backends — including those whose mapping card is hidden
    // (Snapmaker U1 / ACE), where get_available_slots() on the card is empty.
    // We source slots straight from AmsState's canonical accessor, build the
    // intended per-tool color/material from the slicer palette (reusing the
    // mapping card's already-parsed tool_info_, filtered to the precise
    // tools_used set), compute default mappings, then validate.
    //
    // Guarded on tools_used availability: a no-op until EITHER the viewer has
    // parsed (full platforms) OR the headless scan has completed (2D-only) — both
    // feed tools_used_effective(). Before either there is nothing to validate.
    // ------------------------------------------------------------------
    const std::set<int> used = tools_used_effective();
    if (used.empty() && !headless_scan_done_) {
        // Nothing parsed yet and no headless result: defer. (An empty set AFTER a
        // completed headless scan is a legitimate single-extruder result and must
        // still validate, so we only bail when truly nothing is known.)
        return;
    }

    // Per-tool intent for the precise tools_used set, sourced directly from the
    // slicer palette (current_filament_colors_/materials) — the same data the
    // swatches use, populated on ALL platforms — NOT from the card instance's
    // tool_info_ (empty on the U1/headless path → the original "0 tools" bug).
    const auto tools = get_used_tool_info();

    // Validate against the EFFECTIVE mapping the print will actually use, not a
    // freshly-recomputed default. The card's current mappings_ are what
    // PrintStartController::apply_remap() sends to the backend at print-start, so
    // the gate must consult the same vector — otherwise a native remap (AFC /
    // Happy Hare / CFS / AD5X-IFS / toolchanger) would never clear the block.
    //
    // Behavior-preserving at parse time: for editable backends the card seeds
    // mappings_ with the effective defaults until the user edits it (identical
    // result); for U1/ACE the card is hidden and mappings_ is empty, so
    // effective_mappings() resolves the toggle-aware (auto color+type) match —
    // the SAME mapping the color swatches and live render use.
    // Bypass short-circuits all of the above: the filament comes from the external
    // spool, not from any slot, so no mapping can satisfy a tool and every check
    // would report EmptySlot. Backend-agnostic — AFC and Happy Hare both report it.
    preflight_result_ =
        helix::file_preflight(tools, effective_mappings(),
                              prep_manager_ && prep_manager_->enabled_options_need_material());

    // Log every severity separately: "advisory=false" alone cannot tell
    // "nothing was wrong" from "something was, and it was not material". These
    // three are the complete set - every severity other than Ok is one of them.
    const bool color_flagged =
        std::any_of(preflight_result_.checks.begin(), preflight_result_.checks.end(),
                    [](const helix::ToolCheck& c) {
                        return c.severity == helix::ToolCheck::Severity::ColorMismatch;
                    });
    spdlog::debug("[DetailView] Preflight: {} tools, {} checks, advisory={}, block={}, color={}, "
                  "bypass={}",
                  tools.size(), preflight_result_.checks.size(), preflight_result_.has_advisory(),
                  preflight_result_.has_block(), color_flagged,
                  AmsState::instance().any_bypass_active());
}

std::set<int> PrintSelectDetailView::tools_used_effective() const {
    // Prefer whatever the viewer read off the file. helix::ui::gcode_viewer_get_tools_used()
    // owns that source choice for BOTH print surfaces — parsed file on a full
    // load, layer-index scan when streaming — including the single-extruder {0}
    // convention, so a file answers identically here and in the live render
    // regardless of which parse path it happened to take. This used to ask the
    // parsed file directly and so went empty on every streamed file, leaving the
    // headless scan as the only source on exactly the large files.
    //
    // Fall back to the headless scan: it is the only source on a 2D-only build
    // where no viewer exists at all, and it lands first on a cache hit.
    if (gcode_viewer_) {
        const std::set<int> from_viewer = helix::ui::gcode_viewer_get_tools_used(gcode_viewer_);
        if (!from_viewer.empty()) {
            return from_viewer;
        }
    }
    if (headless_tools_used_) {
        return *headless_tools_used_;
    }
    return {};
}

std::set<int> PrintSelectDetailView::get_tools_used() const {
    return tools_used_effective();
}

std::map<int, int> PrintSelectDetailView::get_effective_remap() const {
    // Source the mappings from effective_mappings() — the SAME toggle-aware match
    // the render, swatches, and pre-flight use. On editable backends this is the
    // card's mappings (user edits win); on non-editable backends (Snapmaker U1 /
    // ACE), where the card is hidden and get_mappings() is empty, it is the auto
    // color+type match. This is what turns the U1's emitted SET_PRINT_EXTRUDER_MAP
    // from an empty identity into the real per-tool routing (each logical tool to
    // the physical head holding its matched filament). mapped_slot is the physical
    // head 0..3 on the U1 (slot_index == head).
    //
    // The identity filter itself lives in FilamentMapper: only genuine remaps
    // survive — a real slot assignment differing from the firmware-default head
    // for that tool. Shared so FilamentSensorManager's lane scan and the preprint
    // gcode builders cannot drift from what is actually emitted.
    return helix::effective_file_remap(effective_mappings());
}

void PrintSelectDetailView::set_filament_mappings(std::vector<helix::ToolMapping> mappings) {
    spdlog::debug("[DetailView] set_filament_mappings: {} mapping(s)", mappings.size());
    filament_mapping_card_.set_mappings(std::move(mappings));
}

void PrintSelectDetailView::open_filament_mapping_modal() {
    filament_mapping_card_.open_mapping_modal();
}

void PrintSelectDetailView::publish_card_visibility() {
    // ONE place, and the only writer of any of these subjects, so the sites that
    // learn something new about the card - show(), the footer-palette backfill,
    // render_authoritative_chips() and the two capability observers - cannot
    // drift into publishing different answers from the same state. Reads the
    // card's cached should_show(), so a caller that changed anything the card
    // decides on must run update() first.
    const bool card_visible = filament_mapping_card_.should_show();
    lv_subject_set_int(&filament_mapping_visible_, card_visible ? 1 : 0);

    const auto block = current_remap_block();
    const bool available = block == helix::printer::RemapBlock::None;
    // A hidden card cannot swallow a tap or light a chevron either.
    lv_subject_set_int(&color_card_remappable_, card_visible && available ? 1 : 0);

    // The only refusal worth dressing up. A backend with no route at all, a
    // one-tool job and an in-flight probe all just leave the chevron dark: the
    // chips are still worth showing and there is nothing for the user to do.
    // A missing plugin is the one case where the answer is "not yet, and here
    // is how", so it is the one that greys the card and offers the fix.
    const bool needs_setup = card_visible && block == helix::printer::RemapBlock::NeedsPlugin;
    lv_subject_set_int(&color_card_remap_needs_setup_, needs_setup ? 1 : 0);

    // Available but the finished job will be misfiled: say so here rather than
    // at startup, where it was a claim about the whole app instead of about the
    // one thing it actually costs.
    // Only a GcodeRewrite remap prints a modified copy and leans on the plugin to
    // put the original name back, so only it can lose the history entry. A
    // backend that routes through its own firmware table never touches the job
    // file, and telling its user the file gets rewritten describes something the
    // printer does not do. Same term remap_block() gates its plugin rung on.
    const auto* backend = AmsState::instance().get_backend();
    const bool rewrites_job_file =
        backend != nullptr &&
        backend->get_remap_strategy() == AmsBackend::RemapStrategy::GcodeRewrite;
    const bool degraded =
        available && rewrites_job_file &&
        lv_subject_get_int(
            get_printer_state().versions_state().get_moonraker_history_degraded_subject()) == 1;

    lv_subject_set_int(&color_card_remap_help_visible_,
                       card_visible && (needs_setup || degraded) ? 1 : 0);

    spdlog::debug("[DetailView] filament card: visible={} remap={} setup={} degraded={}",
                  card_visible, helix::printer::remap_block_name(block), needs_setup, degraded);
}

void PrintSelectDetailView::show_plugin_actions() {
    lv_obj_t* anchor = overlay_root_ ? lv_obj_find_by_name(overlay_root_, "more_button") : nullptr;
    if (!anchor)
        return;
    helix::plugin_menu::PrintFileFacts facts;
    facts.name = current_filename_;
    facts.dir = current_path_;
    facts.local_path = current_local_path_;
    facts.meta = cached_file_metadata_;
    facts.materials = current_filament_materials_;
    facts.colors = current_filament_colors_;
    facts.tool_grams = get_tool_grams();
    facts.tools_used = get_tools_used();
    plugin_items_menu_.show_for(lv_obj_get_screen(anchor), anchor,
                                helix::plugin_menu::MenuKind::PrintFile,
                                helix::plugin_menu::print_file_context(facts));
}

void PrintSelectDetailView::show_remap_help_modal() {
    if (current_remap_block() == helix::printer::RemapBlock::NeedsPlugin) {
        helix::ui::ConfirmOptions opts;
        opts.cancel_text = lv_tr("Not now");
        opts.owner_token = object_lifetime_.token();
        helix::ui::modal_confirm(
            lv_tr("Filament Mapping is unavailable"),
            lv_tr("To map filament, HelixScreen rewrites the job file before printing it. "
                  "Without the HelixPrint plugin the rewritten copy is what lands in your print "
                  "history, so finished "
                  "jobs are listed under names like modified_1730824_benchy.gcode instead of the "
                  "file you picked.\n\nInstalling the plugin takes a moment and needs no "
                  "restart."),
            ModalSeverity::Info, lv_tr("Install plugin"),
            [this] {
                if (on_plugin_setup_requested_) {
                    on_plugin_setup_requested_();
                }
            },
            opts);
        return;
    }

    // The other reason the icon is offered: remapping works, but this Moonraker
    // is too old for the plugin to put the original name back afterwards.
    helix::ui::AlertOptions alert_opts;
    alert_opts.owner_token = object_lifetime_.token();
    helix::ui::modal_alert(
        lv_tr("Print history will show the rewritten name"),
        lv_tr("To map filament, HelixScreen rewrites the job file before printing it. This "
              "printer's Moonraker is too old for the HelixPrint plugin to restore the original "
              "filename afterwards, so the finished job is listed under the rewritten name. "
              "Filament Mapping itself works normally."),
        ModalSeverity::Info, lv_tr("OK"), nullptr, alert_opts);
}

helix::printer::RemapBlock PrintSelectDetailView::current_remap_block() const {
    auto* backend = AmsState::instance().get_backend();
    // A card showing what is loaded has no lane to route a tool to.
    if (backend == nullptr || filament_mapping_card_.shows_loaded()) {
        return helix::printer::RemapBlock::NoStrategy;
    }
    return helix::printer::remap_block(*backend, rewrite_block(),
                                       static_cast<int>(get_used_tool_info().size()));
}

helix::GcodeRewriteBlock PrintSelectDetailView::rewrite_block() const {
    // Before set_analysis_dependencies() the view reads the app's printer, as
    // the rest of the card's remap answer does.
    return PrintPreparationManager::gcode_rewrite_block_for(
        printer_state_ ? printer_state_ : &get_printer_state(), api_);
}

void PrintSelectDetailView::publish_rewrite_availability() {
    if (!subjects_initialized_) {
        return; // init_subjects() publishes once the subject exists
    }
    lv_subject_set_int(&gcode_rewrite_available_, helix::gcode_rewrite_available(rewrite_block()));
}

bool PrintSelectDetailView::remap_tap_allowed(const char* what) {
    // Kept as a guard even though the XML clears the card's clickable flag on a
    // non-remappable backend: the flag can only be as fresh as the last publish,
    // and this is the cheap way to make a stale one harmless.
    const auto block = current_remap_block();
    if (block == helix::printer::RemapBlock::NeedsPlugin) {
        // The tap is the help target here, not just the 24px cue: there is no
        // remap to open, and a tap that explains beats a tap that silently does
        // nothing. The Install button lives in that modal.
        spdlog::debug("[PrintSelect] filament {} tap -> remap help", what);
        show_remap_help_modal();
        return false;
    }
    if (block != helix::printer::RemapBlock::None) {
        spdlog::debug("[PrintSelect] filament {} tap ignored: {}", what,
                      helix::printer::remap_block_name(block));
        return false;
    }
    return true;
}

bool PrintSelectDetailView::print_carries_out_chip_pick() const {
    const auto* backend = AmsState::instance().get_backend();
    return backend != nullptr &&
           helix::printer::print_carries_out_stored_pick(backend->get_remap_strategy());
}

void PrintSelectDetailView::on_chip_clicked(size_t chip, lv_obj_t* trigger) {
    if (!remap_tap_allowed("chip")) {
        return;
    }
    // A pick the Print button would not carry out is never recorded inline: the
    // full mapping modal applies it itself.
    if (!print_carries_out_chip_pick()) {
        spdlog::debug("[PrintSelect] filament chip {} tap -> remap modal", chip);
        if (on_remap_requested_) {
            on_remap_requested_();
        }
        return;
    }
    spdlog::debug("[PrintSelect] filament chip {} tap -> slot picker", chip);
    filament_mapping_card_.open_slot_picker(chip, trigger);
}

void PrintSelectDetailView::on_color_card_clicked() {
    // The filament card is the remap entry point on EVERY backend that has one,
    // including those whose mapping is not editable inline (Snapmaker U1).
    if (!remap_tap_allowed("card")) {
        return;
    }
    spdlog::debug("[PrintSelect] filament card tap -> remap modal");
    // on_remap_requested_ is wired by the panel in create_detail_view() right
    // after construction, so the null check is just defensive - a tap before
    // wiring is a no-op, not a crash.
    if (on_remap_requested_) {
        on_remap_requested_();
    }
}

void PrintSelectDetailView::run_when_loaded(helix::SharedCallback<void()> cb) {
    if (!cb) {
        return;
    }
    // Already parsed: preflight_result_ is fresh, run synchronously (main thread).
    if (gcode_loaded_) {
        cb();
        return;
    }
    // Parse still in flight: store; fire_on_loaded() invokes it post-parse.
    on_loaded_cb_ = std::move(cb);
}

void PrintSelectDetailView::fire_on_loaded() {
    if (on_loaded_cb_) {
        auto cb = std::move(on_loaded_cb_);
        on_loaded_cb_ = nullptr;
        cb();
    }
}

void PrintSelectDetailView::run_when_preflight_ready(helix::SharedCallback<void()> cb) {
    if (!cb) {
        return;
    }
    // Already ready: run synchronously.
    if (is_print_start_ready()) {
        cb();
        return;
    }
    on_preflight_ready_cb_ = std::move(cb);

    // Arm a one-shot safety timeout so a stuck/failed scan can never wedge the
    // print. On expiry we fire the deferred attempt anyway (graceful degradation:
    // print without Part A's optimization rather than never starting).
    if (preflight_ready_timeout_timer_) {
        lv_timer_delete(preflight_ready_timeout_timer_);
        preflight_ready_timeout_timer_ = nullptr;
    }
    preflight_ready_timeout_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<PrintSelectDetailView*>(lv_timer_get_user_data(t));
            spdlog::warn("[DetailView] Pre-flight readiness timed out — proceeding without "
                         "tools_used (graceful degradation)");
            // Mark done so a later readiness signal doesn't double-fire, and so
            // is_preflight_ready() returns true for the deferred re-entry.
            // Deliberately does NOT set headless_scan_settled_: the download +
            // scan may still be in flight behind the timeout, and settling
            // here would authorize oversize-reject removal of the canonical
            // file while the scanner is reading it (authoritative-empty
            // poison — see load_gcode_for_preview's oversize gate).
            self->headless_scan_done_ = true;
            self->printer_stop_wait_timed_out_ = true;
            self->fire_on_preflight_ready();
        },
        kPreflightReadyTimeoutMs, this);
    lv_timer_set_repeat_count(preflight_ready_timeout_timer_, 1);
}

bool PrintSelectDetailView::is_print_start_ready() const {
    const bool stop_check_answered = printer_stop_wait_timed_out_ || !prep_manager_ ||
                                     current_filename_.empty() ||
                                     prep_manager_->has_printer_stop_answer_for(current_file_key());
    return is_preflight_ready() && stop_check_answered;
}

void PrintSelectDetailView::fire_on_preflight_ready() {
    // Each half of the readiness calls this when it lands; the other half's
    // arrival calls it again.
    if (!is_print_start_ready()) {
        return;
    }
    if (preflight_ready_timeout_timer_) {
        lv_timer_delete(preflight_ready_timeout_timer_);
        preflight_ready_timeout_timer_ = nullptr;
    }
    if (on_preflight_ready_cb_) {
        auto cb = std::move(on_preflight_ready_cb_);
        on_preflight_ready_cb_ = nullptr;
        cb();
    }
}

void PrintSelectDetailView::refresh_exclude_objects() {
    std::vector<helix::gcode::GCodeObject> scanned;
    if (const auto* scan =
            prep_manager_ ? prep_manager_->scan_result_for(current_file_key()) : nullptr) {
        scanned = scan->objects;
    }
    // An open view shown for another file still holds the previous file's parse.
    const auto* parsed = gcode_viewer_ && viewer_file_ == current_filename_
                             ? helix::ui::gcode_viewer_get_parsed_file(gcode_viewer_)
                             : nullptr;
    exclude_objects_.set_defined_objects_with_geometry(
        helix::ui::object_infos_from(helix::ui::merge_defined_objects(scanned, parsed)));

    const bool available = helix::ui::pre_start_exclude_available(
        helix::ui::printer_has_exclude_object(printer_state_),
        helix::gcode::is_3mf(current_filename_), exclude_objects_.get_defined_objects().size());
    lv_subject_set_int(&detail_exclude_available_, available ? 1 : 0);
    publish_exclude_picks();
    exclude_mode_.refresh_render_badges();
}

helix::FitFile PrintSelectDetailView::get_fit_file() const {
    const auto* scan = prep_manager_ ? prep_manager_->scan_result_for(current_file_key()) : nullptr;
    // An open view shown for another file still holds the previous file's parse.
    std::optional<helix::FitBox> preview;
    if (gcode_viewer_ && viewer_file_ == current_filename_) {
        preview = helix::ui::gcode_viewer_get_model_bounds(gcode_viewer_);
    }
    return helix::fit_file_from_sources(fit_record_, scan ? &scan->fit_head : nullptr,
                                        cached_file_metadata_, preview, tools_used_effective());
}

void PrintSelectDetailView::refresh_fit_check() {
    if (!subjects_initialized_) {
        return;
    }
    const auto machine = helix::current_fit_machine();
    const std::vector<helix::FitFinding> findings = machine && !current_filename_.empty()
                                                        ? helix::check_fit(*machine, get_fit_file())
                                                        : std::vector<helix::FitFinding>{};
    // 0 nothing, then one past the worst FitSeverity: 1 note, 2 warn, 3 block.
    const auto worst = helix::fit_worst(findings);
    const int severity = worst ? static_cast<int>(*worst) + 1 : 0;
    lv_subject_copy_string(&detail_fit_text_,
                           helix::fit_findings_text(findings, helix::FitSeverity::Note).c_str());
    lv_subject_set_int(&detail_fit_severity_, severity);
}

void PrintSelectDetailView::request_fit_record() {
    // A USB stick file has no record on the printer.
    if (!api_ || !printer_state_ || !current_local_path_.empty() ||
        helix::file_analysis_source(
            printer_state_->plugin_status_state().plugin_feature(helix::kFileAnalysisFeature,
                                                                 helix::kFileAnalysisMinVersion),
            current_filename_) != helix::FileAnalysisSource::Plugin) {
        return;
    }
    const std::string key = current_file_key();
    auto token = lifetime_.token();
    helix::request_file_analysis(
        api_, helix::gcode::join_gcode_path(current_path_, current_filename_),
        [this, token, key](helix::FileAnalysisReply reply) {
            if (reply.status != helix::FileAnalysisReply::Status::Ready || !reply.record) {
                return;
            }
            token.defer("DetailView::fit_record", [this, key, fit = reply.record->fit]() {
                if (key == current_file_key()) {
                    fit_record_ = fit;
                    refresh_fit_check();
                }
            });
        });
}

void PrintSelectDetailView::toggle_exclude_pick(const std::string& name) {
    const std::string defined =
        helix::ui::canonical_object_name(exclude_objects_.get_defined_objects(), name);
    if (defined.empty()) {
        return;
    }
    auto picks = exclude_objects_.get_excluded_objects();
    if (picks.erase(defined) == 0) {
        if (!moonraker_internal::is_safe_object_name(defined)) {
            NOTIFY_WARNING(
                lv_tr("{} cannot be skipped: its name has characters the printer "
                      "cannot be sent"),
                helix::ui::object_message_name(exclude_objects_.get_defined_objects(), defined));
            return;
        }
        picks.insert(defined);
    }
    spdlog::info("[DetailView] Object picks for {}: {}", current_filename_, picks.size());
    exclude_objects_.set_excluded_objects(picks);
}

std::vector<std::string> PrintSelectDetailView::exclude_picks() const {
    // Picks the skip option no longer offers (the printer lost [exclude_object])
    // are neither shown nor sent.
    std::vector<std::string> out;
    if (lv_subject_get_int(const_cast<lv_subject_t*>(&detail_exclude_available_)) == 0) {
        return out;
    }
    const auto& picks = exclude_objects_.get_excluded_objects();
    for (const auto& name : exclude_objects_.get_defined_objects()) {
        if (picks.count(name) > 0) {
            out.push_back(name);
        }
    }
    return out;
}

bool PrintSelectDetailView::drop_exclude_picks() {
    if (exclude_objects_.get_excluded_objects().empty()) {
        return false;
    }
    const bool had_offered_picks = !exclude_picks().empty();
    exclude_objects_.set_excluded_objects({});
    return had_offered_picks;
}

bool PrintSelectDetailView::all_objects_picked() const {
    return !exclude_picks().empty() &&
           helix::ui::every_object_picked(exclude_objects_.get_defined_objects(),
                                          exclude_objects_.get_excluded_objects());
}

void PrintSelectDetailView::toggle_exclude_mode() {
    if (exclude_mode_.is_open()) {
        exclude_mode_.hide();
        return;
    }
    if (!tree_alive()) {
        return;
    }
    helix::ui::ExcludeModeTargets targets;
    targets.card = detail_card_;
    targets.columns = helix::ui::find_required(overlay_root_, "content_container", get_name());
    targets.controls_name = "options_section";
    targets.gcode_viewer = gcode_viewer_;
    targets.thumbnail_mode = lv_subject_get_int(&detail_viewer_hidden_) == 1;
    targets.bed = helix::bed_dimensions(api_, printer_state_);
    exclude_mode_.show(targets, &exclude_objects_, helix::ui::ExcludeTapMode::Toggle,
                       [this](const std::string& name) { toggle_exclude_pick(name); });
}

void PrintSelectDetailView::publish_exclude_picks() {
    // What Print will send: the badge, the list and the render all show this.
    const auto picks = exclude_picks();
    const int count = static_cast<int>(picks.size());
    lv_subject_set_int(&detail_exclude_pick_count_, count);
    std::snprintf(detail_exclude_pick_count_text_buf_, sizeof(detail_exclude_pick_count_text_buf_),
                  "%d", count);
    lv_subject_copy_string(&detail_exclude_pick_count_text_, detail_exclude_pick_count_text_buf_);
    if (gcode_viewer_) {
        helix::ui::gcode_viewer_set_excluded_objects(gcode_viewer_, {picks.begin(), picks.end()});
    }
}

void PrintSelectDetailView::publish_mapping_ready() {
    // Two independent questions have to be answered before a chip is honest,
    // and they come from different places. is_preflight_ready() covers WHICH
    // TOOLS (viewer parse or headless scan, and ToolsUsedCache can answer it
    // instantly on a re-open). palette_settled_ covers WHAT COLORS, which the
    // cache says nothing about.
    //
    // Publishing on the tools half alone is what put grey chips on screen: a
    // cache hit satisfied it during show(), the skeleton was retired before any
    // palette existed, and every tool rendered the neutral stand-in. Note
    // "settled", not "non-empty" — a file nothing can supply colors for still
    // has to resolve, or the skeleton never clears.
    const bool ready = is_preflight_ready() && palette_settled_;
    lv_subject_set_int(&detail_mapping_ready_, ready ? 1 : 0);
}

void PrintSelectDetailView::mark_palette_settled() {
    if (palette_settled_) {
        return;
    }
    palette_settled_ = true;
    spdlog::debug("[DetailView] Palette question settled ({} colors)",
                  current_filament_colors_.size());
}

void PrintSelectDetailView::apply_scan_result(std::set<int> tools) {
    headless_tools_used_ = std::move(tools);
    headless_scan_done_ = true;
    // The scan has truly settled — the oversize-reject cleanup may now
    // treat the canonical file as unreferenced (see load_gcode_for_preview).
    headless_scan_settled_ = true;
    spdlog::debug("[DetailView] Headless tools_used scan complete: {} tools",
                  headless_tools_used_->size());

    // Readiness flipped true — publish so the skeleton latch opens (the
    // authoritative render below lands in this same deferred tick).
    publish_mapping_ready();

    // Render the per-tool chips from the REAL used-tool set recovered
    // by the headless scan. On 2D-only platforms (Snapmaker U1, AD5M)
    // the gcode viewer never parses, so try_extract_gcode_colors() —
    // the viewer-parse owner of this render — never fires and the
    // detail panel would otherwise show no color info at all
    // (regression 22d37fd47). Mirror its visibility decision and
    // render here, sourcing the tool set from tools_used_effective()
    // so the chips reflect the precise used tools (e.g. {0,2}), not an
    // over-counted palette.
    //
    // Guard on !is_gcode_loaded(): when the viewer DID parse (full
    // platforms) it already owns the render — don't double-fire.
    if (!is_gcode_loaded()) {
        // refresh_card_from_palette: the card widget renders its chips
        // from tool_info_, so on a backend that takes the headless path
        // (e.g. CFS on a 2D-only platform) it must be fed here just as
        // the viewer-parse path feeds it. Redundant for
        // preflight/remap LOGIC — those source per-tool info from
        // current_filament_colors_/materials via get_used_tool_info(), not
        // from the card instance — but necessary for card display.
        render_authoritative_chips(tools_used_effective(),
                                   /*refresh_card_from_palette=*/true);
    } else {
        // The viewer parse already owns the chip render — re-running it
        // would rebuild identical chips. Pre-flight and the card's used-tool
        // filter still refresh from the scan result (a no-op on full
        // platforms, where the parse already populated both).
        recompute_preflight();
        set_card_used_tools(tools_used_effective());
        // set_used_tools() re-decides visibility from the new set, so the
        // subject has to follow it here too — this branch skips
        // render_authoritative_chips(), which is where that normally happens.
        publish_card_visibility();
    }

    // Release any deferred print attempt waiting on readiness.
    fire_on_preflight_ready();
}

void PrintSelectDetailView::kick_off_headless_tools_scan() {
    // show() owns the headless-state reset (and the cache seed). A cache hit
    // (or an already-completed viewer parse) has already answered the
    // tools-used question; the footer read still runs for the palette and the
    // grams, which ToolsUsedCache does not keep.
    helix::FileStartFactsLoader::ScanRequest req;
    req.file_key = current_filename_.empty() ? std::string{} : current_file_key();
    req.version = current_file_version_;
    req.gcode_end_byte = current_gcode_end_byte_;
    req.need = helix::gcode::footer_read_need(
        headless_scan_done_, !current_filament_colors_.empty(), !headless_tool_grams_.empty());
    req.palette_size = current_filament_colors_.size();
    req.local_source = [this]() { return local_gcode_source(); };
    req.on_moonraker = current_local_path_.empty();
    // ONE shared transfer: the viewer preview joins it, and the copy stays for
    // the preview until view teardown.
    req.download = [this](helix::SharedCallback<void(bool, const std::string&)> done) {
        ensure_gcode_downloaded(std::move(done));
    };

    helix::FileStartFactsLoader::ScanHooks hooks;
    const std::string file_path = req.file_key;
    hooks.on_footer = [this, file_path](const helix::gcode::GcodeFooterSummary& summary,
                                        bool tools_answered) {
        // The selection can move on while the read is in flight; the answer
        // must not land on a file that is no longer shown.
        if (file_path != current_file_key()) {
            spdlog::debug("[DetailView] Footer read landed for a stale file ({}) — discarding",
                          file_path);
            return;
        }
        apply_footer_summary(summary, tools_answered);
    };
    hooks.on_footer_unanswered = [this](bool /*full_scan_follows*/) {
        // The whole-file scan counts Tn commands; it does not read colors. So
        // the footer failing IS the end of the palette question for every
        // source except a viewer load, and in Thumbnail-Only mode there is no
        // viewer load. Settling it here means the chips render "unknown" until
        // a viewer load (if one happens) backfills real colors, rather than
        // sitting on the skeleton for the life of the view.
        mark_palette_settled();
        publish_mapping_ready();
    };
    hooks.on_tools_scanned = [this](std::set<int> tools, bool /*read_file*/) {
        apply_scan_result(std::move(tools));
    };

    switch (facts_loader_.scan(std::move(req), std::move(hooks))) {
    case helix::FileStartFactsLoader::ScanStart::Started:
        return;
    case helix::FileStartFactsLoader::ScanStart::NothingToRead:
        mark_palette_settled();
        publish_mapping_ready();
        return;
    case helix::FileStartFactsLoader::ScanStart::Unreadable:
        // No way to scan: mark done with no result so the gate degrades to
        // "proceed without tools_used" instead of hanging, publish readiness
        // (skeleton resolves immediately), and release any deferred attempt
        // right away rather than making it wait out the safety timeout.
        // Nothing can ever read this file, so the tools and palette questions
        // are as settled as they will get.
        headless_scan_done_ = true;
        headless_scan_settled_ = true;
        mark_palette_settled();
        publish_mapping_ready();
        fire_on_preflight_ready();
        return;
    }
}

void PrintSelectDetailView::apply_footer_summary(const helix::gcode::GcodeFooterSummary& summary,
                                                 bool tools_answered) {
    // Backfill the palette when Moonraker's metadata carried none — the same
    // gap try_extract_gcode_colors() covers from a viewer load, answered here
    // without one (and without waiting out a 69-second index build). The
    // grams come from the same footer line that named the used tools, so the
    // pre-print check can weigh each tool against the lane it is mapped to.
    bool palette_arrived = false;
    if (helix::gcode::backfill_from_footer(summary, current_filament_colors_,
                                           headless_tool_grams_)) {
        spdlog::info(
            "[DetailView] Metadata lacked filament colors — took {} from the G-code footer",
            summary.colours.size());
        // Rebuild the card's tool/slot/mapping state from the real palette
        // BEFORE anything reads should_show() or renders — the cached answer
        // still describes the empty palette show() built the card from, and
        // publishing that is how a card that should now appear stays hidden.
        // Mirrors try_extract_gcode_colors()'s ordering.
        filament_mapping_card_.refresh_palette(current_filament_colors_,
                                               current_filament_materials_);
        publish_card_visibility();
        palette_arrived = true;
    }

    // Answered either way — even an empty colour line is now a settled "no".
    mark_palette_settled();

    if (tools_answered) {
        spdlog::info("[DetailView] Footer read answered tools_used: {} tools",
                     summary.tools_used.size());
        // The footer is the slicer's own accounting of what it emitted, and the
        // loader cached it like a completed scan. It can differ from the
        // Tn scan on a single-extruder file — the footer says {0} where the scan
        // (which sees no Tn at all) says {} — and {0} is the same answer the
        // viewer parse produces, so the two paths agree rather than diverge.
        // apply_scan_result() publishes readiness and renders the chips.
        apply_scan_result(summary.tools_used);
        return;
    }

    // Tools were already settled (cache hit) — this read was for the palette
    // and the grams. show() rendered the chips against an empty palette, so
    // re-render now that there is a real one. refresh_card_from_palette is
    // false because the update above already ran; passing true would rebuild
    // the identical card a second time.
    publish_mapping_ready();
    render_authoritative_chips(tools_used_effective(),
                               /*refresh_card_from_palette=*/!palette_arrived);
}

void PrintSelectDetailView::load_gcode_for_preview() {
    // Skip if no viewer widget
    if (!gcode_viewer_) {
        spdlog::debug("[DetailView] No gcode_viewer_ widget - skipping G-code preview");
        return;
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[DetailView] No API available - skipping G-code preview");
        return;
    }

    // Skip if no filename
    if (current_filename_.empty()) {
        spdlog::debug("[DetailView] No filename - skipping G-code preview");
        return;
    }

    // Clear previous model so stale frames don't flash when viewer becomes visible
    helix::ui::gcode_viewer_clear(gcode_viewer_);

    // Reset first-frame flag: the thumbnail stays on top until the viewer
    // renders, then the first-frame callback flips this to reveal it.
    lv_subject_set_int(&detail_viewer_first_frame_, 0);
    refresh_viewer_visibility();

    // Register one-shot callback: fires after the viewer's first complete
    // render, at which point we hide the thumbnail (revealing the viewer).
    helix::ui::gcode_viewer_set_first_frame_callback(
        gcode_viewer_,
        [](lv_obj_t* /*viewer*/, void* user_data, bool /*success*/) {
            auto* self = static_cast<PrintSelectDetailView*>(user_data);
            spdlog::debug("[DetailView] First frame rendered — revealing viewer");
            lv_subject_set_int(&self->detail_viewer_first_frame_, 1);
            self->refresh_viewer_visibility();
        },
        this);

    // Show loading spinner over thumbnail, and start reporting real progress
    // as soon as the streaming indexer has any to report.
    lv_subject_set_int(&detail_gcode_progress_, 0);
    lv_subject_copy_string(&detail_gcode_progress_text_, lv_tr("Indexing G-code..."));
    lv_subject_set_int(&detail_gcode_loading_, 1);
    start_progress_timer();

    auto tok = lifetime_.token();

    // Metadata fetch, in every render mode and for a .3mf too: it is the only
    // writer of cached_file_metadata_, which PrintStartController's pre-print
    // checks (filament weight, per-tool weights) read. It also re-checks the
    // streaming-safety gate against the authoritative size; in thumbnail mode
    // its show_gcode_viewer(false) calls leave the thumbnail as it is. A USB
    // file has no Moonraker metadata to fetch until it is printed.
    const std::string file_path = helix::gcode::join_gcode_path(current_path_, current_filename_);

    if (current_local_path_.empty()) {
        api_->files().get_file_metadata(
            file_path,
            [this, tok, key = current_file_key()](const FileMetadata& metadata) {
                // L081 Mechanism C: marshal member writes + LVGL/show_gcode_viewer
                // to main thread before touching `this`.
                tok.defer("DetailView::metadata_apply", [this, key, metadata]() {
                    // Cache for PrintStartController's pre-print checks (e.g., filament weight)
                    if (!apply_file_metadata(key, metadata)) {
                        return;
                    }

                    // Check the file against the preview's RAM and CPU limits.
                    // When the shared file already loaded this same size passed the
                    // download's local gate below, so this only bites on the paths
                    // where the ensure-callback hadn't resolved yet.
                    if (helix::ui::preview_gate(metadata.size, "DetailView") !=
                        helix::ui::PreviewGate::Build) {
                        show_gcode_viewer(false);
                        return;
                    }
                    spdlog::debug("[DetailView] G-code size {} bytes - metadata cached",
                                  metadata.size);
                });
            },
            [this, tok](const MoonrakerError& err) {
                // L081 Mechanism C: marshal LVGL show_gcode_viewer to main thread.
                tok.defer("DetailView::metadata_error", [this, err]() {
                    spdlog::debug(
                        "[DetailView] Failed to get G-code metadata: {} - skipping preview",
                        err.message);
                    show_gcode_viewer(false);
                });
            },
            true // silent
        );
    }

    // Check "Thumbnail Only" render mode - skip all gcode downloading/parsing.
    // This is the ONLY user-forced skip: past here we render whatever mode the
    // viewer is in (3D on GLES devices, 2D-layer otherwise), just like the
    // print-status panel. 2D-mode devices used to fall back to the thumbnail
    // here; now they get the real toolpath preview — with the effective
    // lane-matched colors (apply_preview_colors) — so the browser shows the
    // colors the print will actually use. Oversized files still degrade to the
    // thumbnail below via preview_gate().
    // A .3mf takes the same path: it is a zip with no layers for the viewer to
    // parse (prestonbrown/helixscreen#1713).
    if (!helix::ui::preview_viewer_enabled() || helix::gcode::is_3mf(current_filename_)) {
        spdlog::info("[DetailView] Thumbnail Only render mode or a .3mf file - skipping G-code "
                     "load");
        lv_subject_set_int(&detail_gcode_loading_, 0);
        cancel_progress_timer();
        show_gcode_viewer(false);
        return;
    }

    // Shared download: when the file is already on disk the viewer loads
    // immediately, with no wait on the metadata round-trip. On a cold open
    // this starts the ONE transfer the headless tools scan joins. The
    // streaming-safety gate applies to the on-disk bytes either way (same size
    // metadata.size reports); the metadata gate above re-checks the
    // authoritative size on cold downloads.
    ensure_gcode_downloaded([this, tok](bool ok, const std::string& path) {
        if (!ok) {
            spdlog::debug("[DetailView] Shared G-code download unavailable - using thumbnail");
            show_gcode_viewer(false);
            return;
        }

        const size_t local_size =
            tio::open_file(path, "rb") ? static_cast<size_t>(tio::file_size(path).value_or(0)) : 0;
        const helix::ui::PreviewGate gate = helix::ui::preview_gate(local_size, "DetailView");
        if (gate == helix::ui::PreviewGate::TooExpensive) {
            // Affordable once the print ends, so the copy stays for this view
            // and goes with it at teardown, like any download.
            if (helix::ui::is_reclaimable_download(path, get_helix_cache_dir("gcode_temp"))) {
                temp_gcode_path_ = path;
            }
            show_gcode_viewer(false);
            return;
        }
        if (gate == helix::ui::PreviewGate::TooLarge) {
            // The viewer just rejected the file — remove the canonical copy
            // so oversize re-downloads can't pile up on disk (SD-card leak).
            // Gated on the scan having actually SETTLED, not on readiness:
            // the preflight safety timeout flips headless_scan_done_ while a
            // download/scan is still in flight (tap Print on a slow oversize
            // download → timeout → late completion → this reject), and the
            // scanner cannot tell a deleted file from "no tools used" — so
            // removing it mid-scan would persist an authoritative-empty
            // cache entry. While the scan is still pending,
            // on_ui_destroyed() reclaims the file at view teardown instead.
            if (headless_scan_settled_) {
                if (temp_gcode_path_ == path) {
                    temp_gcode_path_.clear();
                }
                reclaim_download(path);
            }
            show_gcode_viewer(false);
            return;
        }

        // Adopt for teardown cleanup, but only what is ours to delete — the
        // same rule reclaim_download() enforces at the sink. A same-host open
        // hands us Moonraker's own print file, and recording that here would be
        // misleading state even though the sink already refuses it.
        if (helix::ui::is_reclaimable_download(path, get_helix_cache_dir("gcode_temp"))) {
            temp_gcode_path_ = path;
        }
        spdlog::info("[DetailView] Using G-code file ({} bytes): {}", local_size, path);
        // A current analysis record's layer index spares a streaming load
        // its own pass over the file.
        const std::string key = current_file_key();
        helix::with_layer_index_sidecar(
            current_local_path_.empty() ? api_ : nullptr,
            printer_state_ ? printer_state_->plugin_status_state().plugin_feature(
                                 helix::kFileAnalysisFeature, helix::kFileAnalysisMinVersion)
                           : helix::PluginFeatureState::Absent,
            helix::gcode::join_gcode_path(current_path_, current_filename_),
            helix::ui::gcode_viewer_would_stream(gcode_viewer_, path), tok,
            [this, key, path](std::string sidecar) {
                if (key == current_file_key()) {
                    begin_viewer_load(path, std::move(sidecar));
                }
            });
    });
}

void PrintSelectDetailView::begin_viewer_load(const std::string& path,
                                              std::string layer_index_sidecar) {
    // Reached from a download callback, after which the tree may have died.
    if (!gcode_viewer_) {
        return;
    }
    viewer_file_ = current_filename_;
    // Set up the (single) load callback, then load the file. The body was
    // identical in the former cached-file and post-download paths.
    helix::ui::gcode_viewer_set_load_callback(
        gcode_viewer_,
        [](lv_obj_t* viewer, void* user_data, bool success) {
            auto* self = static_cast<PrintSelectDetailView*>(user_data);
            if (!success) {
                if (helix::ui::gcode_viewer_load_skipped_for_cost(viewer)) {
                    spdlog::info("[DetailView] G-code preview skipped: too costly on this board "
                                 "now");
                } else {
                    spdlog::warn("[DetailView] G-code load failed");
                }
                self->show_gcode_viewer(false);
                return;
            }
            self->gcode_loaded_ = true;
            // The viewer parse satisfies pre-flight readiness — publish so the
            // skeleton latch opens. try_extract_gcode_colors() below finishes
            // the authoritative chip render in this same callback (one paint).
            self->publish_mapping_ready();

            // Show all layers, no ghost (preview = full model)
            helix::ui::gcode_viewer_set_print_progress(viewer, -1);

            // Apply preview colors respecting the sliced/actual toggle
            // (default actual: AMS/slicer base then mapped overrides).
            self->apply_preview_colors();

            // Extract colors from parsed gcode when metadata lacked them.
            // This also computes preflight_result_ — it MUST run before
            // fire_on_loaded() so any deferred print-attempt sees fresh checks.
            self->try_extract_gcode_colors(viewer);

            // The whole file is parsed: definitions past the scan window now
            // appear, before a deferred Print reads the picks.
            self->refresh_exclude_objects();
            // Parse + pre-flight are now complete: release any deferred
            // run_when_loaded() callback (e.g. a print tapped pre-parse).
            self->fire_on_loaded();
            // The viewer parse also satisfies pre-flight readiness on full
            // platforms — release any run_when_preflight_ready() attempt.
            self->fire_on_preflight_ready();

            // A parse that found no layers has nothing to draw, and revealing
            // it would hide the thumbnail behind an empty viewer.
            if (helix::ui::gcode_viewer_get_layer_count(viewer) == 0) {
                spdlog::debug("[DetailView] G-code has no layers - keeping thumbnail");
                self->show_gcode_viewer(false);
                return;
            }

            // Unpause, show, then reset camera (must be visible for layout)
            helix::ui::gcode_viewer_set_paused(viewer, false);
            self->show_gcode_viewer(true);
            lv_obj_update_layout(viewer);
            helix::ui::gcode_viewer_reset_camera(viewer);

            spdlog::debug("[DetailView] G-code preview loaded successfully");
        },
        this);
    helix::ui::gcode_viewer_set_layer_index_sidecar(gcode_viewer_, std::move(layer_index_sidecar));
    helix::ui::gcode_viewer_load_file(gcode_viewer_, path.c_str());
}

// ============================================================================
// Pre-print Estimate Label Update
// ============================================================================

static void update_prep_time_label() {
    if (!s_detail_view_instance || !s_detail_view_instance->get_prep_manager()) {
        return;
    }
    auto* mgr = s_detail_view_instance->get_prep_manager();
    const auto& metadata = s_detail_view_instance->get_file_metadata();
    mgr->recalculate_estimate(metadata ? &*metadata : nullptr,
                              s_detail_view_instance->current_file_key());

    int estimate_s = lv_subject_get_int(mgr->get_preprint_estimate_subject());

    if (estimate_s <= 0) {
        lv_subject_copy_string(s_detail_view_instance->get_prep_time_estimate_subject(), "");
        s_detail_view_instance->set_prep_estimate_visible(0);
        return;
    }

    // Round: >120s to nearest 30s, <=120s to nearest 10s
    int rounded = estimate_s > 120 ? ((estimate_s + 15) / 30) * 30 : ((estimate_s + 5) / 10) * 10;
    int mins = rounded / 60;
    int secs = rounded % 60;
    char buf[48];
    if (mins > 0 && secs > 0) {
        snprintf(buf, sizeof(buf), "~%d:%02d prep time", mins, secs);
    } else if (mins > 0) {
        snprintf(buf, sizeof(buf), "~%d min prep time", mins);
    } else {
        snprintf(buf, sizeof(buf), "~%d sec prep time", secs);
    }
    lv_subject_copy_string(s_detail_view_instance->get_prep_time_estimate_subject(), buf);
    s_detail_view_instance->set_prep_estimate_visible(1);
}

bool PrintSelectDetailView::apply_file_metadata(const std::string& file_key,
                                                const FileMetadata& metadata) {
    if (file_key != current_file_key()) {
        spdlog::debug("[DetailView] Dropping metadata for {}: {} is shown now", file_key,
                      current_file_key());
        return false;
    }
    cached_file_metadata_ = metadata;
    update_prep_time_label();
    refresh_fit_check();
    return true;
}

// ============================================================================
// Dynamic option-row population
// ============================================================================
//
// `pre_print_options_container_` is populated from the active printer's
// `PrePrintOptionSet`. Each option becomes a row with a label + switch in a
// flat list (categories are sort keys only — no subheaders). This replaces
// the previous hardcoded XML rows + per-option static callbacks.
//
// The renderer owns one heap-allocated lv_subject_t per option (the toggle
// state). We register a state provider on `prep_manager_` so that
// `collect_macro_skip_params()` and friends can read these subjects without
// needing to know about their LVGL pointers — they query by id.
//
// Per-row visibility: the renderer's `VisibilitySubjectLookup` callback is
// invoked for each option; returning nullptr leaves the row unconditionally
// visible. Only the rewrite-gated predicate returns a non-null subject
// (gcode_rewrite_available_, a tri-state); macro-gated options are filtered
// out of the set BEFORE populate() is called (see
// filter_macro_gated_options), so the lookup never sees them.

void PrintSelectDetailView::populate_option_rows() {
    if (!pre_print_options_container_) {
        return;
    }

    if (!printer_state_) {
        return;
    }

    // Macro-analysis rows join the printer's own options when its database
    // entry declares none; see PrintPreparationManager::displayed_options().
    const PrePrintOptionSet option_set =
        prep_manager_ ? prep_manager_->displayed_options()
                      : printer_state_->profile_state().pre_print_option_set();

    // Skip rebuild only when rows are already populated AND neither the active
    // printer nor its option ids have changed since they were built. A
    // mid-session printer-type change (e.g. multi-printer setups) or a macro
    // analysis landing after the first render needs a repopulate.
    //
    // The rebuild path is safe: `populate()` calls `clear()` (which deinits
    // every option subject — uninstalling observers from their row widgets)
    // BEFORE `safe_clean_children` deletes the widgets themselves. That
    // ordering is what the renderer's class doc spells out as "case 1" of
    // the lifetime contract — observers are uninstalled while widgets are
    // still alive, so the deferred widget-delete tick has nothing to do for
    // them. Repopulating mid-session is therefore not the race that this
    // early-return originally guarded against.
    const std::string& current_type = printer_state_->profile_state().printer_type();
    std::string rows_key = current_type;
    for (const auto& opt : option_set.options) {
        rows_key += '\n' + opt.id;
    }
    if (option_rows_renderer_.row_count() > 0 && rows_key == last_rendered_rows_) {
        spdlog::trace("[DetailView] Skipping option-row rebuild (already populated for '{}')",
                      current_type);
        return;
    }
    last_rendered_rows_ = rows_key;

    // Honor `PrePrintOption::requires_macro`: hide options whose required
    // macro isn't registered with Klipper. Their toggles would be inert —
    // collect_pre_start_gcode_lines() already drops their gcode at print
    // start (see ui_print_preparation_manager.cpp), so showing the row only
    // confuses users. e.g. K2 Plus "AI Detect" without LOAD_AI_RUN installed.
    //
    // MacroParamCache is populated once during connection discovery and
    // lives until disconnect; this panel rebuilds on printer-type change, so
    // a populate-time filter is correct (no reactive subject needed, unlike
    // the plugin-gated visibility_lookup below).
    PrePrintOptionSet rendered = filter_macro_gated_options(option_set);
    if (rendered.options.size() != option_set.options.size()) {
        spdlog::debug("[DetailView] Filtered {} macro-gated option row(s) (printer='{}')",
                      option_set.options.size() - rendered.options.size(), current_type);
    }

    // Rewrite-gated visibility: HIDE a toggle only when DISABLING it would
    // require rewriting the job (see
    // PrintPreparationManager::disabling_option_requires_plugin). For those
    // options we bind the row to gcode_rewrite_available_; the renderer hides
    // the row when it reads 0 (no plugin, or a transport that keeps no local
    // copy) and keeps it visible at -1 (still checking, startup window) and 1.
    //
    // CAUTION — do NOT hide options the printer handles natively without the
    // plugin. K2 Plus bed_mesh is a MacroParam whose START_PRINT/PRINT_PREPARED
    // takes the skip directly (setup_gcode present), so the predicate returns
    // false and it stays visible. Hiding it was a shipped regression; the
    // predicate now enforces the distinction structurally.
    auto visibility_lookup = [this, &rendered](const std::string& id) -> lv_subject_t* {
        if (!prep_manager_ || !printer_state_) {
            return nullptr;
        }
        const PrePrintOption* opt = rendered.find(id);
        if (opt && prep_manager_->disabling_option_requires_plugin(*opt)) {
            return &gcode_rewrite_available_;
        }
        return nullptr; // Not plugin-dependent: always visible for declared options.
    };

    option_rows_renderer_.populate(pre_print_options_container_, rendered, visibility_lookup,
                                   [this](const std::string& id, int new_state) {
                                       spdlog::debug("[DetailView] Option '{}' toggled: {}", id,
                                                     new_state);
                                       update_prep_time_label();
                                       // An option can make a slot with no
                                       // material set a block.
                                       recompute_preflight();
                                       notify_print_readiness_changed();
                                   });

    // Wire up the option-state provider on the prep manager so that
    // collect_macro_skip_params() reads from these dynamic subjects. -1 means
    // "not bound" — manager falls back to its legacy subject path or the
    // option's default.
    if (prep_manager_) {
        prep_manager_->set_option_state_provider([this](const std::string& id) -> int {
            // Only return 0/1 for ids the renderer actually has rows for.
            // Otherwise defer to the manager's fallback chain.
            auto ids = option_rows_renderer_.rendered_ids();
            for (const auto& rid : ids) {
                if (rid == id) {
                    return option_rows_renderer_.get_state(id, 0);
                }
            }
            return -1;
        });
    }

    // The rows just changed the scroll area's content height without any
    // scroll or size event, so refresh the cue's data here.
    if (options_scroll_) {
        lv_obj_update_layout(options_scroll_);
        update_options_more_below();
    }
}

void PrintSelectDetailView::defer_detail_fit() {
    // Deferred one tick: these events fire mid-layout-pass, when the scroll
    // area's height still reflects the PREVIOUS card height, and measuring
    // that stale pair oscillates (card shrinks -> stale avail reads smaller
    // -> card grows back -> repeat). After the tick the whole tree is
    // consistent, and a fit that writes nothing ends the cycle. At most one
    // deferral is queued at a time: a scroll drag fires this every frame,
    // and one run after the last frame measures the same tree.
    if (!options_scroll_ || !detail_card_ || fit_pending_) {
        return;
    }
    fit_pending_ = true;
    lifetime_.token().defer("DetailView::fit_portrait_preview", [this]() {
        fit_pending_ = false;
        if (!options_scroll_ || !detail_card_) {
            return;
        }
        update_options_more_below();
        fit_portrait_preview();
    });
}

void PrintSelectDetailView::update_options_more_below() {
    if (!options_scroll_) {
        return;
    }
    const int more = lv_obj_get_scroll_bottom(options_scroll_) > 0 ? 1 : 0;
    lv_subject_set_int(&detail_options_more_below_, more);
}

void PrintSelectDetailView::fit_portrait_preview() {
    lv_obj_t* card = detail_card_;
    if (!card || !options_scroll_) {
        return;
    }
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    if (!portrait || lv_subject_get_int(portrait) == 0) {
        // Landscape: the card fills its column by flex; drop any measured
        // portrait height.
        lv_obj_remove_local_style_prop(card, LV_STYLE_HEIGHT, 0);
        return;
    }
    const int width = lv_obj_get_width(card);
    const int avail = lv_obj_get_height(card) + lv_obj_get_height(options_scroll_);
    const int content = lv_obj_get_scroll_y(options_scroll_) + lv_obj_get_height(options_scroll_) +
                        lv_obj_get_scroll_bottom(options_scroll_);

    // The tile grid's top, in the scroll area's content coordinates.
    // lv_obj_get_y is relative to the parent's content box and folds the
    // parent's scroll offset back in, which for the scroll area's direct
    // child IS the conversion into content coordinates; the intermediates
    // never scroll, so the plain chain sum needs no correction.
    lv_obj_t* grid = pre_print_options_container_;
    int grid_top = 0;
    for (lv_obj_t* o = grid; o && o != options_scroll_; o = lv_obj_get_parent(o)) {
        grid_top += lv_obj_get_y(o);
    }
    lv_obj_t* first_tile = grid ? lv_obj_get_child(grid, 0) : nullptr;
    const int tile_h = first_tile ? lv_obj_get_height(first_tile) : 0;
    const int gap = grid ? lv_obj_get_style_pad_row(grid, LV_PART_MAIN) : 0;

    // Micro and tiny: the same breakpoint gate the XML uses for the inline
    // filament card and the hidden prep line.
    lv_subject_t* const bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    const bool fit_all = bp && lv_subject_get_int(bp) < 2;
    const int h = helix::ui::decide_detail_portrait_preview(width, avail, content, grid_top, tile_h,
                                                            gap, fit_all);
    spdlog::trace("[DetailView] fit_portrait_preview: card {} -> {} (avail {} content {} grid_top "
                  "{} tile {} gap {} fit_all {})",
                  lv_obj_get_height(card), h, avail, content, grid_top, tile_h, gap, fit_all);
    if (lv_obj_get_height(card) != h) {
        // DECLARATIVE_OK: measured layout; the height depends on runtime
        // pixel sizes the XML cannot know.
        lv_obj_set_height(card, h);
    }
}

std::map<std::string, bool> PrintSelectDetailView::collect_option_states() const {
    std::map<std::string, bool> out;
    for (const auto& id : option_rows_renderer_.rendered_ids()) {
        out[id] = option_rows_renderer_.get_state(id, 0) != 0;
    }
    return out;
}

void PrintSelectDetailView::seed_option_states(helix::queue::QueuedJobOptions saved) {
    pending_option_seed_ = std::move(saved);
}

} // namespace helix::ui
