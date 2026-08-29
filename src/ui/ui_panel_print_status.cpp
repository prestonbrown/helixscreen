// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_print_status.h"

#include "ui_ams_current_tool.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_exclude_object_map_view.h"
#include "ui_fan_control_overlay.h"
#include "ui_filament_mapping_card.h"
#include "ui_filename_utils.h"
#include "ui_format_utils.h"
#include "ui_gcode_viewer.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_common.h"
#include "ui_panel_print_select.h"
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
#include "config.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "filament_mapper.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "gcode_parser.h"
#include "gcode_preview_setup.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "injection_point_manager.h"
#include "layout_manager.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "observer_factory.h"
#include "preprint_predictor.h"
#include "print_status_layout_decision.h"
#include "print_status_preview_decision.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "system/crash_handler.h"
#include "temp_graph_controller.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/fan_spin_animation.h"
#include "ui/ui_widget_helpers.h"
#include "wizard_config_paths.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::gcode::resolve_gcode_filename;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

// The shared thumbnail path subject never carries the empty string: a file with
// no thumbnail (yet) is published as no_thumbnail_placeholder(). That value is a
// perfectly good image to PUT ON SCREEN, but it is not this print's thumbnail,
// so it must never stamp displayed_file_. ActivePrintMediaManager draws the same
// distinction in has_thumbnail_for() and says why: take the placeholder for a
// thumbnail and "every print would stop at the placeholder", because the marker
// it leaves behind is what tells decide_preview_action() there is nothing left
// to load.
static bool is_no_thumbnail_placeholder(const char* path) {
    return path != nullptr &&
           std::strcmp(path, helix::PrinterPrintState::no_thumbnail_placeholder()) == 0;
}

// Global instance for legacy API and resize callback
static std::unique_ptr<PrintStatusPanel> g_print_status_panel;

// Cached widget pointer for lazy creation (separate from overlay_root_ which
// is managed by OverlayBase). Declared here so teardown callback can null it.
static lv_obj_t* s_cached_panel = nullptr;

// ID of the MemoryMonitor pressure responder. 0 means "not registered".
// Registered lazily on first push_overlay(); unregistered in the static
// panel-destroy callback to prevent calls into a destroyed singleton.
static helix::MemoryMonitor::PressureResponderId s_memory_responder_id = 0;

// Observer factory pattern
using helix::ui::observe_int_sync;
using helix::ui::observe_print_state;
using helix::ui::observe_string;

// Helper to get or create the global instance
PrintStatusPanel& get_global_print_status_panel() {
    if (!g_print_status_panel) {
        g_print_status_panel = std::make_unique<PrintStatusPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("PrintStatusPanel", []() {
            if (s_memory_responder_id != 0) {
                helix::MemoryMonitor::instance().remove_pressure_responder(s_memory_responder_id);
                s_memory_responder_id = 0;
            }
            if (s_cached_panel && g_print_status_panel) {
                g_print_status_panel->destroy_overlay_ui(s_cached_panel);
            }
            s_cached_panel = nullptr;
            g_print_status_panel.reset();
        });
    }
    return *g_print_status_panel;
}

// Drop the cached widget tree if we can, to reclaim ~400-800KB.
// Safe to call from any thread — hops to UI thread via queue_update and
// bails out if the overlay is currently visible. No-op when there's no
// cached tree (e.g. print status was never opened this session).
static void try_reclaim_cached_print_status() {
    helix::ui::queue_update([]() {
        if (!s_cached_panel) {
            return;
        }
        if (NavigationManager::instance().is_panel_in_stack(s_cached_panel)) {
            spdlog::debug(
                "[PrintStatusPanel] Cached tree is currently visible, skipping memory reclaim");
            return;
        }
        if (!g_print_status_panel) {
            return;
        }
        spdlog::warn("[PrintStatusPanel] Pressure response: destroying cached overlay tree");
        g_print_status_panel->destroy_overlay_ui(s_cached_panel);
        // destroy_overlay_ui() nulls s_cached_panel via its by-ref parameter;
        // next push_overlay() will lazily recreate.
    });
}

PrintStatusPanel::PrintStatusPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {
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
    active_tool_observer_ = observe_int_sync<PrintStatusPanel>(
        helix::ToolState::instance().get_active_tool_subject(), this,
        [](PrintStatusPanel* self, int) { self->on_temperature_changed(); },
        helix::ToolState::instance().get_subjects_lifetime());

    // Chamber status text: observe chamber temp to compute Heating/Cooling/Holding status
    chamber_temp_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_chamber_temp_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_chamber_status(); }, ps_subjects);

    // Subscribe to print progress and state
    print_progress_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_progress_subject(), this,
        [](PrintStatusPanel* self, int progress) { self->on_print_progress_changed(progress); },
        ps_subjects);
    print_state_observer_ = observe_print_state<PrintStatusPanel>(
        // RAW_PRINT_STATE_OK: the panel's lifecycle_ adopts the published
        // PrintState (Phase 0b); this observer feeds it the wire transition that
        // derive_print_state() needs alongside the live phase.
        printer_state_.get_print_state_enum_subject(), this,
        [](PrintStatusPanel* self, PrintJobState state) { self->on_print_state_changed(state); },
        ps_subjects);
    // The print's identity can change without the reported filename changing -
    // an override installed at commit, or released when a job is abandoned - so
    // the filename observer below is not enough on its own. This is the same
    // reconcile the old set_thumbnail_source() forced by calling set_filename()
    // on itself, minus the coupling that let the panel and the media manager
    // drift apart (prestonbrown/helixscreen#1339).
    print_identity_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_identity_epoch_subject(), this,
        [](PrintStatusPanel* self, int /*epoch*/) {
            // No marker clearing here on purpose. decide_preview_action() already
            // compares BOTH markers against the new identity and reloads whichever
            // is stale; clearing by hand duplicates that, and clearing only the
            // thumbnail marker - as the first draft of this did - leaves the
            // viewer holding the previous print's geometry, which is the exact
            // bug 921200ab1 fixed.
            self->ensure_preview_current();
        },
        ps_subjects);

    print_filename_observer_ = observe_string<PrintStatusPanel>(
        printer_state_.get_print_filename_subject(), this,
        [](PrintStatusPanel* self, const char* filename) {
            self->on_print_filename_changed(filename);
        },
        ps_subjects);

    // Subscribe to speed/flow factors
    speed_factor_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_speed_factor_subject(), this,
        [](PrintStatusPanel* self, int speed) { self->on_speed_factor_changed(speed); },
        ps_subjects);
    flow_factor_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_flow_factor_subject(), this,
        [](PrintStatusPanel* self, int flow) { self->on_flow_factor_changed(flow); }, ps_subjects);
    gcode_z_offset_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_gcode_z_offset_subject(), this,
        [](PrintStatusPanel* self, int microns) { self->on_gcode_z_offset_changed(microns); },
        ps_subjects);

    // Subscribe to layer tracking for G-code viewer ghost layer updates
    print_layer_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_layer_current_subject(), this,
        [](PrintStatusPanel* self, int layer) { self->on_print_layer_changed(layer); },
        ps_subjects);

    // Re-render layer text when Z position changes (Z updates more frequently than layer count)
    z_position_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_gcode_position_z_subject(), this,
        [](PrintStatusPanel* self, int) {
            int layer = lv_subject_get_int(self->printer_state_.get_print_layer_current_subject());
            self->on_print_layer_changed(layer);
        },
        ps_subjects);

    // Subscribe to wall-clock elapsed time (total_duration includes prep time)
    print_duration_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_elapsed_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_print_duration_changed(seconds); },
        ps_subjects);
    print_time_left_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_time_left_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_print_time_left_changed(seconds); },
        ps_subjects);

    // Subscribe to print start preparation phase subjects
    print_start_phase_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_start_phase_subject(), this,
        [](PrintStatusPanel* self, int phase) { self->on_print_start_phase_changed(phase); },
        ps_subjects);
    print_start_progress_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_start_progress_subject(), this,
        [](PrintStatusPanel* self, int progress) {
            self->on_print_start_progress_changed(progress);
        },
        ps_subjects);
    preprint_remaining_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_preprint_remaining_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_preprint_remaining_changed(seconds); },
        ps_subjects);
    preprint_elapsed_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_preprint_elapsed_subject(), this,
        [](PrintStatusPanel* self, int seconds) { self->on_preprint_elapsed_changed(seconds); },
        ps_subjects);

    // Subscribe to defined objects changes (for objects list button visibility + count)
    exclude_objects_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_defined_objects_version_subject(), this,
        [](PrintStatusPanel* self, int) {
            int available = self->printer_state_.get_defined_objects().size() >= 2 ? 1 : 0;
            lv_subject_set_int(&self->exclude_objects_available_subject_, available);
            self->update_objects_text();
            self->update_view_toggle_position(available != 0);
        },
        ps_subjects);

    // Subscribe to excluded objects changes (for "X of Y obj" count updates)
    excluded_objects_version_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_excluded_objects_version_subject(), this,
        [](PrintStatusPanel* self, int) { self->update_objects_text(); }, ps_subjects);

    // Subscribe to AMS current filament color for gcode viewer color override
    // When a known filament color is available (from Spoolman spool or AMS lane),
    // use it instead of the gcode metadata color for the 2D/3D render
    ams_color_observer_ = observe_int_sync<PrintStatusPanel>(
        AmsState::instance().get_current_color_subject(), this,
        [](PrintStatusPanel* self, int /*color_rgb*/) { self->build_and_apply_tool_colors(); },
        AmsState::instance().get_subjects_lifetime());

    // Also refresh gcode viewer colors when tool_to_slot_map changes (user remap)
    tool_map_version_observer_ = observe_int_sync<PrintStatusPanel>(
        AmsState::instance().get_tool_map_version_subject(), this,
        [](PrintStatusPanel* self, int /*version*/) { self->build_and_apply_tool_colors(); },
        AmsState::instance().get_subjects_lifetime());

    // Adopt the preparing job's identity the moment a job starts preparing, the
    // way ActivePrintMediaManager already does (adac6f7eb gave it this observer
    // and gave the panel none). Without it the panel's `desired` stays on the
    // PREVIOUS print for the whole commit-to-confirmation window, so
    // ensure_preview_current() compares the viewer against the finished print,
    // finds no mismatch, and the clear_gcode that 921200ab1 added never fires -
    // leaving the previous print's model on screen exactly when it was meant to
    // be dropped.
    //
    // observe_int_immediate for the manager's reason: _sync routes through
    // queue_update, so the identity would land AFTER a synchronously dispatched
    // filename update had already reconciled against the stale name. The
    // handler only assigns identity fields and reconciles the preview - no
    // observer lifecycle changes, no widget destruction.
    // Subscribe to the shared print thumbnail path. ActivePrintMediaManager is
    // its sole writer; this panel only reads it.
    // Use observe_string_immediate: the handler only calls lv_image_set_src
    // (no observer lifecycle changes), and set_print_thumbnail is always called
    // from the UI thread via queue_update.
    print_thumbnail_path_observer_ = ui::observe_string_immediate<PrintStatusPanel>(
        printer_state_.get_print_thumbnail_path_subject(), this,
        [](PrintStatusPanel* self, const char* path) {
            // No empty-path branch: ActivePrintMediaManager publishes
            // no_thumbnail_placeholder() for a file with no thumbnail and the
            // subject is seeded with it, so the value is always an image.
            // The subject carries the file the path was produced FOR
            // (set_print_thumbnail writes it before publishing the path), so
            // compare identity instead of assuming the value is ours. A result
            // that lands for the previous print must not be applied, and above
            // all must not advance displayed_file_ — that stamp is what
            // convinced ensure_preview_current() the current file was already
            // on screen, turning activation and print start into no-ops.
            const std::string& for_file = self->printer_state_.get_print_thumbnail_file();
            const std::string& effective = self->printer_state_.get_effective_print_filename();
            if (!effective.empty() && for_file != effective) {
                spdlog::debug("[{}] Ignoring thumbnail published for '{}' (showing '{}')",
                              self->get_name(), for_file, effective);
                return;
            }
            self->cached_thumbnail_path_ = path;
            if (self->print_thumbnail_) {
                lv_image_set_src(self->print_thumbnail_, path);
                spdlog::debug("[{}] Thumbnail updated from shared subject: {}", self->get_name(),
                              path);
                // Record what is ACTUALLY on screen, not what the panel wishes
                // were on screen. The manager publishes the placeholder FOR the
                // incoming file as its clear, so identity matches here even
                // though no thumbnail has been fetched yet; stamping that would
                // retire the reconcile before the real image ever arrives.
                // Clearing is the honest marker: no file's thumbnail is up.
                if (is_no_thumbnail_placeholder(path)) {
                    self->displayed_file_.clear();
                } else {
                    self->displayed_file_ = for_file;
                }
            }
        },
        ps_subjects);

#if defined(HELIX_PLATFORM_ESP32)
    // ESP32 has no disk thumbnail cache, so print_thumbnail_path stays empty and
    // the image arrives as a PSRAM buffer instead. Observe the generation counter
    // ActivePrintMediaManager bumps when it installs one. observe_int_immediate
    // for the same reason as the path observer above: the handler only does
    // lv_image_set_src plus a shared_ptr swap (no observer lifecycle changes, no
    // widget destruction), and the setter always runs on the UI thread — so the
    // extra deferral would only add a frame and a stale-read window.
    print_psram_thumb_observer_ = ui::observe_int_immediate<PrintStatusPanel>(
        printer_state_.get_print_psram_thumb_gen_subject(), this,
        [](PrintStatusPanel* self, int /*gen*/) { self->apply_esp_psram_thumbnail(); },
        ps_subjects);
#endif

    spdlog::debug("[{}] Subscribed to PrinterState subjects", get_name());

    // LED configuration is read lazily by PrintLightTimelapseControls::handle_light_button()
    // At construction time, hardware discovery may not have completed yet.
    // LED state observer is set up on first on_activate() when strips are available.
    led_state_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_led_state_subject(), this,
        [](PrintStatusPanel* self, int state) { self->on_led_state_changed(state); }, ps_subjects);
    spdlog::debug("[{}] LED state observer registered (strips read lazily)", get_name());

    // Subscribe to G-code render mode changes from settings panel
    // This allows real-time updates to the viewer when the user changes the setting
    gcode_render_mode_observer_ = observe_int_sync<PrintStatusPanel>(
        DisplaySettingsManager::instance().subject_gcode_render_mode(), this,
        [](PrintStatusPanel* self, int mode) {
            // A command-line override outranks the saved setting (cmdline > env > settings,
            // as applied below in on_activate). Without this guard the observer fires once
            // at startup with the persisted value and silently overwrites --render-2d /
            // --render-3d about 16ms after they were applied, so the flags appeared to do
            // nothing.
            const auto* rt_config = get_runtime_config();
            if (rt_config && rt_config->gcode_render_mode >= 0) {
                spdlog::debug("[{}] Ignoring settings render mode {} - command line pinned {}",
                              self->get_name(), mode, rt_config->gcode_render_mode);
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
    print_outcome_observer_ = observe_int_sync<PrintStatusPanel>(
        printer_state_.get_print_outcome_subject(), this,
        [](PrintStatusPanel* self, int) { self->recompute_end_overlay_visibility(); }, ps_subjects);
    recompute_end_overlay_visibility();

    // Create filament runout handler (extracted from PrintStatusPanel)
    runout_handler_ = std::make_unique<helix::ui::FilamentRunoutHandler>(api_);
    spdlog::debug("[{}] Created filament runout handler", get_name());
}

PrintStatusPanel::~PrintStatusPanel() {
    // Before deinit_subjects(): the mini-graph's observers are attached to
    // PrinterState subjects, and detaching them after those are freed is the
    // exact use-after-free ObserverGuard exists to prevent. Synchronous delete —
    // nothing will drain the async queue on the way out.
    destroy_temp_graph(/*defer_delete=*/false);

    deinit_subjects();

    // Expire all outstanding async callback tokens before destroying resources
    lifetime_.invalidate();

    // Cancel pending deferred G-code load timer
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }
    cancel_preparing_show_timer();

    // ObserverGuard handles observer cleanup automatically
    resize_registered_ = false;

    // Clean up temp G-code file if any
    if (!temp_gcode_path_.empty()) {
        std::remove(temp_gcode_path_.c_str());
        temp_gcode_path_.clear();
    }

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
    UI_MANAGED_SUBJECT_STRING(layer_text_subject_, layer_text_buf_, "Layer 0 / 0",
                              "print_layer_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(filament_used_text_subject_, filament_used_text_buf_, "",
                              "print_filament_used_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(elapsed_subject_, elapsed_buf_, "0h 00m", "print_elapsed", subjects_);
    UI_MANAGED_SUBJECT_STRING(remaining_subject_, remaining_buf_, "0h 00m", "print_remaining",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(eta_subject_, eta_buf_, "", "print_eta", subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_status_subject_, nozzle_status_buf_, "Off",
                              "print_nozzle_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_status_subject_, bed_status_buf_, "Off", "print_bed_status",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(chamber_status_subject_, chamber_status_buf_, "",
                              "print_chamber_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(speed_subject_, speed_buf_, "100%", "print_speed_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(flow_subject_, flow_buf_, "100%", "print_flow_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(objects_text_subject_, objects_text_buf_, "", "print_objects_text",
                              subjects_);
    // View toggle icon: starts as cube (progress view), flips to layers on complete view.
    // Populated lazily at first update (icon_cube const resolves only after globals load).
    UI_MANAGED_SUBJECT_STRING(view_toggle_icon_subject_, view_toggle_icon_buf_, "",
                              "view_toggle_icon", subjects_);

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
        fans_version_observer_ = observe_int_sync<PrintStatusPanel>(
            printer_state_.get_fans_version_subject(), this,
            [token](PrintStatusPanel* self, int /*v*/) {
                if (token.expired())
                    return;
                self->bind_fan_observers();
            },
            printer_state_.get_subjects_lifetime());
    }
    {
        auto token = lifetime_.token();
        primary_fans_version_observer_ = observe_int_sync<PrintStatusPanel>(
            printer_state_.get_primary_fans_version_subject(), this,
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
            breakpoint_observer_ =
                observe_int_sync<PrintStatusPanel>(bp, this, [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->recompute_fans_density();
                    self->recompute_fans_fit();
                });
        }
    }

    // Density + fit recompute when filament sensor count changes
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "filament_sensor_count");
        if (s) {
            auto token = lifetime_.token();
            filament_sensor_count_observer_ =
                observe_int_sync<PrintStatusPanel>(s, this, [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->recompute_fans_density();
                    self->recompute_fans_fit();
                });
        }
    }

    // Density + fit recompute when AMS slot count changes
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "ams_slot_count");
        if (s) {
            auto token = lifetime_.token();
            ams_slot_count_observer_ =
                observe_int_sync<PrintStatusPanel>(s, this, [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->recompute_fans_density();
                    self->recompute_fans_fit();
                });
        }
    }

    // Density + fit recompute when toolchange panel appears/disappears
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "toolchange_visible");
        if (s) {
            auto token = lifetime_.token();
            toolchange_visible_observer_ =
                observe_int_sync<PrintStatusPanel>(s, this, [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->recompute_fans_density();
                    self->recompute_fans_fit();
                });
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
            scoped_runout_observer_ =
                observe_int_sync<PrintStatusPanel>(s, this, [token](PrintStatusPanel* self, int) {
                    if (token.expired())
                        return;
                    self->recompute_scoped_runout();
                });
        }
    }
    {
        lv_subject_t* s = AmsState::instance().get_slots_version_subject();
        if (s) {
            auto token = lifetime_.token();
            scoped_runout_slots_observer_ = observe_int_sync<PrintStatusPanel>(
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
                    self->build_and_apply_tool_colors();
                },
                AmsState::instance().get_subjects_lifetime());
        }
    }

    // Animation-settings refresh
    animations_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
    {
        auto token = lifetime_.token();
        animations_enabled_observer_ = observe_int_sync<PrintStatusPanel>(
            DisplaySettingsManager::instance().subject_animations_enabled(), this,
            [token](PrintStatusPanel* self, int enabled) {
                if (token.expired())
                    return;
                self->animations_enabled_ = (enabled != 0);
                self->refresh_fan_animations();
            },
            DisplaySettingsManager::instance().get_subjects_lifetime());
    }

    end_overlay_dismissed_observer_ = observe_int_sync<PrintStatusPanel>(
        &end_overlay_dismissed_subject_, this,
        [](PrintStatusPanel* self, int) { self->recompute_end_overlay_visibility(); });

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
    print_message_observer_ = observe_string<PrintStatusPanel>(
        printer_state_.get_print_message_subject(), this,
        [](PrintStatusPanel* self, const char*) { self->recompute_paused_overlay_visibility(); },
        printer_state_.get_subjects_lifetime());

    // Re-evaluate the paused overlay whenever the shared controller's pending
    // action flips (optimistic Pausing/Resuming) — decoupled from our own
    // print_state_enum observer to avoid an ordering race between the two.
    pending_action_observer_ = observe_int_sync<PrintStatusPanel>(
        helix::ui::PrintControlButtons::instance().pending_action_subject(), this,
        [](PrintStatusPanel* self, int) { self->recompute_paused_overlay_visibility(); },
        helix::ui::PrintControlButtons::instance().get_subjects_lifetime());

    // Button enable states driven declaratively from XML (see update_button_states).
    UI_MANAGED_SUBJECT_INT(print_controls_enabled_subject_, 0, "print_controls_enabled", subjects_);

    // Exclude objects availability (0=hidden, 1=visible - shown when >= 2 objects defined)
    // Note: subject already initialized in constructor (needed before observer fires)
    lv_xml_register_subject(nullptr, "exclude_objects_available",
                            &exclude_objects_available_subject_);
    subjects_.register_subject(&exclude_objects_available_subject_);
    SubjectDebugRegistry::instance().register_subject(&exclude_objects_available_subject_,
                                                      "exclude_objects_available",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    // Register XML event callbacks for print status panel buttons
    // (tune overlay subjects/callbacks registered by singleton on first show())
    // (light and timelapse callbacks are registered by light_timelapse_controls_.init_subjects())
    register_xml_callbacks({
        {"on_print_status_tune", on_tune_clicked},
        {"on_print_status_reprint", on_reprint_clicked},
        {"on_temp_card_clicked", on_temp_card_clicked},
        {"on_print_status_graph_clicked", on_temp_graph_clicked},
        {"on_print_status_objects", on_objects_clicked},
        {"on_view_toggle", on_view_toggle_clicked},
        {"on_print_status_dismiss_overlay", on_dismiss_overlay_clicked},
        {"on_print_status_fans_clicked", on_fans_clicked},
    });

    subjects_initialized_ = true;

    // Initial sync of the paused overlay — observers only fire on CHANGE, so
    // a mid-print attach where state is already PAUSED would leave the badge
    // hidden without this explicit recompute.
    recompute_paused_overlay_visibility();

    // Sync initial state from PrinterState (in case app opens while print is in progress)
    // This is necessary because observers only fire on VALUE CHANGE, not on subscribe.
    int initial_progress = lv_subject_get_int(printer_state_.get_print_progress_subject());
    int initial_layer = lv_subject_get_int(printer_state_.get_print_layer_current_subject());
    int initial_total_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject());
    if (initial_progress > 0 || initial_layer > 0 || initial_total_layers > 0) {
        lifecycle_.on_progress_changed(initial_progress);
        lifecycle_.on_layer_changed(initial_layer, initial_total_layers,
                                    printer_state_.has_real_layer_data());
        update_all_displays();
        spdlog::debug("[{}] Synced initial print state: progress={}%, layer={}/{}", get_name(),
                      initial_progress, initial_layer, initial_total_layers);
    }

    // Sync initial preparation state from PrinterState (in case panel opens mid-preparation)
    int initial_phase = lv_subject_get_int(printer_state_.get_print_start_phase_subject());
    if (initial_phase != 0) {
        on_print_start_phase_changed(initial_phase);
        int prog = lv_subject_get_int(printer_state_.get_print_start_progress_subject());
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
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, get_xml_component_name(), nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Setting up panel...", get_name());

    // Width comes from NavigationManager::push_overlay() — this panel declares
    // is_destination() so it renders full width from every entry point (#1178).
    // Use standard overlay panel setup for header/content/back button
    ui_overlay_panel_setup_standard(overlay_root_, parent_screen_, "overlay_header",
                                    "overlay_content");

    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] overlay_content not found!", get_name());
        return nullptr;
    }

    // Find thumbnail section for nested widgets
    lv_obj_t* thumbnail_section = lv_obj_find_by_name(overlay_content, "thumbnail_section");
    if (!thumbnail_section) {
        spdlog::error("[{}] thumbnail_section not found!", get_name());
        return nullptr;
    }

    // Find G-code viewer, thumbnail, and gradient background widgets
    gcode_viewer_ = lv_obj_find_by_name(thumbnail_section, "print_gcode_viewer");
    print_thumbnail_ = lv_obj_find_by_name(thumbnail_section, "print_thumbnail");
    gradient_background_ = lv_obj_find_by_name(thumbnail_section, "gradient_background");

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
            gcode_viewer_, lv_obj_find_by_name(thumbnail_section, "metadata_clip"));

        // Memory-pressure responder calls ui_gcode_viewer_clear_all_active().
        // Flip our mode subject back to thumbnail (0) so the user sees the
        // slicer preview rather than a transparent rectangle.
        ui_gcode_viewer_set_clear_callback(
            gcode_viewer_,
            [](lv_obj_t*, void* ud) {
                auto* panel = static_cast<PrintStatusPanel*>(ud);
                panel->show_gcode_viewer(false);
                panel->lifecycle_.set_gcode_loaded(false);
                // Viewer geometry is gone; clear the GCODE marker so the next
                // ensure_preview_current() reloads it. The thumbnail (fallback)
                // survives, so its marker is left intact.
                panel->gcode_displayed_file_.clear();
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
    btn_timelapse_ = lv_obj_find_by_name(overlay_content, "btn_timelapse");
    btn_tune_ = lv_obj_find_by_name(overlay_content, "btn_tune");
    btn_cancel_ = lv_obj_find_by_name(overlay_content, "btn_cancel");

    // Print complete celebration badge (for animation)
    success_badge_ = lv_obj_find_by_name(overlay_content, "success_badge");
    if (success_badge_) {
        spdlog::debug("[{}]   ✓ Success badge", get_name());
    }

    // Print cancelled badge (for animation)
    cancel_badge_ = lv_obj_find_by_name(overlay_content, "cancel_badge");
    if (cancel_badge_) {
        spdlog::debug("[{}]   ✓ Cancel badge", get_name());
    }

    // Print error badge (for animation)
    error_badge_ = lv_obj_find_by_name(overlay_content, "error_badge");
    if (error_badge_) {
        spdlog::debug("[{}]   ✓ Error badge", get_name());
    }

    // Progress bar widget
    progress_bar_ = lv_obj_find_by_name(overlay_content, "print_progress");
    if (progress_bar_) {
        lv_bar_set_range(progress_bar_, 0, 100);
        // WORKAROUND: LVGL bar has a bug where setting value=0 when cur_value=0
        // causes early return without proper layout update, showing full bar.
        // Force update by setting to 1 first, then 0.
        lv_bar_set_value(progress_bar_, 1, LV_ANIM_OFF);
        lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        spdlog::debug("[{}]   ✓ Progress bar", get_name());
    } else {
        spdlog::error("[{}]   ✗ Progress bar NOT FOUND", get_name());
    }

    // Preparing progress bar (shown during pre-print operations)
    preparing_progress_bar_ = lv_obj_find_by_name(overlay_content, "preparing_progress_bar");
    if (preparing_progress_bar_) {
        lv_bar_set_range(preparing_progress_bar_, 0, 100);
        lv_bar_set_value(preparing_progress_bar_, 0, LV_ANIM_OFF);
        spdlog::debug("[{}]   ✓ Preparing progress bar", get_name());
    }

    // AMS current tool indicator (auto-hides when no AMS or no tool active)
    lv_obj_t* ams_indicator = lv_obj_find_by_name(overlay_content, "ams_current_tool_indicator");
    if (ams_indicator) {
        ui_ams_current_tool_setup(ams_indicator);
        spdlog::debug("[{}]   ✓ AMS current tool indicator", get_name());
    }

    // Check if --gcode-file was specified on command line for this panel
    const auto* config = get_runtime_config();
    if (config->gcode_test_file && gcode_viewer_) {
        // Check file size and memory safety before loading
        // Use 2D streaming check since that's the mode used on memory-constrained devices
        std::ifstream file(config->gcode_test_file, std::ios::binary | std::ios::ate);
        if (file) {
            size_t file_size = static_cast<size_t>(file.tellg());
            if (helix::is_gcode_2d_streaming_safe(file_size)) {
                spdlog::info("[{}] Loading G-code file from command line: {}", get_name(),
                             config->gcode_test_file);
                load_gcode_file(config->gcode_test_file);
            } else {
                spdlog::warn("[{}] G-code file too large for 2D streaming: {} ({} bytes) - using "
                             "thumbnail only",
                             get_name(), config->gcode_test_file, file_size);
            }
        }
    }

    // Restore cached thumbnail if a print was already in progress before panel was displayed
    // This handles the case where a print was started from Mainsail while on the Home panel
#if defined(HELIX_PLATFORM_ESP32)
    // cached_thumbnail_path_ is always empty here (no disk cache) — restore from
    // the PSRAM buffer PrinterState is holding instead.
    apply_esp_psram_thumbnail();
#else
    if (print_thumbnail_ && !cached_thumbnail_path_.empty()) {
        lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
        spdlog::info("[{}] Restored cached thumbnail: {}", get_name(), cached_thumbnail_path_);
    }
#endif

    // Register plugin injection point for print status widgets
    lv_obj_t* extras_container = lv_obj_find_by_name(overlay_root_, "print_status_extras");
    if (extras_container) {
        helix::plugin::InjectionPointManager::instance().register_point("print_status_extras",
                                                                        extras_container);
        spdlog::debug("[{}] Registered injection point: print_status_extras", get_name());
    }

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
    // here — SIZE_CHANGED has no XML binding equivalent (pattern from
    // ui_buffer_meter.cpp:52 and ui_ams_mini_status.cpp:540).
    lv_obj_t* controls_section = lv_obj_find_by_name(overlay_root_, "controls_section");
    if (controls_section) {
        lv_obj_add_event_cb(controls_section, on_controls_size_changed, LV_EVENT_SIZE_CHANGED,
                            this);
        spdlog::debug("[{}] Registered SIZE_CHANGED on controls_section", get_name());
    } else {
        spdlog::warn("[{}] controls_section not found — SIZE_CHANGED not wired", get_name());
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
    int state_enum = lv_subject_get_int(printer_state_.get_print_state_enum_subject());
    spdlog::debug("[{}] on_activate() print_state_enum={}", get_name(), state_enum);

    // Reconcile the preview against the current print state. This single
    // idempotent entry point replaces the former scatter of conditional reload
    // blocks (deferred-gcode kick, want_viewer/!gcode_loaded re-feed, cached
    // thumbnail re-apply). It reads the ACTUAL widget state, so a blank or
    // recreated widget always reloads — re-entry after destroy-on-close or a
    // memory-reclaim cycle is self-healing.
    crash_handler::breadcrumb::note("pstat_act", "ensure_preview");
    ensure_preview_current();

    // Sync button enabled/visibility state with current print state and outcome.
    // XML bindings may have been lost during overlay lifecycle transitions (#546).
    crash_handler::breadcrumb::note("pstat_act", "btn_states");
    update_button_states();

    // Restore render mode from settings before showing the viewer.
    // The render mode observer only fires when is_active_, so settings
    // changed while the panel was hidden must be applied here.
    int render_mode_val = DisplaySettingsManager::instance().get_gcode_render_mode();
    bool thumbnail_only = (render_mode_val == 3);
    if (gcode_viewer_ && !thumbnail_only) {
        auto render_mode = static_cast<GcodeViewerRenderMode>(render_mode_val);
        ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
    }

    // Restore G-code viewer state based on current print conditions.
    // Thumbnail Only mode forces the viewer off regardless of gcode state.
    bool gcode_present = gcode_viewer_ && ui_gcode_viewer_has_content(gcode_viewer_);
    bool show_viewer = !thumbnail_only && lifecycle_.want_viewer() && gcode_present;
    crash_handler::breadcrumb::note("pstat_act", show_viewer ? "viewer_on" : "viewer_off");
    show_gcode_viewer(show_viewer);

    // Sync gcode viewer to current print layer (may have advanced while panel was hidden)
    if (gcode_present && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN)) {
        int current_layer = lv_subject_get_int(printer_state_.get_print_layer_current_subject());
        int total_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject());
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
            recompute_fans_density();
            recompute_fans_fit();
        });
    }

    // Resume the mini-graph and pull in whatever landed while we were off-screen.
    // resume() backfills, so the trace is continuous rather than starting a fresh
    // segment at re-entry.
    if (temp_graph_controller_) {
        temp_graph_controller_->resume();
    }

    crash_handler::breadcrumb::note("pstat_act", "exit");
}

void PrintStatusPanel::on_deactivate() {
    OverlayBase::on_deactivate(); // Sets visible_ = false
    is_active_ = false;
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Cancel pending deferred G-code load (panel is no longer visible)
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

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
            // Viewer geometry is gone; drop the GCODE marker so the next
            // ensure_preview_current() (on re-activation) reloads it. The
            // thumbnail survives, so its marker is left intact.
            gcode_displayed_file_.clear();
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
    if (temp_graph_controller_) {
        temp_graph_controller_->pause();
    }
}

void PrintStatusPanel::cleanup() {
    // Cancel pending deferred G-code load
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }
    cancel_preparing_show_timer();

    OverlayBase::cleanup(); // Sets cleanup_called_ = true
}

void PrintStatusPanel::on_ui_destroyed() {
    spdlog::debug("[{}] on_ui_destroyed() - nulling widget pointers", get_name());

    // Cancel pending deferred G-code load
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

    // Note: LVGL animations are already cancelled by lv_obj_delete() in the base
    // class destroy_overlay_ui() call, so no need to cancel them here.

    // Clean up map view + side list before the widget tree is gone
    side_list_.reset();
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }

    // Deinit exclude manager (holds gcode_viewer_ reference)
    if (exclude_manager_) {
        exclude_manager_->deinit();
        exclude_manager_.reset();
    }

    // Tear the mini-graph down while its container is still addressable, and
    // drop the fit decision with it: a rebuilt tree starts with no controller,
    // so leaving the subject at 1 would un-hide an empty container until the
    // first post-activate recompute.
    destroy_temp_graph();
    preview_slack_h_ = 0;
    if (subjects_initialized_) {
        lv_subject_set_int(&graph_fits_subject_, 0);
    }

    // Null all child widget pointers (widget tree is already deleted by base class)
    progress_bar_ = nullptr;
    preparing_progress_bar_ = nullptr;
    gcode_viewer_ = nullptr;
    print_thumbnail_ = nullptr;
    gradient_background_ = nullptr;
    btn_timelapse_ = nullptr;
    btn_tune_ = nullptr;
    btn_cancel_ = nullptr;
    // Lazy fan control overlay — force re-creation on next click so we don't
    // dereference a pointer into a destroyed widget tree (mirrors FanStackWidget::detach).
    fan_control_panel_ = nullptr;
    success_badge_ = nullptr;
    cancel_badge_ = nullptr;
    error_badge_ = nullptr;

    // Heater icon animators — at this point the widget tree is only hidden
    // and reparented to the top layer (destroy_overlay_ui() defers the actual
    // deletion to the next tick, see overlay_base.h), so the icons are still
    // valid and the binders are still bound. Unbind explicitly here rather
    // than relying on the eventual deferred LV_EVENT_DELETE.
    nozzle_icon_binder_.unbind();
    bed_icon_binder_.unbind();
    chamber_icon_binder_.unbind();

    // Reset widget-dependent state
    resize_registered_ = false;
    is_active_ = false;
    lifecycle_.set_gcode_loaded(false);
    complete_view_mode_ = false;
    // The widget tree is gone, so nothing is displayed. Clearing both markers
    // forces ensure_preview_current() to reload thumbnail + gcode on next open
    // after destroy-on-close. pending_gcode_filename_ is a stale timer payload
    // for a now-deleted viewer — drop it too.
    displayed_file_.clear();
    gcode_displayed_file_.clear();
    pending_gcode_filename_.clear();
}

lv_obj_t* PrintStatusPanel::get_cached_overlay() {
    return s_cached_panel;
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
        NavigationManager::instance().register_overlay_instance(s_cached_panel, &panel, true);

        // Decide whether to destroy the widget tree when the overlay closes.
        // On memory-constrained devices or when currently under pressure, destroy
        // on close to free ~400-800KB. On devices with plenty of available RAM,
        // keep the widget tree alive so re-opening is instant — no thumbnail→3D
        // rebuild jump (issue #618).
        auto mem = helix::get_system_memory_info();
        bool should_destroy = mem.is_low_memory();
        if (should_destroy) {
            NavigationManager::instance().register_overlay_close_callback(s_cached_panel, []() {
                auto& p = get_global_print_status_panel();
                p.destroy_overlay_ui(s_cached_panel);
            });
            spdlog::info("[PrintStatusPanel] Print status overlay created (destroy-on-close, "
                         "{}MB available, {}MB total)",
                         mem.available_mb(), mem.total_mb());
        } else {
            spdlog::info("[PrintStatusPanel] Print status overlay created (persistent, "
                         "{}MB available, {}MB total)",
                         mem.available_mb(), mem.total_mb());
        }

        // Register pressure responder once. The persistent branch above keeps the
        // widget tree alive across overlay closes to avoid the thumbnail→3D
        // rebuild jump — but that decision assumed plenty of RAM at startup.
        // If pressure builds up later (slow leak, second connection, heavy file
        // selection), drop the cached tree to reclaim memory even in persistent
        // mode. No-op if the overlay is currently visible.
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

    NavigationManager::instance().push_overlay(s_cached_panel);
    return true;
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void PrintStatusPanel::format_time(int seconds, char* buf, size_t buf_size) {
    std::string formatted = helix::format::duration_padded(seconds);
    std::snprintf(buf, buf_size, "%s", formatted.c_str());
}

void PrintStatusPanel::cleanup_temp_gcode() {
    if (!temp_gcode_path_.empty()) {
        if (std::remove(temp_gcode_path_.c_str()) == 0) {
            spdlog::debug("[{}] Cleaned up temp G-code file: {}", get_name(), temp_gcode_path_);
        } else {
            spdlog::trace("[{}] Temp G-code file already removed: {}", get_name(),
                          temp_gcode_path_);
        }
        temp_gcode_path_.clear();
    }
}

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
    // During async gcode reload the gradient covers the area — the user should
    // at least see the cached thumbnail underneath.
#if defined(HELIX_PLATFORM_ESP32)
    if (mode == 0 && print_thumbnail_ && esp_thumbnail_) {
        const void* current_src = lv_image_get_src(print_thumbnail_);
        if (!current_src) {
            lv_image_set_src(print_thumbnail_, esp_thumbnail_->dsc());
        }
    }
#else
    if (mode == 0 && print_thumbnail_ && !cached_thumbnail_path_.empty()) {
        const void* current_src = lv_image_get_src(print_thumbnail_);
        if (!current_src) {
            lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
        }
    }
#endif

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
    if (!exclude_manager_)
        return;

    // Map (when in thumbnail mode) parents into thumbnail_section as before;
    // the side list parents into overlay_content (the two-column row) as a
    // FLOATING child so it can slide in from the screen's right edge over the
    // controls column without disturbing flex layout. Map and gcode viewer
    // keep their full size — the list lands on top of the controls.
    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!overlay_content) {
        spdlog::warn("[{}] Cannot show exclude panel: overlay_content not found", get_name());
        return;
    }
    lv_obj_t* thumbnail_section = lv_obj_find_by_name(overlay_content, "thumbnail_section");

    int viewer_mode = lv_subject_get_int(&gcode_viewer_mode_subject_);
    bool thumbnail_mode = (viewer_mode == 0);

    if (thumbnail_mode && thumbnail_section) {
        // Bed dimensions for the overhead map view (thumbnail mode only).
        float bed_w = 235.0f, bed_h = 235.0f;
        if (api_) {
            const auto& vol = api_->hardware().build_volume();
            float w = vol.x_max - vol.x_min;
            float h = vol.y_max - vol.y_min;
            if (w > 0.0f && h > 0.0f) {
                bed_w = w;
                bed_h = h;
            }
        }

        // XML bindings on print_thumbnail/gradient_background hide them when
        // exclude_map_active == 1 — set before creating the map to avoid one
        // frame with the overlay atop still-visible thumbnail/gradient.
        lv_subject_set_int(&exclude_map_active_subject_, 1);

        map_view_ = std::make_unique<helix::ui::ExcludeObjectMapView>();
        map_view_->set_close_callback([this]() { hide_exclude_map_view(); });

        std::shared_ptr<helix::gcode::ParsedGCodeFile> parsed;
        if (gcode_viewer_) {
            const auto* raw = ui_gcode_viewer_get_parsed_file(gcode_viewer_);
            if (raw) {
                parsed = std::shared_ptr<helix::gcode::ParsedGCodeFile>(
                    const_cast<helix::gcode::ParsedGCodeFile*>(raw),
                    [](helix::gcode::ParsedGCodeFile*) {});
            }
        }

        map_view_->create(thumbnail_section, printer_state_.get_excluded_objects_state(), bed_w,
                          bed_h, exclude_manager_.get(), parsed);

        // The side list's X already closes the whole panel — hide the map's
        // duplicate close button so users have one obvious dismiss control.
        if (auto* map_root = map_view_->root()) {
            if (lv_obj_t* map_close = lv_obj_find_by_name(map_root, "close_btn")) {
                lv_obj_add_flag(map_close, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Which edge the list covers depends on where the controls are: the right
    // column in landscape, the bottom of the stack in portrait. Landscape is
    // exact from the flex_grow ratio and needs no measurement; portrait sizes
    // the list to the control stack it has to cover, so measure it here — the
    // panel is already laid out by the time the map view opens. See
    // helix::ui::exclude_side_list_geometry().
    const bool portrait = helix::is_portrait_layout(helix::LayoutManager::instance().type());
    int32_t controls_h = 0;
    int32_t content_h = 0;
    int32_t list_gap = 0;
    if (portrait) {
        lv_obj_update_layout(overlay_content);
        if (lv_obj_t* controls = lv_obj_find_by_name(overlay_content, "controls_section")) {
            controls_h = lv_obj_get_height(controls);
        }
        content_h = lv_obj_get_content_height(overlay_content);
        list_gap = lv_obj_get_style_pad_row(overlay_content, LV_PART_MAIN);
    }
    const auto list_geom =
        helix::ui::exclude_side_list_geometry(portrait, controls_h, content_h, list_gap);

    side_list_ = std::make_unique<helix::ui::ExcludeObjectSideList>();
    side_list_->set_close_callback([this]() { hide_exclude_map_view(); });
    side_list_->set_gcode_viewer(gcode_viewer_);
    side_list_->create(overlay_content, &printer_state_, exclude_manager_.get(), list_geom);

    // Tapping an object in the viewer should request exclude, mirroring the
    // side list's row taps. Installed regardless of current view mode so that
    // switching from thumbnail → 2D/3D while the side list is open still wires
    // up taps. Uninstalled in hide_exclude_map_view().
    if (gcode_viewer_) {
        ui_gcode_viewer_set_object_tap_callback(
            gcode_viewer_,
            [](lv_obj_t* /*viewer*/, const char* name, void* /*user_data*/) {
                if (!name || name[0] == '\0') {
                    return;
                }
                auto& panel = get_global_print_status_panel();
                if (panel.exclude_manager_) {
                    spdlog::info("[PrintStatusPanel] Viewer tap on object: '{}'", name);
                    panel.exclude_manager_->request_exclude(std::string(name));
                }
            },
            nullptr);
    }

    spdlog::debug("[{}] Showed exclude panel (mode={})", get_name(), viewer_mode);
}

void PrintStatusPanel::hide_exclude_map_view() {
    // Detach the viewer tap-to-exclude wire-up and clear any lingering
    // highlight from row-tap symmetry.
    if (gcode_viewer_) {
        ui_gcode_viewer_set_object_tap_callback(gcode_viewer_, nullptr, nullptr);
        ui_gcode_viewer_set_highlighted_objects(gcode_viewer_, {});
    }

    if (side_list_) {
        side_list_->destroy();
        side_list_.reset();
    }
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }
    // Un-hides thumbnail/gradient via the XML bindings on exclude_map_active.
    lv_subject_set_int(&exclude_map_active_subject_, 0);
}

void PrintStatusPanel::load_gcode_file(const char* file_path) {
    if (!gcode_viewer_ || !file_path) {
        spdlog::warn("[{}] Cannot load G-code: viewer={}, path={}", get_name(),
                     gcode_viewer_ != nullptr, file_path != nullptr);
        return;
    }

    spdlog::debug("[{}] Loading G-code file: {}", get_name(), file_path);

    // Register callback to be notified when loading completes
    ui_gcode_viewer_set_load_callback(
        gcode_viewer_,
        [](lv_obj_t* viewer, void* user_data, bool success) {
            auto* self = static_cast<PrintStatusPanel*>(user_data);
            if (!success) {
                spdlog::error("[{}] G-code load failed", self->get_name());
                self->lifecycle_.set_gcode_loaded(false);
                return;
            }

            // Get layer count from loaded geometry
            int max_layer = ui_gcode_viewer_get_max_layer(viewer);
            if (max_layer >= 0)
                spdlog::debug("[{}] G-code loaded: {} layers", self->get_name(), max_layer);
            else
                spdlog::debug("[{}] G-code loaded (renderer pending)", self->get_name());

            // Mark G-code as successfully loaded (enables viewer mode on state changes)
            self->lifecycle_.set_gcode_loaded(true);
            // The viewer now holds the current print's geometry. Record the
            // effective filename as the GCODE marker so ensure_preview_current()
            // treats the viewer as current on re-entry. (The thumbnail marker is
            // recorded independently by the thumbnail path.)
            self->gcode_displayed_file_ = self->printer_state_.get_effective_print_filename();

            // Override extrusion colors with AMS filament colors.
            // For multi-tool prints, applies per-tool AMS slot colors.
            // For single-tool, falls back to current AMS color subject.
            self->build_and_apply_tool_colors();

            // The parsed file now carries the tools this print uses — refresh the
            // print-scoped runout badge (FIX B) so it reflects only those tools.
            self->recompute_scoped_runout();

            // Show viewer if print is active or in terminal state (user can see
            // where print stopped). Only skip in Idle.
            if (self->lifecycle_.want_viewer()) {
                self->show_gcode_viewer(true);
            }

            // Force layout recalculation now that viewer is visible
            lv_obj_update_layout(viewer);
            // Reset camera to fit model to new viewport dimensions
            ui_gcode_viewer_reset_camera(viewer);

            // Set print progress to current layer (not 0!) when joining a print in progress.
            // Read directly from PrinterState subjects to get the latest values.
            int viewer_max_layer = ui_gcode_viewer_get_max_layer(viewer);
            int current_layer =
                lv_subject_get_int(self->printer_state_.get_print_layer_current_subject());
            int total_layers =
                lv_subject_get_int(self->printer_state_.get_print_layer_total_subject());

            // Fallback: if Moonraker metadata didn't provide layer count,
            // use the count from the parsed/indexed gcode file
            if (total_layers == 0 && viewer_max_layer > 0) {
                int layer_count = viewer_max_layer + 1; // max_layer is 0-based
                self->printer_state_.set_print_layer_total(layer_count);
                spdlog::info("[{}] Set total layers from gcode viewer: {}", self->get_name(),
                             layer_count);
            }

            // Update lifecycle state while we're at it
            self->lifecycle_.on_layer_changed(current_layer, total_layers,
                                              self->printer_state_.has_real_layer_data());

            // Map from Moonraker layer count to viewer layer count
            // Note: viewer_max_layer may be -1 if 2D renderer not yet initialized (lazy init)
            int viewer_layer = 0;
            if (viewer_max_layer > 0 && total_layers > 0) {
                viewer_layer = (current_layer * viewer_max_layer) / total_layers;
            } else if (viewer_max_layer <= 0 && current_layer > 0) {
                // 2D renderer not ready yet - use raw current layer, will be corrected later
                // The 2D renderer will use this value when it initializes on first render
                viewer_layer = current_layer;
            }

            // CRITICAL: Defer to avoid lv_obj_invalidate() during render phase
            // This callback runs during lv_timer_handler() which may be mid-render
            struct ViewerProgressCtx {
                lv_obj_t* viewer;
                int layer;
            };
            auto ctx = std::make_unique<ViewerProgressCtx>(ViewerProgressCtx{viewer, viewer_layer});
            helix::ui::queue_update<ViewerProgressCtx>(std::move(ctx), [](ViewerProgressCtx* c) {
                if (c->viewer && lv_obj_is_valid(c->viewer)) {
                    ui_gcode_viewer_set_print_progress(c->viewer, c->layer);
                }
            });

            spdlog::debug("[{}] G-code loaded: initial layer progress set to {} "
                          "(current={}/{}, viewer_max={})",
                          self->get_name(), viewer_layer, current_layer, total_layers,
                          viewer_max_layer);

            // NOTE: PrintStatusPanel does NOT start prints - it only VIEWS them.
            // Prints are started from PrintSelectPanel via the Print button.
            // This callback is for loading G-code into the viewer for visualization only.
            spdlog::debug("[{}] G-code loaded for viewing: {}", self->get_name(),
                          ui_gcode_viewer_get_filename(viewer));
        },
        this);

    // Start loading the file
    ui_gcode_viewer_load_file(gcode_viewer_, file_path);
}

void PrintStatusPanel::update_layer_text() {
    std::string text = helix::ui::format_layer_progress_compact(
        lifecycle_.current_layer(), lifecycle_.total_layers(), printer_state_.layer_is_accurate(),
        lv_subject_get_int(printer_state_.get_gcode_position_z_subject()));
    std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), "%s", text.c_str());
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);
}

void PrintStatusPanel::update_filament_used_text() {
    int filament_mm = lv_subject_get_int(get_printer_state().get_print_filament_used_subject());
    if (filament_mm > 0) {
        std::string fil_str =
            helix::format::format_filament_length(static_cast<double>(filament_mm));
        std::strncpy(filament_used_text_buf_, fil_str.c_str(), sizeof(filament_used_text_buf_) - 1);
        filament_used_text_buf_[sizeof(filament_used_text_buf_) - 1] = '\0';
    } else {
        filament_used_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&filament_used_text_subject_, filament_used_text_buf_);
}

void PrintStatusPanel::update_all_displays() {
    // Guard: don't update if subjects aren't initialized yet
    if (!subjects_initialized_) {
        return;
    }

    // Progress text

    update_layer_text();

    // Filament used text
    update_filament_used_text();

    // Time displays - Preparing: preprint observers own these.
    // Complete: on_print_state_changed sets frozen final values, don't overwrite.
    if (lifecycle_.state() != PrintState::Preparing && lifecycle_.state() != PrintState::Complete) {
        // elapsed_seconds is wall-clock time from Moonraker total_duration (includes prep)
        format_time(lifecycle_.elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
        lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);

        format_time(lifecycle_.remaining_seconds(), remaining_buf_, sizeof(remaining_buf_));
        lv_subject_copy_string(&remaining_subject_, remaining_buf_);
    }

    // Heater status text (Off / Heating... / Ready)
    auto nozzle_heater = helix::ui::temperature::heater_display(lifecycle_.nozzle_current(),
                                                                lifecycle_.nozzle_target());
    std::snprintf(nozzle_status_buf_, sizeof(nozzle_status_buf_), "%s",
                  nozzle_heater.status.c_str());
    lv_subject_copy_string(&nozzle_status_subject_, nozzle_status_buf_);

    auto bed_heater =
        helix::ui::temperature::heater_display(lifecycle_.bed_current(), lifecycle_.bed_target());
    std::snprintf(bed_status_buf_, sizeof(bed_status_buf_), "%s", bed_heater.status.c_str());
    lv_subject_copy_string(&bed_status_subject_, bed_status_buf_);

    // Speeds
    helix::format::format_percent(lifecycle_.speed_percent(), speed_buf_, sizeof(speed_buf_));
    lv_subject_copy_string(&speed_subject_, speed_buf_);

    helix::format::format_percent(lifecycle_.flow_percent(), flow_buf_, sizeof(flow_buf_));
    lv_subject_copy_string(&flow_subject_, flow_buf_);

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

void PrintStatusPanel::handle_reprint_button() {
    // Startup grace period: reject phantom clicks during early boot
    auto elapsed = std::chrono::steady_clock::now() - AppConstants::Startup::PROCESS_START_TIME;
    if (elapsed < AppConstants::Startup::PRINT_START_GRACE_PERIOD) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::warn("[{}] Rejected reprint during startup grace period ({}s < {}s)", get_name(),
                     secs, AppConstants::Startup::PRINT_START_GRACE_PERIOD.count());
        return;
    }

    spdlog::info("[{}] Reprint button clicked - reprinting: {}", get_name(),
                 current_print_filename_);

    if (current_print_filename_.empty()) {
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

    std::string filename = current_print_filename_;

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
        api_->job().start_print(
            filename,
            [this, filename]() { spdlog::info("[{}] Reprint started: {}", get_name(), filename); },
            [this, token = lifetime_.token()](const MoonrakerError& err) {
                // Runs on libhv WS event loop — marshal LVGL work to main.
                token.defer("PrintStatusPanel::reprint_err", [this, err]() {
                    spdlog::error("[{}] Failed to reprint: {}", get_name(), err.message);
                    NOTIFY_ERROR(lv_tr("Failed to reprint: {}"), err.user_message());
                    ui_set_button_enabled(btn_cancel_, true);
                });
            });
    }
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
    auto state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
    if (!helix::print_scopes_runout_badge(state)) {
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
// STATIC TRAMPOLINES
// ============================================================================

void PrintStatusPanel::on_temp_card_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_temp_card_clicked");
    (void)e;
    get_global_print_status_panel().handle_temp_card_click();
    LVGL_SAFE_EVENT_CB_END();
}

// The mini-graph is a summary; the full overlay is the detail view. Tapping it
// opens exactly what tapping the temp chips opens, so both entry points land on
// one code path rather than two that can drift.
void PrintStatusPanel::on_temp_graph_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_temp_graph_clicked");
    (void)e;
    get_global_print_status_panel().handle_temp_card_click();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_dismiss_overlay_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_dismiss_overlay_clicked");
    (void)e;
    // XML binding on each overlay hides when end_overlay_dismissed == 1.
    lv_subject_set_int(&get_global_print_status_panel().end_overlay_dismissed_subject_, 1);
    spdlog::debug("[PrintStatusPanel] Dismissed print end overlay");
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_tune_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_tune_clicked");
    (void)e;
    get_global_print_status_panel().handle_tune_button();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_fans_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_fans_clicked");
    (void)e;
    get_global_print_status_panel().handle_fans_click();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::handle_fans_click() {
    spdlog::debug("[{}] Fans clicked — opening fan control overlay", get_name());

    if (!fan_control_panel_ && parent_screen_) {
        auto& overlay = get_fan_control_overlay();
        if (!overlay.are_subjects_initialized())
            overlay.init_subjects();
        overlay.register_callbacks();
        overlay.set_api(api_);

        fan_control_panel_ = overlay.create(parent_screen_);
        if (!fan_control_panel_) {
            spdlog::error("[{}] Failed to create fan control overlay", get_name());
            return;
        }
        NavigationManager::instance().register_overlay_instance(fan_control_panel_, &overlay);
    }

    if (fan_control_panel_) {
        get_fan_control_overlay().set_api(api_);
        NavigationManager::instance().push_overlay(fan_control_panel_);
    }
}

void PrintStatusPanel::on_reprint_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_reprint_clicked");
    (void)e;
    get_global_print_status_panel().handle_reprint_button();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_objects_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_objects_clicked");
    (void)e;
    auto& panel = get_global_print_status_panel();

    // Toggle the unified exclude panel (map+side-list in thumbnail mode,
    // shrunk-viewer + side-list in 3D/2D mode).
    if (panel.side_list_ && panel.side_list_->is_active()) {
        panel.hide_exclude_map_view();
    } else {
        panel.show_exclude_map_view();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_view_toggle_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_view_toggle_clicked");
    (void)e;
    auto& panel = get_global_print_status_panel();

    panel.complete_view_mode_ = !panel.complete_view_mode_;

    if (panel.complete_view_mode_) {
        // Complete view: show all layers solid (no ghost)
        if (panel.gcode_viewer_) {
            ui_gcode_viewer_set_print_progress(panel.gcode_viewer_, -1);
        }
    } else {
        // Progress view: restore current layer with ghost
        if (panel.gcode_viewer_) {
            int current_layer =
                lv_subject_get_int(panel.printer_state_.get_print_layer_current_subject());
            int total_layers =
                lv_subject_get_int(panel.printer_state_.get_print_layer_total_subject());
            int viewer_max_layer = ui_gcode_viewer_get_max_layer(panel.gcode_viewer_);
            int viewer_layer = current_layer;
            if (total_layers > 0 && viewer_max_layer > 0) {
                viewer_layer = (current_layer * viewer_max_layer) / total_layers;
            }
            ui_gcode_viewer_set_print_progress(panel.gcode_viewer_, viewer_layer);
        }
    }

    const char* icon_text =
        lv_xml_get_const(nullptr, panel.complete_view_mode_ ? "icon_layers" : "icon_cube");
    if (icon_text) {
        lv_subject_copy_string(&panel.view_toggle_icon_subject_, icon_text);
    }

    spdlog::debug("[PrintStatusPanel] View toggle: {}",
                  panel.complete_view_mode_ ? "complete" : "progress");
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_resize_static() {
    // Use global instance for resize callback (registered without user_data)
    if (g_print_status_panel) {
        g_print_status_panel->handle_resize();
    }
}

// ============================================================================
// OBSERVER INSTANCE METHODS
// ============================================================================

void PrintStatusPanel::on_temperature_changed() {
    // Read all temperature values from PrinterState subjects and delegate to lifecycle
    int nz_cur = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int nz_tgt = lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
    int bed_cur = lv_subject_get_int(printer_state_.get_bed_temp_subject());
    int bed_tgt = lv_subject_get_int(printer_state_.get_bed_target_subject());
    lifecycle_.on_temperature_changed(nz_cur, nz_tgt, bed_cur, bed_tgt);

    if (!subjects_initialized_)
        return;

    // Update only temperature-related subjects (not the full display refresh).
    // Temperature observers fire frequently during heating (4 subjects x ~1Hz each),
    // and update_all_displays() re-renders ALL subjects causing visible flickering.
    auto nozzle_heater = helix::ui::temperature::heater_display(lifecycle_.nozzle_current(),
                                                                lifecycle_.nozzle_target());
    std::snprintf(nozzle_status_buf_, sizeof(nozzle_status_buf_), "%s",
                  nozzle_heater.status.c_str());
    lv_subject_copy_string(&nozzle_status_subject_, nozzle_status_buf_);

    auto bed_heater =
        helix::ui::temperature::heater_display(lifecycle_.bed_current(), lifecycle_.bed_target());
    std::snprintf(bed_status_buf_, sizeof(bed_status_buf_), "%s", bed_heater.status.c_str());
    lv_subject_copy_string(&bed_status_subject_, bed_status_buf_);

    spdlog::trace("[{}] Temperatures updated: nozzle {}/{}°C, bed {}/{}°C", get_name(),
                  lifecycle_.nozzle_current(), lifecycle_.nozzle_target(), lifecycle_.bed_current(),
                  lifecycle_.bed_target());
}

void PrintStatusPanel::recompute_end_overlay_visibility() {
    if (!subjects_initialized_)
        return;
    int outcome = lv_subject_get_int(printer_state_.get_print_outcome_subject());
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

    auto primary = printer_state_.get_fan_state().classify_primary_fans();
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
    recompute_aux_composites();

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
    lv_subject_t* subj = printer_state_.get_fan_speed_subject(object_name, lt);
    if (!subj) {
        spdlog::warn("[{}] Fan '{}' subject not available", get_name(), object_name);
        return;
    }

    auto token = lifetime_.token();
    std::string label_copy = speed_label_widget_name;
    std::string icon_copy = icon_widget_name;
    guard = helix::ui::observe_int_sync<PrintStatusPanel>(
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
        lv_subject_t* s = printer_state_.get_fan_speed_subject(name);
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

// ============================================================================
// FAN ROW: ADAPTIVE FIT + CONTENT DENSITY
// ============================================================================

void PrintStatusPanel::recompute_aux_composites() {
    bool aux_present = !aux_fan_name_.empty();
    int density = lv_subject_get_int(&fan_row_density_subject_);
    lv_subject_set_int(&aux_icon_visible_subject_, (aux_present && density == 0) ? 1 : 0);
    lv_subject_set_int(&aux_full_visible_subject_, (aux_present && density != 2) ? 1 : 0);
    lv_subject_set_int(&aux_short_visible_subject_, (aux_present && density == 2) ? 1 : 0);
}

void PrintStatusPanel::recompute_aux_composites_for_measurement(int density, bool aux_present) {
    lv_subject_set_int(&aux_icon_visible_subject_, (aux_present && density == 0) ? 1 : 0);
    lv_subject_set_int(&aux_full_visible_subject_, (aux_present && density != 2) ? 1 : 0);
    lv_subject_set_int(&aux_short_visible_subject_, (aux_present && density == 2) ? 1 : 0);
}

void PrintStatusPanel::recompute_fans_density() {
    spdlog::debug("[{}] recompute_fans_density: entry", get_name());
    if (!overlay_root_) {
        spdlog::debug("[{}] recompute_fans_density: overlay_root_ is null", get_name());
        return;
    }
    lv_obj_t* fan_row = lv_obj_find_by_name(overlay_root_, "print_status_fan_row");
    lv_obj_t* controls = lv_obj_find_by_name(overlay_root_, "controls_section");
    if (!fan_row || !controls) {
        spdlog::debug("[{}] recompute_fans_density: fan_row={} controls={}", get_name(),
                      fmt::ptr(fan_row), fmt::ptr(controls));
        return;
    }

    // First-time measurement: force each density tier and measure the row's
    // natural CONTENT width. The row has width="100%" so `lv_obj_get_width()`
    // returns the column width (useless). We must temporarily set width to
    // LV_SIZE_CONTENT so flex sums child widths.
    if (fan_row_natural_width_[0] == 0) {
        bool was_hidden = lv_obj_has_flag(fan_row, LV_OBJ_FLAG_HIDDEN);
        if (was_hidden)
            lv_obj_remove_flag(fan_row, LV_OBJ_FLAG_HIDDEN);

        int saved_density = lv_subject_get_int(&fan_row_density_subject_);
        lv_obj_set_width(fan_row, LV_SIZE_CONTENT);

        for (int d = 0; d < 3; ++d) {
            lv_subject_set_int(&fan_row_density_subject_, d);
            recompute_aux_composites_for_measurement(d, /*aux_present=*/true);
            lv_obj_update_layout(fan_row);
            fan_row_natural_width_[d] = lv_obj_get_width(fan_row);
        }

        // Restore 100% width and original density
        lv_obj_set_width(fan_row, lv_pct(100));
        lv_subject_set_int(&fan_row_density_subject_, saved_density);
        recompute_aux_composites();
        lv_obj_update_layout(fan_row);

        if (was_hidden)
            lv_obj_add_flag(fan_row, LV_OBJ_FLAG_HIDDEN);

        spdlog::debug("[{}] fan_row natural widths: full={} med={} compact={}", get_name(),
                      fan_row_natural_width_[0], fan_row_natural_width_[1],
                      fan_row_natural_width_[2]);

        if (fan_row_natural_width_[0] <= 0) {
            spdlog::info("[{}] widths zero — retrying on next tick", get_name());
            auto token = lifetime_.token();
            token.defer("PrintStatusPanel::recompute_fans_density_retry",
                        [this]() { recompute_fans_density(); });
            return;
        }
    }

    int controls_w = lv_obj_get_content_width(controls);
    // Slack accounts for measurement-vs-render discrepancy (font metric rounding,
    // gap accounting differences, etc.). Too tight clips; too loose forces a
    // lower-density tier than necessary. 8px is the largest value that still
    // lets density 0 win when the hotend label is at its widest realistic
    // value ("100%") — the visible label changes when the mock's auto heater
    // fan trips, so the cached natural width can be measured against either
    // "0%" or "100%" and we need both to fit.
    constexpr int DENSITY_SLACK = 8;
    int next_density = 2;
    if (controls_w >= fan_row_natural_width_[0] + DENSITY_SLACK)
        next_density = 0;
    else if (controls_w >= fan_row_natural_width_[1] + DENSITY_SLACK)
        next_density = 1;

    int current = lv_subject_get_int(&fan_row_density_subject_);
    spdlog::debug("[{}] fan_row_density check: current={} next={} controls_w={} widths=[{},{},{}]",
                  get_name(), current, next_density, controls_w, fan_row_natural_width_[0],
                  fan_row_natural_width_[1], fan_row_natural_width_[2]);
    if (next_density != current) {
        lv_subject_set_int(&fan_row_density_subject_, next_density);
        recompute_aux_composites();
    }
}

// DECLARATIVE_OK: the ceiling is a function of the card's MEASURED width, which
// no style attribute can express — the measured-layout structural exception.
void PrintStatusPanel::apply_preview_height_cap() {
    if (!overlay_root_) {
        return;
    }
    // Portrait only. The landscape card sits in a row at roughly 380x392
    // (aspect ~1.03), so it could never reach a 1.30 ceiling anyway — but the
    // landscape XML has no absorber to size either, so bail before touching it.
    // There is no slack in landscape by construction, which is exactly what the
    // graph gate needs to hear: report zero rather than leaving a portrait
    // reading latched after a rotation.
    if (!helix::is_portrait_layout(helix::LayoutManager::instance().type())) {
        note_preview_slack(0);
        return;
    }
    lv_obj_t* card = lv_obj_find_by_name(overlay_root_, "thumbnail_section");
    lv_obj_t* strip = lv_obj_find_by_name(overlay_root_, "metadata_clip");
    lv_obj_t* slack = lv_obj_find_by_name(overlay_root_, "preview_slack");
    if (!card || !strip || !slack) {
        return;
    }

    lv_obj_t* content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    lv_obj_update_layout(content ? content : card);

    // The band is the card's CONTENT width; the card's own border/padding is
    // chrome and rides along with the strip in the ceiling.
    const int32_t band_w = lv_obj_get_content_width(card);
    const int32_t chrome_h =
        lv_obj_get_height(strip) + (lv_obj_get_height(card) - lv_obj_get_content_height(card));
    const int32_t max_h = helix::ui::portrait_preview_card_max_height(band_w, chrome_h);
    if (max_h <= 0) {
        return; // not measurable yet; leave the layout alone
    }

    const char* space_md_str = lv_xml_get_const(nullptr, "space_md");
    const int32_t gap = space_md_str ? std::atoi(space_md_str) : 8;

    // Space the card and the absorber share. Invariant under the absorber's own
    // state, which is what makes re-running this a fixed point: when the
    // absorber is visible it costs its height plus one gap, and both come back.
    const bool shown = !lv_obj_has_flag(slack, LV_OBJ_FLAG_HIDDEN);
    const int32_t avail = lv_obj_get_height(card) + (shown ? lv_obj_get_height(slack) + gap : 0);

    const int32_t want = helix::ui::portrait_preview_slack(max_h, avail, gap);

    // The absorber's visibility is not application state, it is the same measured
    // layout decision as its height: hidden is how a fixed-size flex child costs
    // ZERO, because LVGL's flex pass skips hidden children's size AND their gap.
    // A subject here would be a second name for `want > 0` with no other reader.
    if (want != (shown ? lv_obj_get_height(slack) : 0)) {
        if (want > 0) {
            lv_obj_set_height(slack, want);
            // DECLARATIVE_OK: measured-layout absorber; visibility is `want > 0`.
            lv_obj_remove_flag(slack, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_height(slack, 0);
            // DECLARATIVE_OK: measured-layout absorber; hidden is how it costs zero.
            lv_obj_add_flag(slack, LV_OBJ_FLAG_HIDDEN);
        }
        // Settle the absorber before measuring its content width below — the
        // graph's ceiling is a function of that width, and a just-unhidden child
        // has no resolved percentage size until the layout pass runs.
        lv_obj_update_layout(content ? content : slack);
    }

    // Cap the graph, not the absorber. The absorber must keep the WHOLE slack or
    // the preview card grows straight back into it; the leftover under the graph
    // is transparent and reads as background between the graph and the controls.
    // Sized BEFORE note_preview_slack() publishes the fit subject, so the
    // container is never un-hidden at a height nobody chose.
    const int32_t graph_h = helix::ui::portrait_graph_height(lv_obj_get_content_width(slack), want);
    if (graph_h > 0) {
        if (lv_obj_t* graph = lv_obj_find_by_name(slack, "temp_graph_container")) {
            // DECLARATIVE_OK: measured-layout ceiling, same reason as the absorber.
            lv_obj_set_height(graph, graph_h);
        }
    }

    note_preview_slack(want);
    spdlog::debug("[{}] preview cap: band_w={} chrome_h={} max_h={} avail={} slack={} graph_h={}",
                  get_name(), band_w, chrome_h, max_h, avail, want, graph_h);
}

void PrintStatusPanel::note_preview_slack(int32_t slack_h) {
    preview_slack_h_ = slack_h;
    recompute_graph_fits();
}

void PrintStatusPanel::recompute_graph_fits() {
    if (!subjects_initialized_) {
        return;
    }
    const int current = lv_subject_get_int(&graph_fits_subject_);
    const bool next = helix::ui::portrait_graph_fits(preview_slack_h_, current == 1);

    // Build BEFORE publishing, never after: the subject is what un-hides the
    // container, so flipping it first would show an empty box for however many
    // frames the controller takes to draw its first trace.
    if (next) {
        ensure_temp_graph();
    }

    if (static_cast<int>(next) != current) {
        spdlog::debug("[{}] graph_fits {} -> {} (slack={}, needed={})", get_name(), current,
                      static_cast<int>(next), preview_slack_h_,
                      helix::ui::MIN_TEMP_GRAPH_HEIGHT_PX);
        lv_subject_set_int(&graph_fits_subject_, next ? 1 : 0);
    }
}

void PrintStatusPanel::ensure_temp_graph() {
    if (temp_graph_controller_) {
        return; // already live — keep the backfilled trace
    }
    if (!overlay_root_) {
        return;
    }
    lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "temp_graph_container");
    if (!container) {
        return; // landscape variant has no absorber, so no container either
    }

    helix::TempGraphControllerConfig cfg;
    // 180 points, not the 1200-point default. This graph is on screen DURING a
    // print, which is the worst moment to spend redraw time — 1200 points across
    // N series is what froze the K2 Plus touch UI in #979. At 1 Hz sampling 180
    // points is still three minutes of trace, more than the band can resolve.
    cfg.point_count = 180;
    cfg.axis_size = "xs";
    // Lines and target lines only. The band is ~300px wide: axis labels would eat
    // most of the plot, a legend would eat the rest, and gradients are pure fill
    // cost for a strip this short.
    cfg.initial_features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES;

    helix::TempGraphSeriesSpec nozzle;
    nozzle.klipper_name = printer_state_.temperature_state().active_extruder_name();
    nozzle.display_name = lv_tr("Nozzle");
    nozzle.color = helix::TEMP_GRAPH_SERIES_COLORS[0];
    nozzle.show_target = true;

    helix::TempGraphSeriesSpec bed;
    bed.klipper_name = "heater_bed";
    bed.display_name = lv_tr("Bed");
    bed.color = helix::TEMP_GRAPH_SERIES_COLORS[1];
    bed.show_target = true;

    cfg.series = {std::move(nozzle), std::move(bed)};

    // Chamber only when the printer actually has one. printer_has_chamber is the
    // union of heater and sensor.
    //
    // The name must be the DISCOVERED Klipper object ("heater_generic chamber"),
    // not the literal "chamber": TempGraphController::setup_observers() routes to
    // the chamber subjects on the `heater_generic` / `temperature_fan` prefix, and
    // anything else falls through to a TemperatureSensorManager lookup that finds
    // nothing — a series that resolves to no subject renders as a blank line with
    // no error. Prefer the heater (it has a target to draw); fall back to the
    // sensor so sensor-only chambers still graph.
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber");
    if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
        const auto& temp_state = printer_state_.temperature_state();
        const std::string& heater = temp_state.chamber_heater_name();
        const std::string& sensor = temp_state.chamber_sensor_name();
        const std::string& klipper = !heater.empty() ? heater : sensor;
        if (!klipper.empty()) {
            helix::TempGraphSeriesSpec chamber;
            chamber.klipper_name = klipper;
            chamber.display_name = lv_tr("Chamber");
            chamber.color = helix::TEMP_GRAPH_SERIES_COLORS[2];
            chamber.show_target = !heater.empty();
            cfg.series.push_back(std::move(chamber));
        }
    }

    temp_graph_container_ = container;
    temp_graph_controller_ = std::make_unique<helix::TempGraphController>(container, cfg);
    // The controller backfills from TemperatureHistoryManager in its constructor,
    // so the trace is populated the moment the container un-hides rather than
    // growing from blank at the next sample.
    spdlog::debug("[{}] Temperature mini-graph created ({} series, {} points)", get_name(),
                  cfg.series.size(), cfg.point_count);
}

void PrintStatusPanel::destroy_temp_graph(bool defer_delete) {
    temp_graph_container_ = nullptr;
    if (!temp_graph_controller_) {
        return;
    }

    // Detach observers SYNCHRONOUSLY, then defer only the deallocation. The
    // widget tree is already queued for async deletion by the time this runs, so
    // the controller's destructor will find its chart gone (chart_delete_cb nulls
    // it) — but its observers still point at live subjects and must come off
    // before anything else can fire them (#726). Deferring the delete itself
    // keeps it out of the current UpdateQueue batch (#696).
    temp_graph_controller_->detach();
    auto* old = temp_graph_controller_.release();
    if (defer_delete && lv_is_initialized()) {
        lv_async_call([](void* p) { delete static_cast<helix::TempGraphController*>(p); }, old);
    } else {
        delete old;
    }
    spdlog::debug("[{}] Temperature mini-graph torn down", get_name());
}

void PrintStatusPanel::recompute_fans_fit() {
    spdlog::debug("[{}] recompute_fans_fit: entry", get_name());
    if (!overlay_root_) {
        spdlog::debug("[{}] recompute_fans_fit: overlay_root_ is null", get_name());
        return;
    }
    // Cap the preview before measuring: the fan-row budget reads overlay_content
    // and the controls, and both must be measured against the settled column.
    apply_preview_height_cap();

    lv_obj_t* controls = lv_obj_find_by_name(overlay_root_, "controls_section");
    lv_obj_t* fan_row = lv_obj_find_by_name(overlay_root_, "print_status_fan_row");
    if (!controls || !fan_row) {
        spdlog::debug("[{}] recompute_fans_fit: controls={} fan_row={}", get_name(),
                      fmt::ptr(controls), fmt::ptr(fan_row));
        return;
    }

    lv_obj_update_layout(controls);

    if (fan_row_natural_height_ == 0) {
        bool was_hidden = lv_obj_has_flag(fan_row, LV_OBJ_FLAG_HIDDEN);
        if (was_hidden)
            lv_obj_remove_flag(fan_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(fan_row);
        fan_row_natural_height_ = lv_obj_get_height(fan_row);
        if (was_hidden)
            lv_obj_add_flag(fan_row, LV_OBJ_FLAG_HIDDEN);
        spdlog::debug("[{}] fan_row natural height={}", get_name(), fan_row_natural_height_);
        if (fan_row_natural_height_ <= 0) {
            auto token = lifetime_.token();
            token.defer("PrintStatusPanel::recompute_fans_fit_retry",
                        [this]() { recompute_fans_fit(); });
            return;
        }
    }

    // Portrait stacks overlay_content into a column and sizes controls_section
    // to its content, which removes the slack the landscape formula measures
    // against. See helix::ui::fan_row_budget().
    const bool portrait = helix::is_portrait_layout(helix::LayoutManager::instance().type());
    lv_obj_t* content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (portrait && content) {
        lv_obj_update_layout(content);
    }

    int controls_h = lv_obj_get_height(controls);
    int content_h = content ? lv_obj_get_height(content) : 0;
    int used = 0;
    int visible_count = 0;

    auto add_child_height = [&](const char* name) {
        lv_obj_t* o = lv_obj_find_by_name(overlay_root_, name);
        if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
            return;
        used += lv_obj_get_height(o);
        ++visible_count;
    };
    add_child_height("temp_card");
    // Portrait merged the filament/AMS cluster INTO speed_flow_row, so the next
    // line already counts it; landscape never had it as a controls child at all.
    add_child_height("speed_flow_row");

    // button_grid is flex_grow=1 so its OWN height is stretched. Sum the
    // visible button-row children directly to get the natural content height.
    lv_obj_t* btn_grid = lv_obj_find_by_name(overlay_root_, "button_grid");
    if (btn_grid && !lv_obj_has_flag(btn_grid, LV_OBJ_FLAG_HIDDEN)) {
        int btn_grid_used = 0;
        int btn_rows_visible = 0;
        uint32_t n = lv_obj_get_child_count(btn_grid);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_t* row = lv_obj_get_child(btn_grid, i);
            if (!row || lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN))
                continue;
            btn_grid_used += lv_obj_get_height(row);
            ++btn_rows_visible;
        }
        const char* space_sm_str = lv_xml_get_const(nullptr, "space_sm");
        int space_sm = space_sm_str ? std::atoi(space_sm_str) : 4;
        if (btn_rows_visible > 1)
            btn_grid_used += (btn_rows_visible - 1) * space_sm;
        used += btn_grid_used;
        ++visible_count;
    }
    add_child_height("print_status_extras");

    // Account for inter-child gaps: (N-1) gaps between N visible children.
    // The fan row would add one more visible child, so include +1 in gap count.
    const char* space_md_str = lv_xml_get_const(nullptr, "space_md");
    int space_md = space_md_str ? std::atoi(space_md_str) : 8;
    if (visible_count >= 1)
        used += visible_count * space_md; // (visible_count - 1) for existing + 1 for fan row

    int available = helix::ui::fan_row_budget(portrait, controls_h, content_h, used);
    int current = lv_subject_get_int(&fans_fit_subject_);
    int next = current;
    if (current == 1) {
        if (available < fan_row_natural_height_)
            next = 0;
    } else {
        if (available >= fan_row_natural_height_ + 4)
            next = 1;
    }
    if (next != current) {
        spdlog::debug("[{}] fans_fit {} -> {} (portrait={}, controls_h={}, content_h={}, used={}, "
                      "available={}, needed={})",
                      get_name(), current, next, portrait, controls_h, content_h, used, available,
                      fan_row_natural_height_);
        lv_subject_set_int(&fans_fit_subject_, next);
    }
}

// SIZE_CHANGED on controls_section — defers density + fit recompute via lifetime token
void PrintStatusPanel::on_controls_size_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_controls_size_changed");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        auto token = self->lifetime_.token();
        token.defer("PrintStatusPanel::size_changed_recompute", [self]() {
            self->recompute_fans_density();
            self->recompute_fans_fit();
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
    auto state = static_cast<PrintJobState>(
        // RAW_PRINT_STATE_OK: see the optimistic-overlay note below.
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
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
    // sees feedback. Otherwise prefer Klipper's print_stats.message
    // (firmware-supplied descriptor — runout, error wrap, custom macros). If
    // empty AND any configured runout sensor is currently tripped, surface a
    // generic "Filament Runout" hint. Otherwise leave blank → reason label
    // stays hidden.
    std::string reason;
    if (pending == helix::ui::PendingAction::Pausing) {
        reason = lv_tr("Pausing...");
    } else if (pending == helix::ui::PendingAction::Resuming) {
        reason = lv_tr("Resuming...");
    } else if (paused) {
        const char* fw_msg = lv_subject_get_string(printer_state_.get_print_message_subject());
        if (fw_msg && *fw_msg) {
            reason = fw_msg;
        } else if (FilamentSensorManager::instance().has_real_runout()) {
            reason = lv_tr("Filament Runout");
        }
    }
    lv_subject_copy_string(&print_pause_reason_subject_, reason.c_str());
    lv_subject_set_int(&print_pause_reason_visible_subject_, reason.empty() ? 0 : 1);
}

void PrintStatusPanel::update_chamber_status() {
    if (!subjects_initialized_)
        return;

    bool has_heater =
        lv_subject_get_int(printer_state_.get_printer_has_chamber_heater_subject()) != 0;
    int current = lv_subject_get_int(printer_state_.get_chamber_temp_subject());
    int target = lv_subject_get_int(printer_state_.get_chamber_target_subject());

    if (!has_heater || target == 0) {
        // Sensor-only or heater off: no status text
        chamber_status_buf_[0] = '\0';
    } else {
        auto chamber_heater = helix::ui::temperature::heater_display(current, target);
        std::snprintf(chamber_status_buf_, sizeof(chamber_status_buf_), "%s",
                      chamber_heater.status.c_str());
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
    update_filament_used_text();

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
    auto outcome =
        static_cast<PrintOutcome>(lv_subject_get_int(printer_state_.get_print_outcome_subject()));

    // Delegate state mapping and transition logic to lifecycle. The live phase
    // goes in too: without it this derives Printing (or Complete) while the
    // published print_lifecycle correctly says Preparing, and the two disagree
    // for the whole pre-print window.
    const int start_phase = lv_subject_get_int(printer_state_.get_print_start_phase_subject());
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
        if (!displayed_file_.empty() || !gcode_displayed_file_.empty() ||
            lifecycle_.gcode_loaded() || !temp_gcode_path_.empty() ||
            !pending_gcode_filename_.empty()) {
            spdlog::debug("[{}] Clearing thumbnail/gcode tracking (print ended)", get_name());
            // Cancel pending deferred G-code load (print is over)
            if (gcode_load_timer_) {
                lv_timer_delete(gcode_load_timer_);
                gcode_load_timer_ = nullptr;
            }
            cached_thumbnail_path_.clear();
#if defined(HELIX_PLATFORM_ESP32)
            // Release our reference to the PSRAM buffer. Main thread (job-state
            // handler), as EspPsramThumbnail's destructor requires.
            //
            // The src must stop naming the descriptor BEFORE the release: for a
            // variable source lv_image stores the raw pointer (it only strdups
            // paths), and ours can be the last reference — PrinterState drops
            // its own the moment the filename changes, and no replacement
            // arrives at all when the next file has no thumbnail or the fetch
            // fails. The placeholder is what the shared path subject publishes
            // for a file with no thumbnail, so it is a valid src here.
            if (esp_thumbnail_ && print_thumbnail_ &&
                lv_image_get_src(print_thumbnail_) == esp_thumbnail_->dsc()) {
                lv_image_set_src(print_thumbnail_,
                                 helix::PrinterPrintState::no_thumbnail_placeholder());
            }
            esp_thumbnail_.reset();
#endif
            pending_gcode_filename_.clear();
            // The print is over. lifecycle_ already reset its own gcode_loaded
            // flag inside on_job_state_changed(). The
            // widgets keep showing the final frame; the desired file becomes
            // empty, so leave displayed_file_ as-is — a new print's filename
            // change clears it.
            cleanup_temp_gcode();

            // Note: Shared subjects (print_thumbnail_path, print_display_filename)
            // are cleared by ActivePrintMediaManager when print_filename_ becomes empty
        }
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
        format_time(lifecycle_.remaining_seconds(), remaining_buf_, sizeof(remaining_buf_));
        lv_subject_copy_string(&remaining_subject_, remaining_buf_);
    }

    // Freeze display values on Complete (lifecycle already froze the state values)
    if (result.should_freeze_complete) {
        if (progress_bar_) {
            lv_bar_set_value(progress_bar_, 100, LV_ANIM_OFF);
        }

        if (lifecycle_.total_layers() > 0) {
            update_layer_text();
        }

        format_time(lifecycle_.elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
        lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
        format_time(0, remaining_buf_, sizeof(remaining_buf_));
        lv_subject_copy_string(&remaining_subject_, remaining_buf_);

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

    // The e-stop is the estop_fab at the panel root, bound to the estop_visible
    // subject in XML; the header owns only the estop_slot gutter now. Nothing
    // here touches the header's action_button: this panel never configures one,
    // so un-hiding it renders an empty primary-colored pill.
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
    if (subjects_initialized_) {
        helix::format::format_percent(lifecycle_.speed_percent(), speed_buf_, sizeof(speed_buf_));
        lv_subject_copy_string(&speed_subject_, speed_buf_);
    }
    spdlog::trace("[{}] Speed factor updated: {}%", get_name(), speed);
}

void PrintStatusPanel::on_flow_factor_changed(int flow) {
    lifecycle_.on_flow_changed(flow);
    if (subjects_initialized_) {
        helix::format::format_percent(lifecycle_.flow_percent(), flow_buf_, sizeof(flow_buf_));
        lv_subject_copy_string(&flow_subject_, flow_buf_);
    }
    spdlog::trace("[{}] Flow factor updated: {}%", get_name(), flow);
}

void PrintStatusPanel::on_gcode_z_offset_changed(int /* microns */) {
    // Delegate to tune overlay singleton. Resolve the value rather than forwarding
    // the raw live offset: ZMOD zeroes that outside a print, and handing the
    // overlay a phantom zero would make its next baby-step adjust from the wrong
    // base.
    get_print_tune_overlay().update_z_offset_display(
        helix::zoffset::displayed_z_offset_microns(printer_state_));
}

void PrintStatusPanel::on_led_state_changed(int state) {
    // Delegate to light/timelapse controls (extracted Phase 2)
    light_timelapse_controls_.update_led_state(state != 0);
}

void PrintStatusPanel::on_print_layer_changed(int current_layer) {
    // Read total layers from PrinterState and delegate to lifecycle
    int total_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject());
    bool has_real_data = printer_state_.has_real_layer_data();
    if (!lifecycle_.on_layer_changed(current_layer, total_layers, has_real_data)) {
        spdlog::trace("[{}] Ignoring layer update ({}) - guarded by lifecycle", get_name(),
                      current_layer);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    update_layer_text();

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
    auto outcome =
        static_cast<PrintOutcome>(lv_subject_get_int(printer_state_.get_print_outcome_subject()));
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
    format_time(lifecycle_.elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
    lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
    spdlog::trace("[{}] Elapsed updated: {}s (wall-clock from Moonraker)", get_name(), seconds);
}

void PrintStatusPanel::on_print_time_left_changed(int seconds) {
    // Get outcome from PrinterState and delegate guard + state update to lifecycle
    auto outcome =
        static_cast<PrintOutcome>(lv_subject_get_int(printer_state_.get_print_outcome_subject()));
    if (!lifecycle_.on_time_left_changed(seconds, outcome)) {
        spdlog::trace("[{}] Ignoring time_left update ({}) - guarded by lifecycle", get_name(),
                      seconds);
        return;
    }

    // Guard: subjects may not be initialized if called from constructor's observer setup
    if (!subjects_initialized_) {
        return;
    }

    format_time(lifecycle_.remaining_seconds(), remaining_buf_, sizeof(remaining_buf_));
    lv_subject_copy_string(&remaining_subject_, remaining_buf_);

    bool use_24h = DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_24;
    auto eta_str = helix::format::eta_clock_time(lifecycle_.remaining_seconds(), 0, use_24h);
    std::snprintf(eta_buf_, sizeof(eta_buf_), "%s", eta_str.c_str());
    lv_subject_copy_string(&eta_subject_, eta_buf_);

    spdlog::trace("[{}] Time remaining updated: {}s, ETA: {}", get_name(), seconds, eta_buf_);
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
    auto current_job_state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
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
                if (lv_subject_get_int(self->printer_state_.get_print_start_phase_subject()) != 0) {
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
        std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), " ");
        lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);

        // Initialize elapsed display to 0m (preprint observer will update it)
        format_time(0, elapsed_buf_, sizeof(elapsed_buf_));
        lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);

        // Show predicted total as initial remaining estimate (preprint observer refines it)
        int predicted = helix::PreprintPredictor::predicted_total_from_config();
        if (predicted > 0) {
            int total_remaining = lifecycle_.remaining_seconds() + predicted;
            format_time(total_remaining, remaining_buf_, sizeof(remaining_buf_));
            lv_subject_copy_string(&remaining_subject_, remaining_buf_);
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
        // confirmed. ensure_preview_current() reads the real widget state and
        // reloads only what is missing.
        ensure_preview_current();

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
                          : printer_state_.get_estimated_print_time();
    lifecycle_.on_preprint_remaining_changed(seconds, slicer_time);

    if (lifecycle_.state() != PrintState::Preparing) {
        return;
    }

    // Combine preprint prediction with slicer estimate for total remaining time
    int total_remaining = slicer_time + seconds;
    format_time(total_remaining, remaining_buf_, sizeof(remaining_buf_));
    lv_subject_copy_string(&remaining_subject_, remaining_buf_);
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

    format_time(lifecycle_.preprint_elapsed_seconds(), elapsed_buf_, sizeof(elapsed_buf_));
    lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
}

void PrintStatusPanel::update_view_toggle_position(bool objects_visible) {
    if (!overlay_root_)
        return;
    // Resolve the card by name, not by walking up from the viewer: the previews
    // live one level down inside preview_clear_area, while both corner buttons
    // are direct children of thumbnail_section.
    lv_obj_t* card = lv_obj_find_by_name(overlay_root_, "thumbnail_section");
    if (!card)
        return;
    lv_obj_t* btn = lv_obj_find_by_name(card, "btn_view_toggle");
    if (!btn)
        return;

    int32_t space_md = theme_manager_get_spacing("space_md");
    if (objects_visible) {
        lv_obj_t* btn_objects = lv_obj_find_by_name(card, "btn_objects");
        int32_t obj_w = btn_objects ? lv_obj_get_width(btn_objects) : 36;
        lv_obj_set_style_translate_x(btn, space_md + obj_w + space_md, LV_PART_MAIN);
    } else {
        lv_obj_set_style_translate_x(btn, space_md, LV_PART_MAIN);
    }
}

void PrintStatusPanel::update_objects_text() {
    if (!subjects_initialized_)
        return;
    auto& defined = printer_state_.get_defined_objects();
    auto& excluded = printer_state_.get_excluded_objects();
    int total = static_cast<int>(defined.size());
    int active = std::max(0, total - static_cast<int>(excluded.size()));
    if (total >= 2) {
        std::snprintf(objects_text_buf_, sizeof(objects_text_buf_), "%d/%d", active, total);
    } else {
        objects_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&objects_text_subject_, objects_text_buf_);
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
// THUMBNAIL LOADING
// ============================================================================

#if defined(HELIX_PLATFORM_ESP32)
void PrintStatusPanel::apply_esp_psram_thumbnail() {
    auto thumb = printer_state_.get_print_psram_thumbnail();
    if (!thumb) {
        return;
    }
    // Hold the reference for as long as print_thumbnail_'s src points at the
    // descriptor. `previous` keeps the outgoing buffer alive until after the
    // widget stops pointing at it — otherwise the last release could free the
    // descriptor the widget's src still names. Both releases happen here, on
    // the main thread, which is what EspPsramThumbnail's destructor requires.
    auto previous = std::move(esp_thumbnail_);
    esp_thumbnail_ = std::move(thumb);
    if (!print_thumbnail_) {
        spdlog::info("[{}] PSRAM thumbnail held (panel not yet displayed)", get_name());
        return;
    }
    lv_image_set_src(print_thumbnail_, esp_thumbnail_->dsc());
    spdlog::info("[{}] PSRAM thumbnail displayed", get_name());
    // Fallback content for the current print is now on screen; record it so
    // ensure_preview_current() treats the thumbnail as current (mirrors the
    // print_thumbnail_path observer on other platforms).
    const std::string& effective = printer_state_.get_effective_print_filename();
    if (!effective.empty()) {
        displayed_file_ = effective;
    }
}
#endif

// ============================================================================
// G-CODE VIEWER LOADING
// ============================================================================

void PrintStatusPanel::load_gcode_for_viewing(const std::string& filename) {
    spdlog::debug("[{}] Loading G-code for viewing: {}", get_name(), filename);

    // Skip if no viewer widget
    if (!gcode_viewer_) {
        spdlog::debug("[{}] No gcode_viewer_ widget - skipping G-code load", get_name());
        return;
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[{}] No API available - skipping G-code load", get_name());
        return;
    }

    // Check "Thumbnail Only" render mode - skip all gcode downloading/parsing
    if (DisplaySettingsManager::instance().get_gcode_render_mode() == 3) {
        spdlog::info("[{}] G-code render mode is Thumbnail Only - skipping G-code load",
                     get_name());
        show_gcode_viewer(false);
        return;
    }

    // Check config option to disable 3D rendering entirely
    auto* cfg = Config::get_instance();
    bool gcode_3d_enabled = cfg->get<bool>("/display/gcode_3d_enabled", true);
    if (!gcode_3d_enabled) {
        spdlog::info("[{}] G-code 3D rendering disabled via config - using thumbnail only",
                     get_name());
        show_gcode_viewer(false); // Ensure thumbnail is shown, not empty viewer
        return;
    }

    // Generate temp file path - check if we already have a cached copy
    // Use persistent cache directory (not /tmp which may be RAM-backed on embedded)
    std::string cache_dir = get_helix_cache_dir("gcode_temp");
    if (cache_dir.empty()) {
        spdlog::warn("[{}] No writable cache directory - skipping G-code preview", get_name());
        show_gcode_viewer(false);
        return;
    }
    std::string temp_path =
        cache_dir + "/print_view_" + std::to_string(std::hash<std::string>{}(filename)) + ".gcode";

    // Check if file already exists and is non-empty (cached from previous session)
    std::ifstream cached_file(temp_path, std::ios::binary | std::ios::ate);
    if (cached_file && cached_file.tellg() > 0) {
        size_t cached_size = static_cast<size_t>(cached_file.tellg());
        cached_file.close();

        // Check if cached file is safe to render
        if (helix::is_gcode_2d_streaming_safe(cached_size)) {
            spdlog::info("[{}] Using cached G-code file ({} bytes): {}", get_name(), cached_size,
                         temp_path);
            temp_gcode_path_ = temp_path;
            load_gcode_file(temp_path.c_str());
            return;
        } else {
            spdlog::debug("[{}] Cached file too large for 2D streaming, removing", get_name());
            std::remove(temp_path.c_str());
        }
    }

    // Get file metadata to check size before downloading
    // This prevents OOM on memory-constrained devices like AD5M
    std::string metadata_filename = resolve_gcode_filename(filename);

    auto token = lifetime_.token();

    auto download_to_viewer = [this, filename, temp_path](const std::string& root,
                                                          const std::string& download_filename) {
        if (!temp_gcode_path_.empty() && temp_gcode_path_ != temp_path) {
            std::remove(temp_gcode_path_.c_str());
            temp_gcode_path_.clear();
        }

        auto inner_token = lifetime_.token();
        api_->transfers().download_file_to_path(
            root, download_filename, temp_path,
            [this, inner_token, temp_path](const std::string& path) {
                inner_token.defer("PrintStatusPanel::gcode_download_ok", [this, path]() {
                    temp_gcode_path_ = path;
                    spdlog::debug("[{}] Streamed G-code to disk, loading into viewer: {}",
                                  get_name(), path);
                    load_gcode_file(path.c_str());
                });
            },
            [this, inner_token, filename](const MoonrakerError& err) {
                inner_token.defer("PrintStatusPanel::gcode_download_err", [this, filename, err]() {
                    spdlog::warn("[{}] Failed to stream G-code for viewing '{}': {}", get_name(),
                                 filename, err.message);
                    show_gcode_viewer(false);
                });
            });
    };

    // Shared size gate: skip 2D streaming if the file would OOM the device,
    // otherwise stream it into the viewer. Used by both the standard "gcodes"
    // metadata path and the QIDI ".temp" shadow path.
    auto stream_if_safe = [this, download_to_viewer](const std::string& root,
                                                     const std::string& download_target,
                                                     uint64_t size) {
        if (!helix::is_gcode_2d_streaming_safe(size)) {
            auto mem = helix::get_system_memory_info();
            spdlog::warn("[{}] G-code too large for 2D streaming: file={} bytes, available "
                         "RAM={}MB - using thumbnail only",
                         get_name(), size, mem.available_mb());
            show_gcode_viewer(false);
            return;
        }

        spdlog::debug("[{}] G-code size {} bytes - safe to render, streaming to disk...",
                      get_name(), size);
        download_to_viewer(root, download_target);
    };

    auto load_existing_gcode_path = [this, token, filename, stream_if_safe](
                                        const std::string& metadata_target, const std::string& root,
                                        const std::string& download_target) {
        api_->files().get_file_metadata(
            metadata_target,
            [this, token, root, download_target, stream_if_safe](const FileMetadata& metadata) {
                token.defer("PrintStatusPanel::gcode_metadata_ok",
                            [this, root, download_target, metadata, stream_if_safe]() {
                                stream_if_safe(root, download_target, metadata.size);
                            });
            },
            [this, token, filename](const MoonrakerError& err) {
                token.defer("PrintStatusPanel::gcode_metadata_err", [this, filename, err]() {
                    // Metadata only decides whether we need to DOWNLOAD the file.
                    // If the viewer already has geometry — loaded from the cached
                    // copy, or from a local path that Moonraker cannot resolve —
                    // a metadata miss must not tear down a working render. Also
                    // reachable on a transient failure while the file is still
                    // being scanned. This error is silent (no toast), so hiding
                    // the viewer here just left a blank preview for the rest of
                    // the print.
                    if (gcode_viewer_ && ui_gcode_viewer_has_content(gcode_viewer_)) {
                        spdlog::debug("[{}] G-code metadata unavailable for '{}': {} - keeping "
                                      "already-loaded render",
                                      get_name(), filename, err.message);
                        return;
                    }
                    spdlog::debug(
                        "[{}] Failed to get G-code metadata for '{}': {} - skipping 3D render",
                        get_name(), filename, err.message);
                    show_gcode_viewer(false);
                });
            },
            true // silent - don't trigger RPC_ERROR event/toast
        );
    };

    auto use_existing_download_path = [metadata_filename, filename, load_existing_gcode_path]() {
        load_existing_gcode_path(metadata_filename, "gcodes", filename);
    };

    auto ends_with_3mf = [](const std::string& name) {
        if (name.size() < 4) {
            return false;
        }
        const size_t pos = name.size() - 4;
        return (name[pos] == '.' && (name[pos + 1] == '3') &&
                (name[pos + 2] == 'm' || name[pos + 2] == 'M') &&
                (name[pos + 3] == 'f' || name[pos + 3] == 'F'));
    };

    if (ends_with_3mf(filename)) {
        api_->files().list_files(
            ".temp", "", false,
            [this, token, use_existing_download_path,
             stream_if_safe](const std::vector<FileInfo>& files) {
                token.defer("PrintStatusPanel::qidi_3mf_shadow_list_ok",
                            [this, files, use_existing_download_path, stream_if_safe]() {
                                spdlog::debug("[{}] .temp returned {} entries for QIDI native 3MF "
                                              "preview lookup",
                                              get_name(), files.size());

                                // A multi-plate .3mf can leave several
                                // shadow_native_plate_*.gcode files in .temp, and
                                // Moonraker exposes no plate index for the active
                                // print. The active plate's shadow is (re)written at
                                // print start, so the newest-modified match is the
                                // best proxy for "the plate currently printing".
                                const FileInfo* best = nullptr;
                                for (const auto& file : files) {
                                    if (!helix::gcode::is_native_3mf_shadow(file.path)) {
                                        continue;
                                    }
                                    if (best == nullptr || file.modified > best->modified) {
                                        best = &file;
                                    }
                                }

                                if (best != nullptr) {
                                    spdlog::debug(
                                        "[{}] Selected QIDI native 3MF shadow G-code (newest of "
                                        "matches): .temp/{} ({} bytes, modified {})",
                                        get_name(), best->path, best->size, best->modified);

                                    stream_if_safe(".temp", best->path, best->size);
                                    return;
                                }

                                spdlog::debug("[{}] No QIDI native 3MF shadow G-code found; "
                                              "falling back to active filename",
                                              get_name());
                                use_existing_download_path();
                            });
            },
            [this, token, use_existing_download_path](const MoonrakerError& err) {
                token.defer("PrintStatusPanel::qidi_3mf_shadow_list_err",
                            [this, err, use_existing_download_path]() {
                                spdlog::debug(
                                    "[{}] Failed to list .temp for QIDI native 3MF preview: {}; "
                                    "falling back to active filename",
                                    get_name(), err.message);
                                use_existing_download_path();
                            });
            });
        return;
    }

    // All four callbacks below fire on background threads — get_file_metadata's
    // success/error cb runs on libhv's WS event loop, download_file_to_path's
    // runs on HttpExecutor::slow(). They MUST marshal to the main thread via
    // tok.defer before touching LVGL widgets, the gcode_viewer state, or the
    // temp_gcode_path_ member. Pre-fix, the inner success cb called
    // load_gcode_file → ui_gcode_viewer_load_file_async → safe_delete on the
    // HTTP worker, racing the main render loop and producing the L081-cluster
    // heap corruption that surfaces as a SIGSEGV in get_prop_core / layout
    // (#906 family, WKC5J9SK on v0.99.56 ad5x).
    use_existing_download_path();
}

// ============================================================================
// FILAMENT COLOR OVERRIDE
// ============================================================================

bool PrintStatusPanel::build_and_apply_tool_colors() {
    if (!gcode_viewer_ || !ui_gcode_viewer_has_content(gcode_viewer_)) {
        return false;
    }

    // ONE rule, any tool count: color(tool N) = the color of the lane that
    // actually prints N. No palette, no tool-count branch, no active-lane
    // special case — a 1-tool file and an N-tool file take this exact path.
    //
    // What used to be here asked "what mapping SHOULD this print use" (the
    // slicer palette matched against lane colors) and then patched the gaps with
    // fallbacks. That is the right question BEFORE a print, and it still lives
    // in PrintSelectDetailView where it decides what to send. Once the print is
    // underway the question is "what mapping IS in effect", and the firmware
    // answers it exactly — so inferring it here was guessing at something already
    // known, and the guess is what forced the special cases.
    if (ui_gcode_viewer_apply_ams_tool_colors(gcode_viewer_)) {
        return true;
    }

    // Nothing knowable (no routing published, or no lane knows a color). Leave
    // the renderer's slicer colors alone rather than painting a plausible lie.
    return false;
}

// ============================================================================
// PUBLIC API
// ============================================================================

void PrintStatusPanel::set_temp_control_panel(TemperatureService* temp_panel) {
    temp_control_panel_ = temp_panel;
    spdlog::trace("[{}] TemperatureService reference set", get_name());
}

void PrintStatusPanel::schedule_deferred_gcode_load() {
    // Cancel any existing timer (debounce: if filename changes rapidly, only load the latest)
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }

    if (pending_gcode_filename_.empty())
        return;

    // Short delay if already printing (user is actively viewing), longer during
    // homing/heating to avoid memory spike while printer is still preparing
    uint32_t delay_ms =
        (lifecycle_.state() == PrintState::Printing || lifecycle_.state() == PrintState::Paused)
            ? 500
            : 5000;

    spdlog::debug("[{}] Scheduling deferred G-code load in {}ms: {}", get_name(), delay_ms,
                  pending_gcode_filename_);

    gcode_load_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<PrintStatusPanel*>(lv_timer_get_user_data(timer));
            self->gcode_load_timer_ = nullptr; // timer is auto-deleted after one-shot
            if (!self->pending_gcode_filename_.empty()) {
                spdlog::info("[{}] Deferred G-code load firing: {}", self->get_name(),
                             self->pending_gcode_filename_);
                self->load_gcode_for_viewing(self->pending_gcode_filename_);
                self->pending_gcode_filename_.clear();
            }
        },
        delay_ms, this);
    lv_timer_set_repeat_count(gcode_load_timer_, 1); // one-shot
}

void PrintStatusPanel::set_filename(const char* filename) {
    // Store the actual filename (may be a temp file path)
    current_print_filename_ = filename ? filename : "";

    // The identity of the running print - including retiring an override that
    // stopped describing it, and resolving a rewritten temp path - is decided by
    // PrinterPrintState before print_filename_ is ever published. The panel used
    // to redo all of it from its own copy and compare its answer against the one
    // the media manager published; any divergence dropped the thumbnail with no
    // retry (prestonbrown/helixscreen#1339).
    const std::string& effective_filename = printer_state_.get_effective_print_filename();

    // Note: Display filename is now handled by ActivePrintMediaManager
    // PrintStatusPanel only needs to load local resources (gcode viewer, local thumbnail)

    // When the effective filename CHANGES, the widgets are showing the old file
    // (or nothing). Clear each stale per-asset marker so ensure_preview_current()
    // sees the mismatch and reloads that asset. Each marker is cleared only when
    // ITS OWN asset is stale — the thumbnail can already be current while the
    // gcode viewer still holds the previous print, so clearing them together
    // would force a needless thumbnail re-fetch. Idempotent: a repeated observer
    // fire with the same effective filename leaves the markers untouched and
    // ensure_preview_current() becomes a no-op if the widgets already hold valid
    // content.
    if (!effective_filename.empty() && effective_filename != displayed_file_) {
        // Clear stale cached thumbnail from previous print
        cached_thumbnail_path_.clear();
        displayed_file_.clear();
    }
    if (!effective_filename.empty() && effective_filename != gcode_displayed_file_) {
        gcode_displayed_file_.clear();
    }
    ensure_preview_current();
}

void PrintStatusPanel::ensure_preview_current() {
    // Desired state = the effective filename of the current print.
    const std::string& desired = printer_state_.get_effective_print_filename();

    // Read ACTUAL widget state — not intent bools, which can lie after a
    // destroy-on-close / memory-reclaim cycle. This is what makes re-entry
    // self-healing.
    bool thumbnail_has_src = print_thumbnail_ && lv_image_get_src(print_thumbnail_) != nullptr;
    bool gcode_has_content = gcode_viewer_ && ui_gcode_viewer_has_content(gcode_viewer_);

    bool want_viewer = lifecycle_.want_viewer();

    helix::ui::PreviewAction action =
        helix::ui::decide_preview_action(displayed_file_, gcode_displayed_file_, desired,
                                         thumbnail_has_src, gcode_has_content, want_viewer);

    spdlog::debug("[{}] ensure_preview_current: thumb_file='{}' gcode_file='{}' desired='{}' "
                  "thumb_src={} gcode_content={} want_viewer={} -> load_thumb={} load_gcode={} "
                  "clear_gcode={}",
                  get_name(), displayed_file_, gcode_displayed_file_, desired, thumbnail_has_src,
                  gcode_has_content, want_viewer, action.load_thumbnail, action.load_gcode,
                  action.clear_gcode);

    if (desired.empty()) {
        return; // Nothing to show.
    }

    // Drop the previous print's geometry FIRST. The reload below is deferred by
    // seconds while the printer is still preparing, and the viewer keeps
    // rendering what it holds until then, so without this the user watches the
    // last print's model for the whole deferral (#1337-adjacent report: "the
    // image from the previous print is displayed first"). ui_gcode_viewer_clear()
    // fires the clear callback, which flips the view mode back to the thumbnail,
    // so the fallback the user lands on is this print's slicer preview.
    if (action.clear_gcode && gcode_viewer_) {
        spdlog::debug("[{}] Clearing stale G-code geometry (viewer holds another print, "
                      "desired '{}')",
                      get_name(), desired);
        ui_gcode_viewer_clear(gcode_viewer_);
    }

    if (action.load_thumbnail && print_thumbnail_ &&
        helix::ui::is_on_active_screen(print_thumbnail_)) {
        // The overlay root is parented under the active screen, so a thumbnail
        // that no longer roots there has been reparented onto lv_layer_top() to
        // await deletion. Setting its image src would cascade lv_image_set_src →
        // update_align → lv_obj_update_layout across the layer and recurse into
        // sibling condemned grid subtrees whose children may already be freed
        // (#1001). Same guard the home-panel widget applies to its own thumbs.
        //
        // Nothing here fetches: that belongs to ActivePrintMediaManager, the
        // single writer of the shared subject. The only two sources are our own
        // cache and that subject's current value.
        if (!cached_thumbnail_path_.empty() && displayed_file_ == desired) {
            // Cheap re-apply of a thumbnail we already hold for this file.
            crash_handler::breadcrumb::note("pstat_thm", "set_src_pre");
            lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
            crash_handler::breadcrumb::note("pstat_thm", "set_src_post");
            displayed_file_ = desired;
        } else if (printer_state_.get_print_thumbnail_file() == desired) {
            // The subject already carries this file's image, but it was
            // published BEFORE our own view of the filename caught up: the
            // manager observes print_filename synchronously while this panel's
            // filename observer is deferred, so print_thumbnail_path_observer_
            // compared against the PREVIOUS filename and correctly dropped it.
            // Re-reading the subject once the filename lands is what makes that
            // ordering self-healing instead of leaving the previous print's
            // image on the new print's card.
            const char* published =
                lv_subject_get_string(printer_state_.get_print_thumbnail_path_subject());
            cached_thumbnail_path_ = published;
            crash_handler::breadcrumb::note("pstat_thm", "set_src_pre");
            lv_image_set_src(print_thumbnail_, published);
            crash_handler::breadcrumb::note("pstat_thm", "set_src_post");
            if (is_no_thumbnail_placeholder(published)) {
                // Identity matches but there is nothing to adopt: this is the
                // manager's "no thumbnail for this file yet" clear. Show it,
                // leave the marker empty so the next reconcile tries again.
                displayed_file_.clear();
                spdlog::debug("[{}] Published path for '{}' is the no-thumbnail placeholder; "
                              "showing it without marking the preview current",
                              get_name(), desired);
            } else {
                displayed_file_ = desired;
                spdlog::debug("[{}] Adopted already-published thumbnail for '{}': {}", get_name(),
                              desired, published);
            }
        } else {
            // Neither source could supply an image, and nothing here retries:
            // the next reconcile is whatever the manager publishes or the next
            // filename change. Name both identities, because in a log the
            // resulting symptom - the previous print's image sitting under the
            // correct filename - is otherwise indistinguishable from a fetch
            // that simply has not landed yet (#1339).
            spdlog::debug("[{}] No thumbnail source for '{}': subject holds one for '{}'",
                          get_name(), desired, printer_state_.get_print_thumbnail_file());
        }
    }

    if (action.load_gcode) {
        // Queue the (expensive) gcode download. The deferred timer debounces and
        // load_gcode_file's success callback records gcode_displayed_file_.
        // Schedule immediately when active; otherwise on_activate() runs this again.
        pending_gcode_filename_ = desired;
        if (is_active_) {
            schedule_deferred_gcode_load();
        }
    }
}
