// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "led/led_backend.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

class IMoonrakerAPI;
namespace helix {
class IMoonrakerClient;
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

        /// Decompose into base color (max brightness) + brightness percentage +
        /// the W level scaled back up to full brightness. All four channels
        /// feed the brightness max: a white-only Klipper `[led]` reports its
        /// entire level in W, so excluding it reads back as 0% (#1129).
        /// Re-applying `base_color`/`base_white` at `brightness_pct`
        /// reproduces the original color.
        void decompose(uint32_t& base_color, int& brightness_pct, double& base_white) const;
    };

    NativeBackend() = default;

    void set_api(IMoonrakerAPI* api) {
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

    /// Update channel capabilities from configfile config (called during discovery).
    /// Sets has_red_pin, has_green_pin, etc. for strips with configfile data.
    void update_pin_config(const nlohmann::json& config_section);

    // Control methods
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    // `on_queued` is the third disposition IMoonrakerAPI::execute_gcode offers:
    // SET_LED is discretionary, so while an external blocking op holds Klipper's
    // gcode lock the command is queued fire-and-forget and its RPC response is
    // dropped — neither on_success nor on_error will ever fire. Pass on_queued to
    // release a caller-side in-flight counter on that path. It means "accepted for
    // later execution", never "the strip changed", and it runs synchronously on
    // the calling thread. See moonraker_api.h for the full contract.
    void set_color(const std::string& strip_id, double r, double g, double b, double w,
                   SuccessCallback on_success = nullptr, ErrorCallback on_error = nullptr,
                   SuccessCallback on_queued = nullptr);
    void set_brightness(const std::string& strip_id, int brightness_pct, double r, double g,
                        double b, double w, SuccessCallback on_success = nullptr,
                        ErrorCallback on_error = nullptr, SuccessCallback on_queued = nullptr);
    void turn_on(const std::string& strip_id, SuccessCallback on_success = nullptr,
                 ErrorCallback on_error = nullptr, SuccessCallback on_queued = nullptr);
    void turn_off(const std::string& strip_id, SuccessCallback on_success = nullptr,
                  ErrorCallback on_error = nullptr, SuccessCallback on_queued = nullptr);

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
    IMoonrakerAPI* api_ = nullptr;
    std::vector<LedStripInfo> strips_;
    std::unordered_map<std::string, StripColor> strip_colors_;
    ColorChangeCallback color_change_cb_;
};

class LedEffectBackend {
  public:
    LedEffectBackend() = default;

    void set_api(IMoonrakerAPI* api) {
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

    // `caller_surfaces_errors` says whether `on_error` actually shows the user
    // something. A non-null handler is presumed to; pass false to opt out when
    // it only logs, so GcodeErrorRouter keeps ownership of the `!!` broadcast
    // that would otherwise be the only report. See include/rpc_error_policy.h.
    void activate_effect(const std::string& effect_name,
                         NativeBackend::SuccessCallback on_success = nullptr,
                         NativeBackend::ErrorCallback on_error = nullptr,
                         NativeBackend::SuccessCallback on_queued = nullptr,
                         bool caller_surfaces_errors = true);
    void stop_all_effects(NativeBackend::SuccessCallback on_success = nullptr,
                          NativeBackend::ErrorCallback on_error = nullptr,
                          NativeBackend::SuccessCallback on_queued = nullptr,
                          bool caller_surfaces_errors = true);
    void stop_effect(const std::string& effect_name,
                     NativeBackend::SuccessCallback on_success = nullptr,
                     NativeBackend::ErrorCallback on_error = nullptr,
                     NativeBackend::SuccessCallback on_queued = nullptr);

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
    IMoonrakerAPI* api_ = nullptr;
    std::vector<LedEffectInfo> effects_;
};

class WledBackend {
  public:
    WledBackend() = default;

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }
    void set_client(IMoonrakerClient* client) {
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
    IMoonrakerAPI* api_ = nullptr;
    IMoonrakerClient* client_ = nullptr;
    std::vector<LedStripInfo> strips_;
    std::unordered_map<std::string, std::string> strip_addresses_;
    std::unordered_map<std::string, std::vector<WledPresetInfo>> strip_presets_;
    std::unordered_map<std::string, WledStripState> strip_states_;
    static const std::vector<WledPresetInfo> empty_presets_;
};

class MacroBackend {
  public:
    MacroBackend() = default;

    void set_api(IMoonrakerAPI* api) {
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

    /// Check if a macro's state can be tracked (ON_OFF = yes, TOGGLE = no)
    [[nodiscard]] bool has_known_state(const std::string& macro_name) const;

  private:
    IMoonrakerAPI* api_ = nullptr;
    std::vector<LedMacroInfo> macros_;
};

class OutputPinBackend {
  public:
    OutputPinBackend() = default;

    void set_api(IMoonrakerAPI* api) {
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

    // Control methods. `on_queued` mirrors NativeBackend: when the emitted G-code is
    // discretionary and an external blocking op holds Klipper's gcode lock, the
    // command is queued fire-and-forget and its RPC response is dropped — neither
    // on_success nor on_error will ever fire. Callers holding an in-flight counter
    // MUST pass it or the counter wedges (#1129).
    void set_value(const std::string& pin_id, double value,
                   NativeBackend::SuccessCallback on_success = nullptr,
                   NativeBackend::ErrorCallback on_error = nullptr,
                   NativeBackend::SuccessCallback on_queued = nullptr);
    void turn_on(const std::string& pin_id, NativeBackend::SuccessCallback on_success = nullptr,
                 NativeBackend::ErrorCallback on_error = nullptr,
                 NativeBackend::SuccessCallback on_queued = nullptr);
    void turn_off(const std::string& pin_id, NativeBackend::SuccessCallback on_success = nullptr,
                  NativeBackend::ErrorCallback on_error = nullptr,
                  NativeBackend::SuccessCallback on_queued = nullptr);
    void set_brightness(const std::string& pin_id, int brightness_pct,
                        NativeBackend::SuccessCallback on_success = nullptr,
                        NativeBackend::ErrorCallback on_error = nullptr,
                        NativeBackend::SuccessCallback on_queued = nullptr);

    /// Update pin values from Moonraker status JSON
    void update_from_status(const nlohmann::json& status);

    [[nodiscard]] double get_value(const std::string& pin_id) const;
    [[nodiscard]] int brightness_pct(const std::string& pin_id) const;
    [[nodiscard]] bool is_pwm(const std::string& pin_id) const;

    void set_pin_pwm(const std::string& pin_id, bool is_pwm);

  private:
    IMoonrakerAPI* api_ = nullptr;
    std::vector<LedStripInfo> pins_;
    std::unordered_map<std::string, double> pin_values_;
};

class LedController {
  public:
    static LedController& instance();

    void init(IMoonrakerAPI* api, IMoonrakerClient* client);
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

    // Update LED channel capabilities from configfile config section
    // (detects red_pin, green_pin, blue_pin, white_pin for generic [led] sections)
    void update_led_pin_config(const nlohmann::json& configfile_config);

    // Queries
    [[nodiscard]] bool has_any_backend() const;
    [[nodiscard]] std::vector<LedBackendType> available_backends() const;

    // Config persistence
    void load_config();
    void save_config();

    /// Set light state and dispatch to all selected backends.
    /// This is the primary API for turning lights on/off — always updates light_on_.
    void light_set(bool on);

    /// Query tracked LED state from Moonraker to sync subjects after toggle.
    void query_tracked_led_state();

    /// Convenience: turn off all selected strips.
    void turn_off_all();

    /// Set color on all selected native/output_pin strips. Sets light_on_ = true.
    void set_color_all(double r, double g, double b, double w = 0.0);

    /// Set brightness on all selected native/output_pin strips. Sets light_on_ = (pct > 0).
    void set_brightness_all(int brightness_pct);

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

    [[nodiscard]] int get_startup_brightness() const;
    void set_startup_brightness(int brightness_pct);

    /// Apply the "LED on at start" preference. Called from the discovery-complete
    /// handler, which re-runs on every Klippy restart — this applies at most once
    /// per printer session (see startup_preference_applied_).
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

    /// Boolean subject (0/1) reflecting whether at least one strip is selected and
    /// therefore controllable. Drives visibility of action-style UI (Print Status
    /// light toggle, Home LED widgets). Registered globally as "led_controllable"
    /// for direct XML binding.
    lv_subject_t* get_led_controllable_subject() {
        return &led_controllable_;
    }

    /// Int subject (0/1): a light toggle command is currently awaiting its gcode ACK.
    /// Drives button greying while a command is in flight. Registered globally as
    /// "led_command_in_flight" for direct XML binding.
    lv_subject_t* get_led_command_in_flight_subject() {
        return &led_command_in_flight_;
    }
    [[nodiscard]] bool light_command_in_flight() const {
        return in_flight_count_ > 0;
    }

    /// Cached last-used color including white channel
    struct LastColor {
        uint32_t rgb = 0xFFFFFF; // RGB as 0xRRGGBB (color picker compatibility)
        double white = 0.0;      // White channel 0.0-1.0
    };

    [[nodiscard]] uint32_t last_color() const {
        return last_color_.rgb;
    }
    [[nodiscard]] double last_white() const {
        return last_color_.white;
    }
    void set_last_color(uint32_t color);
    void set_last_white(double white);

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
    IMoonrakerAPI* api_ = nullptr;
    IMoonrakerClient* client_ = nullptr;
    helix::AsyncLifetimeGuard lifetime_;

    NativeBackend native_;
    LedEffectBackend effects_;
    WledBackend wled_;
    MacroBackend macro_;
    OutputPinBackend output_pin_;

    // Config state
    std::vector<std::string> selected_strips_;
    LastColor last_color_;
    int last_brightness_ = 100;
    std::vector<uint32_t> color_presets_;
    std::vector<LedMacroInfo> configured_macros_;
    std::vector<std::string> discovered_led_macros_; // Raw macro names from hardware
    bool led_on_at_start_ = false;
    int startup_brightness_ = 80;
    bool light_on_ = false; // Internal light state for abstract API

    /// One-shot latch for apply_startup_preference(). Deliberately NOT reset by
    /// init(): printer_discovery re-runs init() on every discovery, and a Klippy
    /// restart re-triggers discovery for the life of the session. Only deinit()
    /// clears it, which happens on teardown (printer switch / shutdown) — the one
    /// case that really is a fresh start.
    bool startup_preference_applied_ = false;

    /// Dispatch on/off to all selected strips (low-level — callers should use light_set())
    void toggle_all(bool on);

    /// RGBW (0.0-1.0) values computed from saved last_color_/last_white for a
    /// "turn on at given brightness" operation. Applies a safety floor: if
    /// saved state has no color at all (RGB==0 && white==0), returns full
    /// white. If brightness_pct is 0 but saved color is nonzero, treats
    /// effective brightness as 100% to preserve user intent.
    struct ScaledColor {
        double r, g, b, w;
    };
    [[nodiscard]] ScaledColor compute_scaled_last_color(int brightness_pct) const;

    lv_subject_t led_config_version_{};    // Bumped on discover/config changes
    lv_subject_t led_controllable_{};      // 0/1 mirror of !selected_strips_.empty()
    lv_subject_t led_command_in_flight_{}; // 0/1: a light toggle is awaiting its gcode ACK
    int in_flight_count_ = 0;              // outstanding toggle commands awaiting ACK
    ObserverGuard conn_observer_;          // clears in-flight count on any non-CONNECTED transition
    // Clears in-flight count on any exit from klippy READY. A Klipper restart leaves
    // the Moonraker WebSocket up, so conn_observer_ above never sees it (#1129).
    // No paired SubjectLifetime: PrinterState::get_klippy_state_subject() is a static
    // singleton-lifetime subject (no lifetime-token overload).
    ObserverGuard klippy_observer_;
    bool version_subject_initialized_ = false;

    /// Push the current selected_strips_ emptiness into led_controllable_.
    /// Cheap no-op if the value is unchanged. Safe before subject init (skips).
    void publish_controllable_state();
    void update_in_flight_subject();
    void note_command_dispatched();
    void note_command_settled();
    void force_clear_in_flight();

    // Default color presets
    static constexpr uint32_t DEFAULT_COLOR_PRESETS[] = {0xFFFFFF, 0xFFD700, 0xFF6B35, 0x4FC3F7,
                                                         0xFF4444, 0x66BB6A, 0x9C27B0, 0x00BCD4};
    static constexpr size_t DEFAULT_COLOR_PRESETS_COUNT = 8;
};

} // namespace helix::led
