// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_dryer.cpp
 * @brief AmsState: the dryer subjects and the dryer modal's editing values
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ams_dryer.h"
#include "ams_state.h"
#include "ams_state_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {
using ams_state_detail::assert_main_thread;

void AmsState::set_dryer_mirror_unit(int unit) {
    assert_main_thread();
    if (dryer_mirror_unit_ == unit) {
        return;
    }
    dryer_mirror_unit_ = unit;
    sync_dryer_from_backend();
}

void AmsState::sync_dryer_from_backend() {
    assert_main_thread();

    auto* backend = get_backend(0);
    if (!backend) {
        // No backend - clear dryer state
        lv_subject_set_int(&dryer_supported_, 0);
        lv_subject_set_int(&dryer_active_, 0);
        return;
    }

    DryerInfo dryer = helix::dryer_info(*backend, dryer_mirror_unit_);

    // Update integer subjects
    int new_supported = dryer.supported ? 1 : 0;
    lv_subject_set_int(&dryer_supported_, new_supported);
    int new_dryer_active = dryer.active ? 1 : 0;
    lv_subject_set_int(&dryer_active_, new_dryer_active);
    int new_cur_temp = static_cast<int>(dryer.current_temp_c);
    lv_subject_set_int(&dryer_current_temp_, new_cur_temp);
    int new_tgt_temp = static_cast<int>(dryer.target_temp_c);
    lv_subject_set_int(&dryer_target_temp_, new_tgt_temp);
    lv_subject_set_int(&dryer_remaining_min_, dryer.remaining_min);
    int new_progress = dryer.get_progress_pct();
    lv_subject_set_int(&dryer_progress_pct_, new_progress);

    // Text formatting (dryer_current_temp_text_, dryer_target_temp_text_, dryer_time_text_)
    // is handled by observers in AmsDryerCard::setup() — UI-layer responsibility.

    spdlog::trace("[AMS State] Synced dryer - supported={}, active={}, temp={}→{}°C, {}min left",
                  dryer.supported, dryer.active, static_cast<int>(dryer.current_temp_c),
                  static_cast<int>(dryer.target_temp_c), dryer.remaining_min);
}

void AmsState::adjust_modal_temp(int delta_c) {
    assert_main_thread();

    // Get limits from backend if available, fallback to constants
    float min_temp = static_cast<float>(kMinDryerTempC);
    float max_temp = static_cast<float>(kMaxDryerTempC);
    auto* backend = get_backend(0);
    if (backend) {
        DryerInfo dryer = helix::dryer_info(*backend, dryer_mirror_unit_);
        min_temp = dryer.min_temp_c;
        max_temp = dryer.max_temp_c;
    }

    int cur = lv_subject_get_int(&modal_target_temp_);
    int new_temp = cur + delta_c;
    new_temp = std::max(static_cast<int>(min_temp), std::min(new_temp, static_cast<int>(max_temp)));
    lv_subject_set_int(&modal_target_temp_, new_temp);

    spdlog::debug("[AMS State] Modal temp adjusted to {}°C", new_temp);
}

void AmsState::adjust_modal_duration(int delta_min) {
    assert_main_thread();

    // Get max duration from backend if available, fallback to constant
    int max_duration = kMaxDryerDurationMin;
    auto* backend = get_backend(0);
    if (backend) {
        DryerInfo dryer = helix::dryer_info(*backend, dryer_mirror_unit_);
        max_duration = dryer.max_duration_min;
    }

    int cur = lv_subject_get_int(&modal_duration_min_);
    int new_duration = cur + delta_min;
    new_duration = std::max(kMinDryerDurationMin, std::min(new_duration, max_duration));
    lv_subject_set_int(&modal_duration_min_, new_duration);

    spdlog::debug("[AMS State] Modal duration adjusted to {} min", new_duration);
}

void AmsState::set_modal_preset(int temp_c, int duration_min) {
    assert_main_thread();
    lv_subject_set_int(&modal_target_temp_, temp_c);
    lv_subject_set_int(&modal_duration_min_, duration_min);
    spdlog::debug("[AMS State] Modal preset set: {}°C for {} min", temp_c, duration_min);
}
} // namespace helix
