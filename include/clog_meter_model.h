// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "clog_meter_geometry.h"
#include "core/observer_guard.h"

#include <functional>

namespace helix::ui {

/**
 * @brief The clog meter's subjects, subscribed once, delivered as a snapshot.
 *
 * Every presentation of this data reads the same five AmsState subjects, and
 * before this each wired its own observers: five in UiClogBar, three in
 * UiClogMeter, and a fresh set again for each place either is instantiated
 * (the home tile, the Buffer Status modal, the AMS sidebar, the loaded-spool
 * card). Adding a sixth subject meant finding every one of those.
 *
 * A renderer now owns one of these and is handed a whole ClogMeterSample
 * whenever any field moves. The wiring lives here; what a reading *means*
 * lives on the sample (clog_meter_geometry.h), which is pure and tested
 * without LVGL. Renderers are left with only the drawing.
 *
 * Main thread only, like the observers it wraps: AmsState publishes through
 * ui_queue_update(), so callbacks arrive on the UI thread and may touch
 * widgets directly.
 */
class ClogMeterModel {
  public:
    /// Called on every change, with the complete sample rather than the one
    /// field that moved — a renderer that positioned from a partial update
    /// would draw the new value against the old threshold for a frame.
    using Callback = std::function<void(const ClogMeterSample&)>;

    /// Subscribes immediately and does NOT fire the callback for the initial
    /// read: the subclass is still constructing. Call relayout (or whatever
    /// the renderer's draw entry point is) once after construction, which the
    /// renderers already did for their own first paint.
    explicit ClogMeterModel(Callback on_change);
    ~ClogMeterModel();

    ClogMeterModel(const ClogMeterModel&) = delete;
    ClogMeterModel& operator=(const ClogMeterModel&) = delete;

    [[nodiscard]] const ClogMeterSample& sample() const {
        return sample_;
    }

  private:
    /// Update one field and publish the whole sample.
    void publish();

    ClogMeterSample sample_;
    Callback on_change_;
    /// False until the constructor has finished registering: the observers
    /// each fire once on registration, and calling back into a renderer that
    /// is still running its own constructor would draw against half-built
    /// widgets.
    bool ready_ = false;

    ObserverGuard mode_obs_;
    ObserverGuard value_obs_;
    ObserverGuard warning_obs_;
    ObserverGuard danger_obs_;
    ObserverGuard peak_obs_;
};

} // namespace helix::ui
