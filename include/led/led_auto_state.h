// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "json_fwd.h"
#include "led/led_backend.h"
#include "led/led_devices.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace helix {
class PrinterState;
}

namespace helix::led {

/// Describes what LED action to take for a given printer state
struct LedStateAction {
    std::string action_type; // "color", "brightness", "effect", "wled_preset", "macro", "off"
    uint32_t color = 0xFFFFFF;
    int brightness = 100;
    std::string effect_name; // For "effect" action
    int wled_preset = 0;     // For "wled_preset" action
    std::string macro_gcode; // For "macro" action
};

/// Watches printer state subjects and automatically applies LED actions
/// based on configurable state-to-action mappings.
class LedAutoState {
  public:
    static LedAutoState& instance();

    void init(PrinterState& printer_state);
    void deinit();

    [[nodiscard]] bool is_initialized() const {
        return initialized_;
    }
    [[nodiscard]] bool is_enabled() const {
        return enabled_;
    }
    void set_enabled(bool enabled);

    // State mappings
    void set_mapping(const std::string& state_key, const LedStateAction& action);
    [[nodiscard]] const LedStateAction* get_mapping(const std::string& state_key) const;
    [[nodiscard]] const std::unordered_map<std::string, LedStateAction>& mappings() const {
        return mappings_;
    }

    /// Devices the actions drive, as saved; empty means the chamber light.
    [[nodiscard]] const std::vector<std::string>& strips() const {
        return strips_;
    }
    void set_strips(const std::vector<std::string>& ids) {
        strips_ = ids;
    }
    /// strips() that are switchable now, or the chamber light when none are.
    [[nodiscard]] std::vector<std::string> targets() const;

    // Config persistence
    void load_config();
    void save_config();

    // Compute current state key from printer state subjects
    [[nodiscard]] std::string compute_state_key() const;

    // Force re-evaluation (e.g., after config change)
    void evaluate();

  private:
    friend class LedAutoStateTestAccess;
    LedAutoState() = default;
    ~LedAutoState() {
        // Release observers without touching LVGL — during process exit,
        // static destruction order is undefined and observed subjects may
        // already be destroyed. unsubscribe_observers() handles proper cleanup.
        print_state_observer_.release();
        klippy_state_observer_.release();
        extruder_target_observer_.release();
    }
    LedAutoState(const LedAutoState&) = delete;
    LedAutoState& operator=(const LedAutoState&) = delete;

    void on_state_changed();
    void apply_action(const LedStateAction& action);
    void setup_default_mappings();
    void subscribe_observers();
    void unsubscribe_observers();

    bool initialized_ = false;
    bool enabled_ = false;
    PrinterState* printer_state_ = nullptr;

    std::string last_applied_key_;
    std::unordered_map<std::string, LedStateAction> mappings_;
    std::vector<std::string> strips_;

    // Observers — only active when enabled
    ObserverGuard print_state_observer_;
    ObserverGuard klippy_state_observer_;
    ObserverGuard extruder_target_observer_;
};

/// Persist @p m: the auto-state strips, and the device a home light button without
/// its own `led` key adopts the first time it binds.
void stage_light_selection(const SelectionMigration& m);

} // namespace helix::led
