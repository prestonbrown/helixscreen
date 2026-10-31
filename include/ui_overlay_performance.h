// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"

#include <string>
#include <vector>

namespace helix {
namespace ui {

/**
 * @brief Performance overlay: host CPU/memory + per-MCU load rows.
 *
 * Singleton. Call create(parent) once; subsequent calls return the cached root.
 * MCU rows are rebuilt whenever perf_mcu_names changes. The overlay registers a
 * string observer on perf_mcu_names to drive that rebuild.
 *
 * perf_mcu_names is NOT a never-freed singleton: PerformanceState::deinit_subjects()
 * frees it and init_subjects() recreates it on reconnect / printer switch. Per
 * [L077] the observer therefore MUST be created with PerformanceState's
 * subjects_lifetime() token (done in create()) so its ObserverGuard releases
 * safely instead of calling lv_observer_remove() on a freed subject.
 */
class UiOverlayPerformance {
  public:
    static UiOverlayPerformance& instance();

    lv_obj_t* create(lv_obj_t* parent);
    lv_obj_t* root() {
        return root_;
    }

  private:
    friend class UiOverlayPerformanceTestAccess;

    UiOverlayPerformance() = default;

    void rebuild_mcu_rows();

    helix::ui::WidgetRef root_;
    helix::ui::WidgetRef mcu_card_;

    // Last perf_mcu_names value applied to the rows. perf_mcu_names is
    // re-published on essentially every perf sample even when the MCU set is
    // unchanged; gating rebuild_mcu_rows() on a real change to this string
    // avoids tearing down + recreating content-sized rows on every sample
    // (the source of the 32-bit LV_COORD_MAX render crash — see #1061).
    std::string last_mcu_names_;

    // Observer on perf_mcu_names. The subject is freed/recreated by
    // PerformanceState across reconnects, so this observer is created with
    // PerformanceState::subjects_lifetime() in create() ([L077]). No paired
    // SubjectLifetime member is needed — the token is owned by PerformanceState.
    ObserverGuard mcu_names_observer_;
};

} // namespace ui
} // namespace helix
