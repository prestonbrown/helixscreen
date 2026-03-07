// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "moonraker_api.h"
#include "printer_state.h"

#include <atomic>
#include <memory>
#include <string>

namespace helix {

/**
 * @brief Manages display info for the active print (thumbnail, display filename)
 *
 * Decouples shared print media from PrintStatusPanel so that:
 * 1. HomePanel always has current data (regardless of which panels are open)
 * 2. Thread-safe LVGL updates via helix::ui::queue_update()
 * 3. Single point of truth for filename resolution and thumbnail loading
 *
 * Thread Safety:
 * - set_api() must be called from main thread only
 * - set_thumbnail_source() must be called from main thread only
 * - Observer callbacks from PrinterState trigger on main thread (LVGL observer)
 * - All lv_subject updates are deferred to main thread via helix::ui::queue_update()
 *
 * Initialization order: PrinterState -> ActivePrintMediaManager -> Panels
 */
class ActivePrintMediaManager {
  public:
    explicit ActivePrintMediaManager(PrinterState& printer_state);
    ~ActivePrintMediaManager();

    // Non-copyable
    ActivePrintMediaManager(const ActivePrintMediaManager&) = delete;
    ActivePrintMediaManager& operator=(const ActivePrintMediaManager&) = delete;

    /**
     * @brief Set the MoonrakerAPI instance for thumbnail downloads
     *
     * Must be called before thumbnail loading will work.
     *
     * @param api Pointer to MoonrakerAPI (can be nullptr to disable)
     */
    void set_api(MoonrakerAPI* api);

    /**
     * @brief Set the original filename for thumbnail lookup
     *
     * Call this when starting a print with a modified temp file to override
     * the filename used for metadata/thumbnail lookup. This handles the case
     * where Moonraker reports .helix_temp/modified_* but thumbnails are stored
     * under the original filename.
     *
     * @param original_filename The original filename before modification
     */
    void set_thumbnail_source(const std::string& original_filename);

    /**
     * @brief Clear the thumbnail source override
     *
     * Called when print ends to reset state for next print.
     */
    void clear_thumbnail_source();

    /**
     * @brief Set the thumbnail path directly (bypasses Moonraker API lookup)
     *
     * Call this when starting a print with a pre-extracted thumbnail
     * (e.g., from USB drive or embedded in G-code). This sets the thumbnail
     * path subject directly without going through the Moonraker metadata API.
     *
     * @param path Path to the thumbnail file (e.g., "/tmp/helix/thumbnails/extracted.png")
     */
    void set_thumbnail_path(const std::string& path);

  private:
    void process_filename(const char* raw_filename);
    void load_thumbnail_for_file(const std::string& filename);
    void clear_print_info();

    PrinterState& printer_state_;
    MoonrakerAPI* api_ = nullptr;
    ObserverGuard print_filename_observer_;
    std::string thumbnail_source_filename_;
    std::string last_effective_filename_;
    std::string last_loaded_thumbnail_filename_;
    bool last_was_empty_ = false; ///< Prevents repeated "empty filename" log spam
    uint32_t thumbnail_load_generation_ = 0;

    /// Alive flag for ThumbnailLoadContext compatibility (always true for singleton)
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

/**
 * @brief Initialize the global ActivePrintMediaManager singleton
 *
 * Must be called after init_printer_state_subjects() and before panels
 * that depend on print_display_filename/print_thumbnail_path subjects.
 */
void init_active_print_media_manager();

/**
 * @brief Destroy the ActivePrintMediaManager singleton for soft restart
 *
 * Releases the observer guard and destroys the instance. Must be called
 * BEFORE PrinterState::deinit_subjects() to avoid dangling observer pointers.
 */
void deinit_active_print_media_manager();

/**
 * @brief Get the global ActivePrintMediaManager instance
 *
 * @return Reference to the singleton instance
 * @throws std::runtime_error if called before init_active_print_media_manager()
 */
ActivePrintMediaManager& get_active_print_media_manager();

} // namespace helix
