// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "led/led_backend.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

class MoonrakerAPI;
namespace helix {
class MoonrakerClient;
}

namespace helix {
class PrinterDiscovery;
}

namespace helix::led {

class NativeBackend {
  public:
    /// Cached RGBW color for a strip (0.0-1.0 range)
    struct StripColor {
        double r = 0.0, g = 0.0, b = 0.0, w = 0.0;

        /// Convert to packed RGB uint32 (ignoring W channel)
        [[nodiscard]] uint32_t to_rgb() const;

        /// Decompose into base color (max brightness) + brightness percentage
        void decompose(uint32_t& base_color, int& brightness_pct) const;
    };

    NativeBackend() = default;

    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }

    [[nodiscard]] LedBackendType type() const {
        return LedBackendType::NATIVE;
    }
    [[nodiscard]] bool is_available() const {
        return !strips_.empty();
    }
    [[nodiscard]] const std::vector<LedStripInfo>& strips() const {
        return strips_;
    }

    void add_strip(const LedStripInfo& strip);
    void clear();

    // Control methods
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    void set_color(const std::string& strip_id, double r, double g, double b, double w,
                   SuccessCallback on_success = nullptr, ErrorCallback on_error = nullptr);
    void set_brightness(const std::string& strip_id, int brightness_pct, double r, double g,
                        double b, double w, SuccessCallback on_success = nullptr,
                        ErrorCallback on_error = nullptr);
    void turn_on(const std::string& strip_id, SuccessCallback on_success = nullptr,
                 ErrorCallback on_error = nullptr);
    void turn_off(const std::string& strip_id, SuccessCallback on_success = nullptr,
                  ErrorCallback on_error = nullptr);

    /// Update per-strip color cache from Moonraker status update JSON
    void update_from_status(const nlohmann::json& status);

    /// Get cached color for a strip (returns white if unknown)
    [[nodiscard]] StripColor get_strip_color(const std::string& strip_id) const;

    /// Check if we have a cached color for a strip
    [[nodiscard]] bool has_strip_color(const std::string& strip_id) const;

    /// Register/unregister a callback for strip color changes (called on main thread)
    using ColorChangeCallback =
        std::function<void(const std::string& strip_id, const StripColor& color)>;
    void set_color_change_callback(ColorChangeCallback cb) {
        color_change_cb_ = std::move(cb);
    }
    void clear_color_change_callback() {
        color_change_cb_ = nullptr;
    }

  private:
    MoonrakerAPI* api_ = nullptr;
    std::vector<LedStripInfo> strips_;
    std::unordered_map<std::string, StripColor> strip_colors_;
    ColorChangeCallback color_change_cb_;
};

class LedEffectBackend {
  public:
    LedEffectBackend() = default;

    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }

    [[nodiscard]] LedBackendType type() const {
        return LedBackendType::LED_EFFECT;
    }
    [[nodiscard]] bool is_available() const {
        return !effects_.empty();
    }
    [[nodiscard]] const std::vector<LedEffectInfo>& effects() const {
        return effects_;
    }

    void add_effect(const LedEffectInfo& effect);
    void clear();

    void activate_effect(const std::string& effect_name,
                         NativeBackend::SuccessCallback on_success = nullptr,
                         NativeBackend::ErrorCallback on_error = nullptr);
    void stop_all_effects(NativeBackend::SuccessCallback on_success = nullptr,
                          NativeBackend::ErrorCallback on_error = nullptr);

    // Set target LEDs for a specific effect by name
    void set_effect_targets(const std::string& effect_name,
                            const std::vector<std::string>& targets);

    // Return only effects whose target_leds contains the given strip ID
    [[nodiscard]] std::vector<LedEffectInfo> effects_for_strip(const std::string& strip_id) const;

    /// Update effect enabled states from Moonraker status update JSON
    void update_from_status(const nlohmann::json& status);

    /// Get whether a specific effect is currently enabled
    [[nodiscard]] bool is_effect_enabled(const std::string& effect_name) const;

    // Parse Klipper "leds" config format ("neopixel:name") to our format ("neopixel name")
    static std::string parse_klipper_led_target(const std::string& klipper_format);

    // Helper: map effect name keywords to icon hints
    static std::string icon_hint_for_effect(const std::string& effect_name);
    // Helper: convert config name to display name
    static std::string display_name_for_effect(const std::string& config_name);

  private:
    MoonrakerAPI* api_ = nullptr;
    std::vector<LedEffectInfo> effects_;
};

class WledBackend {
  public:
    WledBackend() = default;

    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }
    void set_client(MoonrakerClient* client) {
        client_ = client;
    }

    [[nodiscard]] LedBackendType type() const {
        return LedBackendType::WLED;
    }
    [[nodiscard]] bool is_available() const {
        return !strips_.empty();
    }
    [[nodiscard]] const std::vector<LedStripInfo>& strips() const {
        return strips_;
    }

    void add_strip(const LedStripInfo& strip);
    void clear();

    // WLED-specific control
    void set_on(const std::string& strip_name, NativeBackend::SuccessCallback on_success = nullptr,
                NativeBackend::ErrorCallback on_error = nullptr);
    void set_off(const std::string& strip_name, NativeBackend::SuccessCallback on_success = nullptr,
                 NativeBackend::ErrorCallback on_error = nullptr);
    void set_brightness(const std::string& strip_name, int brightness,
                        NativeBackend::SuccessCallback on_success = nullptr,
                        NativeBackend::ErrorCallback on_error = nullptr);
    void set_preset(const std::string& strip_name, int preset_id,
                    NativeBackend::SuccessCallback on_success = nullptr,
                    NativeBackend::ErrorCallback on_error = nullptr);
    void toggle(const std::string& strip_name, NativeBackend::SuccessCallback on_success = nullptr,
                NativeBackend::ErrorCallback on_error = nullptr);

    // Per-strip address management (IP/hostname from Moonraker server config)
    void set_strip_address(const std::string& strip_id, const std::string& address);
    [[nodiscard]] std::string get_strip_address(const std::string& strip_id) const;

    // Per-strip preset management (fetched from WLED device)
    void set_strip_presets(const std::string& strip_id, const std::vector<WledPresetInfo>& presets);
    [[nodiscard]] const std::vector<WledPresetInfo>&
    get_strip_presets(const std::string& strip_id) const;

    // Per-strip runtime state (from Moonraker status polling)
    void update_strip_state(const std::string& strip_id, const WledStripState& state);
    [[nodiscard]] WledStripState get_strip_state(const std::string& strip_id) const;

    // Poll Moonraker for current WLED status and update strip_states_
    void poll_status(std::function<void()> on_complete = nullptr);

    // Fetch preset names from WLED device directly (http://<address>/presets.json)
    void fetch_presets_from_device(const std::string& strip_id,
                                   std::function<void()> on_complete = nullptr);

  private:
    MoonrakerAPI* api_ = nullptr;
    MoonrakerClient* client_ = nullptr;
    std::vector<LedStripInfo> strips_;
    std::unordered_map<std::string, std::string> strip_addresses_;
    std::unordered_map<std::string, std::vector<WledPresetInfo>> strip_presets_;
    std::unordered_map<std::string, WledStripState> strip_states_;
    static const std::vector<WledPresetInfo> empty_presets_;
};

class MacroBackend {
  public:
    MacroBackend() = default;

    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }

    [[nodiscard]] LedBackendType type() const {
        return LedBackendType::MACRO;
    }
    [[nodiscard]] bool is_available() const {
        return !macros_.empty();
    }
    [[nodiscard]] const std::vector<LedMacroInfo>& macros() const {
        return macros_;
    }

    void add_macro(const LedMacroInfo& macro);
    void clear();

    void execute_on(const std::string& macro_name,
                    NativeBackend::SuccessCallback on_success = nullptr,
                    NativeBackend::ErrorCallback on_error = nullptr);
    void execute_off(const std::string& macro_name,
                     NativeBackend::SuccessCallback on_success = nullptr,
                     NativeBackend::ErrorCallback on_error = nullptr);
    void execute_toggle(const std::string& macro_name,
                        NativeBackend::SuccessCallback on_success = nullptr,
                        NativeBackend::ErrorCallback on_error = nullptr);
    void execute_custom_action(const std::string& macro_gcode,
                               NativeBackend::SuccessCallback on_success = nullptr,
                               NativeBackend::ErrorCallback on_error = nullptr);

    /// Check if a macro is currently "on" (optimistic tracking)
    [[nodiscard]] bool is_on(const std::string& macro_name) const;

    /// Check if a macro's state can be tracked (ON_OFF = yes, TOGGLE = no)
    [[nodiscard]] bool has_known_state(const std::string& macro_name) const;

  private:
    MoonrakerAPI* api_ = nullptr;
    std::vector<LedMacroInfo> macros_;
    std::unordered_map<std::string, bool> macro_states_; // Optimistic state tracking
};

class OutputPinBackend {
  public:
    OutputPinBackend() = default;

    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }

    [[nodiscard]] LedBackendType type() const {
        return LedBackendType::OUTPUT_PIN;
    }
    [[nodiscard]] bool is_available() const {
        return !pins_.empty();
    }
    [[nodiscard]] const std::vector<LedStripInfo>& pins() const {
        return pins_;
    }

    void add_pin(const LedStripInfo& pin);
    void clear();

    // Control methods
    void set_value(const std::string& pin_id, double value,
                   NativeBackend::SuccessCallback on_success = nullptr,
                   NativeBackend::ErrorCallback on_error = nullptr);
    void turn_on(const std::string& pin_id, NativeBackend::SuccessCallback on_success = nullptr,
                 NativeBackend::ErrorCallback on_error = nullptr);
    void turn_off(const std::string& pin_id, NativeBackend::SuccessCallback on_success = nullptr,
                  NativeBackend::ErrorCallback on_error = nullptr);
    void set_brightness(const std::string& pin_id, int brightness_pct,
                        NativeBackend::SuccessCallback on_success = nullptr,
                        NativeBackend::ErrorCallback on_error = nullptr);

    /// Update pin values from Moonraker status JSON
    void update_from_status(const nlohmann::json& status);

    [[nodiscard]] double get_value(const std::string& pin_id) const;
    [[nodiscard]] bool is_on(const std::string& pin_id) const;
    [[nodiscard]] int brightness_pct(const std::string& pin_id) const;
    [[nodiscard]] bool is_pwm(const std::string& pin_id) const;

    void set_pin_pwm(const std::string& pin_id, bool is_pwm);

    using ValueChangeCallback = std::function<void(const std::string& pin_id, double value)>;
    void set_value_change_callback(ValueChangeCallback cb) {
        value_change_cb_ = std::move(cb);
    }
    void clear_value_change_callback() {
        value_change_cb_ = nullptr;
    }

  private:
    MoonrakerAPI* api_ = nullptr;
    std::vector<LedStripInfo> pins_;
    std::unordered_map<std::string, double> pin_values_;
    ValueChangeCallback value_change_cb_;
};

class LedController {
  public:
    static LedController& instance();

    void init(MoonrakerAPI* api, MoonrakerClient* client);
    void deinit();

    [[nodiscard]] bool is_initialized() const {
        return initialized_;
    }

    // Backend accessors
    NativeBackend& native() {
        return native_;
    }
    LedEffectBackend& effects() {
        return effects_;
    }
    WledBackend& wled() {
        return wled_;
    }
    MacroBackend& macro() {
        return macro_;
    }
    OutputPinBackend& output_pin() {
        return output_pin_;
    }

    const NativeBackend& native() const {
        return native_;
    }
    const LedEffectBackend& effects() const {
        return effects_;
    }
    const WledBackend& wled() const {
        return wled_;
    }
    const MacroBackend& macro() const {
        return macro_;
    }
    const OutputPinBackend& output_pin() const {
        return output_pin_;
    }

    // Discovery
    void discover_from_hardware(const helix::PrinterDiscovery& hardware);
    void discover_wled_strips(); ///< Async WLED discovery via Moonraker HTTP bridge

    // Update effect target LEDs from configfile config section
    void update_effect_targets(const nlohmann::json& configfile_config);

    // Update output_pin PWM config from configfile config section
    void update_output_pin_config(const nlohmann::json& configfile_config);

    // Queries
    [[nodiscard]] bool has_any_backend() const;
    [[nodiscard]] std::vector<LedBackendType> available_backends() const;

    // Config persistence
    void load_config();
    void save_config();

    // Toggle all selected strips on/off
    void toggle_all(bool on);

    // Determine which backend a given strip belongs to
    [[nodiscard]] LedBackendType backend_for_strip(const std::string& strip_id) const;

    /// Get all selectable strips across all backends (native + WLED + non-PRESET macros)
    /// Macro entries use "macro:" prefixed IDs.
    [[nodiscard]] std::vector<LedStripInfo> all_selectable_strips() const;

    /// Get the first available strip to use as default selection.
    /// Priority: first selected > first native > first WLED > first non-PRESET macro.
    /// Returns empty string if nothing available.
    [[nodiscard]] std::string first_available_strip() const;

    /// Whether the current selection's state can be reliably tracked.
    /// Returns false if ANY selected strip is a TOGGLE macro (state unknown).
    [[nodiscard]] bool light_state_trackable() const;

    /// Toggle light state and dispatch to all selected backends.
    void light_toggle();

    /// Get composite on/off state across all selected backends.
    [[nodiscard]] bool light_is_on() const;

    /// Sync internal light state from actual hardware (e.g., from PrinterLedState subjects).
    /// Call this when the real LED state is known so that light_toggle() sends the correct command.
    void sync_light_state(bool is_on);

    // LED on at start preference
    [[nodiscard]] bool get_led_on_at_start() const;
    void set_led_on_at_start(bool enabled);

    // Apply startup preference (call at boot after printer is ready)
    void apply_startup_preference();

    // Config accessors
    [[nodiscard]] const std::vector<std::string>& selected_strips() const {
        return selected_strips_;
    }
    void set_selected_strips(const std::vector<std::string>& strips);

    /// Version subject bumped on discover_from_hardware() and set_selected_strips().
    /// UI widgets observe this to rebind when LED config changes.
    lv_subject_t* get_led_config_version_subject() {
        return &led_config_version_;
    }

    [[nodiscard]] uint32_t last_color() const {
        return last_color_;
    }
    void set_last_color(uint32_t color);

    [[nodiscard]] int last_brightness() const {
        return last_brightness_;
    }
    void set_last_brightness(int brightness);

    [[nodiscard]] const std::vector<uint32_t>& color_presets() const {
        return color_presets_;
    }
    void set_color_presets(const std::vector<uint32_t>& presets);

    [[nodiscard]] const std::vector<LedMacroInfo>& configured_macros() const {
        return configured_macros_;
    }
    void set_configured_macros(const std::vector<LedMacroInfo>& macros);

    /// Rebuild the macro backend from the current configured_macros list.
    /// Call after modifying macros via set_configured_macros() so the overlay sees the changes.
    void rebuild_macro_backend();

    [[nodiscard]] const std::vector<std::string>& discovered_macros() const {
        return discovered_led_macros_;
    }

  private:
    LedController() = default;
    ~LedController() = default;
    LedController(const LedController&) = delete;
    LedController& operator=(const LedController&) = delete;

    bool initialized_ = false;
    MoonrakerAPI* api_ = nullptr;
    MoonrakerClient* client_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    NativeBackend native_;
    LedEffectBackend effects_;
    WledBackend wled_;
    MacroBackend macro_;
    OutputPinBackend output_pin_;

    // Config state
    std::vector<std::string> selected_strips_;
    uint32_t last_color_ = 0xFFFFFF;
    int last_brightness_ = 100;
    std::vector<uint32_t> color_presets_;
    std::vector<LedMacroInfo> configured_macros_;
    std::vector<std::string> discovered_led_macros_; // Raw macro names from hardware
    bool led_on_at_start_ = false;
    bool light_on_ = false;             // Internal light state for abstract API
    lv_subject_t led_config_version_{}; // Bumped on discover/config changes
    bool version_subject_initialized_ = false;

    // Default color presets
    static constexpr uint32_t DEFAULT_COLOR_PRESETS[] = {0xFFFFFF, 0xFFD700, 0xFF6B35, 0x4FC3F7,
                                                         0xFF4444, 0x66BB6A, 0x9C27B0, 0x00BCD4};
    static constexpr size_t DEFAULT_COLOR_PRESETS_COUNT = 8;
};

} // namespace helix::led
