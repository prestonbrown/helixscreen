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

#include "ui_observer_guard.h"

#include "capability_chip_text.h"
#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <array>
#include <string>
#include <vector>

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
 * get_printer_manager_overlay().show(parent_screen);
 * @endcode
 */
class PrinterManagerOverlay : public OverlayBase {
  public:
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
     * @brief Create the overlay UI from XML and wire the name editor
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

    const char* xml_component() const override {
        return "printer_manager_overlay";
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
    void on_deactivating(DeactivateReason reason) override;

    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }

  private:
    friend struct PrinterManagerOverlayTestAccess;

    void on_ui_destroyed() override;

    //
    // === Internal Methods ===
    //

    /**
     * @brief Refresh printer name, model, image, and version from config
     *
     * Called from on_activate() to ensure data is current.
     */
    void refresh_printer_info();

    /**
     * @brief Recompute every capability chip's text and the overlay-owned
     *        visibility subjects from the current printer
     *
     * Runs once per activation, so a printer switched while the overlay was
     * closed never shows the previous printer's details, and again whenever
     * discovery, a backend or a count a chip shows changes while it is open.
     */
    void refresh_capability_chips();

    /// Schedule one refresh_capability_chips(); further requests before it runs join it.
    void request_chip_refresh();

    /// Observe what the chips read; replaces any earlier observers and asks for one refresh.
    void observe_chip_inputs();

    //
    // === Subject State ===
    //

    SubjectManager subjects_;

    lv_subject_t printer_manager_name_{};
    lv_subject_t printer_manager_model_{};
    lv_subject_t helix_version_{};

    /// One string subject per capability chip, in CapChip order.
    static constexpr size_t kChipTextLen = 96;
    std::array<lv_subject_t, helix::kCapChipCount> chip_text_{};
    std::array<std::array<char, kChipTextLen>, helix::kCapChipCount> chip_text_bufs_{};

    /// Visibility for chips whose fact has no app-wide subject.
    lv_subject_t has_kinematics_{};
    lv_subject_t has_build_volume_{};
    lv_subject_t has_filament_system_{};
    lv_subject_t has_tool_changer_{};
    lv_subject_t has_mcus_{};

    /// Everything a chip's text or visibility reads that can change while open.
    std::vector<ObserverGuard> chip_input_observers_;
    bool chip_refresh_pending_ = false;
    int chip_refresh_count_ = 0; ///< refreshes run, for tests

    char name_buf_[128] = {};
    char model_buf_[128] = {};
    char version_buf_[32] = {};

    //
    // === Widget References ===
    //

    /// Printer image widget (set programmatically - exception to declarative rule)
    lv_obj_t* printer_image_obj_ = nullptr;

    // Name editing state (subject-driven visibility via bind_flag_if_eq in XML)
    lv_subject_t name_editing_{};
    lv_obj_t* name_input_ = nullptr;

    void start_name_edit();
    void finish_name_edit();
    void cancel_name_edit();

    /// Stored image path (must outlive lv_image_set_src for path stability)
    std::string current_image_path_;
};

/// Global instance, built on first use and freed with StaticPanelRegistry.
inline PrinterManagerOverlay& get_printer_manager_overlay() {
    return helix::lazy_global<PrinterManagerOverlay>("PrinterManagerOverlay");
}

/// Free the global instance now; the next accessor call builds a fresh one.
inline void destroy_printer_manager_overlay() {
    helix::detail::lazy_global_slot<PrinterManagerOverlay>().reset();
}
