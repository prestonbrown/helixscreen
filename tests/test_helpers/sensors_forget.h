// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/update_queue.h"
#include "temperature_sensor_manager.h"

namespace helix::test {

/// Leaves the temperature sensor manager, a process singleton, holding no sensors and
/// with no refresh queued. A case that discovers sensors into it publishes a deferred
/// refresh that would otherwise outlive the case.
struct SensorsForget {
    ~SensorsForget() {
        helix::sensors::TemperatureSensorManager::instance().discover({});
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace helix::test
