// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_printer_manager_overlay.h
 * @brief Printer Manager overlay - displays printer info and management options
 *
 * Shows printer name, model, image, and HelixScreen version information.
 * Entry point for future printer management features (rename, reconfigure, etc).
 *
 * @pattern Overlay (lazy init, subject-bound data)
 * @threading Main thread only
 *
 * @see PrinterDetector for printer image lookup
 * @see Config for printer name/type configuration
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <string>

/**
 * @class PrinterManagerOverlay
 * @brief Overlay displaying printer information and management controls
 *
 * Displays:
 * - Printer name (from config)
 * - Printer model/type (from config)
 * - Printer image (from printer database)
 * - HelixScreen version
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = get_printer_manager_overlay();
 * overlay.init_subjects();
 * overlay.create(parent_screen);
 * @endcode
 */
class PrinterManagerOverlay : public OverlayBase {
  public:
    /**
     * @brief Default constructor - zeroes all buffers
     */
    PrinterManagerOverlay();

    /**
     * @brief Destructor - cleans up subjects if LVGL still initialized
     */
    ~PrinterManagerOverlay() override;

    // Non-copyable (inherited from OverlayBase)

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize subjects for XML data binding
     *
     * Registers printer_manager_name, printer_manager_model, and helix_version
     * subjects for XML binding.
     */
    void init_subjects() override;

    /**
     * @brief Create the overlay UI from XML
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers chip click callbacks for navigable capability chips.
     * Back button is handled by the overlay_panel component.
     */
    void register_callbacks() override;

    /**
     * @brief Get human-readable overlay name
     * @return "Printer Manager"
     */
    const char* get_name() const override {
        return "Printer Manager";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Refreshes printer info from config on each activation.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being closed
     *
     * Cancels any in-progress name edit and hides the keyboard
     * to restore screen position.
     */
    void on_deactivate() override;

  private:
    //
    // === Internal Methods ===
    //

    /**
     * @brief Refresh printer name, model, image, and version from config
     *
     * Called from on_activate() to ensure data is current.
     */
    void refresh_printer_info();

    //
    // === Subject State ===
    //

    SubjectManager subjects_;

    lv_subject_t printer_manager_name_;
    lv_subject_t printer_manager_model_;
    lv_subject_t helix_version_;

    char name_buf_[128];
    char model_buf_[128];
    char version_buf_[32];

    //
    // === Widget References ===
    //

    /// Printer image widget (set programmatically - exception to declarative rule)
    lv_obj_t* printer_image_obj_ = nullptr;

    // Name editing widgets
    lv_obj_t* name_heading_ = nullptr;
    lv_obj_t* name_input_ = nullptr;
    bool name_editing_ = false;

    void start_name_edit();
    void finish_name_edit();
    void cancel_name_edit();

    /// Stored image path (must outlive lv_image_set_src for path stability)
    std::string current_image_path_;

    // Cached panel pointers for lazy creation
    lv_obj_t* bed_mesh_panel_ = nullptr;
    lv_obj_t* spoolman_panel_ = nullptr;
    lv_obj_t* screws_tilt_panel_ = nullptr;
    lv_obj_t* input_shaper_panel_ = nullptr;
    lv_obj_t* retraction_panel_ = nullptr;
    lv_obj_t* timelapse_panel_ = nullptr;
    lv_obj_t* fan_control_panel_ = nullptr;

    //
    // === Static Callbacks ===
    //

    // Chip navigation callbacks
    static void on_chip_bed_mesh_clicked(lv_event_t* e);
    static void on_chip_leds_clicked(lv_event_t* e);
    static void on_chip_adxl_clicked(lv_event_t* e);
    static void on_chip_retraction_clicked(lv_event_t* e);
    static void on_chip_spoolman_clicked(lv_event_t* e);
    static void on_chip_timelapse_clicked(lv_event_t* e);
    static void on_chip_screws_tilt_clicked(lv_event_t* e);
    static void on_chip_ams_clicked(lv_event_t* e);
    static void on_chip_fans_clicked(lv_event_t* e);
    static void on_chip_speaker_clicked(lv_event_t* e);

    // Name editing callbacks
    static void pm_printer_name_clicked_cb(lv_event_t* e);
    static void pm_name_input_ready_cb(lv_event_t* e);
    static void pm_name_input_cancel_cb(lv_event_t* e);

    // Action row callbacks
    static void change_printer_image_clicked_cb(lv_event_t* e);
    static void pm_manage_printers_clicked_cb(lv_event_t* e);
    void handle_change_printer_image_clicked();
    void handle_manage_printers_clicked();
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton PrinterManagerOverlay
 */
PrinterManagerOverlay& get_printer_manager_overlay();

/**
 * @brief Destroy the global PrinterManagerOverlay instance
 */
void destroy_printer_manager_overlay();
