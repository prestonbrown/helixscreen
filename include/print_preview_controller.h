// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file print_preview_controller.h
 * @brief What the print-status preview shows for the running print.
 *
 * Owns the two "what is on screen" markers (thumbnail and G-code geometry), the
 * debounced G-code load, the viewer hand-off and the per-tool colors. The panel
 * keeps the widget tree and the view-mode decision; this reconciles the preview
 * widgets against PrinterState's effective print filename and asks the panel to
 * switch view modes through Host.
 *
 * @threading Main thread only.
 */

#pragma once

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "gcode_preview_fetcher.h"
#include "lvgl.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#if defined(HELIX_PLATFORM_ESP32)
#include "esp_psram_thumbnail.h"
#endif

#include <functional>
#include <memory>
#include <string>

namespace helix::ui {

class PrintPreviewController {
  public:
    /// What the panel does on the controller's behalf.
    struct Host {
        /// Switch between the thumbnail and the G-code viewer.
        std::function<void(bool show_viewer)> show_viewer;
        /// The viewer finished loading a parsed file; re-derive whatever reads it.
        std::function<void()> parsed_file_loaded;
        /// Is the panel on screen? A G-code load is only queued while it is.
        std::function<bool()> is_active;
    };

    PrintPreviewController(std::string log_tag, PrinterState& printer_state,
                           PrintLifecycleState& lifecycle, Host host);
    ~PrintPreviewController();

    PrintPreviewController(const PrintPreviewController&) = delete;
    PrintPreviewController& operator=(const PrintPreviewController&) = delete;

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
        fetcher_.set_api(api);
    }

    /// Point the controller at the panel's preview widgets. Either may be null.
    void attach_widgets(lv_obj_t* thumbnail, lv_obj_t* viewer);

    /// Forget the widgets before their tree is deleted: the viewer must not
    /// report a load into a controller whose widgets are gone.
    void detach_widgets();

    /// The widget tree is gone: drop the widgets, every pending load and both
    /// markers, so the next open reloads everything.
    void on_tree_destroyed();

    /// Reconcile the thumbnail and the G-code viewer against the effective
    /// print. Reads the ACTUAL widget state, so it is safe and idempotent to
    /// call at any time and self-heals after a destroy-on-close.
    void ensure_current();

    /// The print's filename changed: drop whichever marker is stale, then
    /// reconcile.
    void on_filename_changed();

    /// The shared thumbnail subject published @p path.
    void on_thumbnail_published(const char* path);

    /// Re-apply the thumbnail we already hold to a freshly built tree.
    void restore_cached_thumbnail();

    /// Point a blank thumbnail widget at the image we hold, if any.
    void reapply_thumbnail_if_blank();

#if defined(HELIX_PLATFORM_ESP32)
    /// Pull the PSRAM thumbnail from PrinterState and show it, or release the
    /// held one when PrinterState has cleared it.
    void apply_psram_thumbnail();
    void release_psram_thumbnail();
#endif

    /// Fetch @p filename's G-code and load it into the viewer.
    void load_for_viewing(const std::string& filename);

    /// Load the local file @p file_path into the viewer as print @p print_filename's
    /// G-code. @p layer_index_sidecar: the plugin's layer index for it, or empty.
    void load_file(const char* file_path, const std::string& print_filename,
                   std::string layer_index_sidecar = {});

    /// Re-color the viewer's extrusion with the AMS lane colors.
    bool apply_tool_colors();

    /// The viewer lost its geometry (memory reclaim, leaving a finished print):
    /// the next reconcile must reload it. The thumbnail survives.
    void forget_gcode() {
        gcode_displayed_file_.clear();
    }

    /// The print ended: drop pending loads, the copy on disk and the held image.
    void on_print_ended();

    /// Delete a queued debounced load.
    void cancel_pending_load();

    /// Delete the G-code copy this controller downloaded.
    void discard_file() {
        fetcher_.discard_file();
    }

    /// File whose image is in the thumbnail widget.
    const std::string& displayed_file() const {
        return displayed_file_;
    }
    /// File whose geometry is in the viewer.
    const std::string& gcode_displayed_file() const {
        return gcode_displayed_file_;
    }
    /// Path most recently accepted from the shared thumbnail subject.
    const std::string& cached_thumbnail_path() const {
        return cached_thumbnail_path_;
    }

  private:
    bool is_load_for_effective_print(const std::string& print_filename) const;
    void schedule_deferred_load();
    static void on_viewer_loaded(lv_obj_t* viewer, void* user_data, bool success);

    std::string log_tag_;
    PrinterState& printer_state_;
    PrintLifecycleState& lifecycle_;
    Host host_;
    IMoonrakerAPI* api_ = nullptr;
    GcodePreviewFetcher fetcher_;

    helix::ui::WidgetRef print_thumbnail_;
    lv_obj_t* gcode_viewer_ = nullptr;

    std::string displayed_file_;
    std::string gcode_displayed_file_;
    /// Print whose G-code the viewer's current load is for. load_file() writes
    /// it in the same call that starts the viewer load, and the viewer reports
    /// only its newest load, so the load callback always reads the name of the
    /// load it is reporting.
    std::string gcode_load_filename_;
    std::string cached_thumbnail_path_;

    /// File to load once the debounce timer fires.
    std::string pending_gcode_filename_;
    /// One-shot debounce for the G-code load: a longer delay while the printer
    /// is still homing and heating keeps the download out of that memory spike.
    lv_timer_t* gcode_load_timer_ = nullptr;

#if defined(HELIX_PLATFORM_ESP32)
    /// PSRAM-resident thumbnail shown in print_thumbnail_. There is no cache
    /// file on this platform; this reference keeps the image buffer alive.
    /// Main-thread only (its destructor drops the LVGL image cache entry).
    std::shared_ptr<EspPsramThumbnail> esp_thumbnail_;
#endif

    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
