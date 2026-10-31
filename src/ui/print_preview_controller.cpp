// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_preview_controller.h"

#include "ui_gcode_viewer.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "file_analysis.h"
#include "gcode_preview_setup.h"
#include "print_status_preview_decision.h"
#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {

// The shared thumbnail path subject never carries the empty string: a file with
// no thumbnail (yet) is published as no_thumbnail_placeholder(). That value is a
// perfectly good image to PUT ON SCREEN, but it is not this print's thumbnail,
// so it must never stamp displayed_file_. ActivePrintMediaManager draws the same
// distinction in has_thumbnail_for() and says why: take the placeholder for a
// thumbnail and "every print would stop at the placeholder", because the marker
// it leaves behind is what tells decide_preview_action() there is nothing left
// to load.
bool is_no_thumbnail_placeholder(const char* path) {
    return path != nullptr &&
           std::strcmp(path, helix::PrinterPrintState::no_thumbnail_placeholder()) == 0;
}

} // namespace

PrintPreviewController::PrintPreviewController(std::string log_tag, PrinterState& printer_state,
                                               PrintLifecycleState& lifecycle, Host host)
    : log_tag_(std::move(log_tag)), printer_state_(printer_state), lifecycle_(lifecycle),
      host_(std::move(host)), fetcher_(log_tag_) {
    fetcher_.set_rendered_probe(
        [this]() { return gcode_viewer_ && helix::ui::gcode_viewer_has_content(gcode_viewer_); });
}

PrintPreviewController::~PrintPreviewController() {
    cancel_pending_load();
}

void PrintPreviewController::attach_widgets(lv_obj_t* thumbnail, lv_obj_t* viewer) {
    print_thumbnail_ = thumbnail;
    gcode_viewer_ = viewer;
}

void PrintPreviewController::detach_widgets() {
    if (gcode_viewer_) {
        helix::ui::gcode_viewer_set_load_callback(gcode_viewer_, nullptr, nullptr);
    }
    gcode_viewer_ = nullptr;
    print_thumbnail_ = nullptr;
}

void PrintPreviewController::on_tree_destroyed() {
    cancel_pending_load();
    fetcher_.cancel();
    detach_widgets();
    // Nothing is displayed any more. Clearing both markers makes the next
    // ensure_current() reload thumbnail and G-code after destroy-on-close.
    displayed_file_.clear();
    gcode_displayed_file_.clear();
    pending_gcode_filename_.clear();
}

void PrintPreviewController::cancel_pending_load() {
    if (gcode_load_timer_) {
        lv_timer_delete(gcode_load_timer_);
        gcode_load_timer_ = nullptr;
    }
}

void PrintPreviewController::on_filename_changed() {
    const std::string& effective_filename =
        printer_state_.print_state().get_effective_print_filename();

    // When the effective filename CHANGES, the widgets are showing the old file
    // (or nothing). Clear each stale per-asset marker so ensure_current() sees
    // the mismatch and reloads that asset. Each marker is cleared only when ITS
    // OWN asset is stale: the thumbnail can already be current while the viewer
    // still holds the previous print, so clearing both would force a needless
    // thumbnail re-fetch. Idempotent: a repeated fire with the same filename
    // leaves the markers untouched.
    if (!effective_filename.empty() && effective_filename != displayed_file_) {
        cached_thumbnail_path_.clear();
        displayed_file_.clear();
    }
    if (!effective_filename.empty() && effective_filename != gcode_displayed_file_) {
        gcode_displayed_file_.clear();
    }
    ensure_current();
}

void PrintPreviewController::on_thumbnail_published(const char* path) {
    // No empty-path branch: ActivePrintMediaManager publishes
    // no_thumbnail_placeholder() for a file with no thumbnail and the subject is
    // seeded with it, so the value is always an image.
    // The subject carries the file the path was produced FOR (set_print_thumbnail
    // writes it before publishing the path), so compare identity instead of
    // assuming the value is ours. A result that lands for the previous print must
    // not be applied, and above all must not advance displayed_file_: that stamp
    // is what convinces ensure_current() the current file is already on screen.
    const std::string& for_file = printer_state_.print_state().get_print_thumbnail_file();
    const std::string& effective = printer_state_.print_state().get_effective_print_filename();
    if (!effective.empty() && for_file != effective) {
        spdlog::debug("[{}] Ignoring thumbnail published for '{}' (showing '{}')", log_tag_,
                      for_file, effective);
        return;
    }
    cached_thumbnail_path_ = path;
    if (print_thumbnail_) {
        lv_image_set_src(print_thumbnail_, path);
        spdlog::debug("[{}] Thumbnail updated from shared subject: {}", log_tag_, path);
        // Record what is ACTUALLY on screen. The manager publishes the placeholder
        // FOR the incoming file as its clear, so identity matches here even though
        // no thumbnail has been fetched yet; stamping that would retire the
        // reconcile before the real image ever arrives.
        if (is_no_thumbnail_placeholder(path)) {
            displayed_file_.clear();
        } else {
            displayed_file_ = for_file;
        }
    }
}

void PrintPreviewController::restore_cached_thumbnail() {
#if defined(HELIX_PLATFORM_ESP32)
    // cached_thumbnail_path_ is always empty here (no disk cache): restore from
    // the PSRAM buffer PrinterState is holding instead.
    apply_psram_thumbnail();
#else
    if (print_thumbnail_ && !cached_thumbnail_path_.empty()) {
        lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
        spdlog::info("[{}] Restored cached thumbnail: {}", log_tag_, cached_thumbnail_path_);
    }
#endif
}

void PrintPreviewController::reapply_thumbnail_if_blank() {
    if (!print_thumbnail_) {
        return;
    }
#if defined(HELIX_PLATFORM_ESP32)
    if (esp_thumbnail_ && !lv_image_get_src(print_thumbnail_)) {
        lv_image_set_src(print_thumbnail_, esp_thumbnail_->dsc());
    }
#else
    if (!cached_thumbnail_path_.empty() && !lv_image_get_src(print_thumbnail_)) {
        lv_image_set_src(print_thumbnail_, cached_thumbnail_path_.c_str());
    }
#endif
}

void PrintPreviewController::on_print_ended() {
    if (displayed_file_.empty() && gcode_displayed_file_.empty() && !lifecycle_.gcode_loaded() &&
        !fetcher_.owns_file() && pending_gcode_filename_.empty()) {
        return;
    }
    spdlog::debug("[{}] Clearing thumbnail/gcode tracking (print ended)", log_tag_);
    cancel_pending_load();
    cached_thumbnail_path_.clear();
#if defined(HELIX_PLATFORM_ESP32)
    release_psram_thumbnail();
#endif
    pending_gcode_filename_.clear();
    // The widgets keep showing the final frame, and the desired file is now
    // empty, so displayed_file_ stays as it is: a new print's filename change
    // clears it.
    fetcher_.discard_file();
}

bool PrintPreviewController::is_load_for_effective_print(const std::string& print_filename) const {
    return print_filename == printer_state_.print_state().get_effective_print_filename();
}

void PrintPreviewController::load_file(const char* file_path, const std::string& print_filename,
                                       std::string layer_index_sidecar) {
    // A fetch that started for the print showing when it was requested can
    // reach this point after the print has moved on - the metadata lookup and
    // the download both cross the network. Calling helix::ui::gcode_viewer_load_file()
    // here would replace whatever the viewer currently shows with this stale
    // print's geometry, so route to ensure_current() instead: it
    // reconciles against whichever print is effective NOW.
    if (!is_load_for_effective_print(print_filename)) {
        spdlog::debug("[{}] Dropping G-code load for '{}': no longer the effective print ('{}')",
                      log_tag_, print_filename,
                      printer_state_.print_state().get_effective_print_filename());
        ensure_current();
        return;
    }

    if (!gcode_viewer_ || !file_path) {
        spdlog::warn("[{}] Cannot load G-code: viewer={}, path={}", log_tag_,
                     gcode_viewer_ != nullptr, file_path != nullptr);
        return;
    }

    spdlog::debug("[{}] Loading G-code file: {}", log_tag_, file_path);

    // Register callback to be notified when loading completes
    helix::ui::gcode_viewer_set_load_callback(gcode_viewer_,
                                              &PrintPreviewController::on_viewer_loaded, this);

    // Start loading the file
    gcode_load_filename_ = print_filename;
    helix::ui::gcode_viewer_set_layer_index_sidecar(gcode_viewer_, std::move(layer_index_sidecar));
    helix::ui::gcode_viewer_load_file(gcode_viewer_, file_path);
}

void PrintPreviewController::on_viewer_loaded(lv_obj_t* viewer, void* user_data, bool success) {
    auto* self = static_cast<PrintPreviewController*>(user_data);

    // The print can change again while the viewer builds this load in
    // the background - the entry check in load_file() only knew
    // the print was current when the load STARTED. Applying this
    // result now would swap the already-displayed print's geometry for
    // one that is no longer running, so drop it here too and let
    // ensure_current() (re)load whichever print is effective.
    if (!self->is_load_for_effective_print(self->gcode_load_filename_)) {
        spdlog::debug("[{}] Dropping G-code load result for '{}': no longer the effective print "
                      "('{}')",
                      self->log_tag_, self->gcode_load_filename_,
                      self->printer_state_.print_state().get_effective_print_filename());
        self->ensure_current();
        return;
    }

    if (!success) {
        if (helix::ui::gcode_viewer_load_skipped_for_cost(viewer)) {
            spdlog::info("[{}] G-code preview skipped: too costly on this board now",
                         self->log_tag_);
        } else {
            spdlog::error("[{}] G-code load failed", self->log_tag_);
        }
        self->lifecycle_.set_gcode_loaded(false);
        return;
    }

    // Get layer count from loaded geometry
    int max_layer = helix::ui::gcode_viewer_get_max_layer(viewer);
    if (max_layer >= 0)
        spdlog::debug("[{}] G-code loaded: {} layers", self->log_tag_, max_layer);
    else
        spdlog::debug("[{}] G-code loaded (renderer pending)", self->log_tag_);

    // Mark G-code as successfully loaded (enables viewer mode on state changes)
    self->lifecycle_.set_gcode_loaded(true);
    // The viewer now holds the geometry of the print this load was for.
    // Record that name as the GCODE marker: ensure_current() then
    // treats the viewer as current only for that print, and a load that
    // landed for an earlier print reads as stale. (The thumbnail marker
    // is recorded independently by the thumbnail path.)
    self->gcode_displayed_file_ = self->gcode_load_filename_;

    // Hand the scan's scheduled pauses to print state, where both
    // progress surfaces read them, under the same name: a load that
    // lands after the print switched publishes a list that does not
    // match the new print, and its markers stay hidden.
    {
        if (auto scan = helix::ui::gcode_viewer_get_pause_scan(viewer)) {
            self->printer_state_.print_state().set_pause_scan(std::move(scan),
                                                              self->gcode_load_filename_);
        }
    }

    // Override extrusion colors with AMS filament colors.
    // For multi-tool prints, applies per-tool AMS slot colors.
    // For single-tool, falls back to current AMS color subject.
    self->apply_tool_colors();

    // The parsed file now carries the tools this print uses and its objects'
    // geometry; the panel re-derives what reads them.
    self->host_.parsed_file_loaded();

    // Show viewer if print is active or in terminal state (user can see
    // where print stopped). Only skip in Idle.
    if (self->lifecycle_.want_viewer()) {
        self->host_.show_viewer(true);
    }

    // Force layout recalculation now that viewer is visible
    lv_obj_update_layout(viewer);
    // Reset camera to fit model to new viewport dimensions
    helix::ui::gcode_viewer_reset_camera(viewer);

    // Set print progress to current layer (not 0!) when joining a print in progress.
    // Read directly from PrinterState subjects to get the latest values.
    int viewer_max_layer = helix::ui::gcode_viewer_get_max_layer(viewer);
    int current_layer =
        lv_subject_get_int(self->printer_state_.print_state().get_print_layer_current_subject());
    int total_layers =
        lv_subject_get_int(self->printer_state_.print_state().get_print_layer_total_subject());

    // Fallback: if Moonraker metadata didn't provide layer count,
    // use the count from the parsed/indexed gcode file
    if (total_layers == 0 && viewer_max_layer > 0) {
        int layer_count = viewer_max_layer + 1; // max_layer is 0-based
        self->printer_state_.print_state().set_print_layer_total(layer_count);
        spdlog::info("[{}] Set total layers from gcode viewer: {}", self->log_tag_, layer_count);
    }

    // Update lifecycle state while we're at it
    self->lifecycle_.on_layer_changed(current_layer, total_layers,
                                      self->printer_state_.print_state().has_real_layer_data());

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
    helix::ui::queue_widget_update(viewer, [viewer_layer](lv_obj_t* live) {
        helix::ui::gcode_viewer_set_print_progress(live, viewer_layer);
    });

    spdlog::debug("[{}] G-code loaded: initial layer progress set to {} "
                  "(current={}/{}, viewer_max={})",
                  self->log_tag_, viewer_layer, current_layer, total_layers, viewer_max_layer);

    // NOTE: PrintStatusPanel does NOT start prints - it only VIEWS them.
    // Prints are started from PrintSelectPanel via the Print button.
    // This callback is for loading G-code into the viewer for visualization only.
    spdlog::debug("[{}] G-code loaded for viewing: {}", self->log_tag_,
                  helix::ui::gcode_viewer_get_filename(viewer));
}

void PrintPreviewController::load_for_viewing(const std::string& filename) {
    spdlog::debug("[{}] Loading G-code for viewing: {}", log_tag_, filename);

    // Skip if no viewer widget
    if (!gcode_viewer_) {
        spdlog::debug("[{}] No gcode_viewer_ widget - skipping G-code load", log_tag_);
        return;
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[{}] No API available - skipping G-code load", log_tag_);
        return;
    }

    // ensure_current() queues this fetch through a debounce timer (up
    // to 5s), and the print can move on before the timer fires. Checking here
    // avoids starting a cache lookup, metadata fetch or download for a print
    // that is already known to be the wrong one.
    if (!is_load_for_effective_print(filename)) {
        spdlog::debug("[{}] Skipping G-code fetch for '{}': no longer the effective print ('{}')",
                      log_tag_, filename,
                      printer_state_.print_state().get_effective_print_filename());
        ensure_current();
        return;
    }

    // A .3mf started from here prints as G-code of its own; that is what renders.
    const std::string source = helix::gcode::preview_gcode_for(
        filename, lv_subject_get_string(printer_state_.print_state().get_print_filename_subject()));
    if (source != filename) {
        spdlog::debug("[{}] Previewing '{}', the G-code printing for '{}'", log_tag_, source,
                      filename);
    }
    fetcher_.fetch(
        source,
        [this, filename, source](const std::string& path) {
            // A current analysis record's layer index spares a streaming load
            // its own pass over the file.
            helix::with_layer_index_sidecar(
                api_,
                printer_state_.plugin_status_state().plugin_feature(helix::kFileAnalysisFeature,
                                                                    helix::kFileAnalysisMinVersion),
                source, helix::ui::gcode_viewer_would_stream(gcode_viewer_, path),
                lifetime_.token(), [this, filename, path](std::string sidecar) {
                    load_file(path.c_str(), filename, std::move(sidecar));
                });
        },
        [this](helix::ui::GcodePreviewFetcher::Unavailable) { host_.show_viewer(false); });
}

bool PrintPreviewController::apply_tool_colors() {
    if (!gcode_viewer_ || !helix::ui::gcode_viewer_has_content(gcode_viewer_)) {
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
    if (helix::ui::gcode_viewer_apply_ams_tool_colors(gcode_viewer_)) {
        return true;
    }

    // Nothing knowable (no routing published, or no lane knows a color). Leave
    // the renderer's slicer colors alone rather than painting a plausible lie.
    return false;
}

void PrintPreviewController::schedule_deferred_load() {
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

    spdlog::debug("[{}] Scheduling deferred G-code load in {}ms: {}", log_tag_, delay_ms,
                  pending_gcode_filename_);

    gcode_load_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<PrintPreviewController*>(lv_timer_get_user_data(timer));
            self->gcode_load_timer_ = nullptr; // timer is auto-deleted after one-shot
            if (!self->pending_gcode_filename_.empty()) {
                spdlog::info("[{}] Deferred G-code load firing: {}", self->log_tag_,
                             self->pending_gcode_filename_);
                self->load_for_viewing(self->pending_gcode_filename_);
                self->pending_gcode_filename_.clear();
            }
        },
        delay_ms, this);
    lv_timer_set_repeat_count(gcode_load_timer_, 1); // one-shot
}

#if defined(HELIX_PLATFORM_ESP32)
void PrintPreviewController::release_psram_thumbnail() {
    // Main thread (job-state and generation handlers), as EspPsramThumbnail's
    // destructor requires.
    //
    // The src must stop naming the descriptor BEFORE the release: for a variable
    // source lv_image stores the raw pointer (it only strdups paths), and ours can
    // be the last reference. The placeholder is what the shared path subject
    // publishes for a file with no thumbnail, so it is a valid src here.
    if (esp_thumbnail_ && print_thumbnail_ &&
        lv_image_get_src(print_thumbnail_) == esp_thumbnail_->dsc()) {
        lv_image_set_src(print_thumbnail_, helix::PrinterPrintState::no_thumbnail_placeholder());
    }
    esp_thumbnail_.reset();
}

void PrintPreviewController::apply_psram_thumbnail() {
    auto thumb = printer_state_.print_state().get_print_psram_thumbnail();
    if (!thumb) {
        // PrinterState cleared it for a new file or a cleared print. Holding the
        // old buffer keeps PSRAM the next file's decode needs.
        release_psram_thumbnail();
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
        spdlog::info("[{}] PSRAM thumbnail held (panel not yet displayed)", log_tag_);
        return;
    }
    lv_image_set_src(print_thumbnail_, esp_thumbnail_->dsc());
    spdlog::info("[{}] PSRAM thumbnail displayed", log_tag_);
    // Fallback content for the current print is now on screen; record it so
    // ensure_current() treats the thumbnail as current (mirrors the
    // print_thumbnail_path observer on other platforms).
    const std::string& effective = printer_state_.print_state().get_effective_print_filename();
    if (!effective.empty()) {
        displayed_file_ = effective;
    }
}
#endif

void PrintPreviewController::ensure_current() {
    // Desired state = the effective filename of the current print.
    const std::string& desired = printer_state_.print_state().get_effective_print_filename();

    // Read ACTUAL widget state — not intent bools, which can lie after a
    // destroy-on-close / memory-reclaim cycle. This is what makes re-entry
    // self-healing.
    bool thumbnail_has_src = print_thumbnail_ && lv_image_get_src(print_thumbnail_) != nullptr;
    bool gcode_has_content = gcode_viewer_ && helix::ui::gcode_viewer_has_content(gcode_viewer_);

    bool want_viewer = lifecycle_.want_viewer();

    // Thumbnail Only answers for the whole G-code pipeline, not just what is
    // drawn: skipping the fetch here is what keeps a large file off the disk,
    // out of the layer indexer and away from the background render pass while
    // the printer needs the CPU.
    bool viewer_enabled = helix::ui::preview_viewer_enabled();

    helix::ui::PreviewAction action = helix::ui::decide_preview_action(
        displayed_file_, gcode_displayed_file_, desired, thumbnail_has_src, gcode_has_content,
        want_viewer, viewer_enabled);

    spdlog::debug("[{}] ensure_preview_current: thumb_file='{}' gcode_file='{}' desired='{}' "
                  "thumb_src={} gcode_content={} want_viewer={} viewer_enabled={} -> "
                  "load_thumb={} load_gcode={} clear_gcode={}",
                  log_tag_, displayed_file_, gcode_displayed_file_, desired, thumbnail_has_src,
                  gcode_has_content, want_viewer, viewer_enabled, action.load_thumbnail,
                  action.load_gcode, action.clear_gcode);

    if (desired.empty()) {
        return; // Nothing to show.
    }

    // Drop the previous print's geometry FIRST. The reload below is deferred by
    // seconds while the printer is still preparing, and the viewer keeps
    // rendering what it holds until then, so without this the user watches the
    // last print's model for the whole deferral (#1337-adjacent report: "the
    // image from the previous print is displayed first"). helix::ui::gcode_viewer_clear()
    // fires the clear callback, which flips the view mode back to the thumbnail,
    // so the fallback the user lands on is this print's slicer preview.
    if (action.clear_gcode && gcode_viewer_) {
        spdlog::debug("[{}] Clearing stale G-code geometry (viewer holds another print, "
                      "desired '{}')",
                      log_tag_, desired);
        helix::ui::gcode_viewer_clear(gcode_viewer_);
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
        } else if (printer_state_.print_state().get_print_thumbnail_file() == desired) {
            // The subject already carries this file's image, but it was
            // published BEFORE our own view of the filename caught up: the
            // manager observes print_filename synchronously while this panel's
            // filename observer is deferred, so print_thumbnail_path_observer_
            // compared against the PREVIOUS filename and correctly dropped it.
            // Re-reading the subject once the filename lands is what makes that
            // ordering self-healing instead of leaving the previous print's
            // image on the new print's card.
            const char* published = lv_subject_get_string(
                printer_state_.print_state().get_print_thumbnail_path_subject());
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
                              log_tag_, desired);
            } else {
                displayed_file_ = desired;
                spdlog::debug("[{}] Adopted already-published thumbnail for '{}': {}", log_tag_,
                              desired, published);
            }
        } else {
            // Neither source could supply an image, and nothing here retries:
            // the next reconcile is whatever the manager publishes or the next
            // filename change. Name both identities, because in a log the
            // resulting symptom - the previous print's image sitting under the
            // correct filename - is otherwise indistinguishable from a fetch
            // that simply has not landed yet (#1339).
            spdlog::debug("[{}] No thumbnail source for '{}': subject holds one for '{}'", log_tag_,
                          desired, printer_state_.print_state().get_print_thumbnail_file());
        }
    }

    if (action.load_gcode) {
        // Queue the (expensive) gcode download. The deferred timer debounces and
        // load_gcode_file's success callback records gcode_displayed_file_.
        // Schedule immediately when active; otherwise on_activate() runs this again.
        pending_gcode_filename_ = desired;
        if (host_.is_active()) {
            schedule_deferred_load();
        }
    }
}

} // namespace helix::ui
