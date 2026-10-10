// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_print_status.h"

#include "ui_ams_current_tool.h"
#include "ui_breakpoint.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_exclude_object_badges.h"
#include "ui_fan_control_overlay.h"
#include "ui_filament_mapping_card.h"
#include "ui_filename_utils.h"
#include "ui_format_utils.h"
#include "ui_gcode_viewer.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_next_tick.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_common.h"
#include "ui_panel_print_select.h"
#include "ui_pause_markers.h"
#include "ui_print_start_controller.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "bed_dimensions.h"
#include "config.h"
#include "display_manager.h"
#include "display_numbering.h"
#include "display_settings_manager.h"
#include "filament_mapper.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "gcode_parser.h"
#include "gcode_preview_fetcher.h"
#include "gcode_preview_setup.h"
#include "gcode_render_mode_policy.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "layout_manager.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "observer_factory.h"
#include "pre_start_exclude.h"
#include "preprint_predictor.h"
#include "print_start_checks.h"
#include "print_status_layout_decision.h"
#include "print_status_preview_decision.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "system/crash_handler.h"
#include "text_io.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "tune_controller.h"
#include "ui/fan_spin_animation.h"
#include "ui/ui_widget_helpers.h"
#include "wizard_config_paths.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::gcode::resolve_gcode_filename;

namespace tio = helix::text_io;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#if HELIX_HAS_CAMERA
// Defined in src/ui/panel_widgets/camera_widget.cpp; that directory is not on
// this file's include path, so forward-declare rather than including the
// header (same pattern as ui_settings_hardware.cpp).
namespace helix {
void open_standalone_camera_fullscreen(lv_obj_t* parent_screen);
}
#endif

// Take the panel's LV_EVENT_DELETE hook off the tree @p root names and clear
// the tracking pointer. The lv_is_initialized() guard is what makes this safe
// from ~PrintStatusPanel(), which can run during static destruction after
// lv_deinit(). Shared by all three uninstall paths (destructor, explicit
// teardown, and the re-install in create()) so they cannot drift apart.
static void uninstall_root_delete_hook(lv_obj_t*& root, lv_event_cb_t cb, void* owner) {
    if (root != nullptr && lv_is_initialized()) {
        lv_obj_remove_event_cb_with_user_data(root, cb, owner);
    }
    root = nullptr;
}

// Cached widget pointer for lazy creation (separate from overlay_root_ which
// is managed by OverlayBase). Declared here so teardown callback can null it.
static lv_obj_t* s_cached_panel = nullptr;

// ID of the MemoryMonitor pressure responder. 0 means "not registered".
// Registered lazily on first push_overlay(); unregistered in the static
// panel-destroy callback to prevent calls into a destroyed singleton.
static helix::MemoryMonitor::PressureResponderId s_memory_responder_id = 0;

// Each print status tree this process creates takes the next number, so every
// creation and destruction line names its tree, and a repeat creation can say
// how the tree before it died.
using helix::ui::PrintStatusTreeDestroyCause;

static int s_tree_number = 0;
static int s_last_destroyed_tree = 0;
static PrintStatusTreeDestroyCause s_last_destroy_cause = PrintStatusTreeDestroyCause::OverlayClose;
static PrintState s_last_destroy_state = PrintState::Idle;

static void log_tree_destroyed(PrintStatusTreeDestroyCause cause, PrintState state) {
    s_last_destroyed_tree = s_tree_number;
    s_last_destroy_cause = cause;
    s_last_destroy_state = state;
    spdlog::info("[PrintStatusPanel] Print status tree #{} destroyed: {} while {}", s_tree_number,
                 helix::ui::print_status_tree_destroy_cause_name(cause), print_state_name(state));
}

// On a device logging at WARN a repeat creation is the only trace that the
// preview was rebuilt, so print_status_recreation_warns() decides its level.
static void log_tree_created(PrintState state, size_t available_mb) {
    ++s_tree_number;
    if (s_tree_number == 1) {
        spdlog::info("[PrintStatusPanel] Print status tree #1 created while {} ({}MB available)",
                     print_state_name(state), available_mb);
        return;
    }
    const bool destruction_recorded = s_last_destroyed_tree == s_tree_number - 1;
    const spdlog::level::level_enum level =
        helix::ui::print_status_recreation_warns(state, destruction_recorded, s_last_destroy_cause,
                                                 s_last_destroy_state)
            ? spdlog::level::warn
            : spdlog::level::info;
    if (destruction_recorded) {
        spdlog::log(level,
                    "[PrintStatusPanel] Print status tree #{} created while {} ({}MB available); "
                    "tree #{} was destroyed: {} while {}",
                    s_tree_number, print_state_name(state), available_mb, s_last_destroyed_tree,
                    helix::ui::print_status_tree_destroy_cause_name(s_last_destroy_cause),
                    print_state_name(s_last_destroy_state));
    } else {
        spdlog::log(level,
                    "[PrintStatusPanel] Print status tree #{} created while {} ({}MB available); "
                    "tree #{} has no recorded destruction",
                    s_tree_number, print_state_name(state), available_mb, s_tree_number - 1);
    }
}

// Observer factory pattern
using helix::ui::find_required;
using helix::ui::observe;
using helix::ui::observe_print_state;

// Helper to get or create the global instance
PrintStatusPanel& get_global_print_status_panel() {
    return helix::lazy_global_with_teardown<PrintStatusPanel>(
        "PrintStatusPanel",
        []() {
            if (s_memory_responder_id != 0) {
                helix::MemoryMonitor::instance().remove_pressure_responder(s_memory_responder_id);
                s_memory_responder_id = 0;
            }
            PrintStatusPanel::destroy_cached_overlay(
                PrintStatusTreeDestroyCause::PanelRegistryTeardown);
            s_cached_panel = nullptr;
        },
        get_printer_state(), nullptr);
}

// Drop the cached widget tree if we can, to reclaim ~400-800KB.
// Safe to call from any thread — hops to UI thread via queue_update and
// bails out if the overlay is currently visible. No-op when there's no
// cached tree (e.g. print status was never opened this session).
static void try_reclaim_cached_print_status() {
    helix::ui::queue_update("ui_panel_print_status::try_reclaim_cached_print_status", []() {
        if (!s_cached_panel) {
            return;
        }
        if (helix::nav::is_in_stack(s_cached_panel)) {
            spdlog::debug(
                "[PrintStatusPanel] Cached tree is currently visible, skipping memory reclaim");
            return;
        }
        if (!helix::lazy_global_if_exists<PrintStatusPanel>()) {
            return;
        }
        spdlog::warn("[PrintStatusPanel] Pressure response: destroying cached overlay tree");
        // Nulls s_cached_panel; the next push_overlay() recreates the tree.
        PrintStatusPanel::destroy_cached_overlay(PrintStatusTreeDestroyCause::MemoryReclaim);
    });
}

PrintStatusPanel::PrintStatusPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : printer_state_(printer_state), api_(api),
      preview_(get_name(), printer_state, lifecycle_,
               {[this](bool show) { show_gcode_viewer(show); },
                [this]() {
                    recompute_scoped_runout();
                    exclude_mode_.refresh_render_badges();
                },
                [this]() { return is_active_; }}),
      progress_text_(printer_state, lifecycle_),
      layout_fitter_(
          get_name(), printer_state,
          {&fan_row_density_subject_, &aux_icon_visible_subject_, &aux_full_visible_subject_,
           &aux_short_visible_subject_, &fans_fit_subject_, &graph_fits_subject_},
          {[this]() { return overlay_root_; }, [this]() { return subjects_initialized_; },
           [this]() { return !aux_fan_name_.empty(); }}) {
    preview_.set_api(api);
    // Pre-init local subject used by observer callback below (fires immediately on subscribe)
    lv_subject_init_int(&exclude_objects_available_subject_, 0);

    // Death signal for every PrinterState-owned subject observed below. This panel
    // is a process-lifetime singleton (get_global_print_status_panel()), so it
    // routinely outlives a PrinterState::deinit_subjects() cycle — printer
    // switching in production, per-fixture teardown in tests. Without the token
    // each guard keeps a pointer to an observer node that lv_subject_deinit()
    // already freed, and the next reset() calls lv_observer_remove() on freed
    // memory: SIGSEGV at lv_observer.c:584 dereferencing observer->subject.
    // Subjects owned by this panel or by other singletons take no token here.
    const SubjectLifetime ps_subjects = printer_state_.get_subjects_lifetime();

    // Subscribe to temperature subjects using bundle (replaces 4 individual observers)
    temp_observers_.setup_sync(
        this, printer_state_, [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); });

    // Subscribe to active tool changes (refreshes nozzle temp with tool name prefix)
    active_tool_observer_ = observe<int>(
        helix::ToolState::instance().get_active_tool_subject(), this,
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        helix::ToolState::instance().get_subjects_lifetime());

    // Chamber status text: observe chamber temp to compute Heating/Cooling/Holding status
    chamber_temp_observer_ = observe<int>(
        printer_state_.temperature_state().get_chamber_temp_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_chamber_status(); }, ps_subjects);

    // Subscribe to print progress and state
    print_progress_observer_ = observe<int>(
        printer_state_.print_state().get_print_progress_subject(), this,
        [](PrintStatusPanel* self, int progress) { self->on_print_progress_changed(progress); },
        ps_subjects);
    print_state_observer_ = observe_print_state<PrintStatusPanel>(
        // RAW_PRINT_STATE_OK: the panel's lifecycle_ adopts the published
        // PrintState (Phase 0b); this observer feeds it the wire transition that
        // derive_print_state() needs alongside the live phase.
        printer_state_.print_state().get_print_state_enum_subject(), this,
        [](PrintStatusPanel* self, PrintJobState state) { self->on_print_state_changed(state); },
        ps_subjects);
    // The print's identity can change without the reported filename changing -
    // an override installed at commit, or released when a job is abandoned - so
    // the filename observer below is not enough on its own. This is the same
    // reconcile the old set_thumbnail_source() forced by calling set_filename()
    // on itself, minus the coupling that let the panel and the media manager
    // drift apart (prestonbrown/helixscreen#1339).
    print_identity_observer_ = observe<int>(
        printer_state_.print_state().get_print_identity_epoch_subject(), this,
        [](PrintStatusPanel* self, int /*epoch*/) {
            // No marker clearing here on purpose. decide_preview_action() already
            // compares BOTH markers against the new identity and reloads whichever
            // is stale; clearing by hand duplicates that, and clearing only the
            // thumbnail marker leaves the viewer holding the previous print's
            // geometry.
            self->preview_.ensure_current();
        },
        ps_subjects);

    print_filename_observer_ = observe<const char*>(
        printer_state_.print_state().get_print_filename_subject(), this,
        [](PrintStatusPanel* self, const char* filename) {
            self->on_print_filename_changed(filename);
        },
        ps_subjects);

    // Subscribe to speed/flow factors
    speed_factor_observer_ = observe<int>(
        printer_state_.motion_state().get_speed_factor_subject(), this,
        [](PrintStatusPanel* self, int speed) { self->on_speed_factor_changed(speed); },
        ps_subjects);
    flow_factor_observer_ = observe<int>(
        printer_state_.motion_state().get_flow_factor_subject(), this,
        [](PrintStatusPanel* self, int flow) { self->on_flow_factor_changed(flow); }, ps_subjects);
    // The physical-units readout also moves with the live toolhead and
    // extruder velocities, and with the speed/flow units preference.
    live_velocity_observer_ = observe<int>(
        printer_state_.motion_state().get_live_velocity_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_speed_flow_text(); }, ps_subjects);
    extruder_velocity_observer_ = observe<int>(
        printer_state_.motion_state().get_live_extruder_velocity_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_speed_flow_text(); }, ps_subjects);
    physical_units_observer_ = observe<int>(
        DisplaySettingsManager::instance().subject_speed_flow_physical_units(), this,
        [](PrintStatusPanel* self, int) { self->update_speed_flow_text(); },
        DisplaySettingsManager::instance().get_subjects_lifetime());
    gcode_z_offset_observer_ = observe<int>(
        printer_state_.motion_state().get_gcode_z_offset_subject(), this,
        [](PrintStatusPanel* self, int microns) { self->on_gcode_z_offset_changed(microns); },
        ps_subjects);

    // Subscribe to layer tracking for G-code viewer ghost layer updates
    print_layer_observer_ = observe<int>(
        printer_state_.print_state().get_print_layer_current_subject(), this,
        [](PrintStatusPanel* self, int layer) { self->on_print_layer_changed(layer); },
        ps_subjects);

    // Re-render layer text when Z position changes (Z updates more frequently than layer count)
    z_position_observer_ = observe<int>(
        printer_state_.motion_state().get_gcode_position_z_subject(), this,
        [](PrintStatusPanel* self, int) {
            int layer = lv_subject_get_int(
                self->printer_state_.print_state().get_print_layer_current_subject());
            self->on_print_layer_changed(layer);
        },
        ps_subjects);

    // Subscribe to wall-clock elapsed time (total_duration includes prep time)
    print_duration_observer_ = observe<int>(
        printer_state_.print_state().get_print_elapsed_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_print_duration_changed(seconds); },
        ps_subjects);
    print_time_left_observer_ = observe<int>(
        printer_state_.print_state().get_print_time_left_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_print_time_left_changed(seconds); },
        ps_subjects);

    // Subscribe to print start preparation phase subjects
    print_start_phase_observer_ = observe<int>(
        printer_state_.print_state().get_print_start_phase_subject(), this,
        [](PrintStatusPanel* self, int phase) { self->on_print_start_phase_changed(phase); },
        ps_subjects);
    print_start_progress_observer_ = observe<int>(
        printer_state_.print_state().get_print_start_progress_subject(), this,
        [](PrintStatusPanel* self, int progress) {
            self->on_print_start_progress_changed(progress);
        },
        ps_subjects);
    preprint_remaining_observer_ = observe<int>(
        printer_state_.print_state().get_preprint_remaining_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_preprint_remaining_changed(seconds); },
        ps_subjects);
    preprint_elapsed_observer_ = observe<int>(
        printer_state_.print_state().get_preprint_elapsed_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_preprint_elapsed_changed(seconds); },
        ps_subjects);

    // The objects button needs [exclude_object] and a multi-object print;
    // either can arrive first.
    const auto refresh_available = [](PrintStatusPanel* self, int) {
        self->refresh_exclude_objects_available();
    };
    exclude_objects_observer_ =
        observe<int>(printer_state_.excluded_objects_state().get_defined_objects_version_subject(),
                     this, refresh_available, ps_subjects);
    exclude_object_capability_observer_ = observe<int>(
        printer_state_.capabilities_state().subject(helix::Capability::HasExcludeObject), this,
        refresh_available, ps_subjects);

    // Subscribe to excluded objects changes (for "X of Y obj" count updates)
    excluded_objects_version_observer_ = observe<int>(
        printer_state_.excluded_objects_state().get_excluded_objects_version_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_objects_text(); }, ps_subjects);

    // Subscribe to AMS current filament color for gcode viewer color override
    // When a known filament color is available (from Spoolman spool or AMS lane),
    // use it instead of the gcode metadata color for the 2D/3D render
    ams_color_observer_ = observe<int>(
        AmsState::instance().get_current_color_subject(), this,
        [](PrintStatusPanel* self, int /*color_rgb*/) { self->preview_.apply_tool_colors(); },
        AmsState::instance().get_subjects_lifetime());

    // Also refresh gcode viewer colors when tool_to_slot_map changes (user remap)
    tool_map_version_observer_ = observe<int>(
        AmsState::instance().get_tool_map_version_subject(), this,
        [](PrintStatusPanel* self, int /*version*/) { self->preview_.apply_tool_colors(); },
        AmsState::instance().get_subjects_lifetime());

    // Adopt the preparing job's identity the moment a job starts preparing,
    // mirroring the observer ActivePrintMediaManager already has for the same
    // purpose. Without it the panel's `desired` stays on the PREVIOUS print
    // for the whole commit-to-confirmation window, so ensure_current()
    // compares the viewer against the finished print, finds no mismatch, and
    // clear_gcode never fires - leaving the previous print's model on screen
    // exactly when it was meant to be dropped.
    //
    // Dispatch::Immediate for the manager's reason: the deferred default routes
    // through queue_update, so the identity would land AFTER a synchronously dispatched
    // filename update had already reconciled against the stale name. The
    // handler only assigns identity fields and reconciles the preview - no
    // observer lifecycle changes, no widget destruction.
    // Subscribe to the shared print thumbnail path. ActivePrintMediaManager is
    // its sole writer; this panel only reads it.
    // Use Dispatch::Immediate: the handler only calls lv_image_set_src
    // (no observer lifecycle changes), and set_print_thumbnail is always called
    // from the UI thread via queue_update.
    print_thumbnail_path_observer_ = ui::observe<const char*>(
        printer_state_.print_state().get_print_thumbnail_path_subject(), this,
        [](PrintStatusPanel* self, const char* path) {
            self->preview_.on_thumbnail_published(path);
        },
        ps_subjects, ui::Dispatch::Immediate);

#if defined(HELIX_PLATFORM_ESP32)
    // ESP32 has no disk thumbnail cache, so print_thumbnail_path stays empty and
    // the image arrives as a PSRAM buffer instead. Observe the generation counter
    // ActivePrintMediaManager bumps when it installs one. observe<int>
    // for the same reason as the path observer above: the handler only does
    // lv_image_set_src plus a shared_ptr swap (no observer lifecycle changes, no
    // widget destruction), and the setter always runs on the UI thread — so the
    // extra deferral would only add a frame and a stale-read window.
    print_psram_thumb_observer_ = ui::observe<int>(
        printer_state_.print_state().get_print_psram_thumb_gen_subject(), this,
        [](PrintStatusPanel* self, int /*gen*/) { self->preview_.apply_psram_thumbnail(); },
        ps_subjects, ui::Dispatch::Immediate);
#endif

    spdlog::debug("[{}] Subscribed to PrinterState subjects", get_name());

    // Subscribe to G-code render mode changes from settings panel
    // This allows real-time updates to the viewer when the user changes the setting
    gcode_render_mode_observer_ = observe<int>(
        DisplaySettingsManager::instance().subject_gcode_render_mode(), this,
        [](PrintStatusPanel* self, int mode) {
            // A command-line mode or HELIX_GCODE_MODE outranks the saved setting, so a
            // settings change (including the one the subject fires at startup with the
            // persisted value) must not reach the viewer while either is set.
            const auto* rt_config = get_runtime_config();
            const auto decision = helix::gcode_viewer::decide_preview_mode(
                rt_config ? rt_config->gcode_render_mode : -1,
                std::getenv("HELIX_GCODE_MODE") != nullptr, mode);
            if (decision.source == helix::gcode_viewer::PreviewModeSource::CommandLine ||
                decision.source == helix::gcode_viewer::PreviewModeSource::Environment) {
                spdlog::debug("[{}] Ignoring settings render mode {} - pinned above settings",
                              self->get_name(), mode);
                return;
            }
            spdlog::info("[{}] G-code render mode changed from settings: {}", self->get_name(),
                         mode);
            if (self->gcode_viewer_ && self->is_active_) {
                // Apply the new render mode (skip "Thumbnail Only" mode = 3)
                if (mode == 3) {
                    // Thumbnail Only - hide the viewer
                    self->show_gcode_viewer(false);
                } else {
                    auto render_mode = static_cast<GcodeViewerRenderMode>(mode);
                    ui_gcode_viewer_set_render_mode(self->gcode_viewer_, render_mode);
                    // Update viewer mode subject to trigger XML visibility bindings
                    if (ui_gcode_viewer_has_content(self->gcode_viewer_)) {
                        self->show_gcode_viewer(true);
                    }
                }
            }
        },
        DisplaySettingsManager::instance().get_subjects_lifetime());
    spdlog::debug("[{}] G-code render mode observer registered", get_name());

    // End-overlay visibility: derive three show_* bool subjects from print_outcome
    // and end_overlay_dismissed_. XML binds each overlay's hidden flag to a single
    // subject, avoiding the L042 two-observer race that made the error overlay
    // pop at startup when end_overlay_dismissed==0 unhide-raced the outcome check.
    print_outcome_observer_ = observe<int>(
        printer_state_.print_state().get_print_outcome_subject(), this,
        [](PrintStatusPanel* self, int) { self->recompute_end_overlay_visibility(); }, ps_subjects);
    recompute_end_overlay_visibility();

    // Create filament runout handler (extracted from PrintStatusPanel)
    runout_handler_ = std::make_unique<helix::ui::FilamentRunoutHandler>(api_);
    spdlog::debug("[{}] Created filament runout handler", get_name());
}

PrintStatusPanel::~PrintStatusPanel() {
    // The widget tree can outlive this panel: StaticPanelRegistry::destroy_all()
    // runs before lv_deinit(), and destroy_overlay_ui()'s deletion is deferred.
    // Uninstall the delete hook while the object is still valid, or the tree's
    // eventual teardown would call on_root_deleted() on freed memory. A null
    // delete_hook_root_ means the tree already died (the hook fired) or the
    // explicit teardown path removed it — nothing left to uninstall.
    uninstall_root_delete_hook(delete_hook_root_, on_root_deleted, this);

    // Before deinit_subjects(): the mini-graph's observers are attached to
    // PrinterState subjects, and detaching them after those are freed is the
    // exact use-after-free ObserverGuard exists to prevent. Synchronous delete —
    // nothing will drain the async queue on the way out.
    layout_fitter_.destroy_temp_graph(/*defer_delete=*/false);

    deinit_subjects();

    // Expire all outstanding async callback tokens before destroying resources
    lifetime_.invalidate();

    preview_.cancel_pending_load();
    cancel_preparing_show_timer();

    // ObserverGuard handles observer cleanup automatically
    resize_registered_ = false;

    preview_.discard_file();

    // CRITICAL: Check if LVGL is still initialized before calling LVGL functions.
    // During static destruction, LVGL may already be torn down.
    if (lv_is_initialized()) {
        // Note: lv_anim_delete() is NOT called here for bar widgets because
        // LVGL bar animations use var=&bar->cur_value_anim (internal struct),
        // not the bar object pointer. Passing the bar pointer misses the
        // animation entirely. lv_bar_destructor() handles cancellation
        // correctly using the internal pointers when lv_obj_delete() runs.

        // Deinit exclude manager before LVGL teardown
        if (exclude_manager_) {
            exclude_manager_->deinit();
        }
        // Modal subclasses (runout_modal_, etc.) use RAII cleanup
        // Their destructors will call hide() automatically
    }
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void PrintStatusPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize all subjects with default values
    // Note: Display filename is now handled by ActivePrintMediaManager via print_display_filename
    progress_text_.init_subjects(subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_status_subject_, nozzle_status_buf_, "", "print_nozzle_status",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_status_subject_, bed_status_buf_, "", "print_bed_status",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(chamber_status_subject_, chamber_status_buf_, "",
                              "print_chamber_status", subjects_);
    UI_MANAGED_SUBJECT_INT(nozzle_status_state_subject_, 0, "print_nozzle_status_state", subjects_);
    UI_MANAGED_SUBJECT_INT(bed_status_state_subject_, 0, "print_bed_status_state", subjects_);
    UI_MANAGED_SUBJECT_INT(chamber_status_state_subject_, 0, "print_chamber_status_state",
                           subjects_);
    UI_MANAGED_SUBJECT_STRING(objects_text_subject_, objects_text_buf_, "", "print_objects_text",
                              subjects_);
    // View toggle icon: starts as cube (progress view), flips to layers on complete view.
    // Populated lazily at first update (icon_cube const resolves only after globals load).
    UI_MANAGED_SUBJECT_STRING(view_toggle_icon_subject_, view_toggle_icon_buf_, "",
                              "view_toggle_icon", subjects_);
    // Camera button label: "Camera" only where Row 2 has breathing room (narrow
    // axis >= LARGE, e.g. 1024x600); "Cam" at MEDIUM and below (800x480 and the
    // cramped portrait row) where the full word barely fits. The XML expression
    // engine is int-only, so a string label conditioned on ui_breakpoint needs
    // this subject — same shape as view_toggle_icon above.
    UI_MANAGED_SUBJECT_STRING(camera_button_label_subject_, camera_button_label_buf_, lv_tr("Cam"),
                              "camera_button_label", subjects_);
    if (lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint")) {
        update_camera_button_label(lv_subject_get_int(bp));
        auto token = lifetime_.token();
        camera_label_observer_ = observe<int>(
            bp, this,
            [token](PrintStatusPanel* self, int value) {
                if (token.expired())
                    return;
                self->update_camera_button_label(value);
            },
            subject_never_freed());
    }

    // Initialize light/timelapse controls (extracted Phase 2)
    light_timelapse_controls_.init_subjects();
    light_timelapse_controls_.set_api(api_);
    set_global_light_timelapse_controls(&light_timelapse_controls_);

    // Preparing state subjects
    UI_MANAGED_SUBJECT_INT(preparing_visible_subject_, 0, "preparing_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(preparing_progress_subject_, 0, "preparing_progress", subjects_);

    // Progress bar subject (integer 0-100 for XML bind_value)

    // Viewer mode subject (0=thumbnail, 1=3D gcode viewer, 2=2D gcode viewer)
    UI_MANAGED_SUBJECT_INT(gcode_viewer_mode_subject_, 0, "gcode_viewer_mode", subjects_);
    UI_MANAGED_SUBJECT_INT(exclude_map_active_subject_, 0, "exclude_map_active", subjects_);
    UI_MANAGED_SUBJECT_INT(end_overlay_dismissed_subject_, 0, "end_overlay_dismissed", subjects_);

    // Fan row adaptive-fit + aux presence subjects (set by recompute_fans_fit
    // and bind_fan_speeds respectively; default to 0 so the row stays hidden
    // until the first recompute fires after attach).
    UI_MANAGED_SUBJECT_INT(fans_fit_subject_, 0, "print_status_fans_fit", subjects_);

    // Portrait temperature mini-graph fit (set by recompute_graph_fits from the
    // slack the preview aspect cap parks in the absorber). Defaults to 0 so the
    // graph stays hidden until the first measured recompute after attach —
    // landscape and every non-capped portrait size never leave that state.
    UI_MANAGED_SUBJECT_INT(graph_fits_subject_, 0, "print_status_graph_fits", subjects_);
    UI_MANAGED_SUBJECT_INT(aux_fan_present_subject_, 0, "print_status_aux_fan_present", subjects_);

    // Density + composite subjects for 3-tier adaptive content.
    UI_MANAGED_SUBJECT_INT(fan_row_density_subject_, 0, "print_status_fan_row_density", subjects_);
    UI_MANAGED_SUBJECT_INT(aux_icon_visible_subject_, 0, "print_status_aux_icon_visible",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(aux_full_visible_subject_, 0, "print_status_aux_full_visible",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(aux_short_visible_subject_, 0, "print_status_aux_short_visible",
                           subjects_);

    // Fan classification refresh: on discovery (structural, fans_version) and on
    // runtime part-fan reassignment as fans start/stop (primary_fans_version, #1124).
    {
        auto token = lifetime_.token();
        fans_version_observer_ = observe<int>(
            printer_state_.fan_state().get_fans_version_subject(), this,
            [token](PrintStatusPanel* self, int /*v*/) {
                if (token.expired())
                    return;
                self->bind_fan_observers();
            },
            printer_state_.get_subjects_lifetime());
    }
    {
        auto token = lifetime_.token();
        primary_fans_version_observer_ = observe<int>(
            printer_state_.fan_state().get_primary_fans_version_subject(), this,
            [token](PrintStatusPanel* self, int /*v*/) {
                if (token.expired())
                    return;
                self->bind_fan_observers();
            },
            printer_state_.get_subjects_lifetime());
    }

    // Density + fit recompute on breakpoint change
    {
        lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
        if (bp) {
            auto token = lifetime_.token();
            breakpoint_observer_ = observe<int>(
                bp, this,
                [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->layout_fitter_.recompute_fans_density();
                    self->layout_fitter_.recompute_fans_fit();
                },
                subject_never_freed());
        }
    }

    // Density + fit recompute when filament sensor count changes
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "filament_sensor_count");
        if (s) {
            auto token = lifetime_.token();
            filament_sensor_count_observer_ = observe<int>(
                s, this,
                [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->layout_fitter_.recompute_fans_density();
                    self->layout_fitter_.recompute_fans_fit();
                },
                FilamentSensorManager::instance().get_subjects_lifetime());
        }
    }

    // Density + fit recompute when AMS slot count changes
    {
        lv_subject_t* s = AmsState::instance().get_slot_count_subject();
        if (s) {
            auto token = lifetime_.token();
            ams_slot_count_observer_ = observe<int>(
                s, this,
                [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->layout_fitter_.recompute_fans_density();
                    self->layout_fitter_.recompute_fans_fit();
                },
                AmsState::instance().get_subjects_lifetime());
        }
    }

    // Density + fit recompute when toolchange panel appears/disappears
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "toolchange_visible");
        if (s) {
            auto token = lifetime_.token();
            toolchange_visible_observer_ = observe<int>(
                s, this,
                [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->layout_fitter_.recompute_fans_density();
                    self->layout_fitter_.recompute_fans_fit();
                },
                AmsState::instance().get_subjects_lifetime());
        }
    }

    // Print-scoped runout badge (FIX B): the badge VALUE is AMS lane truth
    // (filament_exist via is_present), so it must refresh on BOTH triggers:
    //   1. the motion-sensor runout subject (a sensor edge), and
    //   2. AMS lane-presence changes (slots_version, bumped whenever slot data
    //      mutates) — so a lane emptying mid-print without a motion-sensor edge
    //      still refreshes the badge (issue 2).
    // The gcode-load / state-change paths also call recompute directly so a
    // newly-parsed file refreshes the badge even if neither subject moved.
    {
        lv_subject_t* s = FilamentSensorManager::instance().get_runout_detected_subject();
        if (s) {
            auto token = lifetime_.token();
            scoped_runout_observer_ = observe<int>(
                s, this,
                [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->recompute_scoped_runout();
                },
                FilamentSensorManager::instance().get_subjects_lifetime());
        }
    }
    {
        lv_subject_t* s = AmsState::instance().get_slots_version_subject();
        if (s) {
            auto token = lifetime_.token();
            scoped_runout_slots_observer_ = observe<int>(
                s, this,
                [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->recompute_scoped_runout();
                    // Slot data changed — a lane's color may have. The two
                    // observers above only cover the ACTIVE lane's color and an
                    // explicit tool→slot remap, so editing a NON-active lane's
                    // color never re-pushed anything to the live preview.
                    // PrintSelectDetailView already refreshes its preview from
                    // this subject, which is why the file browser updated and
                    // print-status did not.
                    self->preview_.apply_tool_colors();
                },
                AmsState::instance().get_subjects_lifetime());
        }
    }

    // Animation-settings refresh
    animations_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
    {
        auto token = lifetime_.token();
        animations_enabled_observer_ = observe<int>(
            DisplaySettingsManager::instance().subject_animations_enabled(), this,
            [token](PrintStatusPanel* self, int enabled) {
                if (token.expired())
                    return;
                self->animations_enabled_ = (enabled != 0);
                self->refresh_fan_animations();
            },
            DisplaySettingsManager::instance().get_subjects_lifetime());
    }

    end_overlay_dismissed_observer_ = observe<int>(
        &end_overlay_dismissed_subject_, this,
        [](PrintStatusPanel* self, int) { self->recompute_end_overlay_visibility(); },
        get_subjects_lifetime());

    // Derived show flags — computed in recompute_end_overlay_visibility() from
    // print_outcome + end_overlay_dismissed. Replaces the racy pair of XML
    // bind_flag observers per overlay (issue L042).
    UI_MANAGED_SUBJECT_INT(show_complete_overlay_subject_, 0, "show_complete_overlay", subjects_);
    UI_MANAGED_SUBJECT_INT(show_cancelled_overlay_subject_, 0, "show_cancelled_overlay", subjects_);
    UI_MANAGED_SUBJECT_INT(show_error_overlay_subject_, 0, "show_error_overlay", subjects_);

    // Pause overlay subjects + observer on print_stats.message. The state-based
    // visibility (show_paused_overlay) is driven from on_print_state_changed(),
    // which already runs on every PrintJobState transition. The reason text,
    // however, can mutate while the printer remains in PAUSED (Klipper updates
    // print_stats.message), so we also recompute on message change.
    UI_MANAGED_SUBJECT_INT(show_paused_overlay_subject_, 0, "show_paused_overlay", subjects_);
    UI_MANAGED_SUBJECT_STRING(print_pause_reason_subject_, print_pause_reason_buf_, "",
                              "print_pause_reason", subjects_);
    UI_MANAGED_SUBJECT_INT(print_pause_reason_visible_subject_, 0, "print_pause_reason_visible",
                           subjects_);
    print_message_observer_ = observe<const char*>(
        printer_state_.print_state().get_print_message_subject(), this,
        [](PrintStatusPanel* self, const char*) { self->recompute_paused_overlay_visibility(); },
        printer_state_.get_subjects_lifetime());

    // Re-evaluate the paused overlay whenever the shared controller's pending
    // action flips (optimistic Pausing/Resuming) — decoupled from our own
    // print_state_enum observer to avoid an ordering race between the two.
    pending_action_observer_ = observe<int>(
        helix::ui::PrintControlButtons::instance().pending_action_subject(), this,
        [](PrintStatusPanel* self, int) { self->recompute_paused_overlay_visibility(); },
        helix::ui::PrintControlButtons::instance().get_subjects_lifetime());

    // Button enable states driven declaratively from XML (see update_button_states).
    UI_MANAGED_SUBJECT_INT(print_controls_enabled_subject_, 0, "print_controls_enabled", subjects_);

    // Exclude objects availability (0=hidden, 1=visible - shown when >= 2 objects defined)
    // Note: subject already initialized in constructor (needed before observer fires)
    helix::xml::register_subject_in_current_scope("exclude_objects_available",
                                                  &exclude_objects_available_subject_);
    subjects_.register_subject(&exclude_objects_available_subject_, "exclude_objects_available");
    SubjectDebugRegistry::instance().register_subject(&exclude_objects_available_subject_,
                                                      "exclude_objects_available",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    // Register XML event callbacks for print status panel buttons
    // (tune overlay subjects/callbacks registered by singleton on first show())
    // (light and timelapse callbacks are registered by light_timelapse_controls_.init_subjects())
    register_xml_callbacks({
        {"on_print_status_tune",
         [](lv_event_t*) { get_global_print_status_panel().handle_tune_button(); }},
        {"on_print_status_units_toggle",
         [](lv_event_t*) {
             auto& settings = DisplaySettingsManager::instance();
             settings.set_speed_flow_physical_units(!settings.get_speed_flow_physical_units());
         }},
        {"on_print_status_camera",
         [](lv_event_t*) {
#if HELIX_HAS_CAMERA
             // No-ops when no webcam is configured or a fullscreen view is already
             // open; reuses an attached home CameraWidget's stream when one exists
             // (single-MJPEG-client safe).
             helix::open_standalone_camera_fullscreen(lv_display_get_screen_active(nullptr));
#else
             spdlog::debug("[PrintStatusPanel] Camera support disabled in this build");
#endif
         }},
        {"on_print_status_files",
         [](lv_event_t*) { get_global_print_status_panel().handle_files_click(); }},
        {"on_print_status_reprint",
         [](lv_event_t*) { get_global_print_status_panel().handle_reprint_button(); }},
        {"on_temp_card_clicked",
         [](lv_event_t*) { get_global_print_status_panel().handle_temp_card_click(); }},
        // The mini-graph is a summary; the full overlay is the detail view. Tapping it
        // opens exactly what tapping the temp chips opens, so both entry points land on
        // one code path rather than two that can drift.
        {"on_print_status_graph_clicked",
         [](lv_event_t*) { get_global_print_status_panel().handle_temp_card_click(); }},
        {"on_print_status_objects",
         [](lv_event_t*) { get_global_print_status_panel().handle_objects_toggle(); }},
        {"on_view_toggle",
         [](lv_event_t*) { get_global_print_status_panel().handle_view_toggle(); }},
        {"on_print_status_dismiss_overlay",
         [](lv_event_t*) {
             // XML binding on each overlay hides when end_overlay_dismissed == 1.
             lv_subject_set_int(&get_global_print_status_panel().end_overlay_dismissed_subject_, 1);
             spdlog::debug("[PrintStatusPanel] Dismissed print end overlay");
         }},
        {"on_print_status_fans_clicked",
         [](lv_event_t*) { get_global_print_status_panel().handle_fans_click(); }},
    });

    subjects_initialized_ = true;

    // Initial sync of the paused overlay — observers only fire on CHANGE, so
    // a mid-print attach where state is already PAUSED would leave the badge
    // hidden without this explicit recompute.
    recompute_paused_overlay_visibility();

    // Sync initial state from PrinterState (in case app opens while print is in progress)
    // This is necessary because observers only fire on VALUE CHANGE, not on subscribe.
    int initial_progress =
        lv_subject_get_int(printer_state_.print_state().get_print_progress_subject());
    int initial_layer =
        lv_subject_get_int(printer_state_.print_state().get_print_layer_current_subject());
    int initial_total_layers =
        lv_subject_get_int(printer_state_.print_state().get_print_layer_total_subject());
    if (initial_progress > 0 || initial_layer > 0 || initial_total_layers > 0) {
        lifecycle_.on_progress_changed(initial_progress);
        lifecycle_.on_layer_changed(initial_layer, initial_total_layers,
                                    printer_state_.print_state().has_real_layer_data());
        update_all_displays();
        spdlog::debug("[{}] Synced initial print state: progress={}%, layer={}/{}", get_name(),
                      initial_progress, initial_layer, initial_total_layers);
    }

    // Sync initial preparation state from PrinterState (in case panel opens mid-preparation)
    int initial_phase =
        lv_subject_get_int(printer_state_.print_state().get_print_start_phase_subject());
    if (initial_phase != 0) {
        on_print_start_phase_changed(initial_phase);
        int prog =
            lv_subject_get_int(printer_state_.print_state().get_print_start_progress_subject());
        on_print_start_progress_changed(prog);
        spdlog::debug("[{}] Synced initial preparation state: phase={}, progress={}%", get_name(),
                      initial_phase, prog);
    }

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "PrintStatusPanelSubjects", []() { get_global_print_status_panel().deinit_subjects(); });

    spdlog::debug("[{}] Subjects initialized (20 subjects)", get_name());
}

void PrintStatusPanel::deinit_subjects() {
    if (!subjects_initialized_)
        return;

    // Tune overlay singleton handles its own cleanup via StaticPanelRegistry

    // Clear light/timelapse global accessor
    set_global_light_timelapse_controls(nullptr);
    light_timelapse_controls_.deinit_subjects();

    // Reset observers on local subjects BEFORE deinit frees them.
    // subjects_.deinit_all() calls lv_subject_deinit which frees observer
    // structs — any ObserverGuard still holding a pointer would crash
    // in its destructor trying to lv_observer_remove() on freed memory.
    end_overlay_dismissed_observer_.reset();
    print_message_observer_.reset();

    // Fan-row observers — lifetimes BEFORE observer guards per [L084]
    camera_label_observer_.reset();
    fans_version_observer_.reset();
    primary_fans_version_observer_.reset();
    animations_enabled_observer_.reset();
    breakpoint_observer_.reset();
    filament_sensor_count_observer_.reset();
    ams_slot_count_observer_.reset();
    scoped_runout_observer_.reset();
    scoped_runout_slots_observer_.reset();
    toolchange_visible_observer_.reset();
    part_speed_lifetime_.reset();
    part_speed_observer_.reset();
    hotend_speed_lifetime_.reset();
    hotend_speed_observer_.reset();
    aux_speed_lifetime_.reset();
    aux_speed_observer_.reset();

    temp_observers_.clear();
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[PrintStatusPanel] Subjects deinitialized");
}

lv_obj_t* PrintStatusPanel::create(lv_obj_t* parent) {
    parent_screen_ = parent;

    // Create overlay root from XML
    overlay_root_ = helix::ui::create_xml_hidden(parent, xml_component());
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // The hook still on a previous root means that tree is alive and this
    // create() replaces it. Logged once nothing below can fail.
    const bool replaces_previous_tree = delete_hook_root_ != nullptr;

    // A rebuild reaches create() with the hook still on the previous root:
    // OverlayBase::rebuild() condemns that tree only AFTER create() has pointed
    // the panel at the successor, and safe_delete_subtree() defers the actual
    // deletion. Left installed, that late event would fire into a panel whose
    // uninstall paths only know delete_hook_root_ — which by then names the
    // successor — so nothing could ever take it off, and a shutdown before the
    // async tick would reach a freed `this`. Take it off first, the way
    // PanelWidget::install_delete_hook() does.
    uninstall_root_delete_hook(delete_hook_root_, on_root_deleted, this);

    // The panel/navigation layer owns this tree; a raw lv_obj_delete() gives
    // the panel no other notice, and the queued observe<int> handlers
    // would run against the freed child pointers on the next drain.
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(overlay_root_, on_root_deleted, LV_EVENT_DELETE, this);
    delete_hook_root_ = overlay_root_;

    spdlog::debug("[{}] Setting up panel...", get_name());

    // Width comes from NavigationManager::push_overlay() — this panel declares
    // is_destination() so it renders full width from every entry point (#1178).
    // Use standard overlay panel setup for header/content/back button
    ui_overlay_panel_setup_standard(overlay_root_, parent_screen_, "overlay_header",
                                    "overlay_content");

    lv_obj_t* overlay_content = find_required(overlay_root_, "overlay_content", get_name());
    // Find thumbnail section for nested widgets
    lv_obj_t* thumbnail_section = find_required(overlay_content, "thumbnail_section", get_name());
    if (!thumbnail_section) {
        return nullptr;
    }

    // A failed create() leaves its hook on a root that never got a tree number,
    // so a replacement is logged only while the last numbered tree is alive.
    if (replaces_previous_tree && s_last_destroyed_tree != s_tree_number) {
        log_tree_destroyed(PrintStatusTreeDestroyCause::ReplacedByRebuild,
                           printer_state_.print_state().get_print_lifecycle());
    }
    const helix::MemoryInfo tree_mem = memory_info_source_();
    log_tree_created(printer_state_.print_state().get_print_lifecycle(), tree_mem.available_mb());
#if defined(HELIX_PLATFORM_ESP32)
    // Same [heap:<stage>] shape as the firmware's boot milestones, so a boot log
    // shows what building this tree on top of home cost.
    spdlog::info("[heap:print-status-up] psram free={}KB", tree_mem.available_kb);
#endif

    // Find G-code viewer, thumbnail, and gradient background widgets
    gcode_viewer_ = lv_obj_find_by_name(thumbnail_section, "print_gcode_viewer");
    print_thumbnail_ = lv_obj_find_by_name(thumbnail_section, "print_thumbnail");
    gradient_background_ = lv_obj_find_by_name(thumbnail_section, "gradient_background");

    preview_.attach_widgets(print_thumbnail_, gcode_viewer_);

    if (gcode_viewer_) {
        spdlog::debug("[{}]   ✓ G-code viewer widget found", get_name());

        helix::ui::apply_preview_render_mode(gcode_viewer_, get_name());

        // Create and initialize exclude object manager
        exclude_manager_ = std::make_unique<helix::ui::PrintExcludeObjectManager>(
            api_, printer_state_, gcode_viewer_);
        exclude_manager_->init();
        spdlog::debug("[{}]   ✓ Created and initialized exclude object manager", get_name());

        // The strip is a flex sibling BELOW the preview here, so this measures no
        // overlap and the render stays centred. Wired anyway so a layout change
        // is picked up without touching this file.
        helix::ui::set_preview_bottom_occluder(
            gcode_viewer_,
            helix::ui::find_required(thumbnail_section, "metadata_clip", get_name()));

        // Memory-pressure responder calls ui_gcode_viewer_clear_all_active().
        // Flip our mode subject back to thumbnail (0) so the user sees the
        // slicer preview rather than a transparent rectangle.
        ui_gcode_viewer_set_clear_callback(
            gcode_viewer_,
            [](lv_obj_t*, void* ud) {
                auto* panel = static_cast<PrintStatusPanel*>(ud);
                panel->show_gcode_viewer(false);
                panel->lifecycle_.set_gcode_loaded(false);
                panel->preview_.forget_gcode();
            },
            this);
    } else {
        spdlog::error("[{}]   ✗ G-code viewer widget NOT FOUND", get_name());
    }
    if (print_thumbnail_) {
        spdlog::debug("[{}]   ✓ Print thumbnail widget found", get_name());
    }
    if (gradient_background_) {
        spdlog::debug("[{}]   ✓ Gradient background widget found", get_name());
    }

    // Force layout calculation
    lv_obj_update_layout(overlay_root_);

    // Register resize callback
    if (auto* dm = DisplayManager::instance()) {
        dm->register_resize_callback(on_resize_static);

        // Force-redraw the gcode viewer on display wake. LVGL's image cache is
        // invalidated when the framebuffer is unblanked, leaving the 3D
        // renderer's cached-blit path with stale references — the canvas can
        // come back blank even though the underlying draw_buf data is intact.
        // Issuing a full re-render here paints from scratch.
        auto token = lifetime_.token();
        dm->register_sleep_callback([this, token](bool sleeping) {
            if (token.expired() || sleeping)
                return;
            if (gcode_viewer_ && !ui_gcode_viewer_is_paused(gcode_viewer_)) {
                ui_gcode_viewer_force_redraw(gcode_viewer_);
            }
        });
    }
    resize_registered_ = true;

    // Store button references for potential state queries (not event wiring - that's in XML)
    btn_timelapse_ = helix::ui::find_required(overlay_content, "btn_timelapse", get_name());
    btn_tune_ = helix::ui::find_required(overlay_content, "btn_tune", get_name());
    btn_cancel_ = helix::ui::find_required(overlay_content, "btn_cancel", get_name());

    // Print complete celebration badge (for animation)
    success_badge_ = helix::ui::find_required(overlay_content, "success_badge", get_name());
    if (success_badge_) {
        spdlog::debug("[{}]   ✓ Success badge", get_name());
    }

    // Print cancelled badge (for animation)
    cancel_badge_ = helix::ui::find_required(overlay_content, "cancel_badge", get_name());
    if (cancel_badge_) {
        spdlog::debug("[{}]   ✓ Cancel badge", get_name());
    }

    // Print error badge (for animation)
    error_badge_ = helix::ui::find_required(overlay_content, "error_badge", get_name());
    if (error_badge_) {
        spdlog::debug("[{}]   ✓ Error badge", get_name());
    }

    // Progress bar widget
    progress_bar_ = find_required(overlay_content, "print_progress", get_name());
    if (progress_bar_) {
        lv_bar_set_range(progress_bar_, 0, 100);
        // WORKAROUND: LVGL bar has a bug where setting value=0 when cur_value=0
        // causes early return without proper layout update, showing full bar.
        // Force update by setting to 1 first, then 0.
        lv_bar_set_value(progress_bar_, 1, LV_ANIM_OFF);
        lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        // Scheduled-pause ticks (prestonbrown/helixscreen#1509); the fill
        // itself stays on the XML bind_value to print_progress_display.
        helix::ui::attach_bar_pause_markers(progress_bar_, printer_state_);
        spdlog::debug("[{}]   ✓ Progress bar", get_name());
    }

    // Preparing progress bar (shown during pre-print operations)
    preparing_progress_bar_ =
        helix::ui::find_required(overlay_content, "preparing_progress_bar", get_name());
    if (preparing_progress_bar_) {
        lv_bar_set_range(preparing_progress_bar_, 0, 100);
        lv_bar_set_value(preparing_progress_bar_, 0, LV_ANIM_OFF);
        spdlog::debug("[{}]   ✓ Preparing progress bar", get_name());
    }

    // AMS current tool indicator (auto-hides when no AMS or no tool active)
    lv_obj_t* ams_indicator =
        helix::ui::find_required(overlay_content, "ams_current_tool_indicator", get_name());
    if (ams_indicator) {
        ui_ams_current_tool_setup(ams_indicator);
        spdlog::debug("[{}]   ✓ AMS current tool indicator", get_name());
    }

    // Check if --gcode-file was specified on command line for this panel
    const auto* config = get_runtime_config();
    if (config->gcode_test_file && gcode_viewer_) {
        // Check file size and memory safety before loading
        // Use 2D streaming check since that's the mode used on memory-constrained devices
        if (tio::open_file(config->gcode_test_file, "rb")) {
            size_t file_size =
                static_cast<size_t>(tio::file_size(config->gcode_test_file).value_or(0));
            if (helix::is_gcode_2d_streaming_safe(file_size)) {
                spdlog::info("[{}] Loading G-code file from command line: {}", get_name(),
                             config->gcode_test_file);
                preview_.load_file(config->gcode_test_file,
                                   printer_state_.print_state().get_effective_print_filename());
            } else {
                spdlog::warn("[{}] G-code file too large for 2D streaming: {} ({} bytes) - using "
                             "thumbnail only",
                             get_name(), config->gcode_test_file, file_size);
            }
        }
    }

    // Restore the thumbnail if a print was already in progress before the panel was
    // displayed (e.g. one started from Mainsail while on the Home panel).
    preview_.restore_cached_thumbnail();

    // Hide initially - NavigationManager will show when pushed
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Seed view toggle icon now that globals.xml has been loaded (init_subjects
    // runs too early to resolve #icon_cube). The subject drives the XML
    // bind_text on btn_view_toggle_icon, so this is the initial render state.
    if (const char* icon = lv_xml_get_const(nullptr, "icon_cube")) {
        lv_subject_copy_string(&view_toggle_icon_subject_, icon);
    }

    // Initial fan classification (may rebind later when fans_version updates)
    bind_fan_observers();

    // Wire LV_EVENT_SIZE_CHANGED on controls_section so any column-width change
    // triggers a density + fit recompute. Direct lv_obj_add_event_cb is correct
    // here: SIZE_CHANGED has no XML binding equivalent (pattern from
    // ui_ams_mini_status.cpp).
    if (lv_obj_t* controls_section = find_required(overlay_root_, "controls_section", get_name())) {
        lv_obj_add_event_cb(controls_section, on_controls_size_changed, LV_EVENT_SIZE_CHANGED,
                            this);
        spdlog::debug("[{}] Registered SIZE_CHANGED on controls_section", get_name());
    }

    // Thermal tint for the temp-card heater icons. The binder owns its own
    // observers, so this needs no hook into on_temperature_changed().
    nozzle_icon_binder_.bind(overlay_root_, printer_state_, helix::HeaterType::Nozzle);
    bed_icon_binder_.bind(overlay_root_, printer_state_, helix::HeaterType::Bed);
    chamber_icon_binder_.bind(overlay_root_, printer_state_, helix::HeaterType::Chamber);

    // Initial density + fit recompute is scheduled from on_activate() — running
    // it here is futile because overlay_root_ is HIDDEN until activation, and a
    // hidden subtree has 0-width layout in LVGL (so measurement returns 0).

    spdlog::debug("[{}] Setup complete!", get_name());
    return overlay_root_;
}

void PrintStatusPanel::on_activate() {
    // Cluster:pstat-async-delete (#906) — fine-grained crumbs through every
    // step of on_activate so the next production crash names which step left
    // the corruption rolling. Pair with the larger breadcrumb ring so these
    // survive the pre-crash tick storm.
    crash_handler::breadcrumb::note("pstat_act", "enter");
    OverlayBase::on_activate(); // Sets visible_ = true
    is_active_ = true;

    // RAW_PRINT_STATE_OK: pairs with the scoped-runout guard's reason below.
    int state_enum =
        lv_subject_get_int(printer_state_.print_state().get_print_state_enum_subject());
    spdlog::debug("[{}] on_activate() print_state_enum={}", get_name(), state_enum);

    // Reconcile the preview against the current print state. This single
    // idempotent entry point replaces the former scatter of conditional reload
    // blocks (deferred-gcode kick, want_viewer/!gcode_loaded re-feed, cached
    // thumbnail re-apply). It reads the ACTUAL widget state, so a blank or
    // recreated widget always reloads — re-entry after destroy-on-close or a
    // memory-reclaim cycle is self-healing.
    crash_handler::breadcrumb::note("pstat_act", "ensure_preview");
    preview_.ensure_current();

    // Sync button enabled/visibility state with current print state and outcome.
    // XML bindings may have been lost during overlay lifecycle transitions (#546).
    crash_handler::breadcrumb::note("pstat_act", "btn_states");
    update_button_states();

    // Re-apply the render-mode ladder before showing the viewer. The settings observer
    // only fires while the panel is active, so a setting changed while it was hidden
    // lands here.
    bool thumbnail_only = !helix::ui::apply_preview_render_mode(gcode_viewer_, get_name());

    // Restore G-code viewer state based on current print conditions.
    // Thumbnail Only mode forces the viewer off regardless of gcode state.
    bool gcode_present = gcode_viewer_ && ui_gcode_viewer_has_content(gcode_viewer_);
    bool show_viewer = !thumbnail_only && lifecycle_.want_viewer() && gcode_present;
    crash_handler::breadcrumb::note("pstat_act", show_viewer ? "viewer_on" : "viewer_off");
    show_gcode_viewer(show_viewer);

    // Sync gcode viewer to current print layer (may have advanced while panel was hidden)
    if (gcode_present && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN)) {
        int current_layer =
            lv_subject_get_int(printer_state_.print_state().get_print_layer_current_subject());
        int total_layers =
            lv_subject_get_int(printer_state_.print_state().get_print_layer_total_subject());
        int viewer_max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
        int viewer_layer = current_layer;
        if (total_layers > 0 && viewer_max_layer > 0) {
            viewer_layer = (current_layer * viewer_max_layer) / total_layers;
        }
        ui_gcode_viewer_set_print_progress(gcode_viewer_, viewer_layer);
    }
    // Fan row adaptive-fit + density recompute: this is the earliest point where
    // overlay_root_ and its parents are visible, so LVGL will actually compute
    // non-zero widths for the row. Deferred so layout has at least one tick to
    // settle after on_activate() un-hides the panel.
    {
        // Re-resolve which fan owns each slot and re-seed the labels. Seeding
        // alone is not enough: the compact row can be stale because the *name* is
        // stale, not just the value — classify_primary_fans() is runtime-adaptive
        // (#1124), so re-reading part_fan_name_'s subject would faithfully
        // re-display the wrong fan. bind_fan_observers() refreshes the names and
        // ends each rebind with a seed, which covers both (#1181).
        bind_fan_observers();

        auto token = lifetime_.token();
        token.defer("PrintStatusPanel::on_activate_fan_row_recompute", [this]() {
            layout_fitter_.recompute_fans_density();
            layout_fitter_.recompute_fans_fit();
        });
    }

    // Resume the mini-graph and pull in whatever landed while we were off-screen.
    // resume() backfills, so the trace is continuous rather than starting a fresh
    // segment at re-entry.
    layout_fitter_.resume_temp_graph();

    crash_handler::breadcrumb::note("pstat_act", "exit");
}

void PrintStatusPanel::on_deactivating(DeactivateReason) {
    is_active_ = false;
    spdlog::debug("[{}] on_deactivating()", get_name());

    // The panel is no longer visible: drop the queued G-code load
    preview_.cancel_pending_load();

    // Note: bar animation cancellation is handled by lv_bar_destructor()
    // when widgets are deleted. Manual lv_anim_delete(bar_ptr) uses the wrong
    // var pointer (bar animations use &bar->cur_value_anim internally).

    // Pause G-code viewer rendering when panel is hidden (CPU optimization)
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, true);

        // Release heavy renderer state when leaving the panel after a print has
        // reached a terminal state. Previously gated on system-wide available
        // memory (< 64MB) — that threshold is "kernel about to OOM," not "we're
        // using too much," so devices with abundant free RAM but heavy process
        // RSS would hold the ParsedGCodeFile + GPU geometry indefinitely after
        // a print ended (telemetry: pi32 held 632MB for 1+ hour post-print).
        // The user has navigated away, the print is over — drop the heavy
        // state. Issue #618's smoothness gain only applies while the print is
        // still active (handled by the state guard below).
        auto state = lifecycle_.state();
        if (state != PrintState::Printing && state != PrintState::Paused &&
            state != PrintState::Preparing) {
            ui_gcode_viewer_clear(gcode_viewer_);
            lifecycle_.set_gcode_loaded(false);
            preview_.forget_gcode();
            spdlog::debug("[{}] Cleared gcode viewer on deactivate (terminal state)", get_name());
        }
    }

    // Hide runout guidance modal if panel is deactivated (e.g., navbar navigation)
    if (runout_handler_) {
        runout_handler_->hide_modal();
    }

    // Stop feeding the mini-graph while it is off-screen — same reasoning as
    // pausing the G-code viewer above. History keeps accumulating in the manager,
    // so on_activate()'s resume() backfills the gap.
    layout_fitter_.pause_temp_graph();
}

void PrintStatusPanel::cleanup() {
    preview_.cancel_pending_load();
    cancel_preparing_show_timer();

    OverlayBase::cleanup(); // Sets cleanup_called_ = true
}

void PrintStatusPanel::on_ui_destroyed() {
    spdlog::debug("[{}] on_ui_destroyed() - nulling widget pointers", get_name());

    // The tree is only detached here (the actual deletion is deferred), so the
    // delete hook is still installed on it. Remove it now: the panel can be
    // destroyed before the deferred delete executes, and that late event must
    // not reach a freed `this`. overlay_root_ is already null —
    // safe_delete_deferred() cleared it — hence the dedicated hook copy.
    uninstall_root_delete_hook(delete_hook_root_, on_root_deleted, this);

    // Drops the queued and in-flight G-code load, the viewer reference and both
    // markers, so the next open reloads everything.
    preview_.on_tree_destroyed();

    // Note: LVGL animations are already cancelled by lv_obj_delete() in the base
    // class destroy_overlay_ui() call, so no need to cancel them here.

    exclude_mode_.hide();

    // Deinit exclude manager (holds gcode_viewer_ reference)
    if (exclude_manager_) {
        exclude_manager_->deinit();
        exclude_manager_.reset();
    }

    // Tear the mini-graph down while its container is still addressable, and
    // drop the fit decision with it: a rebuilt tree starts with no controller,
    // so leaving the subject at 1 would un-hide an empty container until the
    // first post-activate recompute.
    layout_fitter_.on_tree_destroyed();

    // Null all child widget pointers (the tree's actual deletion is deferred by
    // the base class, but every pointer below is dead from here on)
    forget_cached_widgets();

    // Heater icon animators — at this point the widget tree is only hidden
    // and reparented to the top layer (destroy_overlay_ui() defers the actual
    // deletion to the next tick, see overlay_base.h), so the icons are still
    // valid and the binders are still bound. Unbind explicitly here rather
    // than relying on the eventual deferred LV_EVENT_DELETE.
    nozzle_icon_binder_.unbind();
    bed_icon_binder_.unbind();
    chamber_icon_binder_.unbind();

    // Reset widget-dependent state. The kept-tree job watch goes with the tree
    // it watched for.
    kept_tree_job_observer_.reset();
    resize_registered_ = false;
    is_active_ = false;
    lifecycle_.set_gcode_loaded(false);
    complete_view_mode_ = false;
}

void PrintStatusPanel::on_root_deleted(lv_event_t* e) {
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    auto* dying = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only the tree the hook was installed for matters. OverlayBase::rebuild()
    // deletes a replaced root after create() already pointed the panel at the
    // successor, so that event can land while the successor's pointers are
    // live — clearing them then would blank a live tree. (Compared against
    // delete_hook_root_, not overlay_root_: the explicit teardown path has
    // already cleared the latter by the time this deferred event fires.)
    if (dying != self->delete_hook_root_) {
        return;
    }

    self->forget_cached_widgets();
    self->delete_hook_root_ = nullptr;
    if (s_cached_panel == dying) {
        s_cached_panel = nullptr;
    }
    log_tree_destroyed(PrintStatusTreeDestroyCause::WidgetTreeDeleted,
                       self->printer_state_.print_state().get_print_lifecycle());
}

void PrintStatusPanel::forget_cached_widgets() {
    preview_.detach_widgets();
    if (exclude_manager_) {
        exclude_manager_->detach_gcode_viewer();
    }
    overlay_root_ = nullptr;
    progress_bar_ = nullptr;
    preparing_progress_bar_ = nullptr;
    gcode_viewer_ = nullptr;
    print_thumbnail_ = nullptr;
    gradient_background_ = nullptr;
    btn_timelapse_ = nullptr;
    btn_tune_ = nullptr;
    btn_cancel_ = nullptr;
    success_badge_ = nullptr;
    cancel_badge_ = nullptr;
    error_badge_ = nullptr;
}

lv_obj_t* PrintStatusPanel::get_cached_overlay() {
    return s_cached_panel;
}

helix::MemoryInfo (*PrintStatusPanel::memory_info_source_)() = helix::get_system_memory_info;

void PrintStatusPanel::destroy_cached_overlay(PrintStatusTreeDestroyCause cause) {
    auto* panel = helix::lazy_global_if_exists<PrintStatusPanel>();
    if (!s_cached_panel || !panel) {
        return;
    }
    panel->destroy_overlay_ui(s_cached_panel);
    // destroy_overlay_ui() nulls the pointer only when it destroyed a tree.
    if (s_cached_panel == nullptr) {
        log_tree_destroyed(cause, panel->printer_state_.print_state().get_print_lifecycle());
    }
}

void PrintStatusPanel::on_overlay_closed() {
    auto* existing = helix::lazy_global_if_exists<PrintStatusPanel>();
    if (!s_cached_panel || !existing) {
        return;
    }
    PrintStatusPanel& panel = *existing;
    // A close callback runs late: when the slide-out completes, or on the next
    // tick for a navbar or connection-loss close. The tree can be back on screen
    // by then, where this close no longer applies, and a slide-out completion
    // consumes whichever callback the re-push registered.
    if (helix::nav::is_in_stack(s_cached_panel)) {
        helix::nav::on_close(s_cached_panel, on_overlay_closed);
        return;
    }

    const PrintState lifecycle = panel.printer_state_.print_state().get_print_lifecycle();
    const helix::MemoryInfo mem = memory_info_source_();
    if (helix::ui::print_status_destroy_on_close(mem.is_low_memory(), lifecycle)) {
        destroy_cached_overlay(PrintStatusTreeDestroyCause::OverlayClose);
        return;
    }
    spdlog::debug(
        "[PrintStatusPanel] Print status tree #{} kept on close while {} ({}MB available)",
        s_tree_number, print_state_name(lifecycle), mem.available_mb());

    // A navbar close skips on_deactivate() for this persistent overlay. Left
    // active while hidden, the viewer keeps rendering, its 2D catch-up stalls,
    // and the stall watchdog reports a failed preview load on another screen.
    if (panel.is_active_) {
        panel.on_deactivate(DeactivateReason::NavigateAway);
    }

    // Nothing else gives a low-memory host this tree's memory back once the job
    // ends while it is hidden.
    panel.kept_tree_job_observer_ = observe<int>(
        panel.printer_state_.print_state().get_job_holds_machine_subject(), &panel,
        [](PrintStatusPanel* /*self*/, int holds) {
            if (holds == 0) {
                release_kept_tree_after_job();
            }
        },
        panel.printer_state_.get_subjects_lifetime());
}

void PrintStatusPanel::release_kept_tree_after_job() {
    // Queued the way try_reclaim_cached_print_status() is, and checked again
    // when it lands: the tree can be opened or a new print started in between.
    // Unlike that reclaim it honours the close-time decision, so a host with
    // memory to spare keeps the tree.
    helix::ui::queue_update("PrintStatusPanel::release_kept_tree_after_job", []() {
        auto* existing = helix::lazy_global_if_exists<PrintStatusPanel>();
        if (!s_cached_panel || !existing) {
            return;
        }
        PrintStatusPanel& panel = *existing;
        // push_overlay() disarms the watch before its own queued push lands, so
        // a disarmed watch covers a push still on its way to the stack.
        if (!panel.kept_tree_job_observer_) {
            return;
        }
        if (helix::nav::is_in_stack(s_cached_panel)) {
            return;
        }
        if (!helix::ui::print_status_destroy_on_close(
                memory_info_source_().is_low_memory(),
                panel.printer_state_.print_state().get_print_lifecycle())) {
            return;
        }
        destroy_cached_overlay(PrintStatusTreeDestroyCause::JobEndedWhileHidden);
    });
}

bool PrintStatusPanel::push_overlay(lv_obj_t* parent_screen) {
    if (!parent_screen) {
        spdlog::error("[PrintStatusPanel] push_overlay: null parent_screen");
        return false;
    }

    // Lazy-create the widget tree if it was destroyed or never created
    if (!s_cached_panel) {
        auto& panel = get_global_print_status_panel();

        if (!panel.are_subjects_initialized()) {
            panel.init_subjects();
        }

        s_cached_panel = panel.create(parent_screen);
        if (!s_cached_panel) {
            spdlog::error("[PrintStatusPanel] Failed to create print status overlay from XML");
            return false;
        }

        // Register with NavigationManager for lifecycle callbacks (persistent
        // so the registration survives navbar panel switches while cached)
        helix::nav::register_overlay(s_cached_panel, &panel, true);

        // Register the pressure responder once. A close keeps the tree whenever
        // memory is plentiful or a job holds the machine; this is what drops a
        // hidden tree once memory actually runs short. No-op while the overlay
        // is in the navigation stack.
        if (s_memory_responder_id == 0) {
            s_memory_responder_id = helix::MemoryMonitor::instance().add_pressure_responder(
                [](helix::MemoryPressureLevel level) {
                    // Pre-queue bail-out: if there's no cached tree, a
                    // queue_update hop would just land on an empty null check.
                    // Monitor thread reads s_cached_panel without a barrier;
                    // pointer-sized loads are atomic on our targets, and a
                    // false-negative is harmless (queued lambda re-checks).
                    if (level < helix::MemoryPressureLevel::warning || !s_cached_panel) {
                        return;
                    }
                    try_reclaim_cached_print_status();
                });
        }
    }

    // The tree is being shown, so a job ending is no longer a reason to drop it.
    // Disarmed here, not when the queued push lands, so a release already
    // queued finds the watch gone.
    get_global_print_status_panel().kept_tree_job_observer_.reset();

    // Whether a close destroys the tree is decided when the close happens: the
    // print and available memory both move while the overlay is open.
    // NavigationManager consumes the callback when it fires, and a tree that
    // close kept comes back through here without being re-created, so it is
    // registered on every push.
    helix::nav::on_close(s_cached_panel, on_overlay_closed);

    helix::nav::push_overlay(s_cached_panel);
    return true;
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void PrintStatusPanel::show_gcode_viewer(bool show) {
    // Update viewer mode subject - XML bindings handle visibility reactively
    // Mode 0 = thumbnail (gradient + thumbnail visible, gcode viewer hidden)
    // Mode 1 = 3D gcode viewer (gcode visible, gradient + thumbnail hidden, rotate icon shown)
    // Mode 2 = 2D gcode viewer (gcode visible, gradient shown, thumbnail + rotate icon hidden)
    int mode = 0; // Default: thumbnail
    if (show) {
        // Check if the viewer is using 2D mode
        bool is_2d = gcode_viewer_ && ui_gcode_viewer_is_using_2d_mode(gcode_viewer_);
        mode = is_2d ? 2 : 1;
    }
    lv_subject_set_int(&gcode_viewer_mode_subject_, mode);

    // When falling back to thumbnail mode, ensure the image source is applied.
    // During async gcode reload the gradient covers the area - the user should
    // at least see the cached thumbnail underneath.
    if (mode == 0) {
        preview_.reapply_thumbnail_if_blank();
    }

    // Pause/resume rendering based on visibility mode (CPU optimization)
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, !show);
    }

    spdlog::trace("[{}] G-code viewer mode: {} ({})", get_name(), mode,
                  mode == 0 ? "thumbnail" : (mode == 1 ? "3D" : "2D"));

    // Diagnostic: log visibility state of all viewer components
    if (print_thumbnail_) {
        bool thumb_hidden = lv_obj_has_flag(print_thumbnail_, LV_OBJ_FLAG_HIDDEN);
        const void* img_src = lv_image_get_src(print_thumbnail_);
        spdlog::trace("[{}]   -> thumbnail: hidden={}, has_src={}", get_name(), thumb_hidden,
                      img_src != nullptr);
    }
    if (gcode_viewer_) {
        bool viewer_hidden = lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN);
        spdlog::trace("[{}]   -> gcode_viewer: hidden={}", get_name(), viewer_hidden);
    }
    if (gradient_background_) {
        bool grad_hidden = lv_obj_has_flag(gradient_background_, LV_OBJ_FLAG_HIDDEN);
        spdlog::trace("[{}]   -> gradient: hidden={}", get_name(), grad_hidden);
    }
}

void PrintStatusPanel::show_exclude_map_view() {
    if (!exclude_manager_) {
        return;
    }
    lv_obj_t* overlay_content =
        helix::ui::find_required(overlay_root_, "overlay_content", get_name());
    if (!overlay_content) {
        return;
    }
    helix::ui::ExcludeModeTargets targets;
    targets.card = helix::ui::find_required(overlay_content, "thumbnail_section", get_name());
    targets.columns = overlay_content;
    targets.controls_name = "controls_section";
    targets.gcode_viewer = gcode_viewer_;
    targets.map_active = &exclude_map_active_subject_;
    targets.thumbnail_mode = lv_subject_get_int(&gcode_viewer_mode_subject_) == 0;
    const auto bed = helix::bed_dimensions(api_, &printer_state_);
    targets.bed_w_mm = bed.w_mm;
    targets.bed_h_mm = bed.h_mm;
    exclude_mode_.show(targets, &printer_state_.excluded_objects_state(),
                       helix::ui::ExcludeTapMode::ExcludeOnly, [this](const std::string& name) {
                           if (exclude_manager_) {
                               exclude_manager_->request_exclude(name);
                           }
                       });
}

void PrintStatusPanel::hide_exclude_map_view() {
    exclude_mode_.hide();
}

void PrintStatusPanel::update_heater_status_rows() {
    // Glyph state + duty text come from the one shared classifier; the XML row
    // maps the state int to the flame/check/snowflake glyph pair.
    auto nozzle = helix::ui::temperature::classify_heater_status(
        lifecycle_.nozzle_current(), lifecycle_.nozzle_target(),
        lv_subject_get_int(printer_state_.get_heater_power_subject(helix::HeaterType::Nozzle)));
    lv_subject_set_int(&nozzle_status_state_subject_, static_cast<int>(nozzle.state));
    std::snprintf(nozzle_status_buf_, sizeof(nozzle_status_buf_), "%s", nozzle.duty.c_str());
    lv_subject_copy_string(&nozzle_status_subject_, nozzle_status_buf_);

    auto bed = helix::ui::temperature::classify_heater_status(
        lifecycle_.bed_current(), lifecycle_.bed_target(),
        lv_subject_get_int(printer_state_.get_heater_power_subject(helix::HeaterType::Bed)));
    lv_subject_set_int(&bed_status_state_subject_, static_cast<int>(bed.state));
    std::snprintf(bed_status_buf_, sizeof(bed_status_buf_), "%s", bed.duty.c_str());
    lv_subject_copy_string(&bed_status_subject_, bed_status_buf_);
}

void PrintStatusPanel::update_all_displays() {
    // Guard: don't update if subjects aren't initialized yet
    if (!subjects_initialized_) {
        return;
    }

    // Progress text

    progress_text_.refresh_layer();

    // Filament used text
    progress_text_.refresh_filament_used();

    // Time displays - Preparing: preprint observers own these.
    // Complete: on_print_state_changed sets frozen final values, don't overwrite.
    if (lifecycle_.state() != PrintState::Preparing && lifecycle_.state() != PrintState::Complete) {
        // elapsed_seconds is wall-clock time from Moonraker total_duration (includes prep)
        progress_text_.show_elapsed(lifecycle_.elapsed_seconds());

        progress_text_.show_remaining(lifecycle_.remaining_seconds());
    }

    // Heater status (state glyph + duty)
    update_heater_status_rows();

    update_speed_flow_text();

    // Pause/Resume button icon + label are owned by PrintControlButtons now.
}

// ============================================================================
// INSTANCE HANDLERS
// ============================================================================

void PrintStatusPanel::handle_temp_card_click() {
    spdlog::debug("[{}] Temp card clicked - opening temperature graph", get_name());
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::GraphOnly, parent_screen_);
}

void PrintStatusPanel::handle_tune_button() {
    spdlog::info("[{}] Tune button clicked - opening tuning panel", get_name());

    // Use singleton - handles lazy init, subject registration, slider sync, and nav push
    get_print_tune_overlay().show(parent_screen_, api_, printer_state_);
}

std::string PrintStatusPanel::reprint_filename() const {
    // print_stats names the copy that ran, which for a print this app
    // rewrote is a temp file deleted when the print ends. An identity this
    // session recorded at commit names the original exactly; without one, the
    // report places the original only when it encodes the whole path.
    // The recorded identity only fills in what the report cannot place: a
    // report naming another file means another client printed since.
    const auto& print_state = printer_state_.print_state();
    const std::optional<std::string> reported =
        helix::gcode::trusted_original_path(current_print_filename_);
    const std::string& recorded = print_state.get_print_identity_override();
    if (!recorded.empty() && (!reported || *reported == recorded)) {
        return recorded;
    }
    return reported.value_or("");
}

void PrintStatusPanel::handle_reprint_button() {
    // Startup grace period: reject phantom clicks during early boot
    auto elapsed = std::chrono::steady_clock::now() - AppConstants::Startup::PROCESS_START_TIME;
    if (elapsed < AppConstants::Startup::PRINT_START_GRACE_PERIOD) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::warn("[{}] Rejected reprint during startup grace period ({}s < {}s)", get_name(),
                     secs, AppConstants::Startup::PRINT_START_GRACE_PERIOD.count());
        return;
    }

    const std::string filename = reprint_filename();
    spdlog::info("[{}] Reprint button clicked - reprinting: {}", get_name(), filename);

    if (filename.empty() && !current_print_filename_.empty()) {
        // A same-named file elsewhere is not necessarily the one that printed.
        spdlog::warn("[{}] Not reprinting '{}': original file location unknown", get_name(),
                     current_print_filename_);
        NOTIFY_WARNING(lv_tr("Original file location unknown"));
        return;
    }
    if (filename.empty()) {
        spdlog::warn("[{}] No filename to reprint", get_name());
        NOTIFY_WARNING(lv_tr("No file to reprint"));
        return;
    }

    if (!api_) {
        spdlog::error("[{}] Cannot reprint: API not available", get_name());
        NOTIFY_ERROR(lv_tr("Cannot reprint: not connected to printer"));
        return;
    }

    // Disable button immediately to prevent double-press
    ui_set_button_enabled(btn_cancel_, false);

    // Route through PrintStartController so reprint gets the Snapmaker U1 native
    // pre-print send (SET_PRINT_USED_EXTRUDERS ...) that suppresses a spurious
    // filament-feed runout. The controller owns the U1 logic; this panel only
    // supplies tools_used and re-enables its own button on failure.
    auto* select_panel = get_print_select_panel(printer_state_, api_);
    auto* controller = select_panel ? select_panel->get_print_start_controller() : nullptr;
    if (controller) {
        auto tok = lifetime_.token();
        controller->initiate_reprint(
            filename, /*path=*/"", get_tools_used(),
            // on_started: nothing — the PrinterState observer flips the button to Cancel
            // mode when Moonraker confirms Printing (same as the old success callback,
            // which only logged).
            []() {},
            // on_error: re-enable THIS panel's button. Guard with this panel's lifetime
            // token because the controller guards ITSELF, not this status panel (which
            // can be popped mid-flight). The controller already emits the error toast.
            [this, tok]() mutable {
                tok.defer("PrintStatusPanel::reprint_reenable",
                          [this]() { ui_set_button_enabled(btn_cancel_, true); });
            });
    } else {
        // Fallback: controller unreachable — keep the existing direct path so reprint
        // still works (no U1 pre-send).
        spdlog::warn("[{}] No print controller for reprint — using direct start (no U1 pre-send)",
                     get_name());
        helix::warn_printer_stop_check_skipped("a reprint", filename,
                                               "a reprint does not scan the file");
        api_->job().start_print(
            filename,
            lifetime_.bg_cb("PrintStatusPanel::reprint_ok",
                            [this, filename]() {
                                spdlog::info("[{}] Reprint started: {}", get_name(), filename);
                            }),
            [this, token = lifetime_.token()](const MoonrakerError& err) {
                // Runs on libhv WS event loop — marshal LVGL work to main.
                token.defer("PrintStatusPanel::reprint_err", [this, err]() {
                    spdlog::error("[{}] Failed to reprint: {}", get_name(), err.message);
                    helix::ui::notify_error_tr(TR_NOOP("Failed to reprint: {}"), err);
                    ui_set_button_enabled(btn_cancel_, true);
                });
            });
    }
}

void PrintStatusPanel::handle_files_click() {
    spdlog::info("[{}] Files clicked - opening print select during active print", get_name());

    // The navbar-tap decision, queued because this runs inside an LVGL event
    // callback. The switch clears the overlay stack while print status's
    // persistent registration skips on_deactivate(), so the panel keeps
    // collecting data for its next push from the Home print tile. Starting a
    // print there stays blocked by print_select_can_print, same rule as the
    // history reprint path (prestonbrown/helixscreen#1395).
    NavigationManager::instance().request_panel(PanelId::PrintSelect,
                                                NavigationManager::SwitchDispatch::Queued);
}

std::set<int> PrintStatusPanel::get_tools_used() const {
    if (!gcode_viewer_) {
        return {};
    }
    // Mode-independent: reading ParsedGCodeFile directly answered empty for every
    // STREAMED file, which is every file on a memory-constrained printer. That
    // silently disabled both consumers here — the print-scoped runout badge and
    // the Snapmaker reprint's used-extruders preamble.
    return ui_gcode_viewer_get_tools_used(gcode_viewer_);
}

void PrintStatusPanel::recompute_scoped_runout() {
    if (!subjects_initialized_) {
        return;
    }
    auto& fsm = FilamentSensorManager::instance();

    // Print-end / no-active-print: force the badge hidden. When a print ends and
    // the parsed file is dropped, get_tools_used() empties → compute returns -1;
    // but also clear explicitly here so a terminal transition reliably hides the
    // badge even if tools_used hasn't cleared yet (issue 9).
    // RAW_PRINT_STATE_OK: the badge is scoped to the tools the RUNNING file
    // uses. During a preparing window get_tools_used() still describes the
    // previous job, so widening this would scope the badge to the wrong file
    // instead of hiding it — which is why print_scopes_runout_badge() is
    // narrower than PrintLifecycleState::is_active().
    auto state = printer_state_.print_state().get_print_job_state();
    if (!helix::print_scopes_runout_badge(state)) {
        fsm.set_scoped_runout(-1);
        return;
    }

    // The viewer's parsed file can lag a print switch until ensure_current()
    // reloads it: a load's completion only advances the displayed G-code file to name
    // the print it was actually for (load_gcode_file's callback), so a mismatch
    // here means the geometry in the viewer belongs to a different print. Reading
    // get_tools_used() in that window would scope the badge to the wrong print's
    // tools, so treat it the same as no file loaded yet.
    if (preview_.gcode_displayed_file() !=
        printer_state_.print_state().get_effective_print_filename()) {
        fsm.set_scoped_runout(-1);
        return;
    }

    // Scope the runout badge to the tools the active print uses, with AMS lane
    // truth. Resolve tool→slot using the SAME mapping the print actually uses —
    // the applied firmware tool map (backend->get_tool_mapping(), index=tool,
    // value=slot) — so the badge agrees with the pre-print warning even on
    // backends that remap HelixScreen-side (AFC). On U1 the firmware map is
    // identity → reduces to the default head; an empty map falls back to the
    // firmware default inside FilamentSensorManager.
    std::map<int, int> applied_remap;
    if (auto* backend = AmsState::instance().get_backend()) {
        const auto mapping = backend->get_tool_mapping();
        for (int tool = 0; tool < static_cast<int>(mapping.size()); ++tool) {
            if (mapping[tool] >= 0) {
                applied_remap[tool] = mapping[tool];
            }
        }
    }
    int value = fsm.compute_scoped_runout_value(get_tools_used(), applied_remap);
    fsm.set_scoped_runout(value);
}

void PrintStatusPanel::handle_resize() {
    spdlog::debug("[{}] Handling resize event", get_name());

    // Reset gcode viewer camera to fit new dimensions
    if (gcode_viewer_ && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN)) {
        // Force layout recalculation so viewer gets correct dimensions
        lv_obj_update_layout(gcode_viewer_);
        ui_gcode_viewer_reset_camera(gcode_viewer_);
        spdlog::debug("[{}] Reset gcode viewer camera after resize", get_name());
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void PrintStatusPanel::handle_fans_click() {
    spdlog::debug("[{}] Fans clicked — opening fan control overlay", get_name());

    helix::open_fan_control_overlay(parent_screen_);
}

// Toggle the unified exclude panel (map+side-list in thumbnail mode,
// shrunk-viewer + side-list in 3D/2D mode).
void PrintStatusPanel::handle_objects_toggle() {
    if (exclude_mode_.is_open()) {
        hide_exclude_map_view();
    } else {
        show_exclude_map_view();
    }
}

void PrintStatusPanel::handle_view_toggle() {
    complete_view_mode_ = !complete_view_mode_;

    if (complete_view_mode_) {
        // Complete view: show all layers solid (no ghost)
        if (gcode_viewer_) {
            ui_gcode_viewer_set_print_progress(gcode_viewer_, -1);
        }
    } else {
        // Progress view: restore current layer with ghost
        if (gcode_viewer_) {
            int current_layer =
                lv_subject_get_int(printer_state_.print_state().get_print_layer_current_subject());
            int total_layers =
                lv_subject_get_int(printer_state_.print_state().get_print_layer_total_subject());
            int viewer_max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
            int viewer_layer = current_layer;
            if (total_layers > 0 && viewer_max_layer > 0) {
                viewer_layer = (current_layer * viewer_max_layer) / total_layers;
            }
            ui_gcode_viewer_set_print_progress(gcode_viewer_, viewer_layer);
        }
    }

    const char* icon_text =
        lv_xml_get_const(nullptr, complete_view_mode_ ? "icon_layers" : "icon_cube");
    if (icon_text) {
        lv_subject_copy_string(&view_toggle_icon_subject_, icon_text);
    }

    spdlog::debug("[PrintStatusPanel] View toggle: {}",
                  complete_view_mode_ ? "complete" : "progress");
}

void PrintStatusPanel::on_resize_static() {
    // Use global instance for resize callback (registered without user_data)
    if (auto* panel = helix::lazy_global_if_exists<PrintStatusPanel>()) {
        panel->handle_resize();
    }
}

// ============================================================================
// OBSERVER INSTANCE METHODS
// ============================================================================

void PrintStatusPanel::on_temperature_changed() {
    // Read all temperature values from PrinterState subjects and delegate to lifecycle
    int nz_cur =
        lv_subject_get_int(printer_state_.temperature_state().get_active_extruder_temp_subject());
    int nz_tgt =
        lv_subject_get_int(printer_state_.temperature_state().get_active_extruder_target_subject());
    int bed_cur = lv_subject_get_int(printer_state_.temperature_state().get_bed_temp_subject());
    int bed_tgt = lv_subject_get_int(printer_state_.temperature_state().get_bed_target_subject());
    lifecycle_.on_temperature_changed(nz_cur, nz_tgt, bed_cur, bed_tgt);

    if (!subjects_initialized_)
        return;

    // Update only temperature-related subjects (not the full display refresh).
    // Temperature observers fire frequently during heating (4 subjects x ~1Hz each),
    // and update_all_displays() re-renders ALL subjects causing visible flickering.
    update_heater_status_rows();

    spdlog::trace("[{}] Temperatures updated: nozzle {}/{}°C, bed {}/{}°C", get_name(),
                  lifecycle_.nozzle_current(), lifecycle_.nozzle_target(), lifecycle_.bed_current(),
                  lifecycle_.bed_target());
}

void PrintStatusPanel::recompute_end_overlay_visibility() {
    if (!subjects_initialized_)
        return;
    int outcome = lv_subject_get_int(printer_state_.print_state().get_print_outcome_subject());
    bool dismissed = lv_subject_get_int(&end_overlay_dismissed_subject_) != 0;
    int complete = (!dismissed && outcome == static_cast<int>(PrintOutcome::COMPLETE)) ? 1 : 0;
    int cancelled = (!dismissed && outcome == static_cast<int>(PrintOutcome::CANCELLED)) ? 1 : 0;
    int error = (!dismissed && outcome == static_cast<int>(PrintOutcome::ERROR)) ? 1 : 0;
    lv_subject_set_int(&show_complete_overlay_subject_, complete);
    lv_subject_set_int(&show_cancelled_overlay_subject_, cancelled);
    lv_subject_set_int(&show_error_overlay_subject_, error);
}

void PrintStatusPanel::bind_fan_observers() {
    // Reset paired lifetime+observer members. Per [L084], lifetime BEFORE observer.
    part_speed_lifetime_.reset();
    part_speed_observer_.reset();
    hotend_speed_lifetime_.reset();
    hotend_speed_observer_.reset();
    aux_speed_lifetime_.reset();
    aux_speed_observer_.reset();

    auto primary = printer_state_.fan_state().classify_primary_fans();
    part_fan_name_ = primary.part;
    hotend_fan_name_ = primary.hotend;
    aux_fan_name_ = primary.aux;

    rebind_single_fan(part_speed_observer_, part_speed_lifetime_, part_fan_name_, "part_fan_speed",
                      "part_fan_icon");
    rebind_single_fan(hotend_speed_observer_, hotend_speed_lifetime_, hotend_fan_name_,
                      "hotend_fan_speed", "hotend_fan_icon");
    rebind_single_fan(aux_speed_observer_, aux_speed_lifetime_, aux_fan_name_, "aux_fan_speed",
                      "aux_fan_icon");

    // Aux cluster visibility — subject drives XML bind_flag_if_eq
    lv_subject_set_int(&aux_fan_present_subject_, aux_fan_name_.empty() ? 0 : 1);

    // Recompute composite aux subjects (icon/full/short) with updated aux_present.
    layout_fitter_.recompute_aux_composites();

    spdlog::debug("[{}] Bound fans: part='{}' hotend='{}' aux='{}'", get_name(), part_fan_name_,
                  hotend_fan_name_, aux_fan_name_);
}

void PrintStatusPanel::rebind_single_fan(ObserverGuard& guard, SubjectLifetime& lt,
                                         const std::string& object_name,
                                         const char* speed_label_widget_name,
                                         const char* icon_widget_name) {
    if (object_name.empty()) {
        update_fan_speed_display(speed_label_widget_name, icon_widget_name, 0);
        return;
    }
    lv_subject_t* subj = printer_state_.fan_state().get_fan_speed_subject(object_name, lt);
    if (!subj) {
        spdlog::warn("[{}] Fan '{}' subject not available", get_name(), object_name);
        return;
    }

    auto token = lifetime_.token();
    std::string label_copy = speed_label_widget_name;
    std::string icon_copy = icon_widget_name;
    guard = helix::ui::observe<int>(
        subj, this,
        [token, label_copy, icon_copy](PrintStatusPanel* self, int speed) {
            if (token.expired())
                return;
            self->update_fan_speed_display(label_copy.c_str(), icon_copy.c_str(), speed);
        },
        lt);

    // Seed initial value — observer fires only on change
    update_fan_speed_display(speed_label_widget_name, icon_widget_name, lv_subject_get_int(subj));
}

void PrintStatusPanel::update_fan_speed_display(const char* label_name, const char* icon_name,
                                                int speed) {
    if (!overlay_root_)
        return;
    lv_obj_t* label = lv_obj_find_by_name(overlay_root_, label_name);
    if (label) {
        char buf[8];
        helix::format::format_percent(speed, buf, sizeof(buf));
        lv_label_set_text(label, buf);
    }
    lv_obj_t* icon = lv_obj_find_by_name(overlay_root_, icon_name);
    if (icon) {
        if (!animations_enabled_ || speed <= 0)
            helix::ui::fan_spin_stop(icon);
        else
            helix::ui::fan_spin_start(icon, speed);
    }
}

void PrintStatusPanel::refresh_fan_animations() {
    if (!overlay_root_)
        return;
    auto refresh_one = [this](const std::string& name, const char* icon_widget) {
        if (name.empty())
            return;
        lv_subject_t* s = printer_state_.fan_state().get_fan_speed_subject(name);
        if (!s)
            return;
        lv_obj_t* icon = lv_obj_find_by_name(overlay_root_, icon_widget);
        if (!icon)
            return;
        int sp = lv_subject_get_int(s);
        if (!animations_enabled_ || sp <= 0)
            helix::ui::fan_spin_stop(icon);
        else
            helix::ui::fan_spin_start(icon, sp);
    };
    refresh_one(part_fan_name_, "part_fan_icon");
    refresh_one(hotend_fan_name_, "hotend_fan_icon");
    refresh_one(aux_fan_name_, "aux_fan_icon");
}

// SIZE_CHANGED on controls_section — defers density + fit recompute via lifetime token
void PrintStatusPanel::on_controls_size_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_controls_size_changed");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        auto token = self->lifetime_.token();
        token.defer("PrintStatusPanel::size_changed_recompute", [self]() {
            self->layout_fitter_.recompute_fans_density();
            self->layout_fitter_.recompute_fans_fit();
        });
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::recompute_paused_overlay_visibility() {
    if (!subjects_initialized_)
        return;

    auto pending = static_cast<helix::ui::PendingAction>(
        lv_subject_get_int(helix::ui::PrintControlButtons::instance().pending_action_subject()));

    // RAW_PRINT_STATE_OK: a value question - is the printer reporting paused? -
    // driving the optimistic Pause/Resume overlay. (PAUSED outranks a live phase
    // in derive_print_state(), so the lifecycle would answer identically; the
    // wire is simply the more direct statement of what is being asked.)
    // RAW_PRINT_STATE_OK: see the optimistic-overlay note below.
    auto state = printer_state_.print_state().get_print_job_state();
    // RAW_PRINT_STATE_OK: is the printer REPORTING paused - the optimistic
    // Pause/Resume overlay tracks the printer, not our intent.
    bool paused = (state == PrintJobState::PAUSED);

    // Optimistic overlay state while a Pause/Resume RPC is in flight: show the
    // overlay during a Pausing request even though Klipper still reports
    // PRINTING; hide it as soon as Resuming starts even though Klipper still
    // reports PAUSED. Reason label swaps to a transitional message so the user
    // sees their tap acknowledged without waiting ~20s for Moonraker to
    // confirm.
    bool effective_paused = paused;
    if (pending == helix::ui::PendingAction::Pausing) {
        effective_paused = true;
    } else if (pending == helix::ui::PendingAction::Resuming) {
        effective_paused = false;
    }
    lv_subject_set_int(&show_paused_overlay_subject_, effective_paused ? 1 : 0);

    // Reason resolution. Pending action takes precedence so the user always
    // sees feedback. A confirmed runout is authored copy naming the affected
    // lane, not Klipper's print_stats.message — some backends spell that with
    // a 0-based extruder name (e.g. "e0_filament") we do not control. Only a
    // pause cause we cannot positively identify as a runout falls back to that
    // firmware text. Otherwise leave blank → reason label stays hidden.
    std::string reason;
    if (pending == helix::ui::PendingAction::Pausing) {
        reason = lv_tr("Pausing...");
    } else if (pending == helix::ui::PendingAction::Resuming) {
        reason = lv_tr("Resuming...");
    } else if (paused) {
        if (FilamentSensorManager::instance().has_real_runout()) {
            AmsBackend* backend = AmsState::instance().get_backend();
            std::string lane = backend ? helix::ui::lane_label(helix::ui::active_lane_noun(),
                                                               backend->get_current_slot())
                                       : std::string();
            reason = lane.empty() ? std::string(lv_tr("Filament Runout"))
                                  : std::string(lv_tr("Filament Runout")) + " (" + lane + ")";
        } else {
            const char* fw_msg =
                lv_subject_get_string(printer_state_.print_state().get_print_message_subject());
            if (fw_msg && *fw_msg) {
                reason = fw_msg;
            }
        }
    }
    lv_subject_copy_string(&print_pause_reason_subject_, reason.c_str());
    lv_subject_set_int(&print_pause_reason_visible_subject_, reason.empty() ? 0 : 1);
}

void PrintStatusPanel::update_chamber_status() {
    if (!subjects_initialized_)
        return;

    bool has_heater = lv_subject_get_int(printer_state_.capabilities_state().subject(
                          Capability::HasChamberHeater)) != 0;
    int current = lv_subject_get_int(printer_state_.temperature_state().get_chamber_temp_subject());
    int target =
        lv_subject_get_int(printer_state_.temperature_state().get_chamber_target_subject());

    if (!has_heater || target == 0) {
        // Sensor-only or heater off: no status text
        chamber_status_buf_[0] = '\0';
        lv_subject_set_int(&chamber_status_state_subject_,
                           static_cast<int>(helix::ui::temperature::HeaterStatusState::None));
    } else {
        auto chamber = helix::ui::temperature::classify_heater_status(
            current, target,
            lv_subject_get_int(
                printer_state_.get_heater_power_subject(helix::HeaterType::Chamber)));
        lv_subject_set_int(&chamber_status_state_subject_, static_cast<int>(chamber.state));
        std::snprintf(chamber_status_buf_, sizeof(chamber_status_buf_), "%s", chamber.duty.c_str());
    }
    lv_subject_copy_string(&chamber_status_subject_, chamber_status_buf_);
}

void PrintStatusPanel::on_print_progress_changed(int progress) {
    // Delegate state guard and clamping to lifecycle
    if (!lifecycle_.on_progress_changed(progress)) {
        spdlog::trace("[{}] Ignoring progress update ({}) - guarded by lifecycle", get_name(),
                      progress);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Update progress text

    // Update progress bar with smooth animation (300ms ease-out) if animations enabled
    // This complements the subject binding with animated transitions
    if (progress_bar_) {
        lv_anim_enable_t anim_enable =
            DisplaySettingsManager::instance().get_animations_enabled() ? LV_ANIM_ON : LV_ANIM_OFF;
        lv_bar_set_value(progress_bar_, lifecycle_.progress(), anim_enable);
    }

    // Update filament used text (evolves during active printing)
    progress_text_.refresh_filament_used();

    spdlog::trace("[{}] Progress updated: {}%", get_name(), lifecycle_.progress());
}

void PrintStatusPanel::apply_new_print_resets(bool reset_progress_bar,
                                              bool clear_excluded_objects) {
    if (reset_progress_bar) {
        if (progress_bar_) {
            lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        }
        complete_view_mode_ = false;
        // Reset toggle icon to default (progress view)
        if (const char* icon = lv_xml_get_const(nullptr, "icon_cube")) {
            lv_subject_copy_string(&view_toggle_icon_subject_, icon);
        }
        // Clear any prior end-overlay dismissal so the next outcome surfaces.
        lv_subject_set_int(&end_overlay_dismissed_subject_, 0);
        spdlog::debug("[{}] Reset progress bar and view toggle for new print", get_name());
    }

    if (clear_excluded_objects && exclude_manager_) {
        exclude_manager_->clear_excluded_objects();
        spdlog::debug("[{}] Cleared excluded objects for new print", get_name());
    }
}

void PrintStatusPanel::on_print_state_changed(PrintJobState job_state) {
    spdlog::debug("[{}] on_print_state_changed() job_state={} current_state={}", get_name(),
                  static_cast<int>(job_state), static_cast<int>(lifecycle_.state()));

    // Get outcome from PrinterState for lifecycle decision-making
    auto outcome = static_cast<PrintOutcome>(
        lv_subject_get_int(printer_state_.print_state().get_print_outcome_subject()));

    // Delegate state mapping and transition logic to lifecycle. The live phase
    // goes in too: without it this derives Printing (or Complete) while the
    // published print_lifecycle correctly says Preparing, and the two disagree
    // for the whole pre-print window.
    const int start_phase =
        lv_subject_get_int(printer_state_.print_state().get_print_start_phase_subject());
    auto result = lifecycle_.on_job_state_changed(job_state, outcome, start_phase);
    if (!result.state_changed) {
        return;
    }

    // Refresh the print-scoped runout badge (FIX B) on every meaningful state
    // transition. When a print ends and the viewer's parsed file is dropped,
    // get_tools_used() empties → scoped value -1 → badge hides.
    recompute_scoped_runout();

    // Note: Badge/Reprint button visibility is now handled via the print_outcome subject,
    // which persists the terminal state (Complete/Cancelled/Error) until a new print starts.
    // The print_state_enum subject now always reflects the true Moonraker state.

    // Terminal→Idle: Moonraker sends STANDBY after Complete/Cancelled/Error.
    // Clean up tracking data but keep the display frozen — the user should see
    // the final print state until a new print starts.
    bool from_terminal_to_idle = result.print_ended && (result.old_state == PrintState::Complete ||
                                                        result.old_state == PrintState::Cancelled ||
                                                        result.old_state == PrintState::Error);

    // Clear thumbnail and G-code tracking when print ends
    if (result.print_ended) {
        // Note: Shared subjects (print_thumbnail_path, print_display_filename)
        // are cleared by ActivePrintMediaManager when print_filename_ becomes empty
        preview_.on_print_ended();
    }

    if (from_terminal_to_idle) {
        // Terminal→Idle: Moonraker sends zeroed subjects (progress=0, layer=0) in the
        // same batch as STANDBY. The XML subject bindings update widgets directly,
        // bypassing lifecycle guards. Re-freeze the display values to counteract this.
        if (subjects_initialized_) {
            // Re-freeze progress bar (XML bind_value="print_progress" set it to 0)
            if (progress_bar_) {
                lv_bar_set_value(progress_bar_, lifecycle_.progress(), LV_ANIM_OFF);
            }

            // Re-freeze gcode viewer layer. The viewer has its own observer on
            // print_layer_current_subject that already zeroed the display before
            // the lifecycle guard kicked in; push the frozen mapped layer back.
            // Deferred via queue_update for the same reason as the live update
            // path below — observer callbacks can fire mid-render.
            if (gcode_viewer_) {
                int viewer_max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
                int viewer_layer = lifecycle_.map_current_layer_to_viewer(viewer_max_layer);
                struct ViewerProgressCtx {
                    lv_obj_t* viewer;
                    int layer;
                };
                auto ctx = std::make_unique<ViewerProgressCtx>(
                    ViewerProgressCtx{gcode_viewer_, viewer_layer});
                helix::ui::queue_update<ViewerProgressCtx>(
                    std::move(ctx), [](ViewerProgressCtx* c) {
                        if (c->viewer && lv_obj_is_valid(c->viewer)) {
                            ui_gcode_viewer_set_print_progress(c->viewer, c->layer);
                        }
                    });
            }
        }
        // Don't call update_all_displays() or show_gcode_viewer() — keep display frozen
        spdlog::debug("[{}] Print state changed: {} -> {} (display frozen)", get_name(),
                      print_job_state_to_string(job_state), static_cast<int>(result.new_state));
    } else {
        update_all_displays();
        update_button_states();
        show_gcode_viewer(result.should_show_viewer);
        spdlog::debug("[{}] Print state changed: {} -> {}", get_name(),
                      print_job_state_to_string(job_state), static_cast<int>(result.new_state));
    }

    // Delegate runout guidance handling to the handler
    if (runout_handler_) {
        runout_handler_->on_print_state_changed(result.old_state, result.new_state);
    }

    // Update the "Print Paused" overlay any time the job state moves —
    // covers PRINTING→PAUSED, PAUSED→PRINTING, PAUSED→CANCELLED, mid-print attach.
    recompute_paused_overlay_visibility();

    if (result.should_reset_progress_bar || result.should_clear_excluded_objects) {
        apply_new_print_resets(result.should_reset_progress_bar,
                               result.should_clear_excluded_objects);
    }

    // Transition remaining display from preprint observer back to Moonraker's time_left
    if (result.new_state == PrintState::Printing) {
        progress_text_.show_remaining(lifecycle_.remaining_seconds());
    }

    // Freeze display values on Complete (lifecycle already froze the state values)
    if (result.should_freeze_complete) {
        if (progress_bar_) {
            lv_bar_set_value(progress_bar_, 100, LV_ANIM_OFF);
        }

        if (lifecycle_.total_layers() > 0) {
            progress_text_.refresh_layer();
        }

        progress_text_.show_elapsed(lifecycle_.elapsed_seconds());
        progress_text_.show_remaining(0);

        animate_print_complete();

        spdlog::info("[{}] Print complete! Final progress: {}%, layer: {}/{}, elapsed: {}s",
                     get_name(), lifecycle_.progress(), lifecycle_.current_layer(),
                     lifecycle_.total_layers(), lifecycle_.elapsed_seconds());
    }

    if (result.should_animate_error) {
        animate_print_error();
        spdlog::info("[{}] Print failed at progress: {}%", get_name(), lifecycle_.progress());
    }

    if (result.should_animate_cancelled) {
        animate_print_cancelled();
        spdlog::debug("[{}] Print cancelled at progress: {}%", get_name(), lifecycle_.progress());
    }

    // Nothing here touches the header's action_button: this panel never
    // configures one, so un-hiding it renders an empty primary-colored pill.
}

void PrintStatusPanel::on_print_filename_changed(const char* filename) {
    // Check if this is a non-empty filename (new print starting)
    bool has_filename = filename && filename[0] != '\0';

    // Guard: preserve final values when in Complete state and filename is empty
    // Moonraker sends empty filename when transitioning to Standby, but we want
    // to keep showing the completed print's filename. However, if a NEW print
    // starts (non-empty filename), we should accept it even if current_state_
    // hasn't been updated yet (race condition between state and filename observers)
    if (lifecycle_.state() == PrintState::Complete && !has_filename) {
        spdlog::trace("[{}] Ignoring empty filename update in Complete state", get_name());
        return;
    }

    if (has_filename) {
        std::string raw_filename = filename;

        // Call set_filename() which is idempotent (won't reload if effective filename unchanged)
        // Only log when filename actually changes to avoid log spam
        if (raw_filename != current_print_filename_) {
            spdlog::debug("[{}] Filename changed: {}", get_name(), raw_filename);
        }
        set_filename(filename);
    }
}

void PrintStatusPanel::on_speed_factor_changed(int speed) {
    lifecycle_.on_speed_changed(speed);
    update_speed_flow_text();
    spdlog::trace("[{}] Speed factor updated: {}%", get_name(), speed);
}

void PrintStatusPanel::on_flow_factor_changed(int flow) {
    lifecycle_.on_flow_changed(flow);
    update_speed_flow_text();
    spdlog::trace("[{}] Flow factor updated: {}%", get_name(), flow);
}

void PrintStatusPanel::update_speed_flow_text() {
    if (!subjects_initialized_) {
        return;
    }
    progress_text_.refresh_speed_flow();
}

void PrintStatusPanel::on_gcode_z_offset_changed(int /* microns */) {
    // Delegate to tune overlay singleton. Resolve the value rather than forwarding
    // the raw live offset: ZMOD zeroes that outside a print, and handing the
    // overlay a phantom zero would make its next baby-step adjust from the wrong
    // base.
    get_print_tune_overlay().update_z_offset_display(
        helix::zoffset::displayed_z_offset_microns(printer_state_));
}

void PrintStatusPanel::on_print_layer_changed(int current_layer) {
    // Read total layers from PrinterState and delegate to lifecycle
    int total_layers =
        lv_subject_get_int(printer_state_.print_state().get_print_layer_total_subject());
    bool has_real_data = printer_state_.print_state().has_real_layer_data();
    if (!lifecycle_.on_layer_changed(current_layer, total_layers, has_real_data)) {
        spdlog::trace("[{}] Ignoring layer update ({}) - guarded by lifecycle", get_name(),
                      current_layer);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    progress_text_.refresh_layer();

    // Update G-code viewer ghost layer if panel is active and viewer is visible
    if (is_active_ && gcode_viewer_ && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN) &&
        !complete_view_mode_) {
        // Map from Moonraker layer count (e.g., 240) to viewer layer count (e.g., 2912)
        // The slicer metadata and parsed G-code often have different layer counts
        int viewer_max_layer = ui_gcode_viewer_get_max_layer(gcode_viewer_);
        int viewer_layer = lifecycle_.map_current_layer_to_viewer(viewer_max_layer);

        // CRITICAL: Defer to avoid lv_obj_invalidate() during render phase
        // Observer callbacks can fire during lv_timer_handler() which may be mid-render
        struct ViewerProgressCtx {
            lv_obj_t* viewer;
            int layer;
        };
        auto ctx =
            std::make_unique<ViewerProgressCtx>(ViewerProgressCtx{gcode_viewer_, viewer_layer});
        helix::ui::queue_update<ViewerProgressCtx>(std::move(ctx), [](ViewerProgressCtx* c) {
            if (c->viewer && lv_obj_is_valid(c->viewer)) {
                ui_gcode_viewer_set_print_progress(c->viewer, c->layer);
            }
        });

        spdlog::trace("[{}] G-code viewer ghost layer updated to {} (Moonraker: {}/{})", get_name(),
                      viewer_layer, current_layer, lifecycle_.total_layers());
    }
}

void PrintStatusPanel::on_print_duration_changed(int seconds) {
    // Get outcome from PrinterState and delegate guard + state update to lifecycle
    auto outcome = static_cast<PrintOutcome>(
        lv_subject_get_int(printer_state_.print_state().get_print_outcome_subject()));
    if (!lifecycle_.on_duration_changed(seconds, outcome)) {
        spdlog::trace("[{}] Ignoring duration update ({}) - guarded by lifecycle", get_name(),
                      seconds);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // total_duration from Moonraker already includes prep time (wall-clock elapsed)
    progress_text_.show_elapsed(lifecycle_.elapsed_seconds());
    spdlog::trace("[{}] Elapsed updated: {}s (wall-clock from Moonraker)", get_name(), seconds);
}

void PrintStatusPanel::on_print_time_left_changed(int seconds) {
    // Get outcome from PrinterState and delegate guard + state update to lifecycle
    auto outcome = static_cast<PrintOutcome>(
        lv_subject_get_int(printer_state_.print_state().get_print_outcome_subject()));
    if (!lifecycle_.on_time_left_changed(seconds, outcome)) {
        spdlog::trace("[{}] Ignoring time_left update ({}) - guarded by lifecycle", get_name(),
                      seconds);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    progress_text_.show_time_left(lifecycle_.remaining_seconds());
}

void PrintStatusPanel::cancel_preparing_show_timer() {
    if (preparing_show_timer_) {
        helix::ui::lv_timer_cancel_safe(preparing_show_timer_);
        preparing_show_timer_ = nullptr;
    }
}

void PrintStatusPanel::on_print_start_phase_changed(int phase) {
    // Phase 0 = IDLE (not preparing), non-zero = preparing
    bool preparing = (phase != 0);

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Delegate state transition to lifecycle. RAW_PRINT_STATE_OK: the panel's
    // PrintLifecycleState derives its own PrintState from (wire, phase) via
    // derive_print_state(), so this feeds it the wire half deliberately.
    auto current_job_state = printer_state_.print_state().get_print_job_state();
    bool state_changed = lifecycle_.on_start_phase_changed(phase, current_job_state);

    // Update preparing visibility, debounced on the way UP only. Hiding is
    // immediate: once preparation is over the overlay must go at once.
    if (!preparing) {
        cancel_preparing_show_timer();
        lv_subject_set_int(&preparing_visible_subject_, 0);
    } else if (lv_subject_get_int(&preparing_visible_subject_) == 0 && !preparing_show_timer_) {
        preparing_show_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<PrintStatusPanel*>(lv_timer_get_user_data(t));
                self->preparing_show_timer_ = nullptr;
                lv_timer_delete(t);
                // Re-check: preparation may have ended while we waited.
                if (lv_subject_get_int(
                        self->printer_state_.print_state().get_print_start_phase_subject()) != 0) {
                    lv_subject_set_int(&self->preparing_visible_subject_, 1);
                }
            },
            PREPARING_SHOW_DELAY_MS, this);
        lv_timer_set_repeat_count(preparing_show_timer_, 1);
    }

    if (preparing && !was_preparing_) {
        // Tune/Timelapse enablement follows PrintState, so it has to be
        // republished on the way IN to Preparing as well as on the way out.
        // Without this it only happened to be right because a normal start
        // navigates, and on_activate() republishes; Reprint leaves the panel
        // already active, so Tune stayed greyed for the whole window.
        update_button_states();

        // Idle→Preparing edge ONLY. The pre-print phase number changes many
        // times during one preparation, so these one-time resets must not
        // re-run on every sub-phase or the progress bar / elapsed flicker back
        // to zero repeatedly. The message and progress observers keep the
        // display live for the remainder of preparation.
        //
        // Preserve the thumbnail — it was loaded for the current print by the
        // filename observer or ActivePrintMediaManager. The preparing phase
        // fires concurrently with thumbnail loading, so clearing here would
        // race and discard a valid thumbnail. Stale thumbnails from a previous
        // print are cleared by the print_ended path in on_print_state_changed.
        if (progress_bar_) {
            lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        }
        progress_text_.clear_layer();

        // Initialize elapsed display to 0m (preprint observer will update it)
        progress_text_.show_elapsed(0);

        // Show predicted total as initial remaining estimate (preprint observer refines it)
        int predicted = helix::PreprintPredictor::predicted_total_from_config();
        if (predicted > 0) {
            int total_remaining = lifecycle_.remaining_seconds() + predicted;
            progress_text_.show_remaining(total_remaining);
        }
    } else if (!preparing && state_changed) {
        // Preparation complete - lifecycle restored state from current job state

        // The per-job resets have to fire HERE for a print started in-app. The
        // panel is already Preparing before Moonraker reports printing, so
        // on_job_state_changed() derives Preparing == current, reports
        // state_changed=false and returns early: its should_reset_progress_bar /
        // should_clear_excluded_objects never become true. Exiting Preparing is
        // the only edge that sees the new print at all.
        //
        // Without this, print B opened in print A's completion view (a dismissed
        // end overlay stays dismissed, complete_view_mode_ survives a cached
        // panel) and carried print A's excluded objects. Externally started
        // prints were unaffected, because those go Idle -> Printing.
        if (lifecycle_.state() == PrintState::Printing) {
            apply_new_print_resets(/*reset_progress_bar=*/true,
                                   /*clear_excluded_objects=*/true);
        }

        update_all_displays();
        update_button_states();

        // Reconcile the preview now that the print is no longer preparing. The
        // viewer may need the deferred gcode load kicked and the thumbnail
        // confirmed. ensure_current() reads the real widget state and
        // reloads only what is missing.
        preview_.ensure_current();

        spdlog::debug("[{}] Restored state to {} after preparation complete", get_name(),
                      static_cast<int>(lifecycle_.state()));
    }
    was_preparing_ = preparing;
    spdlog::debug("[{}] Print start phase changed: {} (visible={})", get_name(), phase, preparing);
}

void PrintStatusPanel::on_print_start_progress_changed(int progress) {
    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    lv_subject_set_int(&preparing_progress_subject_, progress);

    // Animate bar for smooth visual feedback
    if (preparing_progress_bar_) {
        lv_anim_enable_t anim_enable =
            DisplaySettingsManager::instance().get_animations_enabled() ? LV_ANIM_ON : LV_ANIM_OFF;
        lv_bar_set_value(preparing_progress_bar_, progress, anim_enable);
    }
    spdlog::trace("[{}] Print start progress: {}%", get_name(), progress);
}

void PrintStatusPanel::on_preprint_remaining_changed(int seconds) {
    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Delegate to lifecycle (handles Preparing guard internally)
    // Fall back to get_estimated_print_time() if remaining_seconds hasn't been seeded yet
    int slicer_time = lifecycle_.remaining_seconds() > 0
                          ? lifecycle_.remaining_seconds()
                          : printer_state_.print_state().get_estimated_print_time();
    lifecycle_.on_preprint_remaining_changed(seconds, slicer_time);

    if (lifecycle_.state() != PrintState::Preparing) {
        return;
    }

    // Combine preprint prediction with slicer estimate for total remaining time
    int total_remaining = slicer_time + seconds;
    progress_text_.show_remaining(total_remaining);
    spdlog::trace("[{}] Preprint remaining: {}s preprint + {}s slicer = {}s", get_name(), seconds,
                  slicer_time, total_remaining);
}

void PrintStatusPanel::on_preprint_elapsed_changed(int seconds) {
    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    // Delegate to lifecycle (handles Preparing guard internally)
    lifecycle_.on_preprint_elapsed_changed(seconds);

    if (lifecycle_.state() != PrintState::Preparing) {
        return;
    }

    progress_text_.show_elapsed(lifecycle_.preprint_elapsed_seconds());
}

void PrintStatusPanel::update_view_toggle_position(bool objects_visible) {
    if (!overlay_root_)
        return;
    // Resolve the card by name, not by walking up from the viewer: the previews
    // live one level down inside preview_clear_area, while both corner buttons
    // are direct children of thumbnail_section.
    lv_obj_t* card = helix::ui::find_required(overlay_root_, "thumbnail_section", get_name());
    if (!card)
        return;
    lv_obj_t* btn = helix::ui::find_required(card, "btn_view_toggle", get_name());
    if (!btn)
        return;

    int32_t space_md = theme_manager_get_spacing("space_md");
    if (objects_visible) {
        lv_obj_t* btn_objects = helix::ui::find_required(card, "btn_objects", get_name());
        int32_t obj_w = btn_objects ? lv_obj_get_width(btn_objects) : 36;
        lv_obj_set_style_translate_x(btn, space_md + obj_w + space_md, LV_PART_MAIN);
    } else {
        lv_obj_set_style_translate_x(btn, space_md, LV_PART_MAIN);
    }
}

void PrintStatusPanel::refresh_exclude_objects_available() {
    // Klipper reports defined objects only from the G-code file it is
    // printing, so a print is never a 3MF.
    const int available = helix::ui::pre_start_exclude_available(
                              helix::ui::printer_has_exclude_object(&printer_state_), false,
                              printer_state_.excluded_objects_state().get_defined_objects().size())
                              ? 1
                              : 0;
    lv_subject_set_int(&exclude_objects_available_subject_, available);
    update_objects_text();
    update_view_toggle_position(available != 0);
}

void PrintStatusPanel::update_objects_text() {
    if (!subjects_initialized_)
        return;
    auto& defined = printer_state_.excluded_objects_state().get_defined_objects();
    auto& excluded = printer_state_.excluded_objects_state().get_excluded_objects();
    int total = static_cast<int>(defined.size());
    int active = std::max(0, total - static_cast<int>(excluded.size()));
    if (total >= 2) {
        std::snprintf(objects_text_buf_, sizeof(objects_text_buf_), "%d/%d", active, total);
    } else {
        objects_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&objects_text_subject_, objects_text_buf_);
}

void PrintStatusPanel::update_camera_button_label(int breakpoint_value) {
    // Full word only from LARGE up (narrow axis >= 551px, e.g. 1024x600); the
    // 800x480 Row 2 and the cramped portrait row get the short form.
    const char* label =
        breakpoint_value >= to_int(UiBreakpoint::Large) ? lv_tr("Camera") : lv_tr("Cam");
    lv_subject_copy_string(&camera_button_label_subject_, label);
}

void PrintStatusPanel::update_button_states() {
    // Drive button enable via subjects; XML `bind_state_if_eq ... state="disabled"
    // ref_value="0"` toggles LV_STATE_DISABLED, and ui_button dims on disabled.
    auto state = lifecycle_.state();
    bool controls_enabled = PrintLifecycleState::is_active(state);

    // The pause/resume and stop button enable states are owned by
    // PrintControlButtons now; this panel only drives the timelapse/tune gate.
    lv_subject_set_int(&print_controls_enabled_subject_, controls_enabled ? 1 : 0);

    // Cancel/Reprint visibility is driven entirely by the print_outcome subject
    // via bind_flag_if_eq / bind_flag_if_not_eq on the <ui_button> elements.

    spdlog::debug("[{}] Button states updated: controls={} (state={})", get_name(),
                  controls_enabled ? "enabled" : "disabled", static_cast<int>(state));
}

void PrintStatusPanel::animate_badge_pop_in(lv_obj_t* badge, const char* label) {
    if (!badge) {
        return;
    }

    constexpr int32_t SCALE_FINAL = 256; // 100% scale

    // Skip animation if disabled - show badge in final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_transform_scale(badge, SCALE_FINAL, LV_PART_MAIN);
        lv_obj_set_style_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::debug("[{}] Animations disabled - showing {} badge instantly", get_name(), label);
        return;
    }

    // Pop-in animation: quick scale-up with overshoot, then settle
    constexpr int32_t POP_DURATION_MS = 300;
    constexpr int32_t SETTLE_DURATION_MS = 150;
    constexpr int32_t SCALE_START = 128;     // 50% scale (128/256)
    constexpr int32_t SCALE_OVERSHOOT = 282; // ~110% scale

    // Start badge small and transparent
    lv_obj_set_style_transform_scale(badge, SCALE_START, LV_PART_MAIN);
    lv_obj_set_style_opa(badge, LV_OPA_TRANSP, LV_PART_MAIN);

    // Stage 1: Scale up with overshoot + fade in
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, badge);
    lv_anim_set_values(&scale_anim, SCALE_START, SCALE_OVERSHOOT);
    lv_anim_set_duration(&scale_anim, POP_DURATION_MS);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&scale_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);

    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, badge);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, POP_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    // Stage 2: Settle from overshoot to final size (delayed start)
    lv_anim_t settle_anim;
    lv_anim_init(&settle_anim);
    lv_anim_set_var(&settle_anim, badge);
    lv_anim_set_values(&settle_anim, SCALE_OVERSHOOT, SCALE_FINAL);
    lv_anim_set_duration(&settle_anim, SETTLE_DURATION_MS);
    lv_anim_set_delay(&settle_anim, POP_DURATION_MS);
    lv_anim_set_path_cb(&settle_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&settle_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&settle_anim);

    spdlog::debug("[{}] {} badge animation started", get_name(), label);
}

void PrintStatusPanel::animate_print_complete() {
    animate_badge_pop_in(success_badge_, "complete");
}

void PrintStatusPanel::animate_print_cancelled() {
    animate_badge_pop_in(cancel_badge_, "cancelled");
}

void PrintStatusPanel::animate_print_error() {
    animate_badge_pop_in(error_badge_, "error");
}

// Tune panel handlers delegated to PrintTuneOverlay singleton:
// See get_print_tune_overlay() and handle_*() methods in ui_print_tune_overlay.cpp
// XML callbacks are registered in ui_print_tune_overlay.cpp on first show()

// ============================================================================
// FILAMENT COLOR OVERRIDE
// ============================================================================

// ============================================================================
// PUBLIC API
// ============================================================================

void PrintStatusPanel::set_temp_control_panel(TemperatureService* temp_panel) {
    temp_control_panel_ = temp_panel;
    spdlog::trace("[{}] TemperatureService reference set", get_name());
}

void PrintStatusPanel::set_filename(const char* filename) {
    // Store the actual filename (may be a temp file path)
    current_print_filename_ = filename ? filename : "";

    // The identity of the running print - including retiring an override that
    // stopped describing it, and resolving a rewritten temp path - is decided by
    // PrinterPrintState before print_filename_ is ever published. The panel
    // only reconciles its local resources (G-code viewer, thumbnail) against it
    // (prestonbrown/helixscreen#1339).
    preview_.on_filename_changed();
}
