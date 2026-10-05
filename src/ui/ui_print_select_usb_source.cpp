// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_usb_source.h"

#include "ui_panel_print_select.h" // For PrintFileData
#include "ui_print_select_card_view.h"

#include "gcode_parser.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "http_executor.h"
#include "print_file_data.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "thumbnail_cache.h"
#include "usb_manager.h"

#include <spdlog/spdlog.h>

#include <iterator>

namespace helix::ui {

// Reads the stick: a directory walk plus a header read per file, slow enough
// on a large stick to stall a frame, so it never runs on the UI thread.
UsbScan scan_usb_drives(UsbBackend& backend, const std::vector<UsbDrive>& drives,
                        const std::function<bool()>& cancelled) {
    UsbScan scan;
    // Every drive contributes to one flat list: a file's path already carries
    // its mount point, so a second stick needs no selector to be reachable.
    for (const auto& drive : drives) {
        if (cancelled()) {
            return scan;
        }
        std::vector<UsbGcodeFile> files;
        const UsbError result = backend.scan_for_gcode(drive.mount_path, files, 3);
        if (!result.success()) {
            spdlog::warn("[UsbSource] Failed to scan '{}': {}", drive.label, result.technical_msg);
            continue;
        }
        spdlog::info("[UsbSource] Found {} G-code files on USB drive '{}'", files.size(),
                     drive.label);
        scan.files.insert(scan.files.end(), std::make_move_iterator(files.begin()),
                          std::make_move_iterator(files.end()));
    }

    scan.thumbnails.reserve(scan.files.size());
    for (const auto& file : scan.files) {
        if (cancelled()) {
            return scan;
        }
        std::string cache_path;
        auto best = helix::gcode::get_best_thumbnail(file.path);
        if (!best.png_data.empty()) {
            cache_path = get_thumbnail_cache().save_raw_png("usb:" + file.path, best.png_data);
        }
        scan.thumbnails.push_back(std::move(cache_path));
    }
    return scan;
}

// Subject for source tab state: 0 = Printer (default), 1 = USB
static lv_subject_t s_print_source_is_usb;
// Whether at least one USB drive is currently connected.
static lv_subject_t s_print_source_usb_present;
// Whether Moonraker has direct symlink access to USB files (e.g. Klipper's
// mod creates gcodes/usb -> /media/sda1) — when true, our own source
// selector is redundant, since the files already show up under Printer.
static lv_subject_t s_print_source_moonraker_usb_access;
static bool s_source_subject_initialized = false;
static SubjectManager s_subjects;

void PrintSelectUsbSource::init_subjects() {
    if (s_source_subject_initialized)
        return;
    lv_subject_init_int(&s_print_source_is_usb, 0);
    s_subjects.publish("print_source_is_usb", &s_print_source_is_usb);
    SubjectDebugRegistry::instance().register_subject(&s_print_source_is_usb, "print_source_is_usb",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    lv_subject_init_int(&s_print_source_usb_present, 0);
    s_subjects.publish("print_source_usb_present", &s_print_source_usb_present);
    SubjectDebugRegistry::instance().register_subject(&s_print_source_usb_present,
                                                      "print_source_usb_present",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    lv_subject_init_int(&s_print_source_moonraker_usb_access, 0);
    s_subjects.publish("print_source_moonraker_usb_access", &s_print_source_moonraker_usb_access);
    SubjectDebugRegistry::instance().register_subject(&s_print_source_moonraker_usb_access,
                                                      "print_source_moonraker_usb_access",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    s_source_subject_initialized = true;

    // Self-register cleanup with StaticSubjectRegistry (co-located with init
    // — see CLAUDE.md's "Subject shutdown safety"). Covers all three
    // subjects: s_print_source_is_usb predates this and was never
    // registered before, so this also closes that pre-existing gap.
    StaticSubjectRegistry::instance().register_deinit("PrintSelectUsbSourceSubjects", []() {
        if (s_source_subject_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_source_subject_initialized = false;
            spdlog::trace("[UsbSource] Subjects deinitialized");
        }
    });

    spdlog::debug("[UsbSource] Subjects initialized (print_source_is_usb, "
                  "print_source_usb_present, print_source_moonraker_usb_access)");
}

// ============================================================================
// Setup
// ============================================================================

bool PrintSelectUsbSource::setup(lv_obj_t* panel) {
    if (!panel) {
        return false;
    }

    // Find the source selector container
    source_selector_ = lv_obj_find_by_name(panel, "source_selector");
    if (!source_selector_) {
        spdlog::warn("[UsbSource] Source selector container not found");
        return false;
    }

    // Visibility is declarative (print_select_panel.xml's bind_flag_if on
    // source_selector, driven by print_source_usb_present and
    // print_source_moonraker_usb_access) — no imperative hide/show here.
    // Both subjects default to 0 at init, so the selector already starts
    // hidden by the time this runs.

    // Set initial state - Printer is selected by default
    update_button_states();

    spdlog::debug("[UsbSource] Source selector found (visibility bound to "
                  "print_source_usb_present / print_source_moonraker_usb_access)");
    return true;
}

void PrintSelectUsbSource::set_usb_manager(UsbManager* manager) {
    usb_manager_ = manager;

    // If USB source is currently active, refresh the file list
    if (current_source_ == FileSource::USB && usb_manager_) {
        refresh_files();
    }

    // Reflect current drive presence — covers the startup race where a
    // drive was detected (UsbBackendMock's demo-insert thread, or a real
    // drive already plugged in) before this panel existed to be told about
    // it. Whether the selector actually shows is decided declaratively in
    // XML from this subject combined with print_source_moonraker_usb_access
    // — not decided here, and not conditioned on moonraker_has_usb_access_
    // the way the old imperative check was (that's now the binding's job).
    const bool has_drives = manager && !manager->get_drives().empty();
    if (s_source_subject_initialized) {
        lv_subject_set_int(&s_print_source_usb_present, has_drives ? 1 : 0);
    }
    if (has_drives) {
        spdlog::info("[UsbSource] USB drive already present at setup");
    }

    spdlog::debug("[UsbSource] UsbManager set");
}

// ============================================================================
// Source Selection
// ============================================================================

void PrintSelectUsbSource::select_printer_source() {
    if (current_source_ == FileSource::PRINTER) {
        return; // Already on Printer source
    }

    spdlog::debug("[UsbSource] Switching to Printer source");
    current_source_ = FileSource::PRINTER;
    usb_files_.clear();
    walk_.cancel(); // its result has nowhere to show
    update_button_states();

    if (on_source_changed_) {
        on_source_changed_(FileSource::PRINTER);
    }
}

void PrintSelectUsbSource::select_usb_source() {
    if (current_source_ == FileSource::USB) {
        return; // Already on USB source
    }

    spdlog::debug("[UsbSource] Switching to USB source");
    current_source_ = FileSource::USB;
    update_button_states();

    if (on_source_changed_) {
        on_source_changed_(FileSource::USB);
    }

    // Refresh USB files
    refresh_files();
}

// ============================================================================
// USB Drive Events
// ============================================================================

void PrintSelectUsbSource::on_drive_inserted() {
    // A drive is now present. Whether the selector actually shows — i.e.
    // whether Moonraker also lacks symlink access — is the XML binding's
    // job (print_select_panel.xml combines this with
    // print_source_moonraker_usb_access), not this method's.
    spdlog::debug("[UsbSource] USB drive inserted");
    if (s_source_subject_initialized) {
        lv_subject_set_int(&s_print_source_usb_present, 1);
    }
}

void PrintSelectUsbSource::set_moonraker_has_usb_access(bool has_access) {
    moonraker_has_usb_access_ = has_access;

    // Mirror into the subject that drives source_selector's visibility
    // binding — declarative, not an imperative show/hide here. Unlike the
    // old imperative version (which only ever hid on has_access==true, with
    // no path back to visible if access were revoked), this reacts to
    // has_access going false too: if a drive is still present, the
    // selector correctly reappears.
    if (s_source_subject_initialized) {
        lv_subject_set_int(&s_print_source_moonraker_usb_access, has_access ? 1 : 0);
    }

    if (has_access) {
        // Files are accessible via Printer source; our own picker becomes
        // redundant (the binding hides it) — but if the user was actively
        // viewing the now-redundant USB tab, still switch back to Printer.
        spdlog::debug("[UsbSource] Moonraker has USB symlink access - source selector will hide");

        select_printer_source();
    }
}

void PrintSelectUsbSource::on_drive_removed() {
    spdlog::info("[UsbSource] USB drive removed");

    // The removal event does not say which drive went, so ask the manager
    // what is still mounted: the source stays available while any drive is.
    const bool drives_remain = usb_manager_ && !usb_manager_->get_drives().empty();
    if (s_source_subject_initialized) {
        lv_subject_set_int(&s_print_source_usb_present, drives_remain ? 1 : 0);
    }

    if (drives_remain) {
        if (current_source_ == FileSource::USB) {
            spdlog::debug("[UsbSource] Other drives remain - rescanning USB source");
            refresh_files();
        }
        return;
    }

    select_printer_source();
}

// ============================================================================
// File Operations
// ============================================================================

void PrintSelectUsbSource::refresh_files() {
    usb_files_.clear();
    if (!usb_manager_) {
        spdlog::warn("[UsbSource] UsbManager not available");
        walk_.cancel();
        deliver_empty();
        return;
    }

    auto drives = usb_manager_->get_drives();
    // The worker holds the backend, never the manager: the application
    // destroys the manager before it stops the executors.
    auto backend = usb_manager_->backend_snapshot();
    if (drives.empty() || !backend) {
        spdlog::debug("[UsbSource] No USB drives detected");
        walk_.cancel();
        deliver_empty();
        return;
    }

    // The fast lane, one walk at a time: it holds at most one of the four
    // workers, while the slow lane's single worker would queue the listing
    // behind any large G-code transfer.
    walk_.run([this, backend, drives = std::move(drives)](
                  const helix::SingleFlightWalk::Cancelled& cancelled) -> std::function<void()> {
        auto scan = scan_usb_drives(*backend, drives, cancelled);
        return [this, scan = std::move(scan)]() mutable {
            usb_files_ = std::move(scan.files);
            if (on_files_ready_) {
                on_files_ready_(convert_to_print_file_data(scan.thumbnails));
            }
        };
    });
}

void PrintSelectUsbSource::deliver_empty() {
    if (on_files_ready_) {
        on_files_ready_(std::vector<PrintFileData>{});
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void PrintSelectUsbSource::update_button_states() {
    // Update subject — XML bind_flag_if_not_eq handles button visibility/appearance
    if (s_source_subject_initialized) {
        lv_subject_set_int(&s_print_source_is_usb, current_source_ == FileSource::USB ? 1 : 0);
    }
}

std::vector<PrintFileData>
PrintSelectUsbSource::convert_to_print_file_data(const std::vector<std::string>& thumbnails) const {
    std::vector<PrintFileData> result;
    result.reserve(usb_files_.size());

    const std::string default_thumbnail = PrintSelectCardView::get_default_thumbnail();
    for (size_t i = 0; i < usb_files_.size(); ++i) {
        auto file_data = PrintFileData::from_usb_file(usb_files_[i], default_thumbnail);
        if (i < thumbnails.size() && !thumbnails[i].empty()) {
            file_data.thumbnail_path = thumbnails[i];
        }
        result.push_back(std::move(file_data));
    }

    return result;
}

} // namespace helix::ui
