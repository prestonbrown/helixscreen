// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_selector_model.h"

#include "lvgl/lvgl.h"
#include "wizard_step.h"

#include <memory>
#include <string>
#include <vector>

/**
 * @file ui_wizard_printer_identify.h
 * @brief Wizard printer identification step - name and type configuration
 *
 * Handles printer identification during first-run wizard:
 * - User-entered printer name
 * - Printer type selection from list
 * - Auto-detection via hardware fingerprinting
 * - Configuration persistence
 *
 * ## Class-Based Architecture (Phase 6)
 *
 * This step has been migrated from function-based to class-based design:
 * - Instance members instead of static globals
 * - Static trampolines for LVGL event callbacks
 * - Global singleton getter for backwards compatibility
 *
 * ## Subject Bindings (6 total):
 *
 * - printer_name (string) - User-entered printer name
 * - printer_type_selected (int) - Selected index in list
 * - printer_detection_status (string) - Auto-detection status message
 * - wizard_printer_view (int) - Selector view: 0 vendor tiles, 1 vendor models, 2 search
 * - wizard_printer_vendor_title (string) - Active vendor, bound to the drill-in header
 * - wizard_printer_match_count (int) - Rows visible under the active view
 *
 * ## External Subject:
 *
 * - connection_test_passed (extern) - Controls wizard Next button globally
 *
 * Initialization Order (CRITICAL):
 *   1. Register XML component (wizard_printer_identify.xml)
 *   2. init_subjects()
 *   3. register_callbacks()
 *   4. create(parent)
 */

/**
 * @class WizardPrinterIdentifyStep
 * @brief Printer identification step for the first-run wizard
 *
 * Allows user to enter printer name and select printer type.
 * Supports auto-detection via hardware fingerprinting.
 */
class WizardPrinterIdentifyStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::PrinterIdentify;
    }
    const char* component_name() const override {
        return "wizard_printer_identify";
    }
    const char* log_name() const override {
        return "Wizard Printer";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return ctx.preset.skip_hardware;
    }

    WizardPrinterIdentifyStep();
    ~WizardPrinterIdentifyStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardPrinterIdentifyStep(const WizardPrinterIdentifyStep&) = delete;
    WizardPrinterIdentifyStep& operator=(const WizardPrinterIdentifyStep&) = delete;
    WizardPrinterIdentifyStep(WizardPrinterIdentifyStep&&) = delete;
    WizardPrinterIdentifyStep& operator=(WizardPrinterIdentifyStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     *
     * Creates and registers 6 subjects. Loads existing values from config.
     * Runs auto-detection if no saved type.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_printer_name_changed
     * - on_printer_type_changed
     * - on_wizard_printer_search_changed
     * - on_wizard_vendor_back_clicked
     */
    void register_callbacks() override;

    /**
     * @brief Create the printer identification UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Cleanup resources
     *
     * Saves current values to config and resets UI references.
     */
    void cleanup() override;

    /**
     * @brief Check if printer identification is complete
     *
     * @return true if printer name is entered
     */
    bool is_validated() const override;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard Printer";
    }

    /**
     * @brief Get detection status message for header subtitle
     * @return Detection status string (e.g., "Loaded from configuration", "Voron 2.4")
     */
    const char* get_detection_status() const {
        return printer_detection_status_buffer_;
    }

    /**
     * @brief Find printer type index by name
     *
     * @param printer_name Name to search for in PrinterDetector list
     * @return Index in the list, or get_unknown_list_index() if not found
     */
    static int find_printer_type_index(const std::string& printer_name);

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Printer preview image widget
    lv_obj_t* printer_preview_image_ = nullptr;

    // Printer type list container (populated once, reparented across wizard visits)
    lv_obj_t* printer_type_list_ = nullptr;

    // Vendor tile grid container (browse level of the type selector)
    lv_obj_t* vendor_tiles_ = nullptr;

    // Persistent off-screen container to cache the populated list across wizard
    // step transitions. The wizard framework destroys the step's widget tree on
    // navigation, but we keep the list alive here to avoid expensive rebuild on
    // revisit (~105 buttons + labels = slow on MIPS, see issue #231).
    lv_obj_t* list_cache_container_ = nullptr;

    // Same caching role as list_cache_container_, for the vendor tiles.
    lv_obj_t* tile_cache_container_ = nullptr;

    // Subjects
    lv_subject_t printer_name_;
    lv_subject_t printer_type_selected_;
    lv_subject_t printer_detection_status_;
    // Selector view state: 0 = vendor tiles, 1 = one vendor's models, 2 = search
    lv_subject_t printer_view_;
    // Active vendor's name while drilled in (bound to the header label)
    lv_subject_t vendor_title_;
    // Rows currently visible under the active view (0 shows "No printers found")
    lv_subject_t match_count_;

    // String buffers (must be persistent)
    char printer_name_buffer_[128];
    char printer_detection_status_buffer_[256];
    char vendor_title_buffer_[64];

    // Selector model: one entry per row, index-aligned with the detector's list
    std::vector<helix::ui::SelectorEntry> selector_entries_;
    // Buckets rendered as tiles; entry pointers address selector_entries_
    std::vector<helix::ui::SelectorGroup> vendor_groups_;

    // Vendor whose models are shown when drilled in; empty at browse level
    std::string active_vendor_;
    // Current search query; empty means normal browsing
    std::string search_query_;

    // State tracking
    bool printer_identify_validated_ = false;
    bool subjects_initialized_ = false;
    bool updating_from_subject_ = false; // Re-entry guard for observer loop prevention
    std::string last_detected_url_;      // Track URL to detect printer changes
    std::string detected_kinematics_;    // Detected kinematics for list filtering

    // Event handler implementations
    void handle_printer_name_changed(lv_event_t* e);
    void handle_printer_type_changed(lv_event_t* e);

    // Printer type list helpers
    void populate_printer_type_list();
    void update_list_selection(int selected_index);
    static void on_printer_type_item_clicked(lv_event_t* e);

    // Selector helpers (search + vendor drill-in over selector_entries_)
    void build_selector_entries();
    void populate_vendor_tiles();
    // Applies row visibility + vendor labels + match count for a view, sets
    // the wizard_printer_view subject the XML containers bind to.
    void apply_view(int view);
    // Opens the bucket holding the current selection and scrolls to it, or
    // the tile grid when the selection has no vendor (pseudo-machine or none).
    void enter_initial_view();

    // Static trampolines for LVGL callbacks
    static void on_printer_name_changed_static(lv_event_t* e);
    static void on_printer_type_changed_static(lv_event_t* e);
    static void on_wizard_printer_search_changed(lv_event_t* e);
    static void on_wizard_vendor_back_clicked(lv_event_t* e);
    static void on_vendor_tile_clicked(lv_event_t* e);
};

// ============================================================================
// Auto-Detection Infrastructure
// ============================================================================

/**
 * @brief Printer auto-detection hint (confidence + reasoning)
 */
struct PrinterDetectionHint {
    int type_index;        // Index into PrinterDetector list
    int confidence;        // 0-100 (>=70 = auto-select, <70 = suggest)
    std::string type_name; // Detected printer type name
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global WizardPrinterIdentifyStep instance
 *
 * Creates the instance on first call. Used by wizard framework.
 */
WizardPrinterIdentifyStep* get_wizard_printer_identify_step();
