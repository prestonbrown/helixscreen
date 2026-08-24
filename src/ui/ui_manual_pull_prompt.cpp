// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_manual_pull_prompt.h"

#include "ui_error_reporting.h"

#include "filament_sensor_manager.h"
#include "observer_factory.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

namespace {

/// Held file-static rather than on an owner because the two surfaces that arm
/// this (FilamentPanel, AmsOperationSidebar) have different lifetimes and either
/// may be gone by the time the sensor edge lands.
ObserverGuard s_toolhead_observer;
bool s_armed = false;
bool s_registered = false;

/// Toolhead-detected subject values. -1 is "this printer has no such sensor",
/// which is a third answer and not a synonym for 0.
constexpr int TOOLHEAD_CLEAR = 0;
constexpr int TOOLHEAD_DETECTED = 1;

void fire(const char* via) {
    // Disarm BEFORE notifying. NOTIFY_* is synchronous, and tearing the observer
    // down first is what stops a second edge arriving mid-toast from doubling it.
    helix::ui::disarm_manual_pull_prompt();
    spdlog::info("[ManualPull] Filament clear of the toolhead (via {}) — prompting manual removal",
                 via);
    NOTIFY_INFO(lv_tr("Filament is clear of the toolhead. Pull it out the rest of the way."));
}

} // namespace

namespace helix::ui {

void disarm_manual_pull_prompt() {
    s_armed = false;
    s_toolhead_observer.reset();
}

void arm_manual_pull_prompt() {
    disarm_manual_pull_prompt();

    auto& sensors = FilamentSensorManager::instance();
    lv_subject_t* toolhead = sensors.get_toolhead_detected_subject();
    s_armed = true;

    // Only worth watching when the sensor currently SEES filament. Anything else
    // (already clear, or no sensor at all) can produce no meaningful 1 -> 0 edge,
    // and observe_int_sync fires once on registration — hooking it up in those
    // states would toast immediately, before the retract has moved anything.
    const bool watchable = toolhead && lv_subject_get_int(toolhead) == TOOLHEAD_DETECTED;
    if (watchable) {
        s_toolhead_observer = observe_int_sync<FilamentSensorManager>(
            toolhead, &sensors, [](FilamentSensorManager*, int detected) {
                if (detected == TOOLHEAD_CLEAR) {
                    fire("toolhead sensor");
                }
            });
    }

    if (!s_registered) {
        s_registered = true;
        StaticSubjectRegistry::instance().register_deinit("ManualPullPrompt", []() {
            disarm_manual_pull_prompt();
            s_registered = false;
            spdlog::trace("[ManualPull] Observers released");
        });
    }

    spdlog::debug("[ManualPull] Armed (toolhead sensor {})",
                  watchable ? "watching" : "unavailable — will prompt on completion");
}

void manual_pull_unload_finished() {
    if (!s_armed) {
        return;
    }
    fire("unload complete");
}

} // namespace helix::ui
