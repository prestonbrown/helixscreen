// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"
#include "single_flight_walk.h"
#include "static_panel_registry.h"

#include <string>
#include <vector>

class UsbManager;

namespace helix::settings {

/**
 * @class PrinterImageOverlay
 * @brief Overlay for browsing and selecting printer images
 *
 * Displays shipped and custom printer images in a left-list + right-preview layout.
 * Users can select an image or choose auto-detect mode. Only one image preview
 * is loaded at a time, keeping RAM usage minimal.
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_printer_image_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class PrinterImageOverlay : public OverlayBase {
  public:
    ~PrinterImageOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Printer Image";
    }
    const char* xml_component() const override {
        return "printer_image_overlay";
    }

    void on_activate() override;

    //
    // === Event Handlers (public for the callback table) ===
    //

    void handle_auto_detect();
    void handle_image_selected(const std::string& image_id);

    /// Provide USB manager for USB image import
    void set_usb_manager(UsbManager* manager);

    /// Re-populate custom images list (public for async callback)
    void refresh_custom_images();

    /// A stick image walk is running or its result has not been shown yet.
    [[nodiscard]] bool usb_scan_in_flight() const {
        return usb_walk_.in_flight();
    }

  private:
    //
    // === Internal Methods ===
    //

    void populate_shipped_images();
    void populate_custom_images();
    void scan_usb_drives();
    void populate_usb_images(const std::vector<std::string>& image_paths);
    void handle_usb_import(const std::string& source_path);
    lv_obj_t* create_list_row(lv_obj_t* parent, const std::string& image_id,
                              const std::string& display_name, const char* callback_name);
    void update_selection_indicator(const std::string& active_id);
    void update_preview(const std::string& image_id, const std::string& display_name,
                        const std::string& preview_path);
    std::string get_preview_path_for_id(const std::string& image_id);
    /// printer_image_tag_state for the displayed image.
    void update_tag_state();
    void handle_tag_parts();
    void handle_reset_tags();
    /// Delete the user's tags for `key`, after the reset is confirmed.
    void reset_tags(const std::string& key);

    //
    // === Members ===
    //

    UsbManager* usb_manager_ = nullptr;
    /// Walks the stick for images off the UI thread.
    helix::SingleFlightWalk usb_walk_;

    // RAII subject manager for automatic cleanup
    SubjectManager subjects_;

    // Subjects for declarative USB section bindings
    lv_subject_t usb_visible_subject_{}; // int: 0=hidden, 1=visible
    lv_subject_t usb_status_subject_{};  // string: status text
    char usb_status_buf_[256] = {};

    // Preview subjects for right-side panel
    lv_subject_t preview_src_subject_{};  // pointer: image path for bind_src
    char preview_src_buf_[512] = {};      // buffer for preview path string
    lv_subject_t preview_name_subject_{}; // string: display name
    char preview_name_buf_[128] = {};
    lv_subject_t has_preview_subject_{}; // int: 0=no preview, 1=has preview
    lv_subject_t tag_state_subject_{};   // int: 0=untaggable, 1=untagged by user, 2=user tags
};

inline PrinterImageOverlay& get_printer_image_overlay() {
    return lazy_global<PrinterImageOverlay>("PrinterImageOverlay");
}

} // namespace helix::settings
