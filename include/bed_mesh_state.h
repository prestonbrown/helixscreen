// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "bed_mesh_stats.h"
#include "core/observer_guard.h"
#include "i_moonraker_api.h"
#include "moonraker_types.h"
#include "subject_managed_panel.h"
#include "subscription_guard.h"

namespace helix {

/// The active bed mesh as the rest of the UI reads it: loaded or not, profile name,
/// grid size, extremes, range and profile count, published as subjects. It exists from
/// connect, so a home tile reads current values without the Bed Mesh panel ever having
/// been opened. BedMeshPanel binds the same subjects.
///
/// `bed_mesh_version` bumps on every applied update (a profile switch or a new matrix),
/// so a consumer feeding a renderer re-feeds exactly then.
class BedMeshState {
  public:
    static BedMeshState& instance();

    void init_subjects();
    void deinit_subjects();

    /// Subscribe to the API's bed_mesh status and publish what it already holds.
    /// Replaces an earlier attachment.
    void attach(IMoonrakerAPI* api);
    void detach();

    /// Publish @p active (null: no mesh loaded) and @p profile_count. Main thread.
    void apply(const BedMeshProfile* active, int profile_count);

    /// Re-render the translated text of the current state after a language switch.
    void retranslate();

    [[nodiscard]] const MeshStats& stats() const {
        return stats_;
    }
    [[nodiscard]] const BedMeshProfile& profile() const {
        return profile_;
    }
    [[nodiscard]] int profile_count() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&profile_count_));
    }

    lv_subject_t* get_available_subject() {
        return &available_;
    }
    lv_subject_t* get_version_subject() {
        return &version_;
    }
    lv_subject_t* get_profile_count_subject() {
        return &profile_count_;
    }
    /// Death signal for the subjects; observers outside this class pass it to observe<>().
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

  private:
    BedMeshState() = default;
    void refresh_from_api(IMoonrakerAPI* api);

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    lv_subject_t available_{};
    lv_subject_t profile_name_{};
    lv_subject_t dimensions_{};
    lv_subject_t max_value_{};
    lv_subject_t max_coord_{};
    lv_subject_t min_value_{};
    lv_subject_t min_coord_{};
    lv_subject_t variance_{};
    lv_subject_t profile_count_{};
    lv_subject_t version_{};

    char profile_name_buf_[64] = {};
    char dimensions_buf_[64] = {};
    char max_value_buf_[32] = {};
    char max_coord_buf_[24] = {};
    char min_value_buf_[32] = {};
    char min_coord_buf_[24] = {};
    char variance_buf_[64] = {};

    MeshStats stats_;
    BedMeshProfile profile_;

    SubscriptionGuard subscription_;
    // Declared last so it is torn down first, expiring every token the subscription
    // captured before it is destroyed.
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
