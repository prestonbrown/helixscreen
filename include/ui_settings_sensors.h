// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_sensors.h
 * @brief Settings > Sensors: one section per sensor manager, every row a sensor_row
 *
 * Each SensorManager with a settings_section() gets a section in
 * sensors_overlay.xml. The section is a `<repeat count="sensor_<slug>_row_count">`
 * of sensor_row, bound to row subjects a SensorRowSection fills from the
 * manager's row(i). Rows stay live while the overlay is shown: each section
 * refills when its manager's revision subject moves.
 *
 * Extras beside the rows: the filament master toggle, the input door-check
 * toggle and the chamber assignment dropdowns (which write SettingsManager).
 *
 * @pattern Overlay (lazy init, declarative rows)
 * @threading Main thread only
 */

#pragma once

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "sensor_manager.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace helix::settings {

/**
 * @brief The row subjects of one Settings > Sensors section.
 *
 * Owns sensor_<slug>_row_<field>_<i> for every row it has ever shown, plus
 * sensor_<slug>_row_count. Subjects grow on demand and are never shrunk while
 * the section lives, so a `<repeat>` rebuild never binds a dead subject; they
 * register with the SubjectManager passed in and deinit with it, which must
 * therefore be destroyed before this section.
 *
 * Edits arrive from sensor_row's callbacks with user_data "<slug>:<i>" and go to
 * the section of that slug: a role pick maps the option index through the
 * manager's offered_roles(), never by enum value.
 */
class SensorRowSection {
  public:
    SensorRowSection(sensors::SensorManager& manager, SubjectManager& subjects);
    ~SensorRowSection();
    SensorRowSection(const SensorRowSection&) = delete;
    SensorRowSection& operator=(const SensorRowSection&) = delete;

    [[nodiscard]] const std::string& slug() const {
        return slug_;
    }
    [[nodiscard]] sensors::SensorManager& manager() const {
        return *manager_;
    }

    /// Write every listed sensor of @p manager into the row subjects, growing
    /// them first, then set the row count.
    void fill(const sensors::SensorManager& manager);

    /// Rows the last fill() listed.
    [[nodiscard]] size_t row_count() const {
        return slots_.size();
    }

    /// The user picked option @p option in row @p row's role dropdown.
    void pick_role(size_t row, int option);
    /// The user flipped row @p row's toggle (Enabled or Invert, per the row).
    void set_toggle(size_t row, bool on);

    /// Refill on every revision of the manager, then call @p after_fill;
    /// stop() ends it.
    void watch(std::function<void()> after_fill);
    void stop();

    /// The live section for @p slug, or nullptr.
    [[nodiscard]] static SensorRowSection* find(const std::string& slug);

  private:
    struct Row;
    struct Slot {
        std::string klipper_name;
        sensors::RowToggle toggle = sensors::RowToggle::None;
    };

    void grow(size_t rows);
    /// Whether a revision can have changed what the rows show: always for a
    /// manager whose rows show readings, else only when its entries changed.
    [[nodiscard]] bool rows_may_have_changed() const;

    sensors::SensorManager* manager_;
    SubjectManager& subjects_;
    std::string slug_;
    std::string count_id_;
    lv_subject_t count_{};
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<Slot> slots_;
    std::vector<sensors::SensorEntry> filled_entries_; ///< entries the last fill() read
    ObserverGuard revision_observer_;
    std::function<void()> after_fill_;
};

/**
 * @class SensorSettingsOverlay
 * @brief Overlay listing every sensor manager's sensors
 *
 * @code
 * auto& overlay = helix::settings::get_sensor_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class SensorSettingsOverlay : public OverlayBase {
  public:
    ~SensorSettingsOverlay() override;

    /// Row, master-toggle, door-check and chamber assignment callbacks.
    void register_callbacks() override;

    /// One SensorRowSection per manager with a settings section, plus
    /// sensor_total_count.
    void init_subjects() override;

    const char* get_name() const override {
        return "Sensors";
    }
    const char* xml_component() const override {
        return "sensors_overlay";
    }

    /// Fills every section before the XML is built, so the first expansion
    /// binds filled rows.
    lv_obj_t* create(lv_obj_t* parent) override;

    /// Refills every section and starts the live refresh.
    void on_activate() override;

    void handle_switch_master_toggle_changed(bool enabled);

    /// The section for @p slug, or nullptr (tests).
    [[nodiscard]] SensorRowSection* section(const std::string& slug) const;

  protected:
    void on_deactivating(DeactivateReason reason) override;

  private:
    void refill_all();
    /// sensor_total_count: every section's listed rows, summed (no-sensors placeholder).
    void update_total();

    /// Populate chamber assignment dropdowns from PrinterDiscovery
    void populate_chamber_assignment();

    /// Klipper object behind each chamber dropdown's option index i + 1, rebuilt
    /// with the options on every populate.
    std::vector<std::string> chamber_heater_names_;
    std::vector<std::string> chamber_sensor_names_;

    std::vector<std::unique_ptr<SensorRowSection>> sections_;
    lv_subject_t total_count_{};
    /// Declared after sections_ so it deinits their subjects before they are freed.
    SubjectManager subjects_;
};

inline SensorSettingsOverlay& get_sensor_settings_overlay() {
    return lazy_global<SensorSettingsOverlay>("SensorSettingsOverlay");
}

struct ChamberAssignmentLabels;

/// Translated labels both chamber assignment dropdowns are built from.
ChamberAssignmentLabels chamber_assignment_labels();

} // namespace helix::settings
