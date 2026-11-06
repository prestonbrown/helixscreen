// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "helix_shared_callback.h"
#include "led/led_backend.h"
#include "led/led_devices.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class IMoonrakerAPI;
class IMoonrakerClient;
} // namespace helix

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
    /// Drops the strip list only; the color cache survives a re-discovery.
    void clear();
    /// Drops the color cache (printer switch or teardown).
    void forget_state();

    /// Update channel capabilities from configfile config (called during discovery).
    /// Sets has_red_pin, has_green_pin, etc. for strips with configfile data.
    void update_pin_config(const nlohmann::json& config_section);

    // Control methods
    using SuccessCallback = helix::SharedCallback<void()>;
    using ErrorCallback = helix::SharedCallback<void(const std::string&)>;

    // `on_queued` is the third disposition IMoonrakerAPI::execute_gcode offers:
    // SET_LED is discretionary, so while an external blocking op holds Klipper's
    // gcode lock the command is queued fire-and-forget and its RPC response is
    // dropped — neither on_success nor on_error will ever fire. Pass on_queued to
    // release a caller-side in-flight counter on that path. It means "accepted for
    // later execution", never "the strip changed", and it runs synchronously on
    // the calling thread. See moonraker_api.h for the full contract.
    // `silent` (here and on the other send entry points) marks a non-interactive
    // caller — LedAutoState applying the state theme. The busy-queue toast
    // exists to explain a command the user made, so an automatic send neither
    // shows nor claims it.
    void set_color(const std::string& strip_id, double r, double g, double b, double w,
                   SuccessCallback on_success = nullptr, ErrorCallback on_error = nullptr,
                   SuccessCallback on_queued = nullptr, bool silent = false);
    void turn_off(const std::string& strip_id, SuccessCallback on_success = nullptr,
                  ErrorCallback on_error = nullptr, SuccessCallback on_queued = nullptr,
                  bool silent = false);

    /// Update per-strip color cache from Moonraker status update JSON.
    /// True when @p status carried one of this backend's strips.
    bool update_from_status(const nlohmann::json& status);

    /// Get cached color for a strip (returns white if unknown)
    [[nodiscard]] StripColor get_strip_color(const std::string& strip_id) const;

    /// Check if we have a cached color for a strip
    [[nodiscard]] bool has_strip_color(const std::string& strip_id) const;

    /// Register/unregister a callback for strip color changes (called on main thread)
    using ColorChangeCallback =
        helix::SharedCallback<void(const std::string& strip_id, const StripColor& color)>;
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
                         bool caller_surfaces_errors = true, bool silent = false);
    void stop_all_effects(NativeBackend::SuccessCallback on_success = nullptr,
                          NativeBackend::ErrorCallback on_error = nullptr,
                          NativeBackend::SuccessCallback on_queued = nullptr,
                          bool caller_surfaces_errors = true, bool silent = false);
    void stop_effect(const std::string& effect_name,
                     NativeBackend::SuccessCallback on_success = nullptr,
                     NativeBackend::ErrorCallback on_error = nullptr,
                     NativeBackend::SuccessCallback on_queued = nullptr, bool silent = false);

    // Set target LEDs for a specific effect by name
    void set_effect_targets(const std::string& effect_name,
                            const std::vector<std::string>& targets);

    // Return only effects whose target_leds contains the given strip ID
    [[nodiscard]] std::vector<LedEffectInfo> effects_for_strip(const std::string& strip_id) const;

    /// Update effect enabled states from Moonraker status update JSON.
    /// True when @p status carried one of this backend's effects.
    bool update_from_status(const nlohmann::json& status);

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

    // Per-strip runtime state (from Moonraker status polling)
    void update_strip_state(const std::string& strip_id, const WledStripState& state);
    [[nodiscard]] WledStripState get_strip_state(const std::string& strip_id) const;
    [[nodiscard]] bool has_strip_state(const std::string& strip_id) const;

    // Poll Moonraker for current WLED status and update strip_states_
    void poll_status(helix::SharedCallback<void()> on_complete = nullptr);

  private:
    /// One wled_set_strip call; -1 leaves brightness or preset unchanged.
    void send(const std::string& strip_name, const char* action, int brightness, int preset,
              NativeBackend::SuccessCallback on_success, NativeBackend::ErrorCallback on_error);

    IMoonrakerAPI* api_ = nullptr;
    IMoonrakerClient* client_ = nullptr;
    std::vector<LedStripInfo> strips_;
    std::unordered_map<std::string, WledStripState> strip_states_;

    // Declared last: reverse-declaration destruction invalidates outstanding
    // tokens before any member they touch is gone.
    helix::AsyncLifetimeGuard lifetime_;
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
    /// Run the macro's gcode_field, or its toggle macro when that is empty.
    void run(const std::string& macro_name, std::string LedMacroInfo::*gcode_field,
             const char* what, NativeBackend::SuccessCallback on_success,
             NativeBackend::ErrorCallback on_error);

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
    /// Drops the pin list only; the pin values survive a re-discovery.
    void clear();
    /// Drops the pin values (printer switch or teardown).
    void forget_state();

    // Control methods. `on_queued` mirrors NativeBackend: when the emitted G-code is
    // discretionary and an external blocking op holds Klipper's gcode lock, the
    // command is queued fire-and-forget and its RPC response is dropped — neither
    // on_success nor on_error will ever fire. Callers holding an in-flight counter
    // MUST pass it or the counter wedges (#1129).
    void set_value(const std::string& pin_id, double value,
                   NativeBackend::SuccessCallback on_success = nullptr,
                   NativeBackend::ErrorCallback on_error = nullptr,
                   NativeBackend::SuccessCallback on_queued = nullptr, bool silent = false);
    void turn_on(const std::string& pin_id, NativeBackend::SuccessCallback on_success = nullptr,
                 NativeBackend::ErrorCallback on_error = nullptr,
                 NativeBackend::SuccessCallback on_queued = nullptr, bool silent = false);
    void turn_off(const std::string& pin_id, NativeBackend::SuccessCallback on_success = nullptr,
                  NativeBackend::ErrorCallback on_error = nullptr,
                  NativeBackend::SuccessCallback on_queued = nullptr, bool silent = false);
    void set_brightness(const std::string& pin_id, int brightness_pct,
                        NativeBackend::SuccessCallback on_success = nullptr,
                        NativeBackend::ErrorCallback on_error = nullptr,
                        NativeBackend::SuccessCallback on_queued = nullptr, bool silent = false);

    /// Update pin values from Moonraker status JSON.
    /// True when @p status carried one of this backend's pins.
    bool update_from_status(const nlohmann::json& status);

    [[nodiscard]] double get_value(const std::string& pin_id) const;
    [[nodiscard]] bool has_value(const std::string& pin_id) const;
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

    /// The led capability is turned off for this printer: its firmware declares
    /// lights the hardware does not have, so led_controllable and led_has_devices
    /// read 0 whatever discovery finds. Republishes both.
    void set_lights_disabled(bool disabled);
    void discover_wled_strips(); ///< Async WLED discovery via Moonraker HTTP bridge

    /// Called on the main thread when a WLED discovery settles (see
    /// wled_discovery_pending()), so the app can re-run whatever waited for the
    /// full device set. Survives deinit().
    void set_on_wled_settled(helix::SharedCallback<void()> cb) {
        on_wled_settled_ = std::move(cb);
    }

    /// How long a WLED discovery may go unanswered before it counts as settled.
    static constexpr uint32_t kWledDiscoveryTimeoutMs = 5000;

    /// True from discover_wled_strips() until that discovery settles: strips
    /// arrive, none are configured, it fails, or kWledDiscoveryTimeoutMs passes.
    [[nodiscard]] bool wled_discovery_pending() const {
        return wled_discovery_pending_;
    }

    /// Apply configfile.config: led_effect targets, output_pin PWM, and generic
    /// [led] channel pins. Kept and re-applied by every discover_from_hardware(),
    /// which rebuilds the lists these land on, so the order the two arrive in
    /// does not matter.
    void apply_configfile(const nlohmann::json& configfile_config);

    // Queries
    [[nodiscard]] bool has_any_backend() const;
    [[nodiscard]] std::vector<LedBackendType> available_backends() const;

    // Config persistence
    void load_config();
    void save_config();

    /// Every device the LEDs overlay lists: all_selectable_strips(), then each
    /// named PRESET macro as "macro:<name>".
    [[nodiscard]] std::vector<LedStripInfo> all_devices() const;

    /// Ids of the devices a light button can switch on and off.
    [[nodiscard]] std::vector<std::string> switchable_ids() const;

    /// The printer's main light (see resolve_chamber_light()).
    [[nodiscard]] std::string chamber_light() const;

    /// Device ids a light button whose `led` config is @p key drives.
    [[nodiscard]] std::vector<std::string> light_targets(const std::string& key) const;

    /// What is known about one device's power, brightness and hue right now.
    [[nodiscard]] DeviceState device_state(const std::string& id) const;

    /// Switch exactly @p ids on or off.
    void set_power(const std::vector<std::string>& ids, bool on, bool silent = false);

    /// Switch @p ids per next_power_on(); returns the state sent.
    bool toggle_power(const std::vector<std::string>& ids);

    /// Show a look (an RGB tint plus a W level 0.0-1.0) at @p brightness_pct on
    /// the native and output_pin devices among @p ids, fitted to each device as
    /// fit_look() fits it. 0 switches them off, as set_power(ids, false).
    void set_look(const std::vector<std::string>& ids, uint32_t rgb, double w, int brightness_pct,
                  bool silent = false);

    /// Set brightness on the native and output_pin devices among @p ids, keeping the
    /// last color. 0 switches every device in @p ids off, as set_power(ids, false).
    void set_brightness(const std::vector<std::string>& ids, int brightness_pct,
                        bool silent = false);

    /// Route a Moonraker status frame to the backends; bumps led_state_version
    /// when it carried an LED object. Main thread only; dispatch_status_frame()
    /// is the caller.
    void update_from_status(const nlohmann::json& status);

    /// Hand a printer.objects.query response for LED objects to every status
    /// consumer, as a notification would be. Callable from any thread.
    static void apply_query_response(const nlohmann::json& response);

    /// Poll WLED state over Moonraker, then bump led_state_version on the main
    /// thread and run @p on_done there.
    void refresh_wled_state(helix::SharedCallback<void()> on_done = nullptr);

    /// The backend that owns @p strip_id; nullopt for an id no backend owns, so
    /// nothing is ever sent to a device that is not there. A "macro:" id is always
    /// MACRO, even after its macro device is deleted.
    [[nodiscard]] std::optional<LedBackendType>
    backend_for_strip(const std::string& strip_id) const;

    /// Get all selectable strips across all backends (native + WLED + non-PRESET macros)
    /// Macro entries use "macro:" prefixed IDs.
    [[nodiscard]] std::vector<LedStripInfo> all_selectable_strips() const;

    /// The chamber light's fallback: first native > first WLED > first non-PRESET
    /// macro > first output_pin. Empty string if nothing available.
    [[nodiscard]] std::string first_available_strip() const;

    // LED on at start preference
    [[nodiscard]] bool get_led_on_at_start() const;
    void set_led_on_at_start(bool enabled);

    [[nodiscard]] int get_startup_brightness() const;
    void set_startup_brightness(int brightness_pct);

    /// Apply the "LED on at start" preference to @p targets. Called from the
    /// discovery-complete handler, which re-runs on every Klippy restart — this
    /// applies at most once per printer session (see startup_preference_applied_).
    /// No targets defers to a later call without spending the one shot.
    void apply_startup_preference(const std::vector<std::string>& targets);

    /// Version subject bumped on discover_from_hardware().
    /// UI widgets observe this to rebind when LED config changes.
    lv_subject_t* get_led_config_version_subject() {
        return &led_config_version_;
    }

    /// Int subject bumped whenever a device's live state may have changed.
    /// Registered globally as "led_state_version".
    lv_subject_t* get_led_state_version_subject() {
        return &led_state_version_;
    }

    /// Death signal for led_config_version_ and sibling subjects; pass to
    /// observe_*() — the registry deinit frees their observer nodes.
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    /// Boolean subject (0/1): a chamber light resolves, so a light button has
    /// something to drive. Drives visibility of action-style UI (Print Status
    /// light toggle, Home LED widgets). Registered globally as "led_controllable"
    /// for direct XML binding.
    lv_subject_t* get_led_controllable_subject() {
        return &led_controllable_;
    }

    /// Boolean subject (0/1): the LEDs overlay has a device to list. Registered
    /// globally as "led_has_devices".
    lv_subject_t* get_led_has_devices_subject() {
        return &led_has_devices_;
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

    /// Turn each freshly discovered <BASE>_ON / <BASE>_OFF pair into a ready-to-use
    /// ON_OFF device. Bases already offered are recorded in config so that deleting
    /// a seeded device is not undone by the next discovery.
    void seed_auto_paired_macros();

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
    helix::SharedCallback<void()> on_wled_settled_;
    bool wled_discovery_pending_ = false;
    unsigned wled_discovery_gen_ = 0; ///< A late answer or timeout settles only its own discovery
    void settle_wled_discovery(unsigned gen);

    NativeBackend native_;
    LedEffectBackend effects_;
    WledBackend wled_;
    MacroBackend macro_;
    OutputPinBackend output_pin_;

    // Config state
    /// The pre-1.1 leds/selected_strips (or older leds/selected, leds/strip) as
    /// loaded: migrate_legacy_selection()'s input. Read only, never saved.
    std::vector<std::string> legacy_selection_;
    LastColor last_color_;
    int last_brightness_ = 100;
    std::vector<uint32_t> color_presets_;
    std::vector<LedMacroInfo> configured_macros_;
    std::vector<std::string> discovered_led_macros_; // Raw macro names from hardware
    bool led_on_at_start_ = false;
    int startup_brightness_ = 80;

    /// One-shot latch for apply_startup_preference(). Deliberately NOT reset by
    /// init(): printer_discovery re-runs init() on every discovery, and a Klippy
    /// restart re-triggers discovery for the life of the session. Only deinit()
    /// clears it, which happens on teardown (printer switch / shutdown) — the one
    /// case that really is a fresh start.
    bool startup_preference_applied_ = false;

    /// Send a look to one native strip: fit_look() fitted to it (a look that
    /// lights nothing is white), scaled to @p brightness_pct, where 0 means 100%.
    void send_look(const std::string& strip_id, uint32_t rgb, double w, int brightness_pct,
                   NativeBackend::SuccessCallback on_success = nullptr,
                   NativeBackend::ErrorCallback on_error = nullptr,
                   NativeBackend::SuccessCallback on_queued = nullptr, bool silent = false);

    /// Tells light buttons the set of devices changed: discovery, WLED
    /// strips arriving, a macro device added, edited or deleted.
    void bump_config_version();

    lv_subject_t led_config_version_{};    // Bumped on discover/config changes
    lv_subject_t led_controllable_{};      // 0/1: at least one switchable device exists
    lv_subject_t led_has_devices_{};       // 0/1: the LEDs overlay has a device, PRESET included
    lv_subject_t led_command_in_flight_{}; // 0/1: a light toggle is awaiting its gcode ACK
    lv_subject_t led_state_version_{};     // Bumped when device state may have changed
    int in_flight_count_ = 0;              // outstanding toggle commands awaiting ACK
    ObserverGuard conn_observer_;          // clears in-flight count on any non-CONNECTED transition
    // Clears in-flight count on any exit from klippy READY. A Klipper restart leaves
    // the Moonraker WebSocket up, so conn_observer_ above never sees it (#1129).
    // No paired SubjectLifetime: PrinterState::get_klippy_state_subject() is a static
    // singleton-lifetime subject (no lifetime-token overload).
    ObserverGuard klippy_observer_;
    bool version_subject_initialized_ = false;
    bool lights_disabled_ = false;
    /// Owns the four subjects above: when the registry deinit calls
    /// deinit_all(), the death signal expires before they are freed.
    SubjectManager subjects_;

    /// Push whether a chamber light resolves into led_controllable_, and whether
    /// any LED device exists into led_has_devices_ (the LED Controls tile's gate).
    /// Cheap no-op if the value is unchanged. Safe before subject init (skips).
    void publish_controllable_state();
    void update_in_flight_subject();
    void note_command_dispatched();
    void note_command_settled();
    void force_clear_in_flight();
    void bump_state_version();

    /// Query printer.objects for the Klipper devices set_power() touched, so their
    /// state is read back even when the command changed nothing Klipper publishes.
    void query_led_state();
    std::set<std::string> pending_query_ids_;

    /// The LED sections of the last configfile.config applied; deinit() drops it.
    nlohmann::json configfile_config_;
    void apply_stored_configfile();
    void update_effect_targets(const nlohmann::json& configfile_config);
    void update_output_pin_config(const nlohmann::json& configfile_config);
    // Detects red_pin, green_pin, blue_pin, white_pin for generic [led] sections.
    void update_led_pin_config(const nlohmann::json& configfile_config);

    /// Macro devices have no readable state; toggle_power() alternates on this.
    std::unordered_map<std::string, bool> macro_last_sent_on_;

    /// Stage a saved leds/selected_strips once, the first discovery with no
    /// leds/auto_state/strips saved (see plan_selection_migration()).
    void migrate_legacy_selection();
};

/// Whether the chamber light is known to be on.
[[nodiscard]] bool chamber_light_on();

} // namespace helix::led
