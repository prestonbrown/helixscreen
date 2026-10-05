// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "single_flight_walk.h"
#include "usb_backend.h"

#include <atomic>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct PrintFileData;
class UsbManager;

/**
 * @brief File source for print select panel
 *
 * Defined here (not in ui_panel_print_select.h) to avoid circular dependencies.
 */
enum class FileSource {
    PRINTER = 0, ///< Files from Moonraker (printer storage)
    USB = 1      ///< Files from USB drive
};

namespace helix::ui {

/// A walk of the mounted drives: the G-code found and each file's cached
/// header thumbnail (empty when it has none).
struct UsbScan {
    std::vector<UsbGcodeFile> files;
    std::vector<std::string> thumbnails;
};

/**
 * @brief Walk @p drives for G-code and cache each file's header thumbnail
 *
 * Blocking disk I/O, for a worker thread. @p cancelled is polled before each
 * drive and each file; once it returns true the walk stops and returns what
 * it has.
 */
UsbScan scan_usb_drives(UsbBackend& backend, const std::vector<UsbDrive>& drives,
                        const std::function<bool()>& cancelled);

/**
 * @file ui_print_select_usb_source.h
 * @brief USB file source manager for print selection panel
 *
 * Handles USB drive detection, G-code file scanning, and source switching
 * between Printer (Moonraker) and USB sources.
 *
 * ## Key Features:
 * - USB drive insertion/removal detection
 * - G-code file scanning from USB drives
 * - Source button state management (Printer/USB toggle)
 * - Conversion of USB files to PrintFileData format
 *
 * ## Usage:
 * @code
 * PrintSelectUsbSource usb_source;
 * usb_source.setup(panel, printer_btn, usb_btn);
 * usb_source.set_usb_manager(manager);
 * usb_source.set_on_files_ready([](auto& files) { ... });
 * usb_source.set_on_source_changed([](FileSource source) { ... });
 *
 * // On USB button click:
 * usb_source.select_usb_source();
 *
 * // On USB drive events:
 * usb_source.on_drive_inserted();
 * usb_source.on_drive_removed();
 * @endcode
 */

/**
 * @brief Callback when files are ready from USB source
 * @param files Vector of PrintFileData from USB drive
 */
using UsbFilesReadyCallback = std::function<void(std::vector<PrintFileData>&& files)>;

/**
 * @brief Callback when source changes
 * @param source New file source (PRINTER or USB)
 */
using SourceChangedCallback = std::function<void(FileSource source)>;

/**
 * @brief USB file source manager
 */
class PrintSelectUsbSource {
  public:
    PrintSelectUsbSource() = default;
    ~PrintSelectUsbSource() = default;

    // Non-copyable, non-movable: in-flight scans hold a token keyed to this object
    PrintSelectUsbSource(const PrintSelectUsbSource&) = delete;
    PrintSelectUsbSource& operator=(const PrintSelectUsbSource&) = delete;

    // === Setup ===

    /**
     * @brief Initialize with source selector buttons
     *
     * Finds buttons by name and sets up initial state.
     * Both source buttons are hidden by default until a USB drive is inserted.
     *
     * @param panel Root panel widget (for button lookup)
     * @return true if buttons found successfully
     */
    bool setup(lv_obj_t* panel);

    /**
     * @brief Set UsbManager dependency
     */
    void set_usb_manager(UsbManager* manager);

    // === Callbacks ===

    /**
     * @brief Set callback for when USB files are ready
     */
    void set_on_files_ready(UsbFilesReadyCallback callback) {
        on_files_ready_ = std::move(callback);
    }

    /**
     * @brief Set callback for source changes
     */
    void set_on_source_changed(SourceChangedCallback callback) {
        on_source_changed_ = std::move(callback);
    }

    // === Source Selection ===

    /**
     * @brief Switch to Printer (Moonraker) source
     *
     * Updates button states and invokes source changed callback.
     */
    void select_printer_source();

    /**
     * @brief Switch to USB source
     *
     * Updates button states, scans USB drive, and invokes callbacks.
     */
    void select_usb_source();

    /**
     * @brief Get current file source
     */
    [[nodiscard]] FileSource get_current_source() const {
        return current_source_;
    }

    /// A walk is running or its result has not been delivered yet (UI thread).
    [[nodiscard]] bool is_scanning() const {
        return walk_.in_flight();
    }

    /**
     * @brief Check if USB source is currently active
     */
    [[nodiscard]] bool is_usb_active() const {
        return current_source_ == FileSource::USB;
    }

    // === USB Drive Events ===

    /**
     * @brief Handle USB drive insertion
     *
     * Marks a drive as present (print_source_usb_present subject). Whether
     * the source selector actually becomes visible is a declarative XML
     * binding on that subject combined with print_source_moonraker_usb_access
     * (print_select_panel.xml) — hidden if Moonraker already has symlink
     * access to USB files.
     */
    void on_drive_inserted();

    /**
     * @brief Handle USB drive removal
     *
     * Marks no drive as present (print_source_usb_present subject), which
     * hides the source selector via the same declarative binding as
     * on_drive_inserted(). If USB source was active, switches to Printer
     * and invokes the source changed callback.
     */
    void on_drive_removed();

    /**
     * @brief Set whether Moonraker has direct access to USB files via symlink
     *
     * Mirrors into the print_source_moonraker_usb_access subject, which the
     * source selector's visibility binding combines with
     * print_source_usb_present: hidden whenever Moonraker has access,
     * regardless of drive presence, since files are already reachable via
     * the Printer source. This is the case when Klipper's mod creates
     * a symlink like gcodes/usb -> /media/sda1. Reacts to `has_access` going
     * false too — if a drive is still present, the selector reappears —
     * though the only current call site (PrintSelectPanel::check_moonraker_usb_symlink,
     * ui_panel_print_select.cpp) only ever passes true, so that direction is
     * modeled correctly but not currently reachable from the app.
     *
     * @param has_access true if Moonraker can see USB files via symlink
     */
    void set_moonraker_has_usb_access(bool has_access);

    /**
     * @brief Check if Moonraker has symlink access to USB files
     */
    [[nodiscard]] bool moonraker_has_usb_access() const {
        return moonraker_has_usb_access_;
    }

    // === File Operations ===

    /**
     * @brief Refresh USB file list
     *
     * Walks every connected drive for G-code and extracts header thumbnails
     * on a worker thread, then invokes on_files_ready on the UI thread. One
     * walk runs at a time: a refresh during a walk stops it early and starts
     * one more when it ends. Only the newest refresh delivers, and nothing is
     * delivered once the source has switched back to Printer.
     */
    void refresh_files();

    /**
     * @brief Clear cached USB files
     */
    void clear_files() {
        usb_files_.clear();
    }

    /// Initialize the source subject (call once at startup, before XML is parsed)
    static void init_subjects();

  private:
    // === Dependencies ===
    UsbManager* usb_manager_ = nullptr;

    // === Widget References ===
    lv_obj_t* source_selector_ = nullptr; // Container for all source buttons

    // === State ===
    FileSource current_source_ = FileSource::PRINTER;
    std::vector<UsbGcodeFile> usb_files_;
    bool moonraker_has_usb_access_ = false; ///< True if Moonraker has symlink to USB files

    // === Callbacks ===
    UsbFilesReadyCallback on_files_ready_;
    SourceChangedCallback on_source_changed_;

    /// Every refresh supersedes the walk before it; a switch to Printer
    /// cancels it.
    helix::SingleFlightWalk walk_;

    /// Hand on_files_ready an empty list.
    void deliver_empty();

    // === Internal Methods ===

    /**
     * @brief Update source subject — XML bindings handle button appearance
     */
    void update_button_states();

    /**
     * @brief Convert USB files to PrintFileData format
     * @param thumbnails Cached thumbnail path per usb_files_ entry; empty keeps
     *                   the default thumbnail
     */
    [[nodiscard]] std::vector<PrintFileData>
    convert_to_print_file_data(const std::vector<std::string>& thumbnails) const;
};

} // namespace helix::ui
