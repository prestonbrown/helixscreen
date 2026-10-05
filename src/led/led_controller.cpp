// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_controller.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "color_utils.h"
#include "config.h"
#include "device_display_name.h"
#include "helix/xml/scoped_subject_registry.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "led/led_auto_state.h"
#include "led/led_color_utils.h"
#include "led/led_device_page.h"
#include "led_wled_json.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>

namespace {

/// Parse a JSON color value that may be an integer (legacy) or "#RRGGBB" string.
/// Returns default_val if the JSON is neither a valid integer nor hex string.
uint32_t parse_json_color(const nlohmann::json& j, uint32_t default_val) {
    if (j.is_number()) {
        return static_cast<uint32_t>(j.get<int>());
    }
    if (j.is_string()) {
        uint32_t rgb = 0;
        if (helix::parse_hex_color(j.get<std::string>().c_str(), rgb)) {
            return rgb;
        }
    }
    return default_val;
}

helix::led::LedStripInfo macro_device_info(const helix::led::LedMacroInfo& macro) {
    helix::led::LedStripInfo info;
    info.name = macro.display_name;
    info.id = helix::led::MACRO_STRIP_PREFIX + macro.display_name;
    info.backend = helix::led::LedBackendType::MACRO;
    info.supports_color = false;
    info.supports_white = false;
    return info;
}

} // anonymous namespace

namespace helix::led {

// ============================================================================
// LedController
// ============================================================================

LedController& LedController::instance() {
    static LedController s_instance;
    return s_instance;
}

void LedController::init(IMoonrakerAPI* api, IMoonrakerClient* client) {
    api_ = api;
    client_ = client;

    native_.set_api(api);
    effects_.set_api(api);
    wled_.set_api(api);
    wled_.set_client(client);
    macro_.set_api(api);
    output_pin_.set_api(api);

    // Initialize subjects for UI binding (idempotent).
    // led_controllable is registered globally so XML can bind it by name.
    if (!version_subject_initialized_) {
        lv_subject_init_int(&led_config_version_, 0);
        lv_subject_init_int(&led_controllable_, 0);
        lv_subject_init_int(&led_has_devices_, 0);
        lv_subject_init_int(&led_command_in_flight_, 0);
        lv_subject_init_int(&led_state_version_, 0);
        helix::xml::register_subject_in_current_scope("led_controllable", &led_controllable_);
        helix::xml::register_subject_in_current_scope("led_has_devices", &led_has_devices_);
        helix::xml::register_subject_in_current_scope("led_command_in_flight",
                                                      &led_command_in_flight_);
        helix::xml::register_subject_in_current_scope("led_state_version", &led_state_version_);
        subjects_.register_subject(&led_config_version_);
        subjects_.register_subject(&led_controllable_, "led_controllable");
        subjects_.register_subject(&led_has_devices_, "led_has_devices");
        subjects_.register_subject(&led_command_in_flight_, "led_command_in_flight");
        subjects_.register_subject(&led_state_version_, "led_state_version");
        version_subject_initialized_ = true;
    }
    // Every init(), not only the one that creates the subjects: a registry cleared in
    // between would otherwise leave initialized subjects with no deinit entry. A
    // re-registration keeps its slot in teardown order.
    StaticSubjectRegistry::instance().register_deinit("LedController", [this]() {
        subjects_.deinit_all();
        version_subject_initialized_ = false;
    });

    // Observe printer connection state to clear in-flight count on disconnect.
    // Prevents toggle buttons staying greyed forever when a WebSocket drop
    // arrives while an LED command ACK is pending.
    conn_observer_ = helix::ui::observe<int>(
        get_printer_state().get_printer_connection_state_subject(), this,
        [](LedController* self, int state) {
            if (state != static_cast<int>(helix::ConnectionState::CONNECTED)) {
                self->force_clear_in_flight();
            }
        },
        get_printer_state().get_subjects_lifetime());

    // Second half of the same safety net, for the case the observer above cannot
    // see: printer_connection_state tracks the MOONRAKER WebSocket only. A Klipper
    // restart (M112, FIRMWARE_RESTART, a klippy crash) leaves Moonraker itself up,
    // so that subject never leaves CONNECTED and a leaked dispatch would stay
    // pinned for the rest of the session — both light buttons greyed out for hours
    // (#1129). Any exit from READY kills every RPC Klipper had in flight, so clear.
    // force_clear_in_flight() is a no-op at count 0, so a simultaneous
    // disconnect firing both observers is harmless.
    klippy_observer_ = helix::ui::observe<int>(
        get_printer_state().get_klippy_state_subject(), this,
        [](LedController* self, int state) {
            if (state != static_cast<int>(helix::KlippyState::READY)) {
                self->force_clear_in_flight();
            }
        },
        get_printer_state().get_subjects_lifetime());

    initialized_ = true;
    load_config();
    publish_controllable_state();
    spdlog::info("[LedController] Initialized");
}

void LedController::deinit() {
    lifetime_.invalidate();

    // Unsubscribe the connection-state and klippy-state observers before clearing
    // in-flight state so any queued observer callbacks are neutralised before we reset.
    conn_observer_.reset();
    klippy_observer_.reset();

    // Clear any in-flight count so the subject is reset to 0 before the next
    // init. Deferred settle callbacks from the previous session are now dead
    // (token expired), so they will never decrement the counter themselves.
    force_clear_in_flight();

    native_.clear();
    native_.forget_state();
    effects_.clear();
    wled_.clear();
    macro_.clear();
    output_pin_.clear();
    output_pin_.forget_state();

    native_.set_api(nullptr);
    effects_.set_api(nullptr);
    wled_.set_api(nullptr);
    wled_.set_client(nullptr);
    macro_.set_api(nullptr);
    output_pin_.set_api(nullptr);

    api_ = nullptr;
    client_ = nullptr;
    initialized_ = false;

    // Reset config state to defaults
    legacy_selection_.clear();
    last_color_ = LastColor{};
    last_brightness_ = 100;
    color_presets_.clear();
    configured_macros_.clear();
    discovered_led_macros_.clear();
    led_on_at_start_ = false;
    startup_brightness_ = 80;
    macro_last_sent_on_.clear();
    pending_query_ids_.clear();
    configfile_config_ = nlohmann::json();
    // Re-arm the startup preference. deinit() only runs from
    // PrinterSession::tear_down_printer_state() (printer switch, add-printer wizard)
    // and shutdown — a rediscovery re-runs init() alone and must NOT re-arm it.
    startup_preference_applied_ = false;
    wled_discovery_pending_ = false;

    spdlog::info("[LedController] Deinitialized");
}

bool LedController::has_any_backend() const {
    return native_.is_available() || effects_.is_available() || wled_.is_available() ||
           macro_.is_available() || output_pin_.is_available();
}

std::vector<LedBackendType> LedController::available_backends() const {
    std::vector<LedBackendType> result;
    if (native_.is_available()) {
        result.push_back(LedBackendType::NATIVE);
    }
    if (effects_.is_available()) {
        result.push_back(LedBackendType::LED_EFFECT);
    }
    if (wled_.is_available()) {
        result.push_back(LedBackendType::WLED);
    }
    if (macro_.is_available()) {
        result.push_back(LedBackendType::MACRO);
    }
    if (output_pin_.is_available()) {
        result.push_back(LedBackendType::OUTPUT_PIN);
    }
    return result;
}

void LedController::discover_from_hardware(const helix::PrinterDiscovery& hardware) {
    // Clear existing discovery data
    native_.clear();
    effects_.clear();
    wled_.clear();
    macro_.clear();
    output_pin_.clear();

    // Populate native backend from discovered LEDs
    for (const auto& led_id : hardware.leds()) {
        // Check if this is an output_pin (not a native LED strip)
        if (led_id.rfind("output_pin ", 0) == 0) {
            LedStripInfo pin;
            const std::string display = helix::prettify_name(led_id.substr(11));
            pin.name = display;
            pin.id = led_id;
            pin.backend = LedBackendType::OUTPUT_PIN;
            pin.supports_color = false;
            pin.supports_white = false;
            pin.is_pwm = true; // Default to PWM; configfile will override if needed
            output_pin_.add_pin(pin);
            spdlog::info("[LedController] Discovered output_pin LED: {} -> {}", led_id, display);
            continue;
        }

        LedStripInfo strip;
        strip.id = led_id;
        strip.backend = LedBackendType::NATIVE;
        // Fail-closed: default to white-only (no color). Addressable strips
        // (neopixel/dotstar) are always RGB; update_pin_config() settles their W
        // channel. A generic [led] stays white-only until the configfile proves RGB
        // pins exist, avoiding a meaningless color picker on white-only chamber lights.
        strip.supports_color = false;

        // The display name is the object name without its type prefix
        std::string raw_name;
        if (led_id.rfind("neopixel ", 0) == 0) {
            raw_name = led_id.substr(9);
            strip.supports_color = true; // Addressable: RGB(W); no rgb pins in configfile
            strip.supports_white = true; // Until its color_order is read
        } else if (led_id.rfind("dotstar ", 0) == 0) {
            raw_name = led_id.substr(8);
            strip.supports_color = true; // Addressable RGB; no rgb pins in configfile
            strip.supports_white = false;
        } else if (led_id.rfind("led ", 0) == 0) {
            raw_name = led_id.substr(4);
            // Generic [led]: white-only until the configfile parse proves RGB pins
            // exist (update_pin_config sets both flags from the pins present).
            strip.supports_white = true;
        } else {
            raw_name = led_id;
            strip.supports_white = false;
        }

        strip.name = helix::prettify_name(raw_name);

        native_.add_strip(strip);
        spdlog::debug("[LedController] Discovered native LED strip: {} ({})", strip.name, strip.id);
    }

    if (native_.is_available()) {
        spdlog::info("[LedController] Discovered {} native LED strip(s)", native_.strips().size());
    }

    // LED effects
    for (const auto& effect_name : hardware.led_effects()) {
        LedEffectInfo info;
        info.name = effect_name;
        info.display_name = LedEffectBackend::display_name_for_effect(effect_name);
        info.icon_hint = LedEffectBackend::icon_hint_for_effect(effect_name);
        effects_.add_effect(info);
    }

    if (effects_.is_available()) {
        spdlog::info("[LedController] Discovered {} LED effect(s)", effects_.effects().size());
    }

    // LED macros — store as candidates for settings UI, don't create devices
    discovered_led_macros_.clear();
    for (const auto& macro_name : hardware.led_macros()) {
        discovered_led_macros_.push_back(macro_name);
    }

    if (!discovered_led_macros_.empty()) {
        spdlog::info("[LedController] Discovered {} LED macro candidate(s)",
                     discovered_led_macros_.size());
    }

    // A bare ON/OFF pair is unambiguous, so promote it to a real device rather
    // than leaving the user to hand-build one from the settings dropdowns.
    seed_auto_paired_macros();

    // Repopulate the macro backend — the clear() above wiped what load_config()
    // installed. Same single rebuild path, so the two lists cannot drift.
    rebuild_macro_backend();

    if (macro_.is_available()) {
        spdlog::info("[LedController] Loaded {} configured macro device(s)",
                     macro_.macros().size());
    }

    // The clear() calls above dropped every capability the configfile set.
    apply_stored_configfile();

    migrate_legacy_selection();

    bump_config_version();
    publish_controllable_state();
}

void LedController::bump_config_version() {
    if (version_subject_initialized_) {
        lv_subject_set_int(&led_config_version_, lv_subject_get_int(&led_config_version_) + 1);
        spdlog::debug("[LedController] LED config version bumped to {}",
                      lv_subject_get_int(&led_config_version_));
    }
}

void LedController::migrate_legacy_selection() {
    auto* cfg = Config::get_instance();
    if (legacy_selection_.empty()) {
        return;
    }
    const nlohmann::json* existing = cfg->try_get_json(cfg->df() + AUTO_STATE_STRIPS_PATH);
    if (existing != nullptr && existing->is_array()) {
        return;
    }
    const auto plan = plan_selection_migration(legacy_selection_, switchable_ids());
    spdlog::info("[LedController] Legacy LED selection ({} strip(s)) -> light button '{}', "
                 "auto-state {} strip(s)",
                 legacy_selection_.size(), plan.light_button, plan.auto_state_strips.size());
    stage_light_selection(plan);
}

void LedController::discover_wled_strips() {
    if (!api_) {
        spdlog::warn("[LedController] discover_wled_strips called with no API");
        return;
    }

    spdlog::debug("[LedController] Starting WLED strip discovery via Moonraker");

    const unsigned gen = ++wled_discovery_gen_;
    wled_discovery_pending_ = true;
    // A request that never completes still settles, so nothing waits on it forever.
    struct Timeout {
        LifetimeToken tok;
        LedController* self;
        unsigned gen;
    };
    auto* timer = lv_timer_create(
        [](lv_timer_t* t) {
            auto* data = static_cast<Timeout*>(lv_timer_get_user_data(t));
            data->tok.defer(
                "LedController::wled_discovery_timeout",
                [self = data->self, gen = data->gen]() { self->settle_wled_discovery(gen); });
            delete data;
        },
        WLED_DISCOVERY_TIMEOUT_MS,
        new Timeout{lifetime_.token(), this, gen}); // TIMER_DTOR_OK: LifetimeToken-guarded one-shot
    lv_timer_set_repeat_count(timer, 1);

    auto token = lifetime_.token();
    api_->rest().wled_get_strips(
        [this, token, gen](const RestResponse& resp) {
            // === BG THREAD: parse, validate, build local strip list ===
            // Response format: {"result": {"strips": {strip_name: {details...}, ...}}}
            if (!resp.data.is_object()) {
                spdlog::warn("[LedController] WLED strips response is not a JSON object");
                token.defer("LedController::wled_settled",
                            [this, gen]() { settle_wled_discovery(gen); });
                return;
            }

            const json& strips_data = detail::wled_strip_map(resp.data);

            std::vector<LedStripInfo> discovered;
            if (strips_data.is_object()) {
                for (auto it = strips_data.begin(); it != strips_data.end(); ++it) {
                    LedStripInfo strip;
                    strip.id = it.key();
                    strip.backend = LedBackendType::WLED;
                    strip.supports_color = true;
                    strip.supports_white = true;

                    // Use strip name from response data, or fall back to key
                    std::string raw_name;
                    if (it.value().is_object() && it.value().contains("strip")) {
                        raw_name = helix::json_util::as_string(it.value()["strip"], it.key());
                    } else {
                        raw_name = it.key();
                    }

                    strip.name = helix::prettify_name(raw_name);
                    spdlog::debug("[LedController] Discovered WLED strip: {} ({})", strip.name,
                                  strip.id);
                    discovered.push_back(std::move(strip));
                }
            }

            if (discovered.empty()) {
                spdlog::debug("[LedController] No WLED strips found");
                token.defer("LedController::wled_settled",
                            [this, gen]() { settle_wled_discovery(gen); });
                return;
            }

            // === MAIN THREAD: apply discovered strips, then chain server config fetch ===
            token.defer("LedController::wled_strips_apply",
                        [this, gen, discovered = std::move(discovered)]() mutable {
                            spdlog::info("[LedController] Discovered {} WLED strip(s)",
                                         discovered.size());
                            for (auto& strip : discovered) {
                                wled_.add_strip(strip);
                            }
                            bump_config_version();
                            publish_controllable_state();
                            settle_wled_discovery(gen);

                            // Poll initial status
                            refresh_wled_state();
                        });
        },
        [this, token, gen](const MoonrakerError& err) {
            // WLED not configured is expected on most printers
            spdlog::debug("[LedController] WLED discovery unavailable: {}", err.message);
            token.defer("LedController::wled_settled",
                        [this, gen]() { settle_wled_discovery(gen); });
        });
}

void LedController::settle_wled_discovery(unsigned gen) {
    if (!wled_discovery_pending_ || gen != wled_discovery_gen_) {
        return;
    }
    wled_discovery_pending_ = false;
    if (on_wled_settled_) {
        on_wled_settled_();
    }
}

void LedController::apply_configfile(const nlohmann::json& configfile_config) {
    // Keep only the sections the LED backends read: the whole printer config
    // would otherwise stay resident for the session.
    configfile_config_ = nlohmann::json::object();
    if (configfile_config.is_object()) {
        for (auto it = configfile_config.begin(); it != configfile_config.end(); ++it) {
            const std::string& key = it.key();
            for (const char* prefix :
                 {"led ", "led_effect ", "neopixel", "dotstar", "output_pin "}) {
                if (key.rfind(prefix, 0) == 0) {
                    configfile_config_[key] = it.value();
                    break;
                }
            }
        }
    }
    apply_stored_configfile();
}

void LedController::apply_stored_configfile() {
    if (configfile_config_.empty()) {
        return;
    }
    update_effect_targets(configfile_config_);
    update_output_pin_config(configfile_config_);
    update_led_pin_config(configfile_config_);
}

void LedController::update_effect_targets(const nlohmann::json& configfile_config) {
    if (!configfile_config.is_object()) {
        spdlog::debug("[LedController] update_effect_targets: config is not an object");
        return;
    }

    int updated = 0;
    for (auto it = configfile_config.begin(); it != configfile_config.end(); ++it) {
        const std::string& key = it.key();

        // Match keys like "led_effect breathing", "led_effect fire_comet"
        if (key.rfind("led_effect ", 0) != 0) {
            continue;
        }

        if (!it.value().is_object() || !it.value().contains("leds")) {
            continue;
        }

        const auto& leds_val = it.value()["leds"];
        std::string leds_str;
        if (leds_val.is_string()) {
            leds_str = leds_val.get<std::string>();
        } else {
            continue;
        }

        // Parse the leds field: may contain multiple LED targets separated by newlines
        std::vector<std::string> targets;
        for (std::string_view sv : helix::text_io::lines(leds_str)) {
            std::string line(sv);
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) {
                continue;
            }
            line = line.substr(start);
            size_t end = line.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) {
                line = line.substr(0, end + 1);
            }

            if (line.empty()) {
                continue;
            }

            std::string target = LedEffectBackend::parse_klipper_led_target(line);
            if (!target.empty()) {
                targets.push_back(target);
            }
        }

        if (!targets.empty()) {
            effects_.set_effect_targets(key, targets);
            ++updated;
        }
    }

    spdlog::info("[LedController] Updated effect targets for {} effect(s)", updated);
}

void LedController::update_output_pin_config(const nlohmann::json& configfile_config) {
    if (!configfile_config.is_object()) {
        return;
    }

    for (const auto& pin : output_pin_.pins()) {
        if (!configfile_config.contains(pin.id)) {
            continue;
        }
        // Klipper's output_pin reads `pwm` as getboolean('pwm', False): a section
        // with no pwm line is a digital pin.
        const auto& pin_cfg = configfile_config[pin.id];
        bool is_pwm = false;
        if (pin_cfg.is_object() && pin_cfg.contains("pwm")) {
            const auto& pwm_val = pin_cfg["pwm"];
            if (pwm_val.is_boolean()) {
                is_pwm = pwm_val.get<bool>();
            } else if (pwm_val.is_string()) {
                std::string s = pwm_val.get<std::string>();
                s = helix::text_io::to_lower(s);
                is_pwm = (s == "true" || s == "1" || s == "yes" || s == "on");
            }
        }
        output_pin_.set_pin_pwm(pin.id, is_pwm);
        spdlog::debug("[LedController] Output pin {} PWM: {}", pin.id, is_pwm);
    }
}

void LedController::update_led_pin_config(const nlohmann::json& configfile_config) {
    if (!configfile_config.is_object()) {
        return;
    }

    native_.update_pin_config(configfile_config);
}

// ============================================================================
// NativeBackend
// ============================================================================

namespace {

/// False, after logging and telling on_error, when a backend has no API yet.
/// PrinterSession::init_core_subjects() runs init(nullptr, nullptr), so this is reachable.
bool require_api(const IMoonrakerAPI* api, const char* call, const std::string& target,
                 const NativeBackend::ErrorCallback& on_error) {
    if (api != nullptr) {
        return true;
    }
    spdlog::warn("[LED] {} called with no API ({})", call, target);
    if (on_error) {
        on_error(std::string(call) + ": no API available");
    }
    return false;
}

/// The MoonrakerError adapter every backend hands the API.
std::function<void(const MoonrakerError&)>
forward_error(const NativeBackend::ErrorCallback& on_error) {
    return [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    };
}

/// True when every entry of a neopixel section's color_order has a W. Klipper
/// reads the option as a comma list, one order for the whole chain or one per LED.
bool color_order_has_white(const nlohmann::json& section) {
    const std::string order = helix::json_util::safe_string(section, "color_order");
    if (order.empty()) {
        return false;
    }
    size_t start = 0;
    while (start <= order.size()) {
        const size_t end = std::min(order.find(',', start), order.size());
        if (order.substr(start, end - start).find('W') == std::string::npos) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace

void NativeBackend::update_pin_config(const nlohmann::json& config_section) {
    if (!config_section.is_object()) {
        return;
    }

    for (auto& strip : strips_) {
        // Klipper configfile uses the full section header as key (e.g., "led case_light"),
        // matching strip.id directly — same pattern as update_output_pin_config.
        if (!config_section.contains(strip.id)) {
            continue;
        }

        const auto& led_cfg = config_section[strip.id];
        if (!led_cfg.is_object()) {
            continue;
        }

        // Addressable strips name no channel pins: a neopixel has W where its
        // color_order says so (Klipper's default is GRB, and a chain may list one
        // order per LED), and a dotstar never has W.
        const bool neopixel = strip.id.rfind("neopixel ", 0) == 0;
        if (neopixel || strip.id.rfind("dotstar ", 0) == 0) {
            strip.supports_color = true;
            strip.supports_white = neopixel && color_order_has_white(led_cfg);
            strip.pin_config_known = true;
            spdlog::info("[NativeBackend] Strip '{}' white channel: {}", strip.id,
                         strip.supports_white);
            continue;
        }

        bool has_red = led_cfg.contains("red_pin") && !led_cfg["red_pin"].is_null();
        bool has_green = led_cfg.contains("green_pin") && !led_cfg["green_pin"].is_null();
        bool has_blue = led_cfg.contains("blue_pin") && !led_cfg["blue_pin"].is_null();
        bool has_white = led_cfg.contains("white_pin") && !led_cfg["white_pin"].is_null();

        if (has_red || has_green || has_blue || has_white) {
            strip.has_red_pin = has_red;
            strip.has_green_pin = has_green;
            strip.has_blue_pin = has_blue;
            strip.has_white_pin = has_white;
            strip.pin_config_known = true;

            // Update supports_color / supports_white based on actual pins
            strip.supports_color = (has_red || has_green || has_blue);
            strip.supports_white = has_white;

            spdlog::info("[NativeBackend] Strip '{}' pin config: R={} G={} B={} W={}", strip.id,
                         has_red, has_green, has_blue, has_white);
        }
    }
}

void NativeBackend::add_strip(const LedStripInfo& strip) {
    strips_.push_back(strip);
}

void NativeBackend::clear() {
    strips_.clear();
}

void NativeBackend::forget_state() {
    strip_colors_.clear();
}

void NativeBackend::set_color(const std::string& strip_id, double r, double g, double b, double w,
                              SuccessCallback on_success, ErrorCallback on_error,
                              SuccessCallback on_queued, bool silent) {
    if (!require_api(api_, "NativeBackend::set_color", strip_id, on_error)) {
        return;
    }

    // Clamp color values to valid range
    r = std::clamp(r, 0.0, 1.0);
    g = std::clamp(g, 0.0, 1.0);
    b = std::clamp(b, 0.0, 1.0);
    w = std::clamp(w, 0.0, 1.0);

    // Cache the color we're sending
    strip_colors_[strip_id] = {r, g, b, w};

    spdlog::debug("[NativeBackend] set_color: {} r={:.2f} g={:.2f} b={:.2f} w={:.2f}", strip_id, r,
                  g, b, w);

    // Captured before the wrapper below, which is non-null on every call and
    // would otherwise read our own bookkeeping as a promise the caller never
    // made — silencing GcodeErrorRouter's `!!` copy of the same rejection.
    // See include/rpc_error_policy.h.
    const bool caller_surfaces = (on_error != nullptr);

    api_->set_led(strip_id, r, g, b, w, std::move(on_success), forward_error(on_error),
                  std::move(on_queued), caller_surfaces, silent);
}

void NativeBackend::turn_off(const std::string& strip_id, SuccessCallback on_success,
                             ErrorCallback on_error, SuccessCallback on_queued, bool silent) {
    spdlog::debug("[NativeBackend] turn_off: {}", strip_id);
    // Set all channels to zero
    set_color(strip_id, 0.0, 0.0, 0.0, 0.0, std::move(on_success), std::move(on_error),
              std::move(on_queued), silent);
}

void NativeBackend::StripColor::decompose(uint32_t& base_color, int& brightness_pct,
                                          double& base_white) const {
    const uint8_t ri = to_channel_byte(r);
    const uint8_t gi = to_channel_byte(g);
    const uint8_t bi = to_channel_byte(b);
    const uint8_t wi = to_channel_byte(w);

    // W participates in the brightness max. A Klipper white-only [led] section
    // (white_pin only) puts its entire level in the 4th channel — color_data
    // [[0.0, 0.0, 0.0, 0.15]] — so an RGB-only max reads back as 0%, and
    // re-applying 0% writes the user's light off (#1129).
    const uint8_t max_c = std::max({ri, gi, bi, wi});

    brightness_pct = channel_to_percent(max_c);

    // base_white is the W level scaled back up to full brightness, so that
    // base_white * (brightness_pct / 100) reproduces the reported W exactly.
    base_white = static_cast<double>(scale_channel_to_full(wi, max_c)) / 255.0;

    if (ri == 0 && gi == 0 && bi == 0 && wi > 0) {
        // White-only output: there is no hue to recover from all-zero RGB.
        // Report neutral white as the base so the swatch and the color presets
        // show a dimmed white rather than black. The actual write still goes
        // out on the W channel (see LedControlOverlay::apply_current_color).
        base_color = 0xFFFFFFu;
    } else {
        base_color = (static_cast<uint32_t>(scale_channel_to_full(ri, max_c)) << 16) |
                     (static_cast<uint32_t>(scale_channel_to_full(gi, max_c)) << 8) |
                     scale_channel_to_full(bi, max_c);
    }
}

bool NativeBackend::update_from_status(const nlohmann::json& status) {
    bool carried = false;
    // Non-const iteration: the RGBW capability fix-up below patches the very
    // strip the loop is holding (it used to re-scan strips_ to find it again).
    for (auto& strip : strips_) {
        if (!status.contains(strip.id))
            continue;

        RgbwF parsed;
        if (!parse_color_data(status[strip.id], parsed))
            continue;

        StripColor color;
        color.r = parsed.r;
        color.g = parsed.g;
        color.b = parsed.b;
        color.w = parsed.w;
        strip_colors_[strip.id] = color;
        carried = true;

        // Detect RGBW from the color_data size, only for a strip whose configfile
        // named no channel pins: Klipper reports four channels for every [led].
        const bool has_white = (parsed.channels >= 4);
        if (!strip.pin_config_known && strip.supports_white != has_white) {
            spdlog::info("[NativeBackend] Strip '{}' RGBW detection updated: {} -> {}", strip.id,
                         strip.supports_white, has_white);
            strip.supports_white = has_white;
        }

        if (color_change_cb_) {
            color_change_cb_(strip.id, color);
        }
    }
    return carried;
}

NativeBackend::StripColor NativeBackend::get_strip_color(const std::string& strip_id) const {
    auto it = strip_colors_.find(strip_id);
    if (it != strip_colors_.end())
        return it->second;
    // Default: white at full brightness
    return {1.0, 1.0, 1.0, 0.0};
}

bool NativeBackend::has_strip_color(const std::string& strip_id) const {
    return strip_colors_.count(strip_id) > 0;
}

// ============================================================================
// LedEffectBackend
// ============================================================================

void LedEffectBackend::add_effect(const LedEffectInfo& effect) {
    effects_.push_back(effect);
}

void LedEffectBackend::clear() {
    effects_.clear();
}

void LedEffectBackend::set_effect_targets(const std::string& effect_name,
                                          const std::vector<std::string>& targets) {
    for (auto& effect : effects_) {
        if (effect.name == effect_name) {
            effect.target_leds = targets;
            spdlog::debug("[LedEffectBackend] Set {} target(s) for effect '{}'", targets.size(),
                          effect_name);
            return;
        }
    }
    spdlog::debug("[LedEffectBackend] Effect '{}' not found for target assignment", effect_name);
}

bool LedEffectBackend::update_from_status(const nlohmann::json& status) {
    bool carried = false;
    for (auto& effect : effects_) {
        if (!status.contains(effect.name))
            continue;

        const auto& effect_data = status[effect.name];
        if (!effect_data.is_object() || !effect_data.contains("enabled"))
            continue;

        const bool new_enabled = helix::json_util::as_bool(effect_data["enabled"], effect.enabled);
        carried = true;
        if (new_enabled != effect.enabled) {
            spdlog::debug("[LedEffectBackend] Effect '{}' enabled: {} -> {}", effect.name,
                          effect.enabled, new_enabled);
            effect.enabled = new_enabled;
        }
    }
    return carried;
}

bool LedEffectBackend::is_effect_enabled(const std::string& effect_name) const {
    for (const auto& effect : effects_) {
        if (effect.name == effect_name) {
            return effect.enabled;
        }
    }
    return false;
}

std::vector<LedEffectInfo> LedEffectBackend::effects_for_strip(const std::string& strip_id) const {
    std::vector<LedEffectInfo> result;
    for (const auto& effect : effects_) {
        // Include effects that target this strip, or effects with no targets (unfiltered)
        if (effect.target_leds.empty() ||
            std::find(effect.target_leds.begin(), effect.target_leds.end(), strip_id) !=
                effect.target_leds.end()) {
            result.push_back(effect);
        }
    }
    return result;
}

std::string LedEffectBackend::parse_klipper_led_target(const std::string& klipper_format) {
    // Input: "neopixel:chamber_light" or "neopixel:chamber_light (1-10)"
    // Output: "neopixel chamber_light"
    std::string result = klipper_format;

    // Strip any LED range suffix like " (1-10)"
    auto paren_pos = result.find(" (");
    if (paren_pos == std::string::npos) {
        paren_pos = result.find('(');
    }
    if (paren_pos != std::string::npos) {
        result = result.substr(0, paren_pos);
    }

    // Trim trailing whitespace
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }

    // Replace colon with space: "neopixel:name" -> "neopixel name"
    auto colon_pos = result.find(':');
    if (colon_pos != std::string::npos) {
        result[colon_pos] = ' ';
    }

    return result;
}

void LedEffectBackend::activate_effect(const std::string& effect_name,
                                       NativeBackend::SuccessCallback on_success,
                                       NativeBackend::ErrorCallback on_error,
                                       NativeBackend::SuccessCallback on_queued,
                                       bool caller_surfaces_errors, bool silent) {
    if (!require_api(api_, "LedEffectBackend::activate_effect", effect_name, on_error)) {
        return;
    }

    // Strip "led_effect " prefix to get bare effect name for gcode
    std::string bare_name = effect_name;
    const std::string prefix = "led_effect ";
    if (bare_name.rfind(prefix, 0) == 0) {
        bare_name = bare_name.substr(prefix.size());
    }

    std::string gcode = "SET_LED_EFFECT EFFECT=" + bare_name;
    spdlog::debug("[LedEffectBackend] activate_effect: {} -> gcode: {}", effect_name, gcode);

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = caller_surfaces_errors && (on_error != nullptr);

    api_->execute_gcode(gcode, std::move(on_success), forward_error(on_error),
                        /*timeout_ms=*/0, silent, std::move(on_queued), caller_surfaces);
}

void LedEffectBackend::stop_all_effects(NativeBackend::SuccessCallback on_success,
                                        NativeBackend::ErrorCallback on_error,
                                        NativeBackend::SuccessCallback on_queued,
                                        bool caller_surfaces_errors, bool silent) {
    if (!require_api(api_, "LedEffectBackend::stop_all_effects", "", on_error)) {
        return;
    }

    spdlog::debug("[LedEffectBackend] stop_all_effects: gcode: STOP_LED_EFFECTS");

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = caller_surfaces_errors && (on_error != nullptr);

    api_->execute_gcode("STOP_LED_EFFECTS", std::move(on_success), forward_error(on_error),
                        /*timeout_ms=*/0, silent, std::move(on_queued), caller_surfaces);
}

void LedEffectBackend::stop_effect(const std::string& effect_name,
                                   NativeBackend::SuccessCallback on_success,
                                   NativeBackend::ErrorCallback on_error,
                                   NativeBackend::SuccessCallback on_queued, bool silent) {
    if (!require_api(api_, "LedEffectBackend::stop_effect", effect_name, on_error)) {
        return;
    }

    // Strip "led_effect " prefix to get bare effect name for gcode
    std::string bare_name = effect_name;
    const std::string prefix = "led_effect ";
    if (bare_name.rfind(prefix, 0) == 0) {
        bare_name = bare_name.substr(prefix.size());
    }

    std::string gcode = "SET_LED_EFFECT EFFECT=" + bare_name + " STOP=1";
    spdlog::debug("[LedEffectBackend] stop_effect: {} -> gcode: {}", effect_name, gcode);

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = (on_error != nullptr);

    api_->execute_gcode(gcode, std::move(on_success), forward_error(on_error),
                        /*timeout_ms=*/0, silent, std::move(on_queued), caller_surfaces);
}

std::string LedEffectBackend::icon_hint_for_effect(const std::string& effect_name) {
    // Convert to lowercase for matching
    std::string lower = effect_name;
    lower = helix::text_io::to_lower(lower);

    if (lower.find("breathing") != std::string::npos || lower.find("pulse") != std::string::npos) {
        return "air";
    }
    if (lower.find("fire") != std::string::npos || lower.find("flame") != std::string::npos) {
        return "local_fire_department";
    }
    if (lower.find("rainbow") != std::string::npos) {
        return "palette";
    }
    if (lower.find("chase") != std::string::npos || lower.find("comet") != std::string::npos) {
        return "fast_forward";
    }
    if (lower.find("static") != std::string::npos) {
        return "lightbulb";
    }
    return "auto_awesome";
}

std::string LedEffectBackend::display_name_for_effect(const std::string& config_name) {
    if (config_name.empty()) {
        return "";
    }

    // Strip "led_effect " prefix if present
    std::string raw = config_name;
    const std::string prefix = "led_effect ";
    if (raw.rfind(prefix, 0) == 0) {
        raw = raw.substr(prefix.size());
    }

    if (raw.empty()) {
        return "";
    }

    return helix::prettify_name(raw);
}

// ============================================================================
// WledBackend
// ============================================================================

void WledBackend::add_strip(const LedStripInfo& strip) {
    strips_.push_back(strip);
}

void WledBackend::clear() {
    strips_.clear();
    strip_states_.clear();
}

void WledBackend::send(const std::string& strip_name, const char* action, int brightness,
                       int preset, NativeBackend::SuccessCallback on_success,
                       NativeBackend::ErrorCallback on_error) {
    if (!require_api(api_, "WledBackend", strip_name, on_error)) {
        return;
    }
    spdlog::debug("[WledBackend] {} {} brightness={} preset={}", strip_name, action, brightness,
                  preset);
    api_->rest().wled_set_strip(strip_name, action, brightness, preset, std::move(on_success),
                                forward_error(on_error));
}

void WledBackend::set_on(const std::string& strip_name, NativeBackend::SuccessCallback on_success,
                         NativeBackend::ErrorCallback on_error) {
    if (api_) {
        strip_states_[strip_name].is_on = true; // optimistic
    }
    send(strip_name, "on", -1, -1, std::move(on_success), std::move(on_error));
}

void WledBackend::set_off(const std::string& strip_name, NativeBackend::SuccessCallback on_success,
                          NativeBackend::ErrorCallback on_error) {
    if (api_) {
        strip_states_[strip_name].is_on = false; // optimistic
    }
    send(strip_name, "off", -1, -1, std::move(on_success), std::move(on_error));
}

void WledBackend::set_brightness(const std::string& strip_name, int brightness,
                                 NativeBackend::SuccessCallback on_success,
                                 NativeBackend::ErrorCallback on_error) {
    // WLED takes 0-255
    const int wled_brightness = (std::clamp(brightness, 0, 100) * 255) / 100;
    send(strip_name, "on", wled_brightness, -1, std::move(on_success), std::move(on_error));
}

void WledBackend::set_preset(const std::string& strip_name, int preset_id,
                             NativeBackend::SuccessCallback on_success,
                             NativeBackend::ErrorCallback on_error) {
    send(strip_name, "on", -1, preset_id, std::move(on_success), std::move(on_error));
}

void WledBackend::toggle(const std::string& strip_name, NativeBackend::SuccessCallback on_success,
                         NativeBackend::ErrorCallback on_error) {
    send(strip_name, "toggle", -1, -1, std::move(on_success), std::move(on_error));
}

void WledBackend::update_strip_state(const std::string& strip_id, const WledStripState& state) {
    strip_states_[strip_id] = state;
    spdlog::debug("[WledBackend] Updated state for '{}': on={} brightness={} preset={}", strip_id,
                  state.is_on, state.brightness, state.active_preset);
}

WledStripState WledBackend::get_strip_state(const std::string& strip_id) const {
    auto it = strip_states_.find(strip_id);
    if (it != strip_states_.end()) {
        return it->second;
    }
    return WledStripState{};
}

bool WledBackend::has_strip_state(const std::string& strip_id) const {
    return strip_states_.count(strip_id) > 0;
}

void WledBackend::poll_status(std::function<void()> on_complete) {
    if (!api_) {
        spdlog::debug("[WledBackend] poll_status: no API available");
        if (on_complete)
            on_complete();
        return;
    }

    api_->rest().wled_get_strips(
        lifetime_.bg_cb(
            "WledBackend::poll_status",
            [this, on_complete](const RestResponse& resp) {
                const json& strips_data = detail::wled_strip_map(resp.data);

                if (strips_data.is_object()) {
                    for (auto it = strips_data.begin(); it != strips_data.end(); ++it) {
                        WledStripState state;
                        auto& val = it.value();
                        if (val.is_object()) {
                            // Parse status field: "on"/"off" or boolean "state"
                            if (val.contains("status")) {
                                state.is_on = helix::json_util::as_string(val["status"]) == "on";
                            } else if (val.contains("state")) {
                                state.is_on = helix::json_util::as_bool(val["state"]);
                            }
                            state.brightness = helix::json_util::safe_int(val, "brightness", 255);
                            state.active_preset = helix::json_util::safe_int(val, "preset", -1);
                        }
                        strip_states_[it.key()] = state;
                    }
                }

                if (on_complete)
                    on_complete();
            }),
        lifetime_.bg_cb("WledBackend::poll_status_error", [on_complete](const MoonrakerError& err) {
            spdlog::warn("[WledBackend] Status poll failed: {}", err.message);
            if (on_complete)
                on_complete();
        }));
}

// ============================================================================
// MacroBackend
// ============================================================================

void MacroBackend::add_macro(const LedMacroInfo& macro) {
    if (macro.display_name.empty()) {
        spdlog::warn("[MacroBackend] Rejecting macro with empty display name");
        return;
    }
    macros_.push_back(macro);
}

void MacroBackend::clear() {
    macros_.clear();
}

void MacroBackend::run(const std::string& macro_name, std::string LedMacroInfo::*gcode_field,
                       const char* what, NativeBackend::SuccessCallback on_success,
                       NativeBackend::ErrorCallback on_error) {
    if (!require_api(api_, "MacroBackend", macro_name, on_error)) {
        return;
    }
    const LedMacroInfo* macro = find_macro(macros_, macro_name);
    if (macro == nullptr) {
        spdlog::warn("[MacroBackend] Macro not found: '{}'", macro_name);
        if (on_error) {
            on_error("Macro not found: '" + macro_name + "'");
        }
        return;
    }
    // A toggle macro stands in for a missing on or off one.
    const std::string& gcode =
        !(macro->*gcode_field).empty() ? macro->*gcode_field : macro->toggle_macro;
    if (gcode.empty()) {
        spdlog::warn("[MacroBackend] No {} macro configured for '{}'", what, macro_name);
        if (on_error) {
            on_error(fmt::format("No {} macro configured for '{}'", what, macro_name));
        }
        return;
    }
    spdlog::debug("[MacroBackend] {} {} -> {}", what, macro_name, gcode);
    // Captured before forward_error(), which is non-null on every call and would
    // otherwise claim the report on the caller's behalf. See include/rpc_error_policy.h.
    const bool caller_surfaces = (on_error != nullptr);
    api_->execute_gcode(gcode, std::move(on_success), forward_error(on_error),
                        /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr, caller_surfaces);
}

void MacroBackend::execute_on(const std::string& macro_name,
                              NativeBackend::SuccessCallback on_success,
                              NativeBackend::ErrorCallback on_error) {
    run(macro_name, &LedMacroInfo::on_macro, "on", std::move(on_success), std::move(on_error));
}

void MacroBackend::execute_off(const std::string& macro_name,
                               NativeBackend::SuccessCallback on_success,
                               NativeBackend::ErrorCallback on_error) {
    run(macro_name, &LedMacroInfo::off_macro, "off", std::move(on_success), std::move(on_error));
}

void MacroBackend::execute_toggle(const std::string& macro_name,
                                  NativeBackend::SuccessCallback on_success,
                                  NativeBackend::ErrorCallback on_error) {
    run(macro_name, &LedMacroInfo::toggle_macro, "toggle", std::move(on_success),
        std::move(on_error));
}

void MacroBackend::execute_custom_action(const std::string& macro_gcode,
                                         NativeBackend::SuccessCallback on_success,
                                         NativeBackend::ErrorCallback on_error) {
    if (!require_api(api_, "MacroBackend::execute_custom_action", "", on_error)) {
        return;
    }

    spdlog::debug("[MacroBackend] execute_custom_action: {}", macro_gcode);
    const bool caller_surfaces = (on_error != nullptr);
    api_->execute_gcode(macro_gcode, on_success, forward_error(on_error),
                        /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr, caller_surfaces);
}

bool MacroBackend::has_known_state(const std::string& macro_name) const {
    for (const auto& macro : macros_) {
        if (macro.display_name == macro_name) {
            return macro.type == MacroLedType::ON_OFF;
        }
    }
    return false;
}

// ============================================================================
// OutputPinBackend
// ============================================================================

void OutputPinBackend::add_pin(const LedStripInfo& pin) {
    pins_.push_back(pin);
}

void OutputPinBackend::clear() {
    pins_.clear();
}

void OutputPinBackend::forget_state() {
    pin_values_.clear();
}

void OutputPinBackend::set_value(const std::string& pin_id, double value,
                                 NativeBackend::SuccessCallback on_success,
                                 NativeBackend::ErrorCallback on_error,
                                 NativeBackend::SuccessCallback on_queued, bool silent) {
    if (!require_api(api_, "OutputPinBackend::set_value", pin_id, on_error)) {
        return;
    }

    value = std::clamp(value, 0.0, 1.0);
    // Klipper rejects anything but 0 or 1 for a digital pin.
    if (!is_pwm(pin_id)) {
        value = (value > 0.0) ? 1.0 : 0.0;
    }

    // Extract pin name from "output_pin <name>" format
    std::string pin_name = pin_id;
    if (pin_name.rfind("output_pin ", 0) == 0) {
        pin_name = pin_name.substr(11);
    }

    // Use spdlog's bundled fmt for locale-independent float formatting
    std::string gcode = fmt::format("SET_PIN PIN={} VALUE={:.4f}", pin_name, value);

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = (on_error != nullptr);

    api_->execute_gcode(gcode, std::move(on_success), forward_error(on_error),
                        /*timeout_ms=*/0, silent, std::move(on_queued), caller_surfaces);
}

void OutputPinBackend::turn_on(const std::string& pin_id, NativeBackend::SuccessCallback on_success,
                               NativeBackend::ErrorCallback on_error,
                               NativeBackend::SuccessCallback on_queued, bool silent) {
    set_value(pin_id, 1.0, std::move(on_success), std::move(on_error), std::move(on_queued),
              silent);
}

void OutputPinBackend::turn_off(const std::string& pin_id,
                                NativeBackend::SuccessCallback on_success,
                                NativeBackend::ErrorCallback on_error,
                                NativeBackend::SuccessCallback on_queued, bool silent) {
    set_value(pin_id, 0.0, std::move(on_success), std::move(on_error), std::move(on_queued),
              silent);
}

void OutputPinBackend::set_brightness(const std::string& pin_id, int brightness_pct,
                                      NativeBackend::SuccessCallback on_success,
                                      NativeBackend::ErrorCallback on_error,
                                      NativeBackend::SuccessCallback on_queued, bool silent) {
    const double value = std::clamp(brightness_pct, 0, 100) / 100.0;
    set_value(pin_id, value, std::move(on_success), std::move(on_error), std::move(on_queued),
              silent);
}

// Called from UI thread (via UpdateQueue dispatch in printer_state.cpp)
bool OutputPinBackend::update_from_status(const nlohmann::json& status) {
    bool carried = false;
    for (const auto& pin : pins_) {
        if (!status.contains(pin.id))
            continue;
        const auto& pin_status = status[pin.id];
        if (pin_status.contains("value") && pin_status["value"].is_number()) {
            pin_values_[pin.id] = pin_status["value"].get<double>();
            carried = true;
        }
    }
    return carried;
}

bool OutputPinBackend::has_value(const std::string& pin_id) const {
    return pin_values_.count(pin_id) > 0;
}

double OutputPinBackend::get_value(const std::string& pin_id) const {
    auto it = pin_values_.find(pin_id);
    return (it != pin_values_.end()) ? it->second : 0.0;
}

int OutputPinBackend::brightness_pct(const std::string& pin_id) const {
    return std::clamp(static_cast<int>(get_value(pin_id) * 100.0 + 0.5), 0, 100);
}

bool OutputPinBackend::is_pwm(const std::string& pin_id) const {
    const auto* p = find_strip(pins_, pin_id);
    return p != nullptr && p->is_pwm;
}

void OutputPinBackend::set_pin_pwm(const std::string& pin_id, bool is_pwm) {
    if (auto* p = find_strip(pins_, pin_id)) {
        p->is_pwm = is_pwm;
    }
}

// ============================================================================
// LedController config persistence
// ============================================================================

void LedController::load_config() {
    auto* cfg = Config::get_instance();

    // The one-time fold of the legacy top-level /led block into the active
    // printer's leds/ section lives in migrate_v19_to_v20() (config.cpp); probing
    // /led from here would re-create it as an orphan on every boot (#1129).

    // The legacy selection, newest key first. Read for migrate_legacy_selection()
    // and never written back: the keys stay on disk as they were.
    legacy_selection_ = cfg->get_string_array(cfg->df() + "leds/selected_strips");

    if (legacy_selection_.empty()) {
        for (auto& s : cfg->get_string_array(cfg->df() + "leds/selected")) {
            if (!s.empty()) {
                legacy_selection_.push_back(std::move(s));
            }
        }
    }

    if (legacy_selection_.empty()) {
        const nlohmann::json* legacy_strip_json = cfg->try_get_json(cfg->df() + "leds/strip");
        std::string legacy_strip = (legacy_strip_json != nullptr && legacy_strip_json->is_string())
                                       ? legacy_strip_json->get<std::string>()
                                       : "";
        if (!legacy_strip.empty()) {
            legacy_selection_.push_back(legacy_strip);
        }
    }

    // Last color & brightness
    const nlohmann::json* color_json = cfg->try_get_json(cfg->df() + "leds/last_color");
    last_color_.rgb = color_json != nullptr ? parse_json_color(*color_json, 0xFFFFFF) : 0xFFFFFFu;
    const nlohmann::json* brightness_json = cfg->try_get_json(cfg->df() + "leds/last_brightness");
    last_brightness_ = (brightness_json != nullptr && brightness_json->is_number())
                           ? brightness_json->get<int>()
                           : 100;
    const nlohmann::json* white_json = cfg->try_get_json(cfg->df() + "leds/last_white");
    last_color_.white = (white_json != nullptr && white_json->is_number())
                            ? std::clamp(white_json->get<double>(), 0.0, 1.0)
                            : 0.0;

    // Color presets
    color_presets_.clear();
    const nlohmann::json* presets_json = cfg->try_get_json(cfg->df() + "leds/color_presets");
    if (presets_json != nullptr && presets_json->is_array()) {
        for (const auto& p : *presets_json) {
            uint32_t c = parse_json_color(p, UINT32_MAX);
            if (c != UINT32_MAX) {
                color_presets_.push_back(c);
            }
        }
    }
    color_presets_ = migrate_color_presets(color_presets_);

    // Configured macros
    configured_macros_.clear();
    const nlohmann::json* macros_json = cfg->try_get_json(cfg->df() + "leds/macro_devices");
    if (macros_json != nullptr && macros_json->is_array()) {
        for (const auto& m : *macros_json) {
            if (!m.is_object()) {
                continue;
            }
            LedMacroInfo info;
            info.display_name = helix::json_util::safe_string(m, "name");
            info.on_macro = helix::json_util::safe_string(m, "on_macro");
            info.off_macro = helix::json_util::safe_string(m, "off_macro");
            info.toggle_macro = helix::json_util::safe_string(m, "toggle_macro");

            // Parse type field (with backward compat inference)
            std::string type_str = helix::json_util::safe_string(m, "type");
            if (type_str == "on_off") {
                info.type = MacroLedType::ON_OFF;
            } else if (type_str == "toggle") {
                info.type = MacroLedType::TOGGLE;
            } else if (type_str == "preset") {
                info.type = MacroLedType::PRESET;
            } else {
                // Infer from populated fields
                if (!info.on_macro.empty() && !info.off_macro.empty()) {
                    info.type = MacroLedType::ON_OFF;
                } else if (!info.toggle_macro.empty()) {
                    info.type = MacroLedType::TOGGLE;
                } else {
                    info.type = MacroLedType::TOGGLE; // default
                }
            }

            // Parse presets (new format: macro-only) or legacy (name+macro pairs / custom_actions)
            if (m.contains("presets") && m["presets"].is_array()) {
                for (const auto& p : m["presets"]) {
                    if (p.is_object()) {
                        // Handle both old {name, macro} and new {macro} formats
                        auto macro_val = helix::json_util::safe_string(p, "macro");
                        if (!macro_val.empty()) {
                            info.presets.emplace_back(macro_val);
                        }
                    } else if (p.is_string()) {
                        // Future-proof: bare string array
                        info.presets.emplace_back(p.get<std::string>());
                    }
                }
                if (!info.presets.empty() && type_str.empty()) {
                    info.type = MacroLedType::PRESET;
                }
            } else if (m.contains("custom_actions") && m["custom_actions"].is_array()) {
                // Legacy format: custom_actions -> presets
                for (const auto& a : m["custom_actions"]) {
                    if (a.is_object()) {
                        auto macro_val = helix::json_util::safe_string(a, "macro");
                        if (!macro_val.empty()) {
                            info.presets.emplace_back(macro_val);
                        }
                    }
                }
                if (!info.presets.empty() && type_str.empty()) {
                    info.type = MacroLedType::PRESET;
                }
            }

            if (!info.display_name.empty()) {
                configured_macros_.push_back(info);
            }
        }
    }

    // The persisted list is the source of truth; mirror it into the backend now.
    // Without this, macro devices exist for the overlay (which renders chips from
    // configured_macros()) but not for MacroBackend, whose macros_ was only ever
    // filled by discover_from_hardware(). Between init() and discovery — and
    // forever on a printer where discovery never runs — every macro chip rendered
    // a button whose execute_toggle() found nothing and silently no-opped.
    rebuild_macro_backend();

    // Name macro devices the way switchable_ids() does, so the migration can
    // match them: an unprefixed macro name gains "macro:", a deleted macro drops.
    for (auto& s : legacy_selection_) {
        if (!is_macro_strip_id(s) && find_macro(configured_macros_, s) != nullptr) {
            s = MACRO_STRIP_PREFIX + s;
        }
    }
    legacy_selection_.erase(std::remove_if(legacy_selection_.begin(), legacy_selection_.end(),
                                           [this](const std::string& s) {
                                               return is_macro_strip_id(s) &&
                                                      find_macro(configured_macros_, s) == nullptr;
                                           }),
                            legacy_selection_.end());

    // LED on at start preference
    const nlohmann::json* on_at_start_json = cfg->try_get_json(cfg->df() + "leds/led_on_at_start");
    led_on_at_start_ = on_at_start_json != nullptr && on_at_start_json->is_boolean() &&
                       on_at_start_json->get<bool>();

    const nlohmann::json* startup_brightness_json =
        cfg->try_get_json(cfg->df() + "leds/startup_brightness");
    startup_brightness_ =
        (startup_brightness_json != nullptr && startup_brightness_json->is_number_integer())
            ? std::clamp(startup_brightness_json->get<int>(), 0, 100)
            : 80;

    spdlog::debug("[LedController] Loaded config: {} legacy strips, {} presets, {} macros",
                  legacy_selection_.size(), color_presets_.size(), configured_macros_.size());
}

void LedController::save_config() {
    auto* cfg = Config::get_instance();

    // Last color & brightness (saved as #RRGGBB hex strings)
    cfg->set(cfg->df() + "leds/last_color", helix::color_to_hex_string(last_color_.rgb));
    cfg->set(cfg->df() + "leds/last_brightness", last_brightness_);
    cfg->set(cfg->df() + "leds/last_white", last_color_.white);

    // Color presets (saved as #RRGGBB hex strings)
    nlohmann::json presets_arr = nlohmann::json::array();
    for (const auto& p : color_presets_) {
        presets_arr.push_back(helix::color_to_hex_string(p));
    }
    cfg->set(cfg->df() + "leds/color_presets", presets_arr);

    // Configured macros
    nlohmann::json macros_arr = nlohmann::json::array();
    for (const auto& m : configured_macros_) {
        // Never persist an in-progress draft: on reload it would come back as a
        // device with no name and no way to reach its editor.
        if (m.display_name.empty()) {
            continue;
        }
        nlohmann::json obj;
        obj["name"] = m.display_name;

        // Write type field
        switch (m.type) {
        case MacroLedType::ON_OFF:
            obj["type"] = "on_off";
            break;
        case MacroLedType::TOGGLE:
            obj["type"] = "toggle";
            break;
        case MacroLedType::PRESET:
            obj["type"] = "preset";
            break;
        }

        obj["on_macro"] = m.on_macro;
        obj["off_macro"] = m.off_macro;
        obj["toggle_macro"] = m.toggle_macro;

        nlohmann::json presets_arr_macro = nlohmann::json::array();
        for (const auto& preset_macro : m.presets) {
            presets_arr_macro.push_back({{"macro", preset_macro}});
        }
        obj["presets"] = presets_arr_macro;
        macros_arr.push_back(obj);
    }
    cfg->set(cfg->df() + "leds/macro_devices", macros_arr);

    // LED on at start preference
    cfg->set(cfg->df() + "leds/led_on_at_start", led_on_at_start_);
    cfg->set(cfg->df() + "leds/startup_brightness", startup_brightness_);

    cfg->save();
    spdlog::debug("[LedController] Saved config");
}

void LedController::send_look(const std::string& strip_id, uint32_t rgb, double w,
                              int brightness_pct, NativeBackend::SuccessCallback on_success,
                              NativeBackend::ErrorCallback on_error,
                              NativeBackend::SuccessCallback on_queued, bool silent) {
    const auto* device = find_strip(native_.strips(), strip_id);
    if (device == nullptr) {
        return;
    }
    const double scale = (brightness_pct > 0 ? brightness_pct : 100) / 100.0;
    const Look look = fit_look(rgb, w, *device);
    double r = 0.0, g = 0.0, b = 0.0;
    unpack_rgb(look.rgb, r, g, b);
    native_.set_color(strip_id, r * scale, g * scale, b * scale, look.w * scale,
                      std::move(on_success), std::move(on_error), std::move(on_queued), silent);
}

void LedController::set_power(const std::vector<std::string>& ids, bool on, bool silent) {
    if (ids.empty()) {
        spdlog::debug("[LedController] set_power({}) - no devices", on);
        return;
    }

    spdlog::info("[LedController] set_power({}) for {} device(s)", on, ids.size());

    // When turning off, stop active LED effects on these devices.  LED effects
    // continuously write their own color values to the neopixels, so a bare
    // SET_LED RED=0 will be immediately overridden if effects are still running.
    // Only stop effects that target these devices — not a blanket STOP_LED_EFFECTS
    // which would affect every other strip too (issue #329).
    if (!on && effects_.is_available()) {
        std::set<std::string> stopped;
        for (const auto& strip_id : ids) {
            for (const auto& effect : effects_.effects_for_strip(strip_id)) {
                if (stopped.insert(effect.name).second) {
                    effects_.stop_effect(effect.name, nullptr, nullptr, nullptr, silent);
                }
            }
        }
    }

    // Factory for settle callbacks: increments the in-flight counter at dispatch
    // and returns a (success, error, queued) triple that each decrement it on the
    // main thread via tok.defer() once the gcode ACK (or error, or queue
    // acceptance) lands.
    //
    // The third one is not decoration. SET_LED is discretionary, so while an
    // external blocking op (BED_MESH_CALIBRATE, QGL, a manual probe) holds
    // Klipper's gcode lock, IMoonrakerAPI queues the command fire-and-forget and
    // drops its RPC response — neither of the first two will EVER fire, and the
    // counter would stay pinned, greying both light buttons out for the whole
    // session (#1129). on_queued is the only settle signal on that path.
    //
    // NATIVE and OUTPUT_PIN emit discretionary gcode (SET_LED, SET_PIN) below and
    // pass cbs.queued. MACRO emits user-defined macros, which stay non-discretionary
    // under the default-allow rule and get a real response; WLED goes over HTTP
    // without touching the gcode lock at all. LED_EFFECT's case in this switch is a
    // no-op — it emits nothing and bumps no counter here. Effects are driven
    // separately via activate/stop, none of which hold this in-flight counter.
    struct Settle {
        NativeBackend::SuccessCallback done;
        NativeBackend::ErrorCallback fail;
        NativeBackend::SuccessCallback queued;
    };
    auto tok = lifetime_.token();
    auto make_settle = [this, tok]() {
        note_command_dispatched();
        auto settle = [this, tok]() {
            tok.defer("LedController::led_cmd_settled", [this]() { note_command_settled(); });
        };
        NativeBackend::SuccessCallback on_done = settle;
        NativeBackend::ErrorCallback on_fail = [settle](const std::string& msg) {
            spdlog::warn("[LedController] LED toggle command failed: {}", msg);
            settle();
        };
        NativeBackend::SuccessCallback on_queued = [settle]() {
            spdlog::debug("[LedController] LED toggle queued behind a blocking op — settling "
                          "without an ACK");
            settle();
        };
        return Settle{on_done, on_fail, on_queued};
    };

    bool wled_changed = false;
    for (const auto& strip_id : ids) {
        const auto backend_type = backend_for_strip(strip_id);
        if (!backend_type) {
            spdlog::debug("[LedController] set_power: '{}' is not a discovered device", strip_id);
            continue;
        }
        if (*backend_type == LedBackendType::NATIVE ||
            *backend_type == LedBackendType::OUTPUT_PIN) {
            pending_query_ids_.insert(strip_id);
        }

        switch (*backend_type) {
        case LedBackendType::NATIVE:
            if (on) {
                auto cbs = make_settle();
                send_look(strip_id, last_color_.rgb, last_color_.white, last_brightness_, cbs.done,
                          cbs.fail, cbs.queued, silent);
            } else {
                auto cbs = make_settle();
                native_.turn_off(strip_id, cbs.done, cbs.fail, cbs.queued, silent);
            }
            break;

        case LedBackendType::WLED: {
            auto cbs = make_settle();
            if (on) {
                wled_.set_on(strip_id, cbs.done, cbs.fail);
            } else {
                wled_.set_off(strip_id, cbs.done, cbs.fail);
            }
            wled_changed = true;
            break;
        }

        case LedBackendType::MACRO: {
            if (strip_macro_name(strip_id).empty()) {
                spdlog::warn("[LedController] set_power: skipping macro strip with empty name");
                break;
            }
            const auto* macro = find_macro(configured_macros_, strip_id);
            if (macro == nullptr) {
                break;
            }
            const auto last_sent = macro_last_sent_on_.find(strip_id);
            const bool already_sent =
                last_sent != macro_last_sent_on_.end() && last_sent->second == on;
            macro_last_sent_on_[strip_id] = on;
            switch (macro->type) {
            case MacroLedType::ON_OFF: {
                auto cbs = make_settle();
                if (on) {
                    macro_.execute_on(macro->display_name, cbs.done, cbs.fail);
                } else {
                    macro_.execute_off(macro->display_name, cbs.done, cbs.fail);
                }
                break;
            }
            case MacroLedType::TOGGLE: {
                // Toggling again would undo what was last sent.
                if (already_sent) {
                    break;
                }
                auto cbs = make_settle();
                macro_.execute_toggle(macro->display_name, cbs.done, cbs.fail);
                break;
            }
            case MacroLedType::PRESET: {
                // For preset type, use on/off macros if available
                if (on && !macro->on_macro.empty()) {
                    auto cbs = make_settle();
                    macro_.execute_on(macro->display_name, cbs.done, cbs.fail);
                } else if (!on && !macro->off_macro.empty()) {
                    auto cbs = make_settle();
                    macro_.execute_off(macro->display_name, cbs.done, cbs.fail);
                }
                break;
            }
            }
            break;
        }

        case LedBackendType::OUTPUT_PIN: {
            auto cbs = make_settle();
            if (on) {
                output_pin_.turn_on(strip_id, cbs.done, cbs.fail, cbs.queued, silent);
            } else {
                output_pin_.turn_off(strip_id, cbs.done, cbs.fail, cbs.queued, silent);
            }
            break;
        }

        case LedBackendType::LED_EFFECT:
            // Effects are controlled separately via activate/stop
            break;
        }
    }

    // WLED state is recorded optimistically and no status frame follows it, so
    // this is the only signal its light buttons get.
    if (wled_changed) {
        bump_state_version();
    }

    if (in_flight_count_ == 0) {
        // Nothing awaits a gcode ACK (WLED, macros, or no dispatch at all): read back now.
        // Otherwise note_command_settled() reads back once the last ACK lands.
        query_led_state();
    }
}

bool LedController::toggle_power(const std::vector<std::string>& ids) {
    std::vector<PowerState> states;
    bool last_sent_on = false;
    for (const auto& id : ids) {
        states.push_back(device_state(id).power);
        const auto it = macro_last_sent_on_.find(id);
        last_sent_on = last_sent_on || (it != macro_last_sent_on_.end() && it->second);
    }
    const bool on = next_power_on(states, last_sent_on);
    set_power(ids, on);
    return on;
}

DeviceState LedController::device_state(const std::string& id) const {
    DeviceState s;
    const auto backend = backend_for_strip(id);
    if (!backend) {
        return s;
    }
    switch (*backend) {
    case LedBackendType::NATIVE: {
        if (!native_.has_strip_color(id)) {
            return s;
        }
        uint32_t base = 0;
        int pct = 0;
        double white = 0.0;
        const auto c = native_.get_strip_color(id);
        c.decompose(base, pct, white);
        s.power = pct > 0 ? PowerState::On : PowerState::Off;
        s.brightness = pct;
        const auto* info = find_strip(native_.strips(), id);
        s.has_rgb = info != nullptr && info->supports_color;
        s.rgb = output_rgb(c.r, c.g, c.b, c.w);
        return s;
    }
    case LedBackendType::OUTPUT_PIN:
        if (!output_pin_.has_value(id)) {
            return s;
        }
        s.power = output_pin_.get_value(id) > 0.0 ? PowerState::On : PowerState::Off;
        s.brightness = output_pin_.brightness_pct(id);
        return s;
    case LedBackendType::WLED: {
        if (!wled_.has_strip_state(id)) {
            return s;
        }
        const auto w = wled_.get_strip_state(id);
        s.power = w.is_on ? PowerState::On : PowerState::Off;
        s.brightness = std::clamp(w.brightness * 100 / 255, 0, 100);
        // HelixScreen sends WLED no colors; its look is a neutral white.
        s.has_rgb = true;
        s.rgb = 0xFFFFFF;
        return s;
    }
    case LedBackendType::MACRO:
    case LedBackendType::LED_EFFECT:
        return s;
    }
    return s;
}

void LedController::update_from_status(const nlohmann::json& status) {
    const bool native = native_.update_from_status(status);
    const bool effects = effects_.update_from_status(status);
    const bool pins = output_pin_.update_from_status(status);
    if (native || effects || pins) {
        bump_state_version();
    }
}

void LedController::refresh_wled_state(std::function<void()> on_done) {
    auto tok = lifetime_.token();
    wled_.poll_status([this, tok, on_done]() {
        tok.defer("LedController::wled_state", [this, on_done]() {
            bump_state_version();
            if (on_done) {
                on_done();
            }
        });
    });
}

std::vector<LedStripInfo> LedController::all_devices() const {
    auto devices = all_selectable_strips();
    for (const auto& macro : configured_macros_) {
        if (macro.type == MacroLedType::PRESET && !macro.display_name.empty()) {
            devices.push_back(macro_device_info(macro));
        }
    }
    return devices;
}

std::vector<std::string> LedController::switchable_ids() const {
    std::vector<std::string> ids;
    for (const auto& strip : all_selectable_strips()) {
        ids.push_back(strip.id);
    }
    return ids;
}

std::string LedController::chamber_light() const {
    return resolve_chamber_light(all_selectable_strips(), first_available_strip());
}

bool chamber_light_on() {
    const auto& ctrl = LedController::instance();
    return ctrl.device_state(ctrl.chamber_light()).power == PowerState::On;
}

std::vector<std::string> LedController::light_targets(const std::string& key) const {
    return resolve_light_targets(key, switchable_ids(), chamber_light());
}

std::optional<LedBackendType> LedController::backend_for_strip(const std::string& strip_id) const {
    if (find_strip(native_.strips(), strip_id) != nullptr) {
        return LedBackendType::NATIVE;
    }

    if (find_strip(wled_.strips(), strip_id) != nullptr) {
        return LedBackendType::WLED;
    }

    // Macro devices are matched by display name, with or without "macro:" prefix
    if (find_macro(configured_macros_, strip_id) != nullptr) {
        return LedBackendType::MACRO;
    }

    if (find_strip(output_pin_.pins(), strip_id) != nullptr) {
        return LedBackendType::OUTPUT_PIN;
    }

    // If strip has "macro:" prefix but macro was deleted, still classify as MACRO
    // to prevent the stale ID from hitting NATIVE's set_led() validation
    if (is_macro_strip_id(strip_id)) {
        spdlog::debug("[LedController] Stale macro strip '{}' - no matching configured macro",
                      strip_id);
        return LedBackendType::MACRO;
    }

    return std::nullopt;
}

std::vector<LedStripInfo> LedController::all_selectable_strips() const {
    std::vector<LedStripInfo> result;

    // Native strips
    for (const auto& strip : native_.strips()) {
        result.push_back(strip);
    }

    // WLED strips
    for (const auto& strip : wled_.strips()) {
        result.push_back(strip);
    }

    // Configured macros (skip PRESET type - those aren't toggleable strips,
    // and unnamed drafts, which would render a blank chip that dispatches nothing)
    for (const auto& macro : configured_macros_) {
        if (macro.type == MacroLedType::PRESET || macro.display_name.empty())
            continue;
        result.push_back(macro_device_info(macro));
    }

    // Output pin strips
    for (const auto& p : output_pin_.pins())
        result.push_back(p);

    return result;
}

std::string LedController::first_available_strip() const {
    // First native
    if (!native_.strips().empty()) {
        return native_.strips()[0].id;
    }

    // Fall back to first WLED
    if (!wled_.strips().empty()) {
        return wled_.strips()[0].id;
    }

    // Fall back to first non-PRESET macro (an unnamed draft would yield a bare
    // "macro:" ID that backend_for_strip resolves to nothing)
    for (const auto& macro : configured_macros_) {
        if (macro.type != MacroLedType::PRESET && !macro.display_name.empty()) {
            return MACRO_STRIP_PREFIX + macro.display_name;
        }
    }

    // Fall back to first output_pin
    if (!output_pin_.pins().empty()) {
        return output_pin_.pins()[0].id;
    }

    return "";
}

void LedController::query_led_state() {
    // Moonraker subscriptions only send diffs: STOP_LED_EFFECTS may not trigger a
    // neopixel update, and SET_LED is a no-op when Klipper already had that value.
    // An explicit query guarantees the state reflects the hardware.
    if (pending_query_ids_.empty()) {
        return;
    }
    nlohmann::json query_objects = nlohmann::json::object();
    for (const auto& id : pending_query_ids_) {
        query_objects[id] = nullptr;
    }
    pending_query_ids_.clear();
    if (!client_) {
        return;
    }
    client_->send_jsonrpc(
        "printer.objects.query", {{"objects", query_objects}}, [](const nlohmann::json& response) {
            if (!response.contains("result") || !response["result"].contains("status")) {
                spdlog::warn("[LedController] query_led_state: no result/status in response");
                return;
            }
            const auto& status = response["result"]["status"];
            spdlog::debug("[LedController] query_led_state: got {}",
                          helix::json_util::safe_dump(status).substr(0, 200));
            helix::ui::queue_update("LedController::query_led_state",
                                    [status]() { get_printer_state().update_from_status(status); });
        });
}

void LedController::set_look(const std::vector<std::string>& ids, uint32_t rgb, double w,
                             int brightness_pct, bool silent) {
    // Brightness 0 is off on every backend; send_look() reads 0 as "restore at
    // 100%", which is right for power-on and wrong here.
    if (brightness_pct <= 0) {
        set_power(ids, false, silent);
        return;
    }
    for (const auto& strip_id : ids) {
        auto backend_type = backend_for_strip(strip_id);
        if (backend_type == LedBackendType::NATIVE) {
            send_look(strip_id, rgb, w, brightness_pct, nullptr, nullptr, nullptr, silent);
        } else if (backend_type == LedBackendType::OUTPUT_PIN) {
            // One channel: the look's brightness, as fit_look() gives a colorless device.
            output_pin_.set_brightness(strip_id, brightness_pct, nullptr, nullptr, nullptr, silent);
        }
    }
}

void LedController::set_brightness(const std::vector<std::string>& ids, int brightness_pct,
                                   bool silent) {
    set_look(ids, last_color_.rgb, last_color_.white, brightness_pct, silent);
}

bool LedController::get_led_on_at_start() const {
    return led_on_at_start_;
}

void LedController::set_led_on_at_start(bool enabled) {
    led_on_at_start_ = enabled;
}

int LedController::get_startup_brightness() const {
    return startup_brightness_;
}

void LedController::set_startup_brightness(int brightness_pct) {
    startup_brightness_ = std::clamp(brightness_pct, 0, 100);
    // Also update last_brightness_ so subsequent LED commands use the new value
    last_brightness_ = startup_brightness_;
}

void LedController::apply_startup_preference(const std::vector<std::string>& targets) {
    if (startup_preference_applied_) {
        spdlog::debug("[LedController] Startup preference already applied this session - skipping");
        return;
    }

    if (targets.empty()) {
        // Not our shot yet. WLED strips are discovered asynchronously, so an early
        // discovery can legitimately have nothing to act on — leave the latch clear
        // so a later discovery still gets its one chance.
        spdlog::debug("[LedController] No light targets - startup preference deferred");
        return;
    }

    // The startup opportunity for this printer session is now spent, whether or not
    // the preference is enabled. This runs from the discovery-complete handler, and
    // notify_klippy_ready re-triggers discovery unconditionally, so every
    // FIRMWARE_RESTART / M112 / klippy crash used to switch the lights back on over
    // the user's manual OFF. "At start" means at start.
    startup_preference_applied_ = true;

    if (!led_on_at_start_) {
        spdlog::debug("[LedController] LED on at start disabled - skipping");
        return;
    }

    spdlog::info("[LedController] Applying startup preference: brightness={}%, turning LEDs on",
                 startup_brightness_);
    last_brightness_ = startup_brightness_;
    // Discovery-triggered, not user-asked: a connect or reconnect during a
    // print start must not claim the busy-queue toast as though the user
    // changed the lights.
    set_power(targets, true, /*silent=*/true);
}

void LedController::publish_controllable_state() {
    if (!version_subject_initialized_) {
        return;
    }
    int desired = chamber_light().empty() ? 0 : 1;
    if (lv_subject_get_int(&led_controllable_) != desired) {
        lv_subject_set_int(&led_controllable_, desired);
        spdlog::debug("[LedController] led_controllable={}", desired);
    }
    const int any = all_devices().empty() ? 0 : 1;
    if (lv_subject_get_int(&led_has_devices_) != any) {
        lv_subject_set_int(&led_has_devices_, any);
        spdlog::debug("[LedController] led_has_devices={}", any);
    }
}

void LedController::update_in_flight_subject() {
    if (!version_subject_initialized_) {
        return;
    }
    int desired = in_flight_count_ > 0 ? 1 : 0;
    if (lv_subject_get_int(&led_command_in_flight_) != desired) {
        lv_subject_set_int(&led_command_in_flight_, desired);
        spdlog::debug("[LedController] led_command_in_flight={} ({} outstanding)", desired,
                      in_flight_count_);
    }
}

void LedController::note_command_dispatched() {
    in_flight_count_++;
    update_in_flight_subject();
}

void LedController::note_command_settled() {
    if (in_flight_count_ > 0) {
        in_flight_count_--;
    }
    update_in_flight_subject();
    if (in_flight_count_ == 0) {
        query_led_state();
    }
}

void LedController::bump_state_version() {
    if (version_subject_initialized_) {
        lv_subject_set_int(&led_state_version_, lv_subject_get_int(&led_state_version_) + 1);
    }
}

void LedController::force_clear_in_flight() {
    if (in_flight_count_ == 0) {
        return;
    }
    spdlog::info("[LedController] force-clearing {} in-flight LED command(s)", in_flight_count_);
    in_flight_count_ = 0;
    update_in_flight_subject();
}

void LedController::set_last_color(uint32_t color) {
    last_color_.rgb = color;
}

void LedController::set_last_white(double white) {
    last_color_.white = std::clamp(white, 0.0, 1.0);
}

void LedController::set_last_brightness(int brightness) {
    last_brightness_ = brightness;
}

void LedController::set_color_presets(const std::vector<uint32_t>& presets) {
    color_presets_ = presets;
}

void LedController::set_configured_macros(const std::vector<LedMacroInfo>& macros) {
    // Unnamed entries are kept. The settings editor appends a blank device so
    // the user has a row to fill in, and it indexes back into this list by
    // position, so compacting here silently discarded every "+ Add" press.
    // Everything downstream that cannot address an unnamed device -- the gcode
    // backend, the strip list, the persisted config -- filters it out instead.
    configured_macros_ = macros;
    // Keep MacroBackend in lockstep so no caller can leave the two out of sync.
    // rebuild_macro_backend() republishes led_controllable.
    rebuild_macro_backend();
}

void LedController::seed_auto_paired_macros() {
    auto* cfg = Config::get_instance();

    // Bases we have already offered. Retained after the user deletes the device,
    // so a deliberate dismissal is not undone on the next discovery.
    std::set<std::string> seeded;
    const nlohmann::json* seeded_json = cfg->try_get_json(cfg->df() + "leds/auto_paired_bases");
    if (seeded_json != nullptr && seeded_json->is_array()) {
        for (const auto& b : *seeded_json) {
            if (b.is_string()) {
                seeded.insert(b.get<std::string>());
            }
        }
    }

    const std::set<std::string> available(discovered_led_macros_.begin(),
                                          discovered_led_macros_.end());

    // Macros the user has already wired into a device are not ours to claim.
    std::set<std::string> claimed;
    std::set<std::string> used_names;
    for (const auto& m : configured_macros_) {
        claimed.insert(m.on_macro);
        claimed.insert(m.off_macro);
        claimed.insert(m.toggle_macro);
        claimed.insert(m.presets.begin(), m.presets.end());
        used_names.insert(m.display_name);
    }

    static constexpr const char* ON_SUFFIX = "_ON";
    static constexpr size_t ON_SUFFIX_LEN = 3;

    bool added = false;
    for (const auto& on_macro : discovered_led_macros_) {
        if (on_macro.size() <= ON_SUFFIX_LEN ||
            on_macro.compare(on_macro.size() - ON_SUFFIX_LEN, ON_SUFFIX_LEN, ON_SUFFIX) != 0) {
            continue;
        }
        const std::string base = on_macro.substr(0, on_macro.size() - ON_SUFFIX_LEN);
        const std::string off_macro = base + "_OFF";
        if (available.count(off_macro) == 0 || seeded.count(base) != 0 ||
            claimed.count(on_macro) != 0 || claimed.count(off_macro) != 0) {
            continue;
        }

        LedMacroInfo info;
        info.display_name = pretty_print_macro(base);
        if (info.display_name.empty()) {
            info.display_name = base;
        }
        // Do not shadow a device the user named the same thing -- find_macro keys
        // on display_name and would resolve to whichever came first.
        if (used_names.count(info.display_name) != 0) {
            seeded.insert(base);
            added = true;
            spdlog::debug("[LedController] Skipping auto-pair for '{}' - name already in use",
                          info.display_name);
            continue;
        }

        info.type = MacroLedType::ON_OFF;
        info.on_macro = on_macro;
        info.off_macro = off_macro;
        configured_macros_.push_back(info);
        used_names.insert(info.display_name);
        seeded.insert(base);
        added = true;
        spdlog::info("[LedController] Auto-paired {} / {} as LED device '{}'", on_macro, off_macro,
                     info.display_name);
    }

    if (!added) {
        return;
    }

    nlohmann::json bases = nlohmann::json::array();
    for (const auto& b : seeded) {
        bases.push_back(b);
    }
    cfg->set(cfg->df() + "leds/auto_paired_bases", bases);
    save_config();
}

void LedController::rebuild_macro_backend() {
    macro_.clear();
    for (const auto& m : configured_macros_) {
        // MacroBackend keys on display_name, so an unnamed draft has no
        // reachable identity. Skip it quietly -- it is an expected state while
        // the user is still typing, not a fault worth logging on every keystroke.
        if (m.display_name.empty()) {
            continue;
        }
        macro_.add_macro(m);
    }
    bump_config_version();
    publish_controllable_state();
}

} // namespace helix::led
