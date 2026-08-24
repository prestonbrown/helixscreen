// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_detail_view.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_filename_utils.h"
#include "ui_gcode_viewer.h"
#include "ui_icon.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_print_preparation_manager.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "color_utils.h"
#include "config.h"
#include "display_settings_manager.h"
#include "gcode_footer_summary.h"
#include "gcode_parser.h"
#include "gcode_preview_setup.h"
#include "gcode_temp_reclaim.h"
#include "host_identity.h"
#include "http_executor.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_utils.h"
#include "moonraker_types.h"
#include "observer_factory.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <fstream>

namespace helix::ui {

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

    // Cancel the pre-flight readiness safety timer if still armed (LVGL is known
    // initialized here — checked above).
    if (preflight_ready_timeout_timer_) {
        lv_timer_delete(preflight_ready_timeout_timer_);
        preflight_ready_timeout_timer_ = nullptr;
    }

    // Unregister from NavigationManager (fallback if cleanup() wasn't called)
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
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
    // Whether the viewer has rendered its first frame (0=no, 1=yes). The
    // thumbnail stays on top until this flips to hide the gray gap.
    UI_MANAGED_SUBJECT_INT(detail_viewer_first_frame_, 0, "detail_viewer_first_frame", subjects_);

    // Preview color mode: 0 = actual (loaded slot colors), 1 = sliced (slicer intent)
    UI_MANAGED_SUBJECT_INT(detail_prefer_sliced_colors_, 0, "detail_prefer_sliced_colors",
                           subjects_);

    // Mapping/swatch readiness (0 = skeleton, 1 = authoritative chips rendered).
    // Mirrors is_preflight_ready() — the same readiness the print-start gate
    // waits on. Cache hit => 1 immediately at show(); else flips when the tools
    // scan or viewer parse completes.
    UI_MANAGED_SUBJECT_INT(detail_mapping_ready_, 0, "detail_mapping_ready", subjects_);

    // Filament mismatch warning (0=hidden, 1=visible)
    UI_MANAGED_SUBJECT_INT(filament_mismatch_, 0, "filament_mismatch", subjects_);

    // Filament mapping card visibility (0=hidden, 1=visible). Driven by
    // FilamentMappingCard::should_show() after each update(); XML binds
    // via bind_flag_if_eq in print_file_detail.xml.
    UI_MANAGED_SUBJECT_INT(filament_mapping_visible_, 0, "filament_mapping_visible", subjects_);

    // Legacy color swatches card visibility (0=hidden, 1=visible). Shown
    // only when the mapping card is NOT visible AND the file is multi-tool.
    // The two subjects are mutually exclusive by construction — see show()
    // and the metadata-derived-colors path.
    UI_MANAGED_SUBJECT_INT(color_swatches_visible_, 0, "color_swatches_visible", subjects_);

    // Empty-tools warning visibility (0=hidden, 1=visible). Set by
    // update_color_swatches() when any T-command-referenced slot is empty.
    UI_MANAGED_SUBJECT_INT(empty_tools_warning_, 0, "empty_tools_warning", subjects_);

    // Pre-print time estimate (formatted string for bind_text)
    UI_MANAGED_SUBJECT_STRING(prep_time_estimate_subject_, prep_time_estimate_buf_, "",
                              "preprint_estimate_text", subjects_);

    // Re-color the preview live when a slot's loaded color/presence changes
    // (filament reloaded). Static singleton subject -> plain ObserverGuard, no
    // lifetime token. Handler no-ops while the view is closed.
    slots_version_observer_ = observe_int_sync<PrintSelectDetailView>(
        AmsState::instance().get_slots_version_subject(), this,
        [](PrintSelectDetailView* self, int /*version*/) { self->on_ams_state_changed(); },
        AmsState::instance().get_subjects_lifetime());

    subjects_initialized_ = true;
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

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "print_file_detail", nullptr));

    if (!overlay_root_) {
        LOG_ERROR_INTERNAL("[DetailView] Failed to create detail view from XML");
        NOTIFY_ERROR(lv_tr("Failed to load file details"));
        return nullptr;
    }

    // Set responsive padding for content area
    lv_obj_t* content_container = lv_obj_find_by_name(overlay_root_, "content_container");
    if (content_container) {
        lv_coord_t padding = ui_get_header_content_padding();
        lv_obj_set_style_pad_all(content_container, padding, 0);
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Store reference to print button for enable/disable state management
    print_button_ = lv_obj_find_by_name(overlay_root_, "print_button");

    // Find and configure G-code viewer widget
    gcode_viewer_ = lv_obj_find_by_name(overlay_root_, "detail_gcode_viewer");
    if (gcode_viewer_) {
        spdlog::debug("[DetailView] G-code viewer widget found");
        ui_gcode_viewer_disable_streaming(gcode_viewer_);

        helix::ui::apply_preview_render_mode(gcode_viewer_, "DetailView");

        // Here the strip IS an overlay over the preview's bottom, so this is a
        // real occlusion (~a third of the card) and the render shifts to clear it.
        helix::ui::set_preview_bottom_occluder(
            gcode_viewer_, lv_obj_find_by_name(overlay_root_, "detail_metadata_clip"));

        // Start paused — will resume in on_activate()
        ui_gcode_viewer_set_paused(gcode_viewer_, true);

        // Memory-pressure responder calls ui_gcode_viewer_clear_all_active();
        // flip the mode subject back to thumbnail so the user sees the slicer
        // preview rather than a transparent rectangle.
        ui_gcode_viewer_set_clear_callback(
            gcode_viewer_,
            [](lv_obj_t*, void* ud) {
                auto* self = static_cast<PrintSelectDetailView*>(ud);
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
        lv_obj_find_by_name(overlay_root_, "pre_print_options_container");

    // Look up the color swatches row container (parent card visibility is
    // driven by the color_swatches_visible subject — no flag manipulation here).
    color_swatches_row_ = lv_obj_find_by_name(overlay_root_, "color_swatches_row");

    // Make the color-requirements card tappable so the U1's visible swatches
    // open the native remap modal — matching the AFC/CFS whole-card click in
    // FilamentMappingCard. The card's children already declare
    // clickable=false + event_bubble=true in print_file_detail.xml (L071), so
    // the parent receives the click. lv_obj_add_event_cb on the card mirrors the
    // sibling FilamentMappingCard pattern (allowed exception). The handler gates
    // on the active backend's remap strategy at click time: it only opens the
    // modal for SnapmakerNative — on other backends this card is informational
    // and a different remap path applies, so the tap is a no-op.
    color_requirements_card_ = lv_obj_find_by_name(overlay_root_, "color_requirements_card");
    if (color_requirements_card_) {
        lv_obj_add_flag(color_requirements_card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            color_requirements_card_,
            [](lv_event_t* e) {
                auto* self = static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e));
                self->on_color_card_clicked();
            },
            LV_EVENT_CLICKED, this);
    }

    // Look up and initialize filament mapping card
    lv_obj_t* mapping_card = lv_obj_find_by_name(overlay_root_, "filament_mapping_card");
    lv_obj_t* mapping_rows = lv_obj_find_by_name(overlay_root_, "filament_mapping_rows");
    lv_obj_t* mapping_warning = lv_obj_find_by_name(overlay_root_, "filament_mapping_warning");
    filament_mapping_card_.create(mapping_card, mapping_rows, mapping_warning);
    // Route the card tap to the panel's single remap opener instead of the
    // card's own internal modal, so there is ONE opener and ONE modal instance
    // for every backend (AFC/CFS card tap, U1 swatch tap, preflight "Remap…"
    // all reach PrintSelectPanel::open_remap_modal()). on_remap_requested_ is
    // wired by the panel in create_detail_view() right after construction, so the
    // null check is just defensive — a tap before wiring is a no-op, not a crash.
    filament_mapping_card_.set_on_tap([this]() {
        if (on_remap_requested_) {
            on_remap_requested_();
        }
    });
    filament_mapping_card_.set_on_mappings_changed([this]() {
        // The card already refreshed its own slot/mapping state from the user's
        // edit — just re-color + re-gate. set_mappings() fires this synchronously
        // on the main thread, so a direct call is safe.
        refresh_preview_colors_and_mismatch();
    });

    // Look up history status display
    history_status_row_ = lv_obj_find_by_name(overlay_root_, "history_status_row");
    history_status_icon_ = lv_obj_find_by_name(overlay_root_, "history_status_icon");
    history_status_label_ = lv_obj_find_by_name(overlay_root_, "history_status_label");

    // Initialize print preparation manager (only if not already created —
    // survives destroy-on-close so callbacks set by PrintSelectPanel persist)
    if (!prep_manager_) {
        prep_manager_ = std::make_unique<PrintPreparationManager>();
    }

    spdlog::debug("[DetailView] Detail view created");
    return overlay_root_;
}

void PrintSelectDetailView::set_dependencies(IMoonrakerAPI* api, PrinterState* printer_state) {
    api_ = api;
    printer_state_ = printer_state;

    // Ask once, well before the user opens a file, and never block on the
    // answer. A printer that never replies simply leaves us on the HTTP path.
    resolve_local_gcodes_root();

    if (prep_manager_) {
        prep_manager_->set_dependencies(api_, printer_state_);
        // Per-option toggle state flows through the OptionStateProvider that
        // populate_option_rows() registers with the prep manager — no need to
        // wire individual legacy state/visibility subjects here.
    }
}

// ============================================================================
// Visibility
// ============================================================================

void PrintSelectDetailView::show(const std::string& filename, const std::string& current_path,
                                 const std::string& filament_type,
                                 const std::vector<std::string>& filament_colors,
                                 const std::vector<std::string>& filament_materials,
                                 size_t file_size_bytes, time_t modified_timestamp,
                                 uint64_t gcode_end_byte) {
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

    // Cache parameters for on_activate() to use
    current_filename_ = filename;
    current_path_ = current_path;
    current_filament_type_ = filament_type;
    current_filament_colors_ = filament_colors;
    current_filament_materials_ = filament_materials;
    current_file_size_bytes_ = file_size_bytes;
    current_file_modified_ = modified_timestamp;
    current_gcode_end_byte_ = gcode_end_byte;

    // Clear cached metadata when file selection changes — the new async fetch will repopulate it
    cached_file_metadata_.reset();

    // Clear any used-tools filter carried over from the previously-selected
    // file BEFORE the pre-parse update() below. The card is repopulated from
    // this file's full palette and must show all tools until this file's own
    // gcode parse pushes its real used set (viewer-parse or headless hook).
    // Without this reset a disjoint prior set would mis-filter — or blank — the
    // new file's card pre-parse.
    filament_mapping_card_.set_used_tools(std::nullopt);

    // Update filament mapping card (shown when AMS is available)
    filament_mapping_card_.update(filament_colors, filament_materials);

    // Publish the mapping-card display subjects. The mapping card's visibility
    // depends only on its own state (AMS presence + slicer colors), so it can
    // be decided here. The swatches card, by contrast, must reflect the real
    // per-tool set used by the file — which is only known once the gcode is
    // parsed.
    const bool mapping_visible = filament_mapping_card_.should_show();
    lv_subject_set_int(&filament_mapping_visible_, mapping_visible ? 1 : 0);

    // Swatches start in a neutral "not yet known" state: hidden, no warning.
    // Seeding from the slicer palette index here mislabels chips (a T0+T2 file
    // renders as T0/T1) and inspects the wrong AMS slots. The authoritative
    // render happens in try_extract_gcode_colors() once the gcode viewer has
    // parsed and produced tools_used_indices. Reset every show() so re-selecting
    // a different file never leaks stale swatch state.
    //
    // filament_mismatch_ is likewise neutral-until-parse: seeding it from
    // filament_mapping_card_.has_mismatch() here would flash a value computed
    // against the full slicer palette before the validator runs against the
    // precise tools_used set. The pre-flight validator in
    // try_extract_gcode_colors() is the sole authoritative post-parse writer.
    lv_subject_set_int(&color_swatches_visible_, 0);
    lv_subject_set_int(&empty_tools_warning_, 0);
    lv_subject_set_int(&filament_mismatch_, 0);
    lv_subject_set_int(&detail_prefer_sliced_colors_, 0); // every open starts on actual colors

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
    headless_scan_done_ = false;
    headless_scan_settled_ = false;
    lv_subject_set_int(&detail_mapping_ready_, 0);

    if (auto cached = tools_used_cache_.lookup(current_file_key(), current_file_size_bytes_,
                                               current_file_modified_)) {
        headless_tools_used_ = *cached;
        headless_scan_done_ = true;    // readiness now true; the scan below is skipped
        headless_scan_settled_ = true; // the cached answer IS the settled answer
        spdlog::debug("[DetailView] Tools-used cache hit ({} tools)", cached->size());

        // Seed the authoritative render from the cached set — the same shape
        // as finish_scan()'s !is_gcode_loaded() branch, which the skipped scan
        // would have run. Without this, a reprint's first frame would show the
        // full slicer palette (mapping pills) / no swatches at all until the
        // viewer parse lands — and on Thumbnail-Only / parse-fallback opens,
        // where the parse never fires, the swatch card would never appear.
        render_authoritative_chips(tools_used_effective());
    }

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Register close callback to destroy widget tree when overlay closes.
    // Frees memory when detail view is dismissed. Subjects survive;
    // next show() call re-creates widgets via lazy creation above.
    NavigationManager::instance().register_overlay_close_callback(
        overlay_root_, [this]() { destroy_overlay_ui(overlay_root_); });

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    if (visible_subject_) {
        lv_subject_set_int(visible_subject_, 1);
    }

    // Publish readiness last: 1 when the cache seeded the chip state above,
    // 0 on a miss (skeleton until the scan/parse flips it). Still within this
    // show() call, so the first frame never sees a stale value.
    publish_mapping_ready();

    spdlog::debug("[DetailView] Showing detail view for: {} ({} colors)", filename,
                  filament_colors.size());
}

void PrintSelectDetailView::hide() {
    if (!overlay_root_) {
        return;
    }

    // Pop from navigation stack - on_deactivate() will be called by NavigationManager
    NavigationManager::instance().go_back();

    if (visible_subject_) {
        lv_subject_set_int(visible_subject_, 0);
    }

    spdlog::debug("[DetailView] Detail view hidden");
}

// ============================================================================
// Shared G-code Download (one file + one download for scan + viewer preview)
// ============================================================================

std::string PrintSelectDetailView::canonical_gcode_path() const {
    // Hash the FULL relative path (not just the filename) so same-name files
    // in different directories never collide on one temp file.
    return get_helix_cache_dir("gcode_temp") + "/detail_" +
           std::to_string(std::hash<std::string>{}(current_file_key())) + ".gcode";
}

void PrintSelectDetailView::resolve_local_gcodes_root() {
    if (local_gcodes_root_resolved_ || !api_) {
        return;
    }

    // Only worth asking when Moonraker is this machine. A remote printer's
    // absolute root path names a filesystem we cannot see, and reading a local
    // path that happens to match would be reading the wrong file entirely.
    std::string host;
    if (Config* cfg = Config::get_instance()) {
        host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
    }
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
    if (local_gcodes_root_.empty()) {
        return {};
    }
    const std::string candidate = local_gcodes_root_ + "/" + current_file_key();

    // Size is the same staleness check the cached-copy path uses. It also
    // doubles as the existence and readability probe: a file we cannot open is
    // one we must fetch over HTTP instead.
    std::ifstream f(candidate, std::ios::binary | std::ios::ate);
    if (!f) {
        spdlog::debug("[DetailView] No local G-code at '{}' — falling back to HTTP", candidate);
        return {};
    }
    const auto on_disk_bytes = static_cast<size_t>(f.tellg());
    if (on_disk_bytes == 0) {
        return {};
    }
    if (current_file_size_bytes_ != 0 && on_disk_bytes != current_file_size_bytes_) {
        spdlog::warn("[DetailView] Local G-code size mismatch (disk={}, expected={}) — "
                     "falling back to HTTP",
                     on_disk_bytes, current_file_size_bytes_);
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

std::string PrintSelectDetailView::current_file_key() const {
    return current_path_.empty() ? current_filename_ : current_path_ + "/" + current_filename_;
}

void PrintSelectDetailView::ensure_gcode_downloaded(
    std::function<void(bool ok, const std::string& path)> cb) {
    if (!api_ || get_helix_cache_dir("gcode_temp").empty()) {
        spdlog::warn("[DetailView] No API or cache dir for shared G-code download");
        cb(false, {});
        return;
    }
    // 0. Moonraker runs here, so its copy IS the file — no transfer, no second
    //    copy on the same flash. This only CONSULTS the answer; the resolve is
    //    kicked once from set_dependencies() and never awaited here. Awaiting it
    //    would hang this load outright on any Moonraker that does not answer
    //    server.files.roots — which includes older forks and our own mock
    //    client, neither of which invokes either callback. The path deliberately
    //    never becomes temp_gcode_path_: it is the user's print file, not
    //    something we may delete (reclaim_download() refuses it too).
    if (const std::string local = local_gcode_source(); !local.empty()) {
        spdlog::info("[DetailView] Using Moonraker's own G-code in place: {}", local);
        cb(true, local);
        return;
    }

    const std::string path = canonical_gcode_path();

    // 1. A transfer is already running — join it. Checked BEFORE the disk
    //    probe: the in-flight file is partially written, and a non-empty
    //    tellg() on it must not be mistaken for a complete copy.
    if (gcode_download_in_flight_) {
        gcode_download_waiters_.push_back(std::move(cb));
        return;
    }

    // 2. Already on disk (cached from a previous open of this file). When the
    //    expected size is known, a mismatch means the local copy is stale or
    //    partial — the server file was re-sliced onto the same path, or the
    //    app died mid-transfer and left a truncated download behind. Trusting
    //    those bytes would scan the OLD file and store the wrong tool set
    //    under the NEW (size, mtime) cache key, so drop the copy and
    //    re-download instead of scanning stale bytes.
    if (std::ifstream f(path, std::ios::binary | std::ios::ate); f && f.tellg() > 0) {
        const auto on_disk_bytes = static_cast<size_t>(f.tellg());
        if (current_file_size_bytes_ == 0 || on_disk_bytes == current_file_size_bytes_) {
            cb(true, path);
            return;
        }
        spdlog::warn("[DetailView] Cached G-code size mismatch (disk={}, expected={}) - "
                     "re-downloading",
                     on_disk_bytes, current_file_size_bytes_);
        reclaim_download(path);
        // Fall through to a fresh transfer below.
    }

    // 3. Start the single shared transfer; later callers join via 1.
    gcode_download_in_flight_ = true;
    gcode_download_waiters_.push_back(std::move(cb));
    const std::string file_path =
        current_path_.empty() ? current_filename_ : current_path_ + "/" + current_filename_;
    auto tok = lifetime_.token();
    api_->transfers().download_file_to_path(
        "gcodes", file_path, path,
        [this, tok](const std::string& local) {
            // HTTP thread — marshal member writes + waiter fan-out to the
            // main thread (no bg-thread `this` access, L081 Mechanism C).
            tok.defer("DetailView::gcode_shared_download_done", [this, local]() {
                // Retire the previous file's temp copy (kept from the old
                // pre-download cleanup) and adopt this one for teardown.
                if (!temp_gcode_path_.empty() && temp_gcode_path_ != local) {
                    reclaim_download(temp_gcode_path_);
                }
                temp_gcode_path_ = local;
                gcode_download_in_flight_ = false;
                auto waiters = std::move(gcode_download_waiters_);
                gcode_download_waiters_.clear();
                for (auto& w : waiters)
                    w(true, local);
            });
        },
        [this, tok, path](const MoonrakerError& err) {
            tok.defer("DetailView::gcode_shared_download_fail", [this, err, path]() {
                spdlog::warn("[DetailView] Shared G-code download failed: {}", err.message);
                // Drop any partial file the failed transfer left behind so a
                // later open doesn't mistake it for a complete cached copy.
                reclaim_download(path);
                gcode_download_in_flight_ = false;
                auto waiters = std::move(gcode_download_waiters_);
                gcode_download_waiters_.clear();
                for (auto& w : waiters)
                    w(false, {});
            });
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
    // Idempotent — only rebuilds when the printer type has changed.
    populate_option_rows();

    // Cache file size for safety checks (before modification attempts)
    if (prep_manager_ && current_file_size_bytes_ > 0) {
        prep_manager_->set_cached_file_size(current_file_size_bytes_);
    }

    // Ask the active AMS backend to refresh its slot/state view. Lets users
    // self-recover from any drift between cached UI state and printer truth
    // by navigating away and back. Default backend impl is a no-op; AD5X IFS
    // re-reads Adventurer5M.json + GET_ZCOLOR. Debounced internally.
    if (auto* backend = AmsState::instance().get_backend()) {
        backend->request_resync();
    }

    // Trigger async scan for embedded G-code operations (for conflict detection)
    // The scan happens NOW after registration, so if user navigates away,
    // on_deactivate() will be called and we can check cleanup_called()
    if (!current_filename_.empty() && prep_manager_) {
        prep_manager_->scan_file_for_operations(current_filename_, current_path_);
    }

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

void PrintSelectDetailView::on_deactivate() {
    spdlog::debug("[DetailView] on_deactivate()");

    // Clear and pause gcode viewer immediately so the old model doesn't
    // linger when the user selects a different file
    if (gcode_viewer_) {
        ui_gcode_viewer_clear(gcode_viewer_);
        ui_gcode_viewer_set_paused(gcode_viewer_, true);
    }

    // Reset viewer mode to thumbnail so next open starts clean
    show_gcode_viewer(false);
    lv_subject_set_int(&detail_viewer_first_frame_, 0);
    gcode_loaded_ = false;
    // Readiness drops with the view — headless_scan_done_ goes false too, so
    // the publish below resolves to 0 and the skeleton latch re-arms. show()
    // re-seeds (cache) / re-runs (scan) it on the next open. The settled flag
    // follows: a late finish_scan from a still-running worker is invalidated
    // with the view's lifetime token anyway, and the next open must not treat
    // the previous open's settlement as authorization to delete files.
    headless_scan_done_ = false;
    headless_scan_settled_ = false;
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

    // Hide any open delete confirmation modal
    hide_delete_confirmation();

    // Note: We don't cancel scans here because PrintPreparationManager
    // has its own lifetime guard. Async callbacks in prep_manager_
    // will check cleanup_called() if needed.

    // Call base class
    OverlayBase::on_deactivate();
}

void PrintSelectDetailView::cleanup() {
    spdlog::debug("[DetailView] cleanup()");

    // Pause viewer before subject cleanup to avoid rendering with freed subjects.
    // Drop the first-frame callback too: it captures `this` and writes
    // detail_viewer_first_frame_, which subjects_.deinit_all() below destroys.
    // The viewer outlives this object on some teardown paths, and paused
    // rendering is not a guarantee — unpausing anywhere else would resurrect it.
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, true);
        ui_gcode_viewer_set_first_frame_callback(gcode_viewer_, nullptr, nullptr);
    }

    // Expire all outstanding async tokens
    lifetime_.invalidate();

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

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

void PrintSelectDetailView::on_ui_destroyed() {
    spdlog::debug("[DetailView] on_ui_destroyed() - nulling widget pointers");

    // Invalidate outstanding tokens so in-flight async callbacks (gcode download,
    // metadata fetch, load callbacks) bail out — they captured pointers to
    // widgets that no longer exist (e.g. gcode_viewer_).
    // New tokens from lifetime_.token() will be valid for the next create() cycle.
    lifetime_.invalidate();

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

    // A shared download still in flight for the destroyed session can no
    // longer deliver (its deferred completion was dropped with the token
    // above) and leaves a partial file behind. Drop the waiters so a fresh
    // open starts a new transfer instead of joining a dead one, and remove
    // the canonical file — temp_gcode_path_ above only tracks a COMPLETED
    // download, so the in-flight partial needs its own removal.
    gcode_download_in_flight_ = false;
    gcode_download_waiters_.clear();
    reclaim_download(canonical_gcode_path());

    // Null all child widget pointers (widget tree already deleted by base class)
    // Note: parent_screen_ is NOT nulled — it's the parent screen (not a child
    // widget) and is needed for lazy re-creation in show().
    confirmation_dialog_widget_ = nullptr;
    print_button_ = nullptr;
    gcode_viewer_ = nullptr;

    // Pre-print option checkboxes (kept as inert fields; see create()).
    bed_mesh_checkbox_ = nullptr;
    qgl_checkbox_ = nullptr;
    z_tilt_checkbox_ = nullptr;
    nozzle_clean_checkbox_ = nullptr;
    purge_line_checkbox_ = nullptr;
    timelapse_checkbox_ = nullptr;

    // The dynamic option rows were children of overlay_root_, which has been
    // destroyed by the base class. Drop the renderer's row state and force a
    // rebuild on next show(). Subjects inside the renderer are heap-owned —
    // their observers were attached to the now-deleted row widgets, so
    // dropping the subjects here is safe.
    pre_print_options_container_ = nullptr;
    option_rows_renderer_.clear();
    last_rendered_printer_type_.clear();
    if (prep_manager_) {
        prep_manager_->set_option_state_provider(nullptr);
    }

    color_swatches_row_ = nullptr;
    color_requirements_card_ = nullptr;

    // Filament mapping card
    filament_mapping_card_.on_ui_destroyed();

    // History status display
    history_status_row_ = nullptr;
    history_status_icon_ = nullptr;
    history_status_label_ = nullptr;

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

    confirmation_dialog_widget_ = helix::ui::modal_show_confirmation(
        lv_tr("Delete File?"), msg_buf, ModalSeverity::Warning, lv_tr("Delete"),
        on_confirm_delete_static, on_cancel_delete_static, this);

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
    if (!overlay_root_ || !parent_screen) {
        return;
    }

    lv_obj_t* content_container = lv_obj_find_by_name(overlay_root_, "content_container");
    if (content_container) {
        lv_coord_t padding = ui_get_header_content_padding();
        lv_obj_set_style_pad_all(content_container, padding, 0);
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void PrintSelectDetailView::on_confirm_delete_static(lv_event_t* e) {
    auto* self = static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e));
    if (self) {
        self->hide_delete_confirmation();
        if (self->on_delete_confirmed_) {
            self->on_delete_confirmed_();
        }
    }
}

void PrintSelectDetailView::on_cancel_delete_static(lv_event_t* e) {
    auto* self = static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e));
    if (self) {
        self->hide_delete_confirmation();
    }
}

void PrintSelectDetailView::update_color_swatches(const std::set<int>& tool_indices,
                                                  const std::vector<std::string>& palette_colors) {
    // TWO-TONE chip: the TOP band shows the GCODE FILE intended color (slicer
    // palette) + the gcode tool number ("T0", "T2"); the BOTTOM band shows the
    // ACTUAL present color of the EFFECTIVE mapped slot + that slot's lane
    // number ("1", "4"). A thin divider separates them. We source the mapped
    // slot from the resolved tool→slot mapping (NOT backend->get_slot_info(),
    // since slot != tool on multi-unit backends like the U1). This mirrors
    // recompute_preflight() / open_remap_modal: use the card's current mappings
    // if present (so chips reflect a user edit after Done), else compute the
    // defaults — the same effective mapping the remap dialog shows.
    //
    // No-backend / palette-only: collect_available_slots() is empty and
    // compute_defaults yields mapped_slot < 0, so no lane resolves — the bottom
    // band stays blank and the chip is effectively just the gcode-color top band.
    if (!color_swatches_row_) {
        return;
    }

    helix::ui::safe_clean_children(color_swatches_row_);

    const auto tools = get_used_tool_info(); // real tool_index, intended colors
    auto slots = AmsState::instance().collect_available_slots();
    // Effective (toggle-aware) mapping: card edits win on editable backends;
    // U1/ACE auto-match so the bottom band shows the matched lane, not identity.
    auto mappings = effective_mappings();

    const lv_color_t neutral = theme_manager_get_color("text_muted");

    for (int tool : tool_indices) {
        // Resolve this tool's effective mapping → present slot.
        const helix::AvailableSlot* resolved = nullptr;
        for (const auto& m : mappings) {
            if (m.tool_index != tool) {
                continue;
            }
            if (!m.is_auto && m.mapped_slot >= 0) {
                for (const auto& s : slots) {
                    if (s.slot_index == m.mapped_slot && s.backend_index == m.mapped_backend) {
                        resolved = &s;
                        break;
                    }
                }
            }
            break;
        }

        // TOP band: gcode intended color from the slicer palette. Falls back to
        // the neutral chip default when the palette has no entry for this tool.
        lv_color_t gcode_color = neutral;
        if (tool >= 0 && static_cast<size_t>(tool) < palette_colors.size() &&
            !palette_colors[tool].empty()) {
            gcode_color = theme_manager_parse_hex_color(palette_colors[tool].c_str());
        }

        // BOTTOM band: present color of the effective mapped slot.
        lv_color_t slot_color = neutral;
        bool have_slot_color = false;
        bool slot_is_empty = false;
        std::string slot_number_text;

        if (resolved) {
            slot_number_text = std::to_string(resolved->local_slot_index + 1);
            if (resolved->is_empty) {
                // Mapped to an empty lane — show which lane it routes to but
                // render the empty-slot variant on the bottom band.
                slot_is_empty = true;
            } else {
                slot_color = lv_color_hex(resolved->color_rgb);
                have_slot_color = true;
            }
        }
        // No resolved lane (auto/unmapped or no backend): bottom band stays
        // blank (no number, default fill) — chip reads as just the gcode top.

        auto* swatch =
            static_cast<lv_obj_t*>(lv_xml_create(color_swatches_row_, "filament_swatch", nullptr));
        if (!swatch) {
            continue;
        }
        // Fix the chip width in code: a numeric width on the component <view>
        // root is not honored by lv_xml_create (only "content"/"%"), and the
        // band labels use flex_grow (which contributes 0 to content-width), so
        // without an explicit width the whole chip collapses to 0. A uniform
        // width lets both bands cross-stretch to the same width (connected,
        // full-bleed) and centers the labels. Height comes from the 32px parent
        // row via the XML height="100%". Tunable.
        lv_obj_set_width(swatch, 40);

        // Top band fill + label.
        if (auto* top_band = lv_obj_find_by_name(swatch, "top_band")) {
            lv_obj_set_style_bg_color(top_band, gcode_color, 0);
            if (auto* tool_label = lv_obj_find_by_name(top_band, "tool_label")) {
                lv_label_set_text_fmt(tool_label, "T%d", tool);
                lv_obj_set_style_text_color(tool_label,
                                            theme_manager_get_contrast_color(gcode_color), 0);
            }
        }

        // Bottom band fill (or empty variant) + lane number.
        lv_color_t bottom_text_color = slot_is_empty ? theme_manager_get_color("warning")
                                                     : theme_manager_get_contrast_color(slot_color);
        if (auto* bottom_band = lv_obj_find_by_name(swatch, "bottom_band")) {
            if (slot_is_empty) {
                lv_obj_add_state(bottom_band, LV_STATE_USER_1);
            } else if (have_slot_color) {
                lv_obj_set_style_bg_color(bottom_band, slot_color, 0);
            }
            if (auto* slot_label = lv_obj_find_by_name(bottom_band, "slot_label")) {
                lv_label_set_text(slot_label, slot_number_text.c_str());
                lv_obj_set_style_text_color(slot_label, bottom_text_color, 0);
            }
        }

        // Divider: a color that reads against BOTH band fills. Blend the two
        // band colors (50/50) and take the contrast of the blend, so the rule
        // stays visible whether the bands are light, dark, or mixed.
        if (auto* divider = lv_obj_find_by_name(swatch, "divider")) {
            lv_color_t blend = lv_color_mix(gcode_color, slot_color, LV_OPA_50);
            lv_obj_set_style_bg_color(divider, theme_manager_get_contrast_color(blend), 0);
        }
    }

    // Note: empty_tools_warning_ is published by the pre-flight validator in
    // try_extract_gcode_colors() (the single source of truth), NOT here — this
    // method only renders swatches.
}

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
        ui_icon_set_source(history_status_icon_, "clock");
        ui_icon_set_variant(history_status_icon_, "accent");
        lv_label_set_text(history_status_label_, lv_tr("Currently printing"));
        break;

    case FileHistoryStatus::COMPLETED: {
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set_source(history_status_icon_, "check");
        ui_icon_set_variant(history_status_icon_, "success");
        // Format: "Printed N time(s)"
        char buf[64];
        snprintf(buf, sizeof(buf),
                 lv_tr(success_count == 1 ? "Printed %d time" : "Printed %d times"), success_count);
        lv_label_set_text(history_status_label_, buf);
        break;
    }

    case FileHistoryStatus::FAILED:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set_source(history_status_icon_, "alert");
        ui_icon_set_variant(history_status_icon_, "error");
        lv_label_set_text(history_status_label_, lv_tr("Last print failed"));
        break;

    case FileHistoryStatus::CANCELLED:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set_source(history_status_icon_, "cancel");
        ui_icon_set_variant(history_status_icon_, "warning");
        lv_label_set_text(history_status_label_, lv_tr("Last print cancelled"));
        break;
    }
}

// ============================================================================
// G-code Viewer
// ============================================================================

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
        const bool is_2d = gcode_viewer_ && ui_gcode_viewer_is_using_2d_mode(gcode_viewer_);
        mode = is_2d ? 2 : 1;
    }
    lv_subject_set_int(&detail_gcode_viewer_mode_, mode);

    // Returning to thumbnail mode must also drop the first-frame latch — that
    // latch is what keeps the thumbnail hidden once the viewer has painted
    // (print_file_detail.xml binds detail_viewer_first_frame). Leaving it set
    // while hiding the viewer (the memory-pressure clear callback does exactly
    // that, mid-view) hides BOTH layers and the preview goes blank.
    if (mode == 0) {
        lv_subject_set_int(&detail_viewer_first_frame_, 0);
    }

    // The 3D render is the preview when the viewer is active, so the
    // no-thumbnail placeholder glyph must not sit on top of it. (When the
    // viewer is inactive the print-select panel's has-thumbnail logic owns
    // whether the placeholder shows.)
    if (mode > 0 && overlay_root_) {
        lv_obj_t* no_thumb = lv_obj_find_by_name(overlay_root_, "detail_no_thumbnail_icon");
        if (no_thumb) {
            lv_obj_add_flag(no_thumb, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide loading spinner now that viewer state is resolved
    lv_subject_set_int(&detail_gcode_loading_, 0);

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
        ui_gcode_viewer_set_tool_colors(gcode_viewer_, colors);
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
    // Cheap guard: no work while closed / not yet loaded. on_deactivate() clears
    // gcode_loaded_ and on_ui_destroyed() nulls gcode_viewer_, so this also
    // protects against a dangling viewer pointer after the view is torn down.
    if (!is_visible() || !gcode_loaded_ || !gcode_viewer_) {
        return;
    }
    // Refresh loaded slot colors WITHOUT recomputing mappings (preserve remap).
    filament_mapping_card_.refresh_slot_data();
    refresh_preview_colors_and_mismatch();
}

void PrintSelectDetailView::refresh_preview_colors_and_mismatch() {
    apply_preview_colors();
    lv_subject_set_int(&filament_mismatch_, filament_mapping_card_.has_mismatch() ? 1 : 0);
    // Re-evaluate the pre-flight gate so a subsequent Print reflects the current
    // tool->slot mapping (native remap flow reads get_filament_mappings()).
    recompute_preflight();
    // Re-render the FILAMENTS chips so slot number + present color track the
    // current mapping/slot state.
    if (lv_subject_get_int(&color_swatches_visible_) == 1) {
        update_color_swatches(tools_used_effective(), current_filament_colors_);
    }
}

void PrintSelectDetailView::try_extract_gcode_colors(lv_obj_t* viewer) {
    auto* parsed = ui_gcode_viewer_get_parsed_file(viewer);
    if (!parsed) {
        return;
    }

    // Backfill filament_colors when slicer metadata didn't provide them
    // (Snapmaker and a few other Moonraker variants).
    if (current_filament_colors_.empty() && !parsed->tool_color_palette.empty()) {
        spdlog::info(
            "[DetailView] Metadata lacked filament colors — extracted {} from parsed gcode",
            parsed->tool_color_palette.size());
        current_filament_colors_ = parsed->tool_color_palette;

        // Rebuild the mapping card's internal tool/slot/mapping state with the
        // newly-extracted colors (the card uses them when AMS is present). The
        // filament_mismatch_ / empty_tools_warning_ subjects are NOT published
        // here — the pre-flight validator below is the single source of truth.
        filament_mapping_card_.update(current_filament_colors_, current_filament_materials_);
    }

    // Re-publish the mapping card's own visibility: the pre-parse publish in
    // show() predates the palette backfill above, which can flip should_show().
    lv_subject_set_int(&filament_mapping_visible_, filament_mapping_card_.should_show() ? 1 : 0);

    // Authoritative render from the precise tools_used set — not the slicer
    // palette size (which often over-counts). tools_used_effective() returns
    // exactly parsed->tools_used_indices here (same viewer, non-empty set);
    // it only differs if the parse yielded nothing, in which case the headless
    // scan's answer is the one worth rendering.
    render_authoritative_chips(tools_used_effective());

    // Write-through: the parse just made the used set final for this
    // (path, size, mtime) — persist so the next open of this file renders
    // final chips instantly from the cache instead of the skeleton.
    tools_used_cache_.store(current_file_key(), current_file_size_bytes_, current_file_modified_,
                            tools_used_effective());
}

std::vector<helix::GcodeToolInfo> PrintSelectDetailView::get_used_tool_info() const {
    // Source per-tool info DIRECTLY from the slicer palette (Moonraker metadata,
    // populated on ALL platforms) via the stateless assembler — NOT from the
    // mapping card INSTANCE, whose tool_info_ is empty on the U1/headless path.
    const auto all_tool_info =
        FilamentMappingCard::build_tool_info(current_filament_colors_, current_filament_materials_);
    const std::set<int> used = tools_used_effective();

    std::vector<helix::GcodeToolInfo> tools;
    tools.reserve(used.size());
    for (int tool : used) {
        if (tool >= 0 && static_cast<size_t>(tool) < all_tool_info.size()) {
            auto info = all_tool_info[static_cast<size_t>(tool)];
            info.tool_index = tool; // real gcode tool number, not palette ordinal
            tools.push_back(info);
        }
    }
    return tools;
}

bool PrintSelectDetailView::effective_auto_match() const {
    // Non-editable-card backends (U1 / ACE) have no card UI to flip the
    // persisted auto-color preference, so they always auto-match (color+type);
    // otherwise the persisted default (FALSE) would force positional matching
    // and pick the wrong lane. Editable backends honor the user's setting.
    bool card_editable = false;
    if (auto* backend = AmsState::instance().get_backend()) {
        card_editable = backend->get_tool_mapping_capabilities().editable;
    }
    return !card_editable || SettingsManager::instance().get_auto_color_map();
}

std::vector<helix::ToolMapping> PrintSelectDetailView::effective_mappings() const {
    // Editable backends: the card seeds and owns mappings_, and user edits win.
    auto m = filament_mapping_card_.get_mappings();
    if (!m.empty()) {
        return m;
    }
    // Non-editable backends (U1 / ACE): the card is hidden and get_mappings() is
    // empty — resolve the effective (toggle-aware) mapping the same way the live
    // render does, so swatches + preflight + render all agree.
    return helix::FilamentMapper::effective_mappings(get_used_tool_info(),
                                                     AmsState::instance().collect_available_slots(),
                                                     effective_auto_match());
}

void PrintSelectDetailView::render_authoritative_chips(const std::set<int>& tools_used,
                                                       bool refresh_card_from_palette) {
    // Swatch-card visibility is decided against the PRECISE used-tool set, not
    // the slicer palette size (which over-counts), and only when the mapping
    // card is not already showing the same information.
    const bool mapping_visible = filament_mapping_card_.should_show();
    const bool swatches_visible = !mapping_visible && swatches_card_visible_for(tools_used.size());
    lv_subject_set_int(&color_swatches_visible_, swatches_visible ? 1 : 0);
    if (swatches_visible) {
        update_color_swatches(tools_used, current_filament_colors_);
    }

    // Headless path only (see finish_scan()). Sequenced AFTER the swatch render
    // — which resolves lanes through the card's CURRENT, possibly user-edited,
    // mappings — and BEFORE the gate, which must validate against the rebuilt
    // ones.
    if (refresh_card_from_palette) {
        filament_mapping_card_.update(current_filament_colors_, current_filament_materials_);
    }

    // Backend-agnostic pre-flight validation (single source of truth for
    // filament_mismatch_ + empty_tools_warning_).
    recompute_preflight();

    // Restrict the mapping card to the tools this file actually uses — it was
    // populated from the full slicer palette. Empty/unknown => show all (safe
    // default). Last, and order-independent with respect to the gate above:
    // set_used_tools only DROPS mapping entries whose tool_index is outside
    // tools_used, and the validator only ever looks mappings up by the
    // tool_index of a tool in get_used_tool_info(), which is filtered by the
    // same set.
    filament_mapping_card_.set_used_tools(tools_used);
}

void PrintSelectDetailView::recompute_preflight() {
    // ------------------------------------------------------------------
    // Backend-agnostic pre-flight validation (single source of truth for
    // filament_mismatch_ + empty_tools_warning_).
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

    auto slots = AmsState::instance().collect_available_slots();

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
    const bool bypass_active = AmsState::instance().any_bypass_active();

    auto mapping = effective_mappings();
    preflight_result_ = helix::PreflightValidator::validate(tools, slots, mapping, bypass_active);

    bool any_mismatch = false;
    for (const auto& check : preflight_result_.checks) {
        if (check.severity != helix::ToolCheck::Severity::Ok) {
            any_mismatch = true;
            break;
        }
    }
    lv_subject_set_int(&filament_mismatch_, any_mismatch ? 1 : 0);
    lv_subject_set_int(&empty_tools_warning_, preflight_result_.has_block() ? 1 : 0);

    spdlog::debug(
        "[DetailView] Preflight: {} tools, {} slots, {} checks, mismatch={}, block={}, bypass={}",
        tools.size(), slots.size(), preflight_result_.checks.size(), any_mismatch,
        preflight_result_.has_block(), bypass_active);
}

std::set<int> PrintSelectDetailView::tools_used_effective() const {
    // Prefer the visual viewer's parsed set (full platforms): it carries the
    // single-extruder {0} convention from a color palette. Fall back to the
    // headless scan (2D-only platforms, where the viewer never parses).
    if (gcode_viewer_) {
        if (auto* parsed = ui_gcode_viewer_get_parsed_file(gcode_viewer_)) {
            if (!parsed->tools_used_indices.empty()) {
                return parsed->tools_used_indices;
            }
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
    // default_head(t): the physical head a logical tool routes to with no remap.
    // Tools 0..3 map to their identity head; anything else falls back to head 0.
    auto default_head = [](int tool) { return (tool >= 0 && tool <= 3) ? tool : 0; };

    std::map<int, int> remap;
    // Source the mappings from effective_mappings() — the SAME toggle-aware match
    // the render, swatches, and pre-flight use. On editable backends this is the
    // card's mappings (user edits win); on non-editable backends (Snapmaker U1 /
    // ACE), where the card is hidden and get_mappings() is empty, it is the auto
    // color+type match. This is what turns the U1's emitted SET_PRINT_EXTRUDER_MAP
    // from an empty identity into the real per-tool routing (each logical tool to
    // the physical head holding its matched filament). mapped_slot is the physical
    // head 0..3 on the U1 (slot_index == head).
    for (const auto& m : effective_mappings()) {
        // Only include genuine remaps: a real slot assignment that differs from
        // the firmware-default head for this tool. Identity mappings are omitted
        // (the firmware already routes them).
        if (m.mapped_slot >= 0 && m.mapped_slot != default_head(m.tool_index)) {
            remap[m.tool_index] = m.mapped_slot;
        }
    }
    return remap;
}

void PrintSelectDetailView::set_filament_mappings(std::vector<helix::ToolMapping> mappings) {
    spdlog::debug("[DetailView] set_filament_mappings: {} mapping(s)", mappings.size());
    filament_mapping_card_.set_mappings(std::move(mappings));
}

void PrintSelectDetailView::open_filament_mapping_modal() {
    filament_mapping_card_.open_mapping_modal();
}

void PrintSelectDetailView::on_color_card_clicked() {
    // The color-requirements swatch card is the visible remap entry point on
    // backends whose editable FilamentMappingCard is hidden (e.g. Snapmaker U1).
    // Fire the panel's unified remap opener for ANY backend that supports remap
    // (strategy != None); the panel opener itself guards plugin presence etc.
    // On a non-remappable backend (None) the tap is a deliberate no-op.
    auto* backend = AmsState::instance().get_backend();
    if (!backend || backend->get_remap_strategy() == AmsBackend::RemapStrategy::None) {
        return;
    }
    spdlog::debug("[PrintSelect] swatch tap -> remap modal");
    if (on_remap_requested_) {
        on_remap_requested_();
    }
}

void PrintSelectDetailView::run_when_loaded(std::function<void()> cb) {
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

void PrintSelectDetailView::run_when_preflight_ready(std::function<void()> cb) {
    if (!cb) {
        return;
    }
    // Already ready (viewer parsed or headless scan done): run synchronously.
    if (is_preflight_ready()) {
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
            self->fire_on_preflight_ready();
        },
        PREFLIGHT_READY_TIMEOUT_MS, this);
    lv_timer_set_repeat_count(preflight_ready_timeout_timer_, 1);
}

void PrintSelectDetailView::fire_on_preflight_ready() {
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

void PrintSelectDetailView::publish_mapping_ready() {
    lv_subject_set_int(&detail_mapping_ready_, is_preflight_ready() ? 1 : 0);
}

void PrintSelectDetailView::finish_scan(LifetimeToken tok, std::set<int> tools,
                                        bool authoritative) {
    // Marshals the final state back to the main thread (LVGL + member
    // writes). Callable from any thread — `this` is only dereferenced inside
    // the deferred body.
    tok.defer("DetailView::headless_scan_finish",
              [this, tools = std::move(tools), authoritative]() mutable {
                  apply_scan_result(std::move(tools), authoritative);
              });
}

void PrintSelectDetailView::apply_scan_result(std::set<int> tools, bool authoritative) {
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

    // Write-through, but ONLY when the scan actually read the file
    // (authoritative): the scan just made the used set final for this
    // (path, size, mtime) — persist so the next open of this file renders
    // final chips instantly from the cache. A degraded finish (download
    // failed) carries no result at all — persisting its empty set would
    // cache "no tools" as final truth, and on 2D-only platforms where no
    // viewer parse ever repairs it the file would show all-tool pills
    // forever. A SUCCESSFUL scan that found zero tools is a legitimate
    // single-extruder answer and IS persisted. tools_used_effective()
    // prefers the viewer's parsed set when it exists (same file, same
    // answer), so this is correct on both paths.
    if (authoritative) {
        tools_used_cache_.store(current_file_key(), current_file_size_bytes_,
                                current_file_modified_, tools_used_effective());
    }

    // Render the per-tool color swatches from the REAL used-tool
    // set recovered by the headless scan. On 2D-only platforms
    // (Snapmaker U1, AD5M) the gcode viewer never parses, so
    // try_extract_gcode_colors() — the viewer-parse owner of this
    // render — never fires and the detail panel would otherwise
    // show no color info at all (regression 22d37fd47). Mirror its
    // visibility decision and renderer here, sourcing the tool set
    // from tools_used_effective() so the swatches reflect the
    // precise used tools (e.g. {0,2}), not an over-counted palette.
    //
    // Guard on !is_gcode_loaded(): when the viewer DID parse (full
    // platforms) it already owns the render — don't double-fire.
    if (!is_gcode_loaded()) {
        // refresh_card_from_palette: the card widget renders its own
        // swatches/rows from tool_info_, so on editable-card backends that
        // take the headless path (e.g. CFS on a 2D-only platform) it must
        // be fed here just as the viewer-parse path feeds it. Redundant for
        // preflight/remap LOGIC — those source per-tool info from
        // current_filament_colors_/materials via get_used_tool_info(), not
        // from the card instance — but necessary for card display.
        render_authoritative_chips(tools_used_effective(),
                                   /*refresh_card_from_palette=*/true);
    } else {
        // The viewer parse already owns the swatch render — re-running it
        // would rebuild identical chips. Pre-flight and the card's used-tool
        // filter still refresh from the scan result (a no-op on full
        // platforms, where the parse already populated both).
        recompute_preflight();
        filament_mapping_card_.set_used_tools(tools_used_effective());
    }

    // Release any deferred print attempt waiting on readiness.
    fire_on_preflight_ready();
}

void PrintSelectDetailView::kick_off_headless_tools_scan() {
    // show() owns the headless-state reset (and the cache seed). A cache hit
    // (or an already-completed viewer parse) has already answered the
    // tools-used question — nothing to scan.
    if (headless_scan_done_) {
        spdlog::debug("[DetailView] Tools-used already known (cache/viewer) — skipping scan");
        return;
    }

    if (!api_ || current_filename_.empty()) {
        // No way to scan — mark done with no result so the gate degrades to
        // "proceed without tools_used" instead of hanging, publish readiness
        // (skeleton resolves immediately), and release any deferred attempt
        // right away rather than making it wait out the safety timeout.
        // Nothing can ever download or scan in this state, so the question is
        // as settled as it can be.
        headless_scan_done_ = true;
        headless_scan_settled_ = true;
        publish_mapping_ready();
        fire_on_preflight_ready();
        return;
    }

    auto tok = lifetime_.token();

    // Early-exit stop set (full slicer palette): once every palette tool has
    // been seen the result can't grow — stop reading. Tools beyond the
    // palette are dropped by every downstream consumer anyway.
    std::set<int> stop_set;
    for (size_t i = 0; i < current_filament_colors_.size(); ++i) {
        stop_set.insert(static_cast<int>(i));
    }

    // The file's own footer already states which tools it uses and in what
    // colors, so ask for that first — a single small range request instead of
    // the whole file. Anything it cannot answer falls through to the full
    // scan below, which is the pre-existing behavior unchanged.
    start_tail_summary_scan(tok, stop_set);
}

void PrintSelectDetailView::start_tail_summary_scan(LifetimeToken tok, std::set<int> stop_set) {
    // Sized from Moonraker's gcode_end_byte when it reported one (the footer
    // starts exactly there), otherwise a fixed window. Everything after that
    // offset is the slicer's settings block.
    const size_t window = helix::gcode::gcode_tail_window_bytes(
        static_cast<uint64_t>(current_file_size_bytes_), current_gcode_end_byte_);
    const std::string file_path = current_file_key();

    spdlog::debug("[DetailView] Footer read: last {} bytes of {} (size={}, gcode_end_byte={})",
                  window, file_path, current_file_size_bytes_, current_gcode_end_byte_);

    // Every fall-through lands here: re-run the pre-existing whole-file scan.
    // Deferred because ensure_gcode_downloaded() touches members and both
    // callbacks below arrive on an HTTP thread (L081 Mechanism C).
    auto fall_back = [this, tok, stop_set](const char* why) mutable {
        spdlog::debug("[DetailView] Footer read did not answer ({}) — full scan", why);
        tok.defer("DetailView::tail_summary_fallback", [this, tok, stop_set]() mutable {
            start_full_tools_scan(tok, std::move(stop_set));
        });
    };

    api_->transfers().download_file_tail(
        "gcodes", file_path, window,
        [this, tok, file_path, fall_back](const std::string& tail) mutable {
            // === BG THREAD: pure parse over a local, no `this` access ===
            const helix::gcode::GcodeFooterSummary summary =
                helix::gcode::parse_gcode_footer_summary(tail);
            if (!summary.usable()) {
                fall_back(summary.has_usage_line ? "usage vector is all zero"
                                                 : "no per-tool usage line");
                return;
            }

            tok.defer("DetailView::tail_summary_apply", [this, summary, file_path]() {
                // The selection can move on while the range request is in
                // flight; the deferred body must not answer for a file that is
                // no longer shown (its result would be cached under the NEW
                // file's key).
                if (file_path != current_file_key()) {
                    spdlog::debug("[DetailView] Footer read landed for a stale file ({}) —"
                                  " discarding",
                                  file_path);
                    return;
                }

                // Backfill the palette when Moonraker's metadata carried none
                // — the same gap try_extract_gcode_colors() covers from the
                // viewer parse, answered here without one.
                if (current_filament_colors_.empty() && !summary.colours.empty()) {
                    spdlog::info("[DetailView] Metadata lacked filament colors — took {} from "
                                 "the G-code footer",
                                 summary.colours.size());
                    current_filament_colors_ = summary.colours;
                    lv_subject_set_int(&filament_mapping_visible_,
                                       filament_mapping_card_.should_show() ? 1 : 0);
                }

                spdlog::info("[DetailView] Footer read answered tools_used: {} tools",
                             summary.tools_used.size());

                // Authoritative: the footer is the slicer's own accounting of
                // what it emitted, so it is cached like a completed scan. It
                // can differ from the Tn scan on a single-extruder file — the
                // footer says {0} where the scan (which sees no Tn at all)
                // says {} — and {0} is the same answer the viewer parse
                // produces, so the two paths agree rather than diverge.
                apply_scan_result(summary.tools_used, /*authoritative=*/true);
            });
        },
        [fall_back](const MoonrakerError& error) mutable {
            // === BG THREAD: no `this` — fall_back marshals before touching it ===
            spdlog::debug("[DetailView] Footer read failed: {}", error.message);
            fall_back("transport error");
        });
}

void PrintSelectDetailView::start_full_tools_scan(LifetimeToken tok, std::set<int> stop_set) {
    const std::string path = canonical_gcode_path();

    // ONE shared transfer (the viewer preview joins it — no second download).
    // Once the file is on disk, scan it line-by-line on the slow HTTP lane:
    // off the main thread, memory-safe (the scanner never holds the whole
    // file), result marshaled back by finish_scan via tok.defer. The shared
    // file is NOT deleted here — the viewer preview reads the same copy until
    // view teardown.
    ensure_gcode_downloaded([this, tok, path, stop_set](bool ok, const std::string&) mutable {
        if (!ok) {
            // Download failed — degrade gracefully with an empty set. NOT
            // authoritative: this empty set carries no information about the
            // file, so finish_scan must not write it to the persistent cache
            // (it would freeze "no tools" for this file).
            spdlog::debug("[DetailView] Headless tools scan: no G-code file - degrading");
            finish_scan(tok, {}, /*authoritative=*/false);
            return;
        }
        helix::http::HttpExecutor::slow().submit([this, tok, path, stop_set]() mutable {
            std::set<int> tools = helix::gcode::scan_tools_used_from_file(path, stop_set);
            // The scan read the real file — its result (even an empty set:
            // legitimate single-extruder file) is authoritative and persists.
            finish_scan(tok, std::move(tools), /*authoritative=*/true);
        });
    });
}

bool PrintSelectDetailView::swatches_card_visible_for(size_t tool_count) const {
    // Multi-tool printers: any tool referenced is enough (lane identity matters).
    // Single-extruder: 2+ tools required (manual-swap multi-color files).
    const int ams_slots = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    const bool is_multi_tool_printer =
        helix::ToolState::instance().is_multi_tool() || ams_slots > 1;
    return is_multi_tool_printer ? tool_count > 0 : tool_count > 1;
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
    ui_gcode_viewer_clear(gcode_viewer_);

    // Reset first-frame flag: the thumbnail stays on top until the viewer
    // renders, then the first-frame callback flips this to reveal it.
    lv_subject_set_int(&detail_viewer_first_frame_, 0);

    // Register one-shot callback: fires after the viewer's first complete
    // render, at which point we hide the thumbnail (revealing the viewer).
    ui_gcode_viewer_set_first_frame_callback(
        gcode_viewer_,
        [](lv_obj_t* /*viewer*/, void* user_data, bool /*success*/) {
            auto* self = static_cast<PrintSelectDetailView*>(user_data);
            spdlog::debug("[DetailView] First frame rendered — revealing viewer");
            lv_subject_set_int(&self->detail_viewer_first_frame_, 1);
        },
        this);

    // Show loading spinner over thumbnail
    lv_subject_set_int(&detail_gcode_loading_, 1);

    // Check "Thumbnail Only" render mode - skip all gcode downloading/parsing.
    // This is the ONLY user-forced skip: past here we render whatever mode the
    // viewer is in (3D on GLES devices, 2D-layer otherwise), just like the
    // print-status panel. 2D-mode devices used to fall back to the thumbnail
    // here; now they get the real toolpath preview — with the effective
    // lane-matched colors (apply_preview_colors) — so the browser shows the
    // colors the print will actually use. Oversized files still degrade to the
    // thumbnail below via is_gcode_2d_streaming_safe().
    if (DisplaySettingsManager::instance().get_gcode_render_mode() == 3) {
        spdlog::info("[DetailView] G-code render mode is Thumbnail Only - skipping G-code load");
        lv_subject_set_int(&detail_gcode_loading_, 0);
        show_gcode_viewer(false);
        return;
    }

    auto tok = lifetime_.token();

    // Shared download FIRST: when the file is already on disk the viewer
    // loads immediately — no wait on the metadata round-trip (preserves the
    // old cached-file fast path). On a cold open this starts the ONE
    // transfer the headless tools scan joins. The streaming-safety gate
    // applies to the on-disk bytes either way (same size metadata.size
    // reports); the metadata gate below re-checks the authoritative size on
    // cold downloads.
    ensure_gcode_downloaded([this, tok](bool ok, const std::string& path) {
        if (!ok) {
            spdlog::debug("[DetailView] Shared G-code download unavailable - using thumbnail");
            show_gcode_viewer(false);
            return;
        }

        std::ifstream f(path, std::ios::binary | std::ios::ate);
        const std::streampos end_pos = f ? f.tellg() : std::streampos(0);
        const size_t local_size = end_pos > 0 ? static_cast<size_t>(end_pos) : 0;
        if (!helix::is_gcode_2d_streaming_safe(local_size)) {
            auto mem = helix::get_system_memory_info();
            spdlog::warn("[DetailView] G-code too large for streaming: file={} bytes, "
                         "available RAM={}MB - using thumbnail",
                         local_size, mem.available_mb());
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
        begin_viewer_load(path);
    });

    // Metadata fetch (parallel, as today): populates cached_file_metadata_
    // for PrintStartController's pre-print checks (e.g. filament weight) and
    // re-checks the streaming-safety gate against the authoritative size.
    const std::string file_path =
        current_path_.empty() ? current_filename_ : current_path_ + "/" + current_filename_;

    api_->files().get_file_metadata(
        file_path,
        [this, tok](const FileMetadata& metadata) {
            // L081 Mechanism C: marshal member writes + LVGL/show_gcode_viewer
            // to main thread before touching `this`.
            tok.defer("DetailView::metadata_apply", [this, metadata]() {
                // Cache for PrintStartController's pre-print checks (e.g., filament weight)
                cached_file_metadata_ = metadata;

                // Check if file is safe to render given available RAM. When
                // the shared file already loaded this same size passed the
                // local gate above, so this only bites on the paths where the
                // ensure-callback hadn't resolved yet.
                if (!helix::is_gcode_2d_streaming_safe(metadata.size)) {
                    auto mem = helix::get_system_memory_info();
                    spdlog::warn("[DetailView] G-code too large for streaming: file={} bytes, "
                                 "available RAM={}MB - using thumbnail",
                                 metadata.size, mem.available_mb());
                    show_gcode_viewer(false);
                    return;
                }
                spdlog::debug("[DetailView] G-code size {} bytes - metadata cached", metadata.size);
            });
        },
        [this, tok](const MoonrakerError& err) {
            // L081 Mechanism C: marshal LVGL show_gcode_viewer to main thread.
            tok.defer("DetailView::metadata_error", [this, err]() {
                spdlog::debug("[DetailView] Failed to get G-code metadata: {} - skipping preview",
                              err.message);
                show_gcode_viewer(false);
            });
        },
        true // silent
    );
}

void PrintSelectDetailView::begin_viewer_load(const std::string& path) {
    // Set up the (single) load callback, then load the file. The body was
    // identical in the former cached-file and post-download paths.
    ui_gcode_viewer_set_load_callback(
        gcode_viewer_,
        [](lv_obj_t* viewer, void* user_data, bool success) {
            auto* self = static_cast<PrintSelectDetailView*>(user_data);
            if (!success) {
                spdlog::warn("[DetailView] G-code load failed");
                self->show_gcode_viewer(false);
                return;
            }
            self->gcode_loaded_ = true;
            // The viewer parse satisfies pre-flight readiness — publish so the
            // skeleton latch opens. try_extract_gcode_colors() below finishes
            // the authoritative chip render in this same callback (one paint).
            self->publish_mapping_ready();

            // Show all layers, no ghost (preview = full model)
            ui_gcode_viewer_set_print_progress(viewer, -1);

            // Apply preview colors respecting the sliced/actual toggle
            // (default actual: AMS/slicer base then mapped overrides).
            self->apply_preview_colors();

            // Extract colors from parsed gcode when metadata lacked them.
            // This also computes preflight_result_ — it MUST run before
            // fire_on_loaded() so any deferred print-attempt sees fresh checks.
            self->try_extract_gcode_colors(viewer);

            // Parse + pre-flight are now complete: release any deferred
            // run_when_loaded() callback (e.g. a print tapped pre-parse).
            self->fire_on_loaded();
            // The viewer parse also satisfies pre-flight readiness on full
            // platforms — release any run_when_preflight_ready() attempt.
            self->fire_on_preflight_ready();

            // Unpause, show, then reset camera (must be visible for layout)
            ui_gcode_viewer_set_paused(viewer, false);
            self->show_gcode_viewer(true);
            lv_obj_update_layout(viewer);
            ui_gcode_viewer_reset_camera(viewer);

            spdlog::debug("[DetailView] G-code preview loaded successfully");
        },
        this);
    ui_gcode_viewer_load_file(gcode_viewer_, path.c_str());
}

// ============================================================================
// Pre-print Estimate Label Update
// ============================================================================

static void update_prep_time_label() {
    if (!s_detail_view_instance || !s_detail_view_instance->get_prep_manager()) {
        return;
    }
    auto* mgr = s_detail_view_instance->get_prep_manager();
    mgr->recalculate_estimate();

    int estimate_s = lv_subject_get_int(mgr->get_preprint_estimate_subject());

    if (estimate_s <= 0) {
        lv_subject_copy_string(s_detail_view_instance->get_prep_time_estimate_subject(), "");
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
// visible. Today only the plugin-gated predicate returns a non-null subject
// (the helix_plugin_installed tri-state); macro-gated options are filtered
// out of the set BEFORE populate() is called (see
// filter_macro_gated_options), so the lookup never sees them.

void PrintSelectDetailView::populate_option_rows() {
    if (!pre_print_options_container_) {
        return;
    }

    if (!printer_state_) {
        return;
    }

    const auto& option_set = printer_state_->get_pre_print_option_set();

    // Skip rebuild only when rows are already populated AND the active
    // printer hasn't changed since they were built. Mid-session printer-type
    // changes (e.g. multi-printer setups) need a repopulate so the rows
    // reflect the new option set.
    //
    // The rebuild path is safe: `populate()` calls `clear()` (which deinits
    // every option subject — uninstalling observers from their row widgets)
    // BEFORE `safe_clean_children` deletes the widgets themselves. That
    // ordering is what the renderer's class doc spells out as "case 1" of
    // the lifetime contract — observers are uninstalled while widgets are
    // still alive, so the deferred widget-delete tick has nothing to do for
    // them. Repopulating mid-session is therefore not the race that this
    // early-return originally guarded against.
    const std::string& current_type = printer_state_->get_printer_type();
    if (option_rows_renderer_.row_count() > 0 && current_type == last_rendered_printer_type_) {
        spdlog::trace("[DetailView] Skipping option-row rebuild (already populated for '{}')",
                      current_type);
        return;
    }
    last_rendered_printer_type_ = current_type;

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

    // Plugin-gated visibility: HIDE a toggle only when DISABLING it would
    // require the HelixPrint plugin (see
    // PrintPreparationManager::disabling_option_requires_plugin). For those
    // options we bind the row to the helix_plugin_installed tri-state subject;
    // the renderer hides the row when it reads 0 (plugin confirmed absent) and
    // keeps it visible at -1 (still checking, startup window) and 1 (present).
    //
    // CAUTION — do NOT hide options the printer handles natively without the
    // plugin. K2 Plus bed_mesh is a MacroParam whose START_PRINT/PRINT_PREPARED
    // takes the skip directly (setup_gcode present), so the predicate returns
    // false and it stays visible. Hiding it was a shipped regression; the
    // predicate now enforces the distinction structurally.
    auto visibility_lookup = [this](const std::string& id) -> lv_subject_t* {
        if (!prep_manager_ || !printer_state_) {
            return nullptr;
        }
        const PrePrintOption* opt = printer_state_->get_pre_print_option_set().find(id);
        if (opt && prep_manager_->disabling_option_requires_plugin(*opt)) {
            return printer_state_->get_helix_plugin_installed_subject();
        }
        return nullptr; // Not plugin-dependent: always visible for declared options.
    };

    option_rows_renderer_.populate(pre_print_options_container_, rendered, visibility_lookup,
                                   [](const std::string& id, int new_state) {
                                       spdlog::debug("[DetailView] Option '{}' toggled: {}", id,
                                                     new_state);
                                       update_prep_time_label();
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
}

} // namespace helix::ui
