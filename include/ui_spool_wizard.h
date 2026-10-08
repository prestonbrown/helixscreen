// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"
#include "spoolman_types.h"
#include "subject_managed_panel.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hv/json.hpp"

// Forward declarations
namespace helix::ui {
class ColorPicker;
}

/**
 * @brief 3-step wizard overlay for creating new spools in Spoolman
 *
 * Steps:
 *   0 = Select Vendor
 *   1 = Select Filament
 *   2 = Spool Details (weight, price, lot, notes)
 *
 * Navigation logic is testable without LVGL — step transitions, labels,
 * and can_proceed gating are pure C++ state.
 *
 * XML layout: spool_wizard.xml
 * Capability-gated: Only accessible when printer_has_spoolman=1
 */
class SpoolWizardOverlay : public OverlayBase {
  public:
    SpoolWizardOverlay();
    ~SpoolWizardOverlay() override;

    // ========== Step enum ==========
    enum class Step : int { VENDOR = 0, FILAMENT = 1, SPOOL_DETAILS = 2 };

    static constexpr int STEP_COUNT = 3;

    // ========== OverlayBase interface ==========
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "SpoolWizard";
    }
    void on_activate() override;
    void on_deactivate() override;

    // ========== Step navigation (public for testing) ==========
    Step current_step() const {
        return current_step_;
    }
    void navigate_next();
    void navigate_back();

    // ========== Proceed gating ==========
    bool can_proceed() const {
        return can_proceed_;
    }
    void set_can_proceed(bool val);

    // ========== Step label (pure C++ — no LVGL needed) ==========
    std::string step_label() const;

    // ========== Callbacks ==========
    using CompletionCallback = std::function<void()>;
    using CloseCallback = std::function<void()>;

    void set_completion_callback(CompletionCallback cb) {
        completion_callback_ = std::move(cb);
    }
    void set_close_callback(CloseCallback cb) {
        close_callback_ = std::move(cb);
    }

    /**
     * @brief Called when the user taps "Create Spool" on the final step.
     *
     * Fires the completion callback. The actual Spoolman API call
     * will be wired in a later step.
     */
    void on_create_requested();

    // ========== Filament entry (merged from SpoolmanDB + server) ==========
    struct FilamentEntry {
        std::string name;       ///< e.g., "PLA Red"
        std::string material;   ///< e.g., "PLA"
        std::string color_hex;  ///< e.g., "FF0000"
        std::string color_name; ///< e.g., "Red"
        int server_id = -1;     ///< Spoolman server ID, -1 = not on server
        int vendor_id = -1;
        double density = 0;
        double diameter = 1.75; ///< Filament diameter in mm (default: 1.75)
        double weight = 0;
        double spool_weight = 0;
        int nozzle_temp_min = 0;
        int nozzle_temp_max = 0;
        int bed_temp_min = 0;
        int bed_temp_max = 0;
        bool from_server = false;
        bool from_database = false;
    };

    // ========== Vendor entry (merged from SpoolmanDB + server) ==========
    struct VendorEntry {
        std::string name;
        int server_id = -1;         ///< Spoolman server ID, -1 = DB-only
        bool from_server = false;   ///< Present on Spoolman server
        bool from_database = false; ///< Present in SpoolmanDB
    };

    // ========== Vendor step logic (public for testing) ==========

    /// Merge external DB vendors with server vendors, deduplicate by name
    static std::vector<VendorEntry> merge_vendors(const std::vector<VendorEntry>& external_vendors,
                                                  const std::vector<VendorEntry>& server_vendors);

    /// Filter vendor list by case-insensitive substring match
    static std::vector<VendorEntry> filter_vendor_list(const std::vector<VendorEntry>& vendors,
                                                       const std::string& query);

    /// Load vendors from server + SpoolmanDB, merge, and populate list
    void load_vendors();

    /// Apply search filter and repopulate the vendor list UI
    void filter_vendors(const std::string& query);

    /// Select an existing vendor by index in filtered_vendors_
    void select_vendor(int index);

    /// Set a new (to-be-created) vendor name and URL
    void set_new_vendor(const std::string& name, const std::string& url);

    /// Repopulate the vendor_list UI container from filtered_vendors_
    void populate_vendor_list();

    /// Access to vendor state for testing
    const std::vector<VendorEntry>& all_vendors() const {
        return all_vendors_;
    }
    const std::vector<VendorEntry>& filtered_vendors() const {
        return filtered_vendors_;
    }
    const VendorEntry& selected_vendor() const {
        return selected_vendor_;
    }
    const std::string& new_vendor_name() const {
        return new_vendor_name_;
    }
    const std::string& new_vendor_url() const {
        return new_vendor_url_;
    }

    // ========== Filament step logic (public for testing) ==========

    /// Merge server filaments with external DB filaments, deduplicate by material+color_hex
    static std::vector<FilamentEntry>
    merge_filaments(const std::vector<FilamentInfo>& server_filaments,
                    const std::vector<FilamentInfo>& external_filaments);

    /// The POST /v1/filament body for @p f under @p vendor_id
    static nlohmann::json filament_create_payload(const FilamentEntry& f, int vendor_id);

    /// Load filaments for the selected vendor from server + SpoolmanDB
    void load_filaments();

    /// Select an existing filament by index in all_filaments_
    void select_filament(int index);

    /// Set material for a new filament (auto-fills temps/density from database)
    void set_new_filament_material(const std::string& material);

    /// Set color for a new filament
    void set_new_filament_color(const std::string& hex, const std::string& name);

    /// Repopulate the filament_list UI container from all_filaments_
    void populate_filament_list();

    /// Check if new filament fields are sufficient to proceed
    void update_new_filament_can_proceed();

    /// Access to filament state for testing
    const std::vector<FilamentEntry>& all_filaments() const {
        return all_filaments_;
    }
    const FilamentEntry& selected_filament() const {
        return selected_filament_;
    }
    const std::string& new_filament_material() const {
        return new_filament_material_;
    }
    const std::string& new_filament_color_hex() const {
        return new_filament_color_hex_;
    }
    const std::string& new_filament_color_name() const {
        return new_filament_color_name_;
    }
    const std::string& new_filament_name() const {
        return new_filament_name_;
    }
    int new_filament_nozzle_min() const {
        return new_filament_nozzle_min_;
    }
    int new_filament_nozzle_max() const {
        return new_filament_nozzle_max_;
    }
    int new_filament_bed_min() const {
        return new_filament_bed_min_;
    }
    int new_filament_bed_max() const {
        return new_filament_bed_max_;
    }
    double new_filament_density() const {
        return new_filament_density_;
    }

    // ========== Spool details state (public for testing) ==========
    double spool_remaining_weight() const {
        return spool_remaining_weight_;
    }
    double spool_price() const {
        return spool_price_;
    }
    const std::string& spool_lot_nr() const {
        return spool_lot_nr_;
    }
    const std::string& spool_notes() const {
        return spool_notes_;
    }

  private:
    // ========== Navigation state ==========
    Step current_step_ = Step::VENDOR;
    bool can_proceed_ = false;
    bool callbacks_registered_ = false;

    // ========== Callbacks ==========
    CompletionCallback completion_callback_;
    CloseCallback close_callback_;

    // ========== Subjects ==========
    SubjectManager subjects_;
    lv_subject_t step_subject_;
    lv_subject_t can_proceed_subject_;
    lv_subject_t step_label_subject_;
    lv_subject_t creating_subject_;
    lv_subject_t selected_vendor_name_subject_;
    lv_subject_t summary_vendor_subject_;
    lv_subject_t summary_filament_subject_;
    lv_subject_t show_create_vendor_subject_;
    lv_subject_t show_create_filament_subject_;
    lv_subject_t vendor_count_subject_;
    lv_subject_t filament_count_subject_;
    lv_subject_t vendors_loading_subject_;
    lv_subject_t filaments_loading_subject_;
    lv_subject_t can_create_vendor_subject_;

    // ========== String buffers for subjects ==========
    char step_label_buf_[64] = {};
    char selected_vendor_name_buf_[128] = {};
    char summary_vendor_buf_[128] = {};
    char summary_filament_buf_[128] = {};

    // ========== Vendor step state ==========
    std::vector<VendorEntry> all_vendors_;
    std::vector<VendorEntry> filtered_vendors_;
    VendorEntry selected_vendor_;
    std::string new_vendor_name_;
    std::string new_vendor_url_;
    std::string vendor_search_query_;

    // ========== Filament step state ==========
    std::vector<FilamentEntry> all_filaments_;
    FilamentEntry selected_filament_;
    bool creating_new_filament_ = false;
    std::string new_filament_name_;
    std::string new_filament_material_;
    std::string new_filament_color_hex_;
    std::string new_filament_color_name_;
    int new_filament_nozzle_min_ = 0;
    int new_filament_nozzle_max_ = 0;
    int new_filament_bed_min_ = 0;
    int new_filament_bed_max_ = 0;
    double new_filament_density_ = 0;
    double new_filament_weight_ = 0;
    double new_filament_spool_weight_ = 0;
    std::unique_ptr<helix::ui::ColorPicker> color_picker_;

    // ========== Spool details state ==========
    double spool_remaining_weight_ = 0;
    double spool_price_ = 0;
    std::string spool_lot_nr_;
    std::string spool_notes_;

    // ========== Create vendor modal ==========
    lv_obj_t* create_vendor_dialog_ = nullptr;

    // ========== Create filament modal ==========
    lv_obj_t* create_filament_dialog_ = nullptr;

    // ========== Creation flow tracking ==========
    int created_vendor_id_ = -1;
    int created_filament_id_ = -1;

    // ========== State management ==========
    void reset_state();

    // ========== Navigation helpers ==========
    void navigate_to_step(Step step);
    void update_step_label();
    void sync_subjects();

    // ========== Creation flow helpers ==========
    void set_creating(bool val);
    void create_vendor_then_filament_then_spool();
    void create_filament_then_spool(int vendor_id);
    void create_spool(int filament_id);
    void on_creation_success(const SpoolInfo& spool);
    void on_creation_error(const std::string& message, int rollback_vendor_id = -1,
                           int rollback_filament_id = -1);

    // ========== Static event callbacks ==========
    static void on_wizard_vendor_selected(lv_event_t* e);
    static void on_wizard_back(lv_event_t* e);
    static void on_wizard_next(lv_event_t* e);
    static void on_wizard_create(lv_event_t* e);
    static void on_wizard_show_create_vendor_modal(lv_event_t* e);
    static void on_wizard_cancel_create_vendor(lv_event_t* e);
    static void on_wizard_vendor_search_changed(lv_event_t* e);
    static void on_wizard_new_vendor_name_changed(lv_event_t* e);
    static void on_wizard_new_vendor_url_changed(lv_event_t* e);
    static void on_wizard_confirm_create_vendor(lv_event_t* e);
    static void on_wizard_filament_selected(lv_event_t* e);
    static void on_wizard_show_create_filament_modal(lv_event_t* e);
    static void on_wizard_cancel_create_filament(lv_event_t* e);
    static void on_wizard_material_changed(lv_event_t* e);
    static void on_wizard_new_filament_name_changed(lv_event_t* e);
    static void on_wizard_pick_filament_color(lv_event_t* e);
    static void on_wizard_nozzle_temp_changed(lv_event_t* e);
    static void on_wizard_bed_temp_changed(lv_event_t* e);
    static void on_wizard_filament_weight_changed(lv_event_t* e);
    static void on_wizard_spool_weight_changed(lv_event_t* e);
    static void on_wizard_confirm_create_filament(lv_event_t* e);
    static void on_wizard_remaining_weight_changed(lv_event_t* e);
    static void on_wizard_price_changed(lv_event_t* e);
    static void on_wizard_lot_changed(lv_event_t* e);
    static void on_wizard_notes_changed(lv_event_t* e);
};

// ============================================================================
// Global Instance Accessor
// ============================================================================

/**
 * @brief Get global SpoolWizardOverlay instance
 * @return Reference to the singleton overlay
 *
 * Creates the instance on first call. Used by static callbacks.
 */
SpoolWizardOverlay& get_global_spool_wizard();
