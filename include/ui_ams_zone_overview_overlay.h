// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_zone_overview_overlay.h
 * @brief The environment zone list, for rigs whose zones are not interchangeable.
 *
 * Shown when select_zone_presentation() answers List: too many zones for a tab strip,
 * or zones that differ in whether they can dry. A row drills into
 * AmsEnvironmentOverlay scoped to that one zone.
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Main thread only
 */

#pragma once

#include "ams_environment_zone.h"
#include "helix/xml/indexed_subject_pool.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

namespace helix::ui {

class AmsZoneOverviewOverlay : public OverlayBase {
  public:
    AmsZoneOverviewOverlay();
    ~AmsZoneOverviewOverlay() override;

    AmsZoneOverviewOverlay(const AmsZoneOverviewOverlay&) = delete;
    AmsZoneOverviewOverlay& operator=(const AmsZoneOverviewOverlay&) = delete;

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

    /// Reclaim the row pools. Not on_deactivating(): that fires on every back-navigation
    /// while the widgets survive, so reclaiming there would leave live rows observing
    /// torn-down subject storage.
    void on_ui_destroyed() override;

    const char* get_name() const override {
        return "AMS Zone Overview";
    }

    /// Show the list for @p zones. The caller decides the set: one unit's zones from the
    /// environment indicator, every zone on the printer from the detail overlay's header.
    void show(lv_obj_t* parent_screen, std::vector<helix::printer::EnvironmentZone> zones);

  private:
    static void on_zone_row_clicked(lv_event_t* e);

    /// Size the pools, fill them, then publish the count. Populating first keeps the
    /// repeat from binding to unset subjects and flashing on its first frame.
    void rebuild_rows();

    lv_obj_t*& overlay_ = overlay_root_;

    std::vector<helix::printer::EnvironmentZone> zones_;

    /// False once the widgets are gone. The singleton outlives them, and a deferred
    /// rebuild must not re-grow pools for a repeat that no longer exists.
    bool ui_alive_ = false;

    SubjectManager subjects_;

    lv_subject_t count_subject_{};
    lv_subject_t subtitle_subject_{};
    char subtitle_buf_[96] = {};

    helix::xml::IndexedSubjectPool label_pool_{"zone_ov_label",
                                               helix::xml::IndexedSubjectPool::Type::String};
    helix::xml::IndexedSubjectPool slots_pool_{"zone_ov_slots",
                                               helix::xml::IndexedSubjectPool::Type::String};
    helix::xml::IndexedSubjectPool reading_pool_{"zone_ov_reading",
                                                 helix::xml::IndexedSubjectPool::Type::String};
    helix::xml::IndexedSubjectPool status_pool_{"zone_ov_status",
                                                helix::xml::IndexedSubjectPool::Type::String};
    helix::xml::IndexedSubjectPool verdict_pool_{"zone_ov_verdict",
                                                 helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool group_hidden_pool_{"zone_ov_group_hidden",
                                                      helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool group_text_pool_{"zone_ov_group_text",
                                                    helix::xml::IndexedSubjectPool::Type::String};
};

/// Global instance accessor. Creates on first access and registers for cleanup.
AmsZoneOverviewOverlay& get_ams_zone_overview_overlay();

} // namespace helix::ui
