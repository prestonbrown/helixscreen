// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_controller.h"

#include "color_utils.h"
#include "config.h"
#include "moonraker_api.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <sstream>

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

} // anonymous namespace

namespace helix::led {

/// Strip "macro:" prefix from a strip ID, returning the raw macro name.
/// If the ID doesn't have the prefix, returns it unchanged.
static std::string strip_macro_name(const std::string& id) {
    const std::string prefix = "macro:";
    if (id.rfind(prefix, 0) == 0) {
        return id.substr(prefix.size());
    }
    return id;
}

// ============================================================================
// LedController
// ============================================================================

LedController& LedController::instance() {
    static LedController s_instance;
    return s_instance;
}

void LedController::init(MoonrakerAPI* api, MoonrakerClient* client) {
    alive_ = std::make_shared<std::atomic<bool>>(true);
    api_ = api;
    client_ = client;

    native_.set_api(api);
    effects_.set_api(api);
    wled_.set_api(api);
    wled_.set_client(client);
    macro_.set_api(api);
    output_pin_.set_api(api);

    // Initialize version subject for UI binding (idempotent)
    if (!version_subject_initialized_) {
        lv_subject_init_int(&led_config_version_, 0);
        version_subject_initialized_ = true;
        StaticSubjectRegistry::instance().register_deinit("LedController", [this]() {
            if (version_subject_initialized_) {
                lv_subject_deinit(&led_config_version_);
                version_subject_initialized_ = false;
            }
        });
    }

    initialized_ = true;
    load_config();
    spdlog::info("[LedController] Initialized");
}

void LedController::deinit() {
    alive_->store(false);

    native_.clear();
    effects_.clear();
    wled_.clear();
    macro_.clear();
    output_pin_.clear();

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
    selected_strips_.clear();
    last_color_ = 0xFFFFFF;
    last_brightness_ = 100;
    color_presets_.clear();
    configured_macros_.clear();
    discovered_led_macros_.clear();
    led_on_at_start_ = false;
    light_on_ = false;

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
            // Format display name from "output_pin Enclosure_LEDs" -> "Enclosure LEDs"
            std::string raw_name = led_id.substr(11);
            std::string display = raw_name;
            for (auto& ch : display) {
                if (ch == '_')
                    ch = ' ';
            }
            // Title case
            bool cap_next = true;
            for (auto& ch : display) {
                if (ch == ' ') {
                    cap_next = true;
                } else if (cap_next) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    cap_next = false;
                }
            }
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
        strip.supports_color = true;

        // Determine display name: strip prefix, replace underscores, title case
        std::string raw_name;
        if (led_id.rfind("neopixel ", 0) == 0) {
            raw_name = led_id.substr(9);
            strip.supports_white = true; // Neopixel supports RGBW
        } else if (led_id.rfind("dotstar ", 0) == 0) {
            raw_name = led_id.substr(8);
            strip.supports_white = true; // Dotstar supports RGBW
        } else if (led_id.rfind("led ", 0) == 0) {
            raw_name = led_id.substr(4);
            strip.supports_white = false; // Basic LED is RGB only
        } else {
            raw_name = led_id;
            strip.supports_white = false;
        }

        // Convert raw_name to display name: replace underscores with spaces, title case
        std::string display;
        bool capitalize_next = true;
        for (char c : raw_name) {
            if (c == '_') {
                display += ' ';
                capitalize_next = true;
            } else if (capitalize_next) {
                display += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                capitalize_next = false;
            } else {
                display += c;
            }
        }

        // Fix known abbreviations that title-case gets wrong
        auto fix_abbrev = [](std::string& s, const std::string& wrong, const std::string& right) {
            size_t pos = 0;
            while ((pos = s.find(wrong, pos)) != std::string::npos) {
                // Only replace if it's a whole word (at start/end or surrounded by spaces)
                bool at_start = (pos == 0 || s[pos - 1] == ' ');
                bool at_end = (pos + wrong.size() == s.size() || s[pos + wrong.size()] == ' ');
                if (at_start && at_end) {
                    s.replace(pos, wrong.size(), right);
                }
                pos += right.size();
            }
        };
        fix_abbrev(display, "Led", "LED");
        fix_abbrev(display, "Rgb", "RGB");
        fix_abbrev(display, "Rgbw", "RGBW");

        strip.name = display;

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

    // Populate macro backend from configured macro devices (loaded from config)
    for (const auto& macro_cfg : configured_macros_) {
        macro_.add_macro(macro_cfg);
    }

    if (macro_.is_available()) {
        spdlog::info("[LedController] Loaded {} configured macro device(s)",
                     macro_.macros().size());
    }

    // Auto-select all native strips if nothing is selected yet
    // This ensures LEDs work out-of-the-box on first run (mock or real)
    if (selected_strips_.empty() && native_.is_available()) {
        for (const auto& strip : native_.strips()) {
            selected_strips_.push_back(strip.id);
        }
        save_config();
        spdlog::info("[LedController] Auto-selected {} native strip(s) (first run)",
                     selected_strips_.size());
    }

    // Bump version to notify UI widgets to rebind
    if (version_subject_initialized_) {
        lv_subject_set_int(&led_config_version_, lv_subject_get_int(&led_config_version_) + 1);
        spdlog::debug("[LedController] LED config version bumped to {}",
                      lv_subject_get_int(&led_config_version_));
    }
}

void LedController::discover_wled_strips() {
    if (!api_) {
        spdlog::warn("[LedController] discover_wled_strips called with no API");
        return;
    }

    spdlog::debug("[LedController] Starting WLED strip discovery via Moonraker");

    std::weak_ptr<std::atomic<bool>> weak_alive = alive_;
    api_->rest().wled_get_strips(
        [this, weak_alive](const RestResponse& resp) {
            auto alive = weak_alive.lock();
            if (!alive || !alive->load())
                return;

            // Response format: {"result": {strip_name: {details...}, ...}}
            if (!resp.data.is_object()) {
                spdlog::warn("[LedController] WLED strips response is not a JSON object");
                return;
            }

            // Moonraker wraps response in "result" key
            const json& strips_data =
                resp.data.contains("result") ? resp.data["result"] : resp.data;

            int count = 0;
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
                        raw_name = it.value()["strip"].get<std::string>();
                    } else {
                        raw_name = it.key();
                    }

                    // Pretty-print: underscores to spaces, title case, fix abbreviations
                    std::string display;
                    bool cap_next = true;
                    for (char c : raw_name) {
                        if (c == '_') {
                            display += ' ';
                            cap_next = true;
                        } else if (cap_next) {
                            display +=
                                static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            cap_next = false;
                        } else {
                            display += c;
                        }
                    }
                    // Fix known abbreviations
                    auto fix_wled_abbrev = [](std::string& s, const std::string& wrong,
                                              const std::string& right) {
                        size_t pos = 0;
                        while ((pos = s.find(wrong, pos)) != std::string::npos) {
                            bool at_start = (pos == 0 || s[pos - 1] == ' ');
                            bool at_end =
                                (pos + wrong.size() == s.size() || s[pos + wrong.size()] == ' ');
                            if (at_start && at_end) {
                                s.replace(pos, wrong.size(), right);
                            }
                            pos += right.size();
                        }
                    };
                    fix_wled_abbrev(display, "Led", "LED");
                    fix_wled_abbrev(display, "Rgb", "RGB");
                    fix_wled_abbrev(display, "Rgbw", "RGBW");

                    strip.name = display;

                    wled_.add_strip(strip);
                    ++count;
                    spdlog::debug("[LedController] Discovered WLED strip: {} ({})", strip.name,
                                  strip.id);
                }
            }

            if (count > 0) {
                spdlog::info("[LedController] Discovered {} WLED strip(s)", count);

                // Fetch server config to get WLED device addresses
                this->api_->rest().get_server_config(
                    [this, weak_alive](const RestResponse& cfg_resp) {
                        auto alive = weak_alive.lock();
                        if (!alive || !alive->load())
                            return;

                        if (!cfg_resp.data.is_object())
                            return;

                        const json& config_data = cfg_resp.data.contains("result")
                                                      ? cfg_resp.data["result"]
                                                      : cfg_resp.data;

                        const json& cfg =
                            config_data.contains("config") ? config_data["config"] : config_data;

                        if (!cfg.is_object())
                            return;

                        for (auto it = cfg.begin(); it != cfg.end(); ++it) {
                            const std::string& key = it.key();
                            if (key.rfind("wled ", 0) != 0)
                                continue;

                            std::string strip_name = key.substr(5); // strip "wled " prefix
                            if (it.value().is_object() && it.value().contains("address")) {
                                std::string addr = it.value()["address"].get<std::string>();
                                wled_.set_strip_address(strip_name, addr);
                                // Attempt to fetch preset names from the WLED device
                                wled_.fetch_presets_from_device(
                                    strip_name, [this, strip_name, weak_alive]() {
                                        auto alive = weak_alive.lock();
                                        if (!alive || !alive->load())
                                            return;

                                        // If fetch didn't populate presets (mock/offline), set
                                        // defaults
                                        if (wled_.get_strip_presets(strip_name).empty()) {
                                            wled_.set_strip_presets(strip_name, {{1, "Preset 1"},
                                                                                 {2, "Preset 2"},
                                                                                 {3, "Preset 3"},
                                                                                 {4, "Preset 4"},
                                                                                 {5, "Preset 5"}});
                                            spdlog::debug(
                                                "[LedController] Set default presets for '{}'",
                                                strip_name);
                                        }
                                    });
                            }
                        }
                    },
                    [](const MoonrakerError& err) {
                        spdlog::warn(
                            "[LedController] Failed to fetch server config for WLED addresses: {}",
                            err.message);
                    });

                // Poll initial status
                wled_.poll_status();
            } else {
                spdlog::debug("[LedController] No WLED strips found");
            }
        },
        [](const MoonrakerError& err) {
            // WLED not configured is expected on most printers
            spdlog::debug("[LedController] WLED discovery unavailable: {}", err.message);
        });
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
        std::istringstream stream(leds_str);
        std::string line;
        while (std::getline(stream, line)) {
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
        if (configfile_config.contains(pin.id)) {
            const auto& pin_cfg = configfile_config[pin.id];
            if (pin_cfg.contains("pwm")) {
                const auto& pwm_val = pin_cfg["pwm"];
                bool is_pwm = false;
                if (pwm_val.is_boolean()) {
                    is_pwm = pwm_val.get<bool>();
                } else if (pwm_val.is_string()) {
                    std::string s = pwm_val.get<std::string>();
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    is_pwm = (s == "true" || s == "1" || s == "yes");
                }
                output_pin_.set_pin_pwm(pin.id, is_pwm);
                spdlog::debug("[LedController] Output pin {} PWM: {}", pin.id, is_pwm);
            }
        }
    }
}

// ============================================================================
// NativeBackend
// ============================================================================

void NativeBackend::add_strip(const LedStripInfo& strip) {
    strips_.push_back(strip);
}

void NativeBackend::clear() {
    strips_.clear();
}

void NativeBackend::set_color(const std::string& strip_id, double r, double g, double b, double w,
                              SuccessCallback on_success, ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[NativeBackend] set_color called with no API (strip={})", strip_id);
        if (on_error) {
            on_error("NativeBackend: no API available");
        }
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

    api_->set_led(strip_id, r, g, b, w, on_success, [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    });
}

void NativeBackend::set_brightness(const std::string& strip_id, int brightness_pct, double r,
                                   double g, double b, double w, SuccessCallback on_success,
                                   ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[NativeBackend] set_brightness called with no API (strip={})", strip_id);
        if (on_error) {
            on_error("NativeBackend: no API available");
        }
        return;
    }

    // Clamp brightness to valid range and apply as scalar to color
    brightness_pct = std::clamp(brightness_pct, 0, 100);
    double scale = brightness_pct / 100.0;

    spdlog::debug("[NativeBackend] set_brightness: {} {}% (scale={:.2f})", strip_id, brightness_pct,
                  scale);

    set_color(strip_id, r * scale, g * scale, b * scale, w * scale, on_success, on_error);
}

void NativeBackend::turn_on(const std::string& strip_id, SuccessCallback on_success,
                            ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[NativeBackend] turn_on called with no API (strip={})", strip_id);
        if (on_error) {
            on_error("NativeBackend: no API available");
        }
        return;
    }

    spdlog::debug("[NativeBackend] turn_on: {}", strip_id);
    api_->set_led_on(strip_id, on_success, [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    });
}

void NativeBackend::turn_off(const std::string& strip_id, SuccessCallback on_success,
                             ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[NativeBackend] turn_off called with no API (strip={})", strip_id);
        if (on_error) {
            on_error("NativeBackend: no API available");
        }
        return;
    }

    spdlog::debug("[NativeBackend] turn_off: {}", strip_id);
    api_->set_led_off(strip_id, on_success, [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    });
}

uint32_t NativeBackend::StripColor::to_rgb() const {
    uint8_t ri = static_cast<uint8_t>(std::clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t gi = static_cast<uint8_t>(std::clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t bi = static_cast<uint8_t>(std::clamp(b, 0.0, 1.0) * 255.0 + 0.5);
    return (ri << 16) | (gi << 8) | bi;
}

void NativeBackend::StripColor::decompose(uint32_t& base_color, int& brightness_pct) const {
    uint8_t ri = static_cast<uint8_t>(std::clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t gi = static_cast<uint8_t>(std::clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t bi = static_cast<uint8_t>(std::clamp(b, 0.0, 1.0) * 255.0 + 0.5);
    uint8_t max_c = std::max({ri, gi, bi});

    brightness_pct = (max_c * 100 + 127) / 255;
    if (brightness_pct < 1 && max_c > 0)
        brightness_pct = 1;

    if (max_c > 0 && max_c < 255) {
        uint8_t fr = static_cast<uint8_t>(std::min(255, ri * 255 / max_c));
        uint8_t fg = static_cast<uint8_t>(std::min(255, gi * 255 / max_c));
        uint8_t fb = static_cast<uint8_t>(std::min(255, bi * 255 / max_c));
        base_color = (fr << 16) | (fg << 8) | fb;
    } else {
        base_color = (ri << 16) | (gi << 8) | bi;
    }
}

void NativeBackend::update_from_status(const nlohmann::json& status) {
    for (const auto& strip : strips_) {
        if (!status.contains(strip.id))
            continue;

        const auto& led = status[strip.id];
        if (!led.contains("color_data") || !led["color_data"].is_array() ||
            led["color_data"].empty())
            continue;

        const auto& first = led["color_data"][0];
        if (!first.is_array() || first.size() < 3)
            continue;

        StripColor color;
        color.r = first[0].get<double>();
        color.g = first[1].get<double>();
        color.b = first[2].get<double>();
        color.w = (first.size() >= 4) ? first[3].get<double>() : 0.0;
        strip_colors_[strip.id] = color;

        if (color_change_cb_) {
            color_change_cb_(strip.id, color);
        }
    }
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

void LedEffectBackend::update_from_status(const nlohmann::json& status) {
    for (auto& effect : effects_) {
        if (!status.contains(effect.name))
            continue;

        const auto& effect_data = status[effect.name];
        if (!effect_data.is_object() || !effect_data.contains("enabled"))
            continue;

        bool new_enabled = effect_data["enabled"].get<bool>();
        if (new_enabled != effect.enabled) {
            spdlog::debug("[LedEffectBackend] Effect '{}' enabled: {} -> {}", effect.name,
                          effect.enabled, new_enabled);
            effect.enabled = new_enabled;
        }
    }
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
                                       NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[LedEffectBackend] activate_effect called with no API (effect={})",
                     effect_name);
        if (on_error) {
            on_error("No API connection available");
        }
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

    api_->execute_gcode(gcode, on_success, [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    });
}

void LedEffectBackend::stop_all_effects(NativeBackend::SuccessCallback on_success,
                                        NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[LedEffectBackend] stop_all_effects called with no API");
        if (on_error) {
            on_error("No API connection available");
        }
        return;
    }

    spdlog::debug("[LedEffectBackend] stop_all_effects: gcode: STOP_LED_EFFECTS");

    api_->execute_gcode("STOP_LED_EFFECTS", on_success, [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    });
}

std::string LedEffectBackend::icon_hint_for_effect(const std::string& effect_name) {
    // Convert to lowercase for matching
    std::string lower = effect_name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

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

    // Replace underscores with spaces and title case each word
    std::string result;
    bool capitalize_next = true;
    for (char c : raw) {
        if (c == '_') {
            result += ' ';
            capitalize_next = true;
        } else if (capitalize_next) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capitalize_next = false;
        } else {
            result += c;
        }
    }
    return result;
}

// ============================================================================
// WledBackend
// ============================================================================

const std::vector<WledPresetInfo> WledBackend::empty_presets_;

void WledBackend::add_strip(const LedStripInfo& strip) {
    strips_.push_back(strip);
}

void WledBackend::clear() {
    strips_.clear();
    strip_addresses_.clear();
    strip_presets_.clear();
    strip_states_.clear();
}

void WledBackend::set_on(const std::string& strip_name, NativeBackend::SuccessCallback on_success,
                         NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[WledBackend] set_on called with no API (strip={})", strip_name);
        if (on_error) {
            on_error("WledBackend: no API available");
        }
        return;
    }

    spdlog::debug("[WledBackend] set_on: {}", strip_name);
    // Optimistic state tracking
    strip_states_[strip_name].is_on = true;
    api_->rest().wled_set_strip(strip_name, "on", -1, -1, on_success,
                                [on_error](const MoonrakerError& err) {
                                    if (on_error) {
                                        on_error(err.message);
                                    }
                                });
}

void WledBackend::set_off(const std::string& strip_name, NativeBackend::SuccessCallback on_success,
                          NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[WledBackend] set_off called with no API (strip={})", strip_name);
        if (on_error) {
            on_error("WledBackend: no API available");
        }
        return;
    }

    spdlog::debug("[WledBackend] set_off: {}", strip_name);
    // Optimistic state tracking
    strip_states_[strip_name].is_on = false;
    api_->rest().wled_set_strip(strip_name, "off", -1, -1, on_success,
                                [on_error](const MoonrakerError& err) {
                                    if (on_error) {
                                        on_error(err.message);
                                    }
                                });
}

void WledBackend::set_brightness(const std::string& strip_name, int brightness,
                                 NativeBackend::SuccessCallback on_success,
                                 NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[WledBackend] set_brightness called with no API (strip={})", strip_name);
        if (on_error) {
            on_error("WledBackend: no API available");
        }
        return;
    }

    // Convert 0-100% to 0-255 for WLED
    brightness = std::clamp(brightness, 0, 100);
    int wled_brightness = (brightness * 255) / 100;

    spdlog::debug("[WledBackend] set_brightness: {} {}% -> WLED {}", strip_name, brightness,
                  wled_brightness);
    api_->rest().wled_set_strip(strip_name, "on", wled_brightness, -1, on_success,
                                [on_error](const MoonrakerError& err) {
                                    if (on_error) {
                                        on_error(err.message);
                                    }
                                });
}

void WledBackend::set_preset(const std::string& strip_name, int preset_id,
                             NativeBackend::SuccessCallback on_success,
                             NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[WledBackend] set_preset called with no API (strip={})", strip_name);
        if (on_error) {
            on_error("WledBackend: no API available");
        }
        return;
    }

    spdlog::debug("[WledBackend] set_preset: {} preset={}", strip_name, preset_id);
    api_->rest().wled_set_strip(strip_name, "on", -1, preset_id, on_success,
                                [on_error](const MoonrakerError& err) {
                                    if (on_error) {
                                        on_error(err.message);
                                    }
                                });
}

void WledBackend::toggle(const std::string& strip_name, NativeBackend::SuccessCallback on_success,
                         NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[WledBackend] toggle called with no API (strip={})", strip_name);
        if (on_error) {
            on_error("WledBackend: no API available");
        }
        return;
    }

    spdlog::debug("[WledBackend] toggle: {}", strip_name);
    api_->rest().wled_set_strip(strip_name, "toggle", -1, -1, on_success,
                                [on_error](const MoonrakerError& err) {
                                    if (on_error) {
                                        on_error(err.message);
                                    }
                                });
}

void WledBackend::set_strip_address(const std::string& strip_id, const std::string& address) {
    strip_addresses_[strip_id] = address;
    spdlog::debug("[WledBackend] Set address for '{}': {}", strip_id, address);
}

std::string WledBackend::get_strip_address(const std::string& strip_id) const {
    auto it = strip_addresses_.find(strip_id);
    if (it != strip_addresses_.end()) {
        return it->second;
    }
    return "";
}

void WledBackend::set_strip_presets(const std::string& strip_id,
                                    const std::vector<WledPresetInfo>& presets) {
    strip_presets_[strip_id] = presets;
    spdlog::debug("[WledBackend] Set {} preset(s) for '{}'", presets.size(), strip_id);
}

const std::vector<WledPresetInfo>&
WledBackend::get_strip_presets(const std::string& strip_id) const {
    auto it = strip_presets_.find(strip_id);
    if (it != strip_presets_.end()) {
        return it->second;
    }
    return empty_presets_;
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

void WledBackend::poll_status(std::function<void()> on_complete) {
    if (!api_) {
        spdlog::debug("[WledBackend] poll_status: no API available");
        if (on_complete)
            on_complete();
        return;
    }

    api_->rest().wled_get_strips(
        [this, on_complete](const RestResponse& resp) {
            const json& strips_data =
                resp.data.contains("result") ? resp.data["result"] : resp.data;

            if (strips_data.is_object()) {
                for (auto it = strips_data.begin(); it != strips_data.end(); ++it) {
                    WledStripState state;
                    auto& val = it.value();
                    if (val.is_object()) {
                        // Parse status field: "on"/"off" or boolean "state"
                        if (val.contains("status")) {
                            state.is_on = val["status"].get<std::string>() == "on";
                        } else if (val.contains("state")) {
                            state.is_on = val["state"].get<bool>();
                        }
                        state.brightness = val.value("brightness", 255);
                        state.active_preset = val.value("preset", -1);
                    }
                    strip_states_[it.key()] = state;
                }
            }

            if (on_complete)
                on_complete();
        },
        [on_complete](const MoonrakerError& err) {
            spdlog::warn("[WledBackend] Status poll failed: {}", err.message);
            if (on_complete)
                on_complete();
        });
}

void WledBackend::fetch_presets_from_device(const std::string& strip_id,
                                            std::function<void()> on_complete) {
    auto addr_it = strip_addresses_.find(strip_id);
    if (addr_it == strip_addresses_.end() || addr_it->second.empty()) {
        spdlog::debug("[WledBackend] No address for strip '{}' - can't fetch presets", strip_id);
        if (on_complete)
            on_complete();
        return;
    }

    std::string url = "http://" + addr_it->second + "/presets.json";
    spdlog::debug("[WledBackend] Fetching presets from {}", url);

    // Use libhv's synchronous HTTP client in an async thread via the API's REST mechanism
    // Since we can't use call_rest_get (it's bound to Moonraker's base URL), we make
    // a direct HTTP request. The API's launch_http_thread isn't accessible from here,
    // so for the initial implementation, we skip the actual HTTP call and rely on
    // mock presets being set directly via set_strip_presets().
    // Real device fetching will be implemented when a live WLED device is available for testing.
    spdlog::debug(
        "[WledBackend] Direct WLED HTTP fetch not yet implemented - use set_strip_presets()");
    if (on_complete)
        on_complete();
}

// ============================================================================
// MacroBackend
// ============================================================================

void MacroBackend::add_macro(const LedMacroInfo& macro) {
    macros_.push_back(macro);
}

void MacroBackend::clear() {
    macros_.clear();
    macro_states_.clear();
}

void MacroBackend::execute_on(const std::string& macro_name,
                              NativeBackend::SuccessCallback on_success,
                              NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[MacroBackend] execute_on called with no API (macro={})", macro_name);
        if (on_error) {
            on_error("MacroBackend: no API available");
        }
        return;
    }

    // Find macro by display_name
    for (const auto& macro : macros_) {
        if (macro.display_name == macro_name) {
            std::string gcode;
            if (!macro.on_macro.empty()) {
                gcode = macro.on_macro;
            } else if (!macro.toggle_macro.empty()) {
                gcode = macro.toggle_macro;
            } else {
                spdlog::warn("[MacroBackend] No on macro configured for '{}'", macro_name);
                if (on_error) {
                    on_error("No on macro configured for '" + macro_name + "'");
                }
                return;
            }
            spdlog::debug("[MacroBackend] execute_on: {} -> {}", macro_name, gcode);
            macro_states_[macro_name] = true;
            api_->execute_gcode(gcode, on_success, [on_error](const MoonrakerError& err) {
                if (on_error) {
                    on_error(err.message);
                }
            });
            return;
        }
    }

    spdlog::warn("[MacroBackend] Macro not found: '{}'", macro_name);
    if (on_error) {
        on_error("Macro not found: '" + macro_name + "'");
    }
}

void MacroBackend::execute_off(const std::string& macro_name,
                               NativeBackend::SuccessCallback on_success,
                               NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[MacroBackend] execute_off called with no API (macro={})", macro_name);
        if (on_error) {
            on_error("MacroBackend: no API available");
        }
        return;
    }

    // Find macro by display_name
    for (const auto& macro : macros_) {
        if (macro.display_name == macro_name) {
            std::string gcode;
            if (!macro.off_macro.empty()) {
                gcode = macro.off_macro;
            } else if (!macro.toggle_macro.empty()) {
                gcode = macro.toggle_macro;
            } else {
                spdlog::warn("[MacroBackend] No off macro configured for '{}'", macro_name);
                if (on_error) {
                    on_error("No off macro configured for '" + macro_name + "'");
                }
                return;
            }
            spdlog::debug("[MacroBackend] execute_off: {} -> {}", macro_name, gcode);
            macro_states_[macro_name] = false;
            api_->execute_gcode(gcode, on_success, [on_error](const MoonrakerError& err) {
                if (on_error) {
                    on_error(err.message);
                }
            });
            return;
        }
    }

    spdlog::warn("[MacroBackend] Macro not found: '{}'", macro_name);
    if (on_error) {
        on_error("Macro not found: '" + macro_name + "'");
    }
}

void MacroBackend::execute_toggle(const std::string& macro_name,
                                  NativeBackend::SuccessCallback on_success,
                                  NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[MacroBackend] execute_toggle called with no API (macro={})", macro_name);
        if (on_error) {
            on_error("MacroBackend: no API available");
        }
        return;
    }

    // Find macro by display_name
    for (const auto& macro : macros_) {
        if (macro.display_name == macro_name) {
            if (!macro.toggle_macro.empty()) {
                spdlog::debug("[MacroBackend] execute_toggle: {} -> {}", macro_name,
                              macro.toggle_macro);
                // Toggle macros flip state optimistically (but state is unknowable)
                auto it = macro_states_.find(macro_name);
                macro_states_[macro_name] = (it == macro_states_.end()) ? true : !it->second;
                api_->execute_gcode(macro.toggle_macro, on_success,
                                    [on_error](const MoonrakerError& err) {
                                        if (on_error) {
                                            on_error(err.message);
                                        }
                                    });
            } else {
                spdlog::warn("[MacroBackend] No toggle macro configured for '{}'", macro_name);
                if (on_error) {
                    on_error("No toggle macro configured for '" + macro_name + "'");
                }
            }
            return;
        }
    }

    spdlog::warn("[MacroBackend] Macro not found: '{}'", macro_name);
    if (on_error) {
        on_error("Macro not found: '" + macro_name + "'");
    }
}

void MacroBackend::execute_custom_action(const std::string& macro_gcode,
                                         NativeBackend::SuccessCallback on_success,
                                         NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[MacroBackend] execute_custom_action called with no API");
        if (on_error) {
            on_error("MacroBackend: no API available");
        }
        return;
    }

    spdlog::debug("[MacroBackend] execute_custom_action: {}", macro_gcode);
    api_->execute_gcode(macro_gcode, on_success, [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    });
}

bool MacroBackend::is_on(const std::string& macro_name) const {
    auto it = macro_states_.find(macro_name);
    return it != macro_states_.end() && it->second;
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
    pin_values_.clear();
}

void OutputPinBackend::set_value(const std::string& pin_id, double value,
                                 NativeBackend::SuccessCallback on_success,
                                 NativeBackend::ErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[OutputPinBackend] set_value called with no API (pin={})", pin_id);
        return;
    }

    value = std::clamp(value, 0.0, 1.0);

    // Extract pin name from "output_pin <name>" format
    std::string pin_name = pin_id;
    if (pin_name.rfind("output_pin ", 0) == 0) {
        pin_name = pin_name.substr(11);
    }

    // Use spdlog's bundled fmt for locale-independent float formatting
    std::string gcode = fmt::format("SET_PIN PIN={} VALUE={:.4f}", pin_name, value);
    api_->execute_gcode(gcode, on_success, [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    });
}

void OutputPinBackend::turn_on(const std::string& pin_id, NativeBackend::SuccessCallback on_success,
                               NativeBackend::ErrorCallback on_error) {
    set_value(pin_id, 1.0, on_success, on_error);
}

void OutputPinBackend::turn_off(const std::string& pin_id,
                                NativeBackend::SuccessCallback on_success,
                                NativeBackend::ErrorCallback on_error) {
    set_value(pin_id, 0.0, on_success, on_error);
}

void OutputPinBackend::set_brightness(const std::string& pin_id, int brightness_pct,
                                      NativeBackend::SuccessCallback on_success,
                                      NativeBackend::ErrorCallback on_error) {
    double value = std::clamp(brightness_pct, 0, 100) / 100.0;
    // Non-PWM pins only accept 0 or 1 — clamp to avoid Klipper errors
    if (!is_pwm(pin_id)) {
        value = (value > 0.0) ? 1.0 : 0.0;
    }
    set_value(pin_id, value, on_success, on_error);
}

// Called from UI thread (via UpdateQueue dispatch in printer_state.cpp)
void OutputPinBackend::update_from_status(const nlohmann::json& status) {
    for (const auto& pin : pins_) {
        if (!status.contains(pin.id))
            continue;
        const auto& pin_status = status[pin.id];
        if (pin_status.contains("value") && pin_status["value"].is_number()) {
            double value = pin_status["value"].get<double>();
            pin_values_[pin.id] = value;
            if (value_change_cb_) {
                value_change_cb_(pin.id, value);
            }
        }
    }
}

double OutputPinBackend::get_value(const std::string& pin_id) const {
    auto it = pin_values_.find(pin_id);
    return (it != pin_values_.end()) ? it->second : 0.0;
}

bool OutputPinBackend::is_on(const std::string& pin_id) const {
    return get_value(pin_id) > 0.0;
}

int OutputPinBackend::brightness_pct(const std::string& pin_id) const {
    return std::clamp(static_cast<int>(get_value(pin_id) * 100.0 + 0.5), 0, 100);
}

bool OutputPinBackend::is_pwm(const std::string& pin_id) const {
    for (const auto& p : pins_) {
        if (p.id == pin_id)
            return p.is_pwm;
    }
    return false;
}

void OutputPinBackend::set_pin_pwm(const std::string& pin_id, bool is_pwm) {
    for (auto& p : pins_) {
        if (p.id == pin_id) {
            p.is_pwm = is_pwm;
            return;
        }
    }
}

// ============================================================================
// LedController config persistence
// ============================================================================

void LedController::load_config() {
    auto* cfg = Config::get_instance();
    if (!cfg) {
        // No config available — apply defaults
        color_presets_.assign(DEFAULT_COLOR_PRESETS,
                              DEFAULT_COLOR_PRESETS + DEFAULT_COLOR_PRESETS_COUNT);
        return;
    }

    // === One-time migration from old /led/ paths ===
    auto& old_strips = cfg->get_json("/led/selected_strips");
    if (old_strips.is_array() && !old_strips.empty()) {
        auto& new_strips = cfg->get_json("/printer/leds/selected_strips");
        if (!new_strips.is_array() || new_strips.empty()) {
            spdlog::info("[LedController] Migrating config from /led/ to /printer/leds/");
            cfg->set("/printer/leds/selected_strips", old_strips);

            auto& old_color_json = cfg->get_json("/led/last_color");
            if (old_color_json.is_number()) {
                cfg->set("/printer/leds/last_color", old_color_json.get<int>());
            }
            auto& old_brightness_json = cfg->get_json("/led/last_brightness");
            if (old_brightness_json.is_number()) {
                cfg->set("/printer/leds/last_brightness", old_brightness_json.get<int>());
            }
            auto& old_presets = cfg->get_json("/led/color_presets");
            if (old_presets.is_array() && !old_presets.empty()) {
                cfg->set("/printer/leds/color_presets", old_presets);
            }
            auto& old_macros = cfg->get_json("/led/macro_devices");
            if (old_macros.is_array() && !old_macros.empty()) {
                cfg->set("/printer/leds/macro_devices", old_macros);
            }
            cfg->save();
        }
    }

    // Selected strips
    selected_strips_.clear();
    auto& strips_json = cfg->get_json("/printer/leds/selected_strips");
    if (strips_json.is_array()) {
        for (const auto& s : strips_json) {
            if (s.is_string()) {
                selected_strips_.push_back(s.get<std::string>());
            }
        }
    }

    // Legacy migration: /printer/leds/selected (JSON array from old SettingsManager)
    if (selected_strips_.empty()) {
        auto& legacy_selected = cfg->get_json("/printer/leds/selected");
        if (legacy_selected.is_array()) {
            for (const auto& s : legacy_selected) {
                if (s.is_string() && !s.get<std::string>().empty()) {
                    selected_strips_.push_back(s.get<std::string>());
                }
            }
            if (!selected_strips_.empty()) {
                spdlog::info("[LedController] Migrated {} strip(s) from /printer/leds/selected",
                             selected_strips_.size());
            }
        }
    }

    // Legacy migration: /printer/leds/strip (single string, oldest format)
    if (selected_strips_.empty()) {
        auto& legacy_strip_json = cfg->get_json("/printer/leds/strip");
        std::string legacy_strip =
            legacy_strip_json.is_string() ? legacy_strip_json.get<std::string>() : "";
        if (!legacy_strip.empty()) {
            selected_strips_.push_back(legacy_strip);
            spdlog::info("[LedController] Migrated legacy single strip: {}", legacy_strip);
        }
    }

    // Last color & brightness
    auto& color_json = cfg->get_json("/printer/leds/last_color");
    last_color_ = parse_json_color(color_json, 0xFFFFFF);
    auto& brightness_json = cfg->get_json("/printer/leds/last_brightness");
    last_brightness_ = brightness_json.is_number() ? brightness_json.get<int>() : 100;

    // Color presets
    color_presets_.clear();
    auto& presets_json = cfg->get_json("/printer/leds/color_presets");
    if (presets_json.is_array()) {
        for (const auto& p : presets_json) {
            uint32_t c = parse_json_color(p, UINT32_MAX);
            if (c != UINT32_MAX) {
                color_presets_.push_back(c);
            }
        }
    }
    if (color_presets_.empty()) {
        color_presets_.assign(DEFAULT_COLOR_PRESETS,
                              DEFAULT_COLOR_PRESETS + DEFAULT_COLOR_PRESETS_COUNT);
    }

    // Configured macros
    configured_macros_.clear();
    auto& macros_json = cfg->get_json("/printer/leds/macro_devices");
    if (macros_json.is_array()) {
        for (const auto& m : macros_json) {
            if (!m.is_object()) {
                continue;
            }
            LedMacroInfo info;
            info.display_name = m.value("name", "");
            info.on_macro = m.value("on_macro", "");
            info.off_macro = m.value("off_macro", "");
            info.toggle_macro = m.value("toggle_macro", "");

            // Parse type field (with backward compat inference)
            std::string type_str = m.value("type", "");
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
                        auto macro_val = p.value("macro", "");
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
                        auto macro_val = a.value("macro", "");
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

    // Config migration: ensure macro strips have "macro:" prefix
    bool needs_resave = false;
    for (auto& s : selected_strips_) {
        // Check if this matches a configured macro display_name but lacks the prefix
        if (s.rfind("macro:", 0) != 0) {
            for (const auto& m : configured_macros_) {
                if (m.display_name == s) {
                    spdlog::info(
                        "[LedController] Migrating unprefixed macro strip '{}' -> 'macro:{}'", s,
                        s);
                    s = "macro:" + s;
                    needs_resave = true;
                    break;
                }
            }
        }
    }
    if (needs_resave) {
        // Save just the strips (can't call save_config() since we haven't finished loading)
        auto* cfg2 = Config::get_instance();
        if (cfg2) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& s2 : selected_strips_)
                arr.push_back(s2);
            cfg2->set("/printer/leds/selected_strips", arr);
            cfg2->save();
        }
    }

    // LED on at start preference
    auto& on_at_start_json = cfg->get_json("/printer/leds/led_on_at_start");
    led_on_at_start_ = on_at_start_json.is_boolean() ? on_at_start_json.get<bool>() : false;

    spdlog::debug("[LedController] Loaded config: {} strips, {} presets, {} macros",
                  selected_strips_.size(), color_presets_.size(), configured_macros_.size());
}

void LedController::save_config() {
    auto* cfg = Config::get_instance();
    if (!cfg) {
        return;
    }

    // Selected strips
    nlohmann::json strips_arr = nlohmann::json::array();
    for (const auto& s : selected_strips_) {
        strips_arr.push_back(s);
    }
    cfg->set("/printer/leds/selected_strips", strips_arr);

    // Last color & brightness (saved as #RRGGBB hex strings)
    cfg->set("/printer/leds/last_color", helix::color_to_hex_string(last_color_));
    cfg->set("/printer/leds/last_brightness", last_brightness_);

    // Color presets (saved as #RRGGBB hex strings)
    nlohmann::json presets_arr = nlohmann::json::array();
    for (const auto& p : color_presets_) {
        presets_arr.push_back(helix::color_to_hex_string(p));
    }
    cfg->set("/printer/leds/color_presets", presets_arr);

    // Configured macros
    nlohmann::json macros_arr = nlohmann::json::array();
    for (const auto& m : configured_macros_) {
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
    cfg->set("/printer/leds/macro_devices", macros_arr);

    // LED on at start preference
    cfg->set("/printer/leds/led_on_at_start", led_on_at_start_);

    cfg->save();
    spdlog::debug("[LedController] Saved config");
}

void LedController::toggle_all(bool on) {
    if (selected_strips_.empty()) {
        spdlog::debug("[LedController] toggle_all({}) - no strips selected", on);
        return;
    }

    spdlog::info("[LedController] toggle_all({}) for {} strip(s)", on, selected_strips_.size());

    for (const auto& strip_id : selected_strips_) {
        auto backend_type = backend_for_strip(strip_id);

        switch (backend_type) {
        case LedBackendType::NATIVE:
            if (on) {
                native_.turn_on(strip_id);
            } else {
                native_.turn_off(strip_id);
            }
            break;

        case LedBackendType::WLED:
            if (on) {
                wled_.set_on(strip_id);
            } else {
                wled_.set_off(strip_id);
            }
            break;

        case LedBackendType::MACRO: {
            // Find the macro device matching this strip_id (by display name)
            std::string raw_name = strip_macro_name(strip_id);
            for (const auto& macro : configured_macros_) {
                if (macro.display_name == raw_name) {
                    switch (macro.type) {
                    case MacroLedType::ON_OFF:
                        if (on) {
                            macro_.execute_on(macro.display_name);
                        } else {
                            macro_.execute_off(macro.display_name);
                        }
                        break;
                    case MacroLedType::TOGGLE:
                        macro_.execute_toggle(macro.display_name);
                        break;
                    case MacroLedType::PRESET:
                        // For preset type, use on/off macros if available
                        if (on && !macro.on_macro.empty()) {
                            macro_.execute_on(macro.display_name);
                        } else if (!on && !macro.off_macro.empty()) {
                            macro_.execute_off(macro.display_name);
                        }
                        break;
                    }
                    break;
                }
            }
            break;
        }

        case LedBackendType::OUTPUT_PIN:
            if (on) {
                output_pin_.turn_on(strip_id);
            } else {
                output_pin_.turn_off(strip_id);
            }
            break;

        case LedBackendType::LED_EFFECT:
            // Effects are controlled separately via activate/stop
            break;
        }
    }
}

LedBackendType LedController::backend_for_strip(const std::string& strip_id) const {
    // Check native strips
    for (const auto& strip : native_.strips()) {
        if (strip.id == strip_id) {
            return LedBackendType::NATIVE;
        }
    }

    // Check WLED strips
    for (const auto& strip : wled_.strips()) {
        if (strip.id == strip_id) {
            return LedBackendType::WLED;
        }
    }

    // Check macro devices (matched by display name, with or without "macro:" prefix)
    std::string raw_name = strip_macro_name(strip_id);
    for (const auto& macro : configured_macros_) {
        if (macro.display_name == raw_name) {
            return LedBackendType::MACRO;
        }
    }

    // Check output_pin strips
    for (const auto& p : output_pin_.pins()) {
        if (p.id == strip_id)
            return LedBackendType::OUTPUT_PIN;
    }

    // Default to native (for backward compat with old configs)
    return LedBackendType::NATIVE;
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

    // Configured macros (skip PRESET type - those aren't toggleable strips)
    for (const auto& macro : configured_macros_) {
        if (macro.type == MacroLedType::PRESET)
            continue;
        LedStripInfo info;
        info.name = macro.display_name;
        info.id = "macro:" + macro.display_name;
        info.backend = LedBackendType::MACRO;
        info.supports_color = false;
        info.supports_white = false;
        result.push_back(info);
    }

    // Output pin strips
    for (const auto& p : output_pin_.pins())
        result.push_back(p);

    return result;
}

std::string LedController::first_available_strip() const {
    // Prefer selected strips
    if (!selected_strips_.empty()) {
        return selected_strips_[0];
    }

    // Fall back to first native
    if (!native_.strips().empty()) {
        return native_.strips()[0].id;
    }

    // Fall back to first WLED
    if (!wled_.strips().empty()) {
        return wled_.strips()[0].id;
    }

    // Fall back to first non-PRESET macro
    for (const auto& macro : configured_macros_) {
        if (macro.type != MacroLedType::PRESET) {
            return "macro:" + macro.display_name;
        }
    }

    // Fall back to first output_pin
    if (!output_pin_.pins().empty()) {
        return output_pin_.pins()[0].id;
    }

    return "";
}

bool LedController::light_state_trackable() const {
    for (const auto& strip_id : selected_strips_) {
        if (backend_for_strip(strip_id) == LedBackendType::MACRO) {
            std::string raw_name = strip_macro_name(strip_id);
            if (!macro_.has_known_state(raw_name)) {
                return false;
            }
        }
    }
    return true;
}

void LedController::light_toggle() {
    light_on_ = !light_on_;
    toggle_all(light_on_);
}

bool LedController::light_is_on() const {
    // Return the optimistic internal state set by light_toggle().
    // This is immediately correct after toggling. For native LEDs, the
    // Moonraker status update will subsequently confirm via PrinterState subjects.
    // For WLED/macros, we track state optimistically in their backends.
    return light_on_;
}

void LedController::sync_light_state(bool is_on) {
    if (light_on_ != is_on) {
        spdlog::debug("[LedController] Syncing light state: {} -> {}", light_on_ ? "ON" : "OFF",
                      is_on ? "ON" : "OFF");
        light_on_ = is_on;
    }
}

bool LedController::get_led_on_at_start() const {
    return led_on_at_start_;
}

void LedController::set_led_on_at_start(bool enabled) {
    led_on_at_start_ = enabled;
}

void LedController::apply_startup_preference() {
    if (!led_on_at_start_) {
        spdlog::debug("[LedController] LED on at start disabled - skipping");
        return;
    }

    if (selected_strips_.empty()) {
        spdlog::debug("[LedController] LED on at start enabled but no strips selected");
        return;
    }

    spdlog::info("[LedController] Applying startup preference: turning LEDs on");
    toggle_all(true);
}

void LedController::set_selected_strips(const std::vector<std::string>& strips) {
    selected_strips_ = strips;

    // Bump version to notify UI widgets to rebind
    if (version_subject_initialized_) {
        lv_subject_set_int(&led_config_version_, lv_subject_get_int(&led_config_version_) + 1);
    }
}

void LedController::set_last_color(uint32_t color) {
    last_color_ = color;
}

void LedController::set_last_brightness(int brightness) {
    last_brightness_ = brightness;
}

void LedController::set_color_presets(const std::vector<uint32_t>& presets) {
    color_presets_ = presets;
}

void LedController::set_configured_macros(const std::vector<LedMacroInfo>& macros) {
    configured_macros_ = macros;
}

void LedController::rebuild_macro_backend() {
    macro_.clear();
    for (const auto& m : configured_macros_) {
        macro_.add_macro(m);
    }
}

} // namespace helix::led
