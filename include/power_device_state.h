// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "json_fwd.h"
#include "moonraker_types.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {
class IMoonrakerAPI;

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

    /// Set discovered devices (called from discovery sequence). A device already
    /// tracked keeps its subject and lifetime and takes the new values in place;
    /// only devices that appeared or disappeared are created or torn down.
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
        // Static destruction order is undefined, so PrinterState's subject may
        // already be gone. The observer carries PrinterState's subjects
        // lifetime, which its destructor marks dead, so reset() skips the
        // removal in that case and still frees the observer's context.
        print_state_observer_.reset();
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

    /// Publish @p info's raw status, or 2 (locked) when it is a powered
    /// locked_while_printing device and a job holds the machine.
    void publish_effective_status(DeviceInfo& info);

    std::unordered_map<std::string, DeviceInfo> devices_;
    ObserverGuard print_state_observer_;
    bool subjects_initialized_ = false;
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix
