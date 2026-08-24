// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "moonraker_types.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

class IMoonrakerAPI;

namespace helix {

/**
 * @brief Singleton tracking power device state from Moonraker
 *
 * Subscribes to notify_power_changed WebSocket events and maintains
 * per-device LVGL subjects (0=off, 1=on, 2=locked).
 *
 * Uses SubjectLifetime pattern for dynamic subject safety since devices
 * can be rediscovered on reconnection.
 */
class PowerDeviceState {
  public:
    static PowerDeviceState& instance();

    /// Register for notify_power_changed WebSocket events
    void subscribe(IMoonrakerAPI& api);

    /// Unregister callback and clean up subjects
    void unsubscribe(IMoonrakerAPI& api);

    /// Set discovered devices (called from discovery sequence)
    void set_devices(const std::vector<PowerDevice>& devices);

    /// Get status subject for a device (0=off, 1=on, 2=locked). Returns nullptr if not found.
    lv_subject_t* get_status_subject(const std::string& device, SubjectLifetime& lt);

    /// Check if a device is configured as locked_while_printing
    bool is_locked_while_printing(const std::string& device) const;

    /// Get list of all tracked device names
    std::vector<std::string> device_names() const;

    /// Update a device's status directly (for mock/testing).
    /// Marshals to UI thread via queue_update.
    void update_device_status(const std::string& device, const std::string& status);

    /// Clean up all LVGL subjects (called by StaticSubjectRegistry)
    void deinit_subjects();

  private:
    PowerDeviceState() = default;
    ~PowerDeviceState() {
        // Release observer without touching LVGL — during process exit,
        // static destruction order is undefined and the observed subject
        // (from PrinterState) may already be destroyed. deinit_subjects()
        // handles proper cleanup during normal lifecycle.
        print_state_observer_.release();
    }

    void on_power_changed(const nlohmann::json& msg);
    void reevaluate_lock_states();
    static int status_string_to_int(const std::string& status);

    struct DeviceInfo {
        std::string name;
        std::string type;
        bool locked_while_printing = false;
        std::unique_ptr<lv_subject_t> status_subject;
        SubjectLifetime lifetime;
        int raw_status = 0; ///< 0=off, 1=on (before lock evaluation)
    };

    std::unordered_map<std::string, DeviceInfo> devices_;
    ObserverGuard print_state_observer_;
    bool subjects_initialized_ = false;
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix
