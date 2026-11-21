// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @file ui_busy_overlay.h
 * @brief Reusable busy/progress overlay for long-running operations
 *
 * Shows a semi-transparent overlay with spinner and progress text during
 * operations like file downloads/uploads. Features a grace period to avoid
 * flicker for fast operations.
 *
 * Usage:
 *   // Start operation - overlay appears after 300ms if still running
 *   BusyOverlay::show("Preparing...");
 *
 *   // Update progress during operation (call from main thread or via ui_queue_update)
 *   BusyOverlay::set_progress("Downloading", 45.0f);  // "Downloading... 45%"
 *
 *   // Operation complete
 *   BusyOverlay::hide();
 */

namespace helix {

class BusyOverlay {
  public:
    /**
     * @brief Request overlay display after grace period
     *
     * Overlay will appear after grace_period_ms if hide() hasn't been called.
     * Safe to call multiple times - subsequent calls update initial_text.
     *
     * @param initial_text Text to display (e.g., "Please wait...")
     * @param grace_period_ms Delay before showing overlay (default 300ms)
     */
    static void show(const std::string& initial_text = "Please wait...",
                     uint32_t grace_period_ms = 300);

    /**
     * @brief Update progress display
     *
     * Formats as "Operation... XX%" (e.g., "Downloading... 45%")
     * Sets the text the overlay shows, or shows once its grace period ends.
     *
     * @note NOT thread-safe! Must be called from the main LVGL thread.
     *       When calling from HTTP/background threads, use helix::ui::queue_update():
     *       @code
     *       helix::ui::queue_update("Caller::progress", [pct]() {
     *           BusyOverlay::set_progress("Downloading", pct);
     *       });
     *       @endcode
     *
     * @param operation Operation name (e.g., "Downloading", "Uploading")
     * @param percent Progress percentage (0-100)
     */
    static void set_progress(const std::string& operation, float percent);

    /**
     * @brief set_progress() from a transfer callback on any thread
     *
     * Queues the update to the main thread. The overlay is process-wide, so
     * the update belongs to no object and runs whoever started the transfer.
     *
     * @param operation Operation name (e.g., "Uploading")
     * @param done Bytes transferred so far
     * @param total Total bytes (0 shows 0%)
     */
    static void queue_progress(const std::string& operation, size_t done, size_t total);

    /**
     * @brief Hide overlay immediately
     *
     * Cancels grace timer if pending, or removes overlay if visible.
     * Safe to call even if overlay was never shown.
     */
    static void hide();

    /**
     * @brief Check if overlay is currently visible
     *
     * @return true if overlay is on screen (not just pending)
     */
    static bool is_visible();

    /**
     * @brief Check if show was called but grace period hasn't elapsed
     *
     * @return true if waiting for grace period
     */
    static bool is_pending();

  private:
    BusyOverlay() = delete; // Static-only class
};

} // namespace helix
