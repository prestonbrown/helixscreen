// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_controller.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "color_utils.h"
#include "config.h"
#include "helix/xml/scoped_subject_registry.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "led/led_color_utils.h"
#include "led_wled_json.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "printer_discovery.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <set>
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
        lv_subject_init_int(&led_command_in_flight_, 0);
        helix::xml::register_subject_in_current_scope("led_controllable", &led_controllable_);
        helix::xml::register_subject_in_current_scope("led_command_in_flight",
                                                      &led_command_in_flight_);
        version_subject_initialized_ = true;
        StaticSubjectRegistry::instance().register_deinit("LedController", [this]() {
            if (version_subject_initialized_) {
                lv_subject_deinit(&led_config_version_);
                lv_subject_deinit(&led_controllable_);
                lv_subject_deinit(&led_command_in_flight_);
                version_subject_initialized_ = false;
            }
        });
    }

    // Observe printer connection state to clear in-flight count on disconnect.
    // Prevents toggle buttons staying greyed forever when a WebSocket drop
    // arrives while an LED command ACK is pending.
    conn_observer_ = helix::ui::observe_int_sync(
        get_printer_state().get_printer_connection_state_subject(), this,
        [](LedController* self, int state) {
            if (state != static_cast<int>(helix::ConnectionState::CONNECTED)) {
                self->force_clear_in_flight();
            }
        });

    // Second half of the same safety net, for the case the observer above cannot
    // see: printer_connection_state tracks the MOONRAKER WebSocket only. A Klipper
    // restart (M112, FIRMWARE_RESTART, a klippy crash) leaves Moonraker itself up,
    // so that subject never leaves CONNECTED and a leaked dispatch would stay
    // pinned for the rest of the session — both light buttons greyed out for hours
    // (#1129). Any exit from READY kills every RPC Klipper had in flight, so clear.
    // force_clear_in_flight() is a no-op at count 0, so a simultaneous
    // disconnect firing both observers is harmless.
    // No paired SubjectLifetime: get_klippy_state_subject() is a static
    // singleton-lifetime subject on PrinterState (no lifetime-token overload).
    klippy_observer_ = helix::ui::observe_int_sync(
        get_printer_state().get_klippy_state_subject(), this, [](LedController* self, int state) {
            if (state != static_cast<int>(helix::KlippyState::READY)) {
                self->force_clear_in_flight();
            }
        });

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
    last_color_ = LastColor{};
    last_brightness_ = 100;
    color_presets_.clear();
    configured_macros_.clear();
    discovered_led_macros_.clear();
    led_on_at_start_ = false;
    startup_brightness_ = 80;
    light_on_ = false;
    // Re-arm the startup preference. deinit() only runs from
    // Application::tear_down_printer_state() (printer switch, add-printer wizard)
    // and shutdown — a rediscovery re-runs init() alone and must NOT re-arm it.
    startup_preference_applied_ = false;

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
        // Fail-closed: default to white-only (no color). Addressable strips
        // (neopixel/dotstar) re-enable color below since their configfile sections
        // carry no red/green/blue_pin and are skipped by update_pin_config(). A
        // generic [led] stays white-only until the configfile proves RGB pins exist,
        // avoiding a meaningless color picker on white-only chamber lights.
        strip.supports_color = false;

        // Determine display name: strip prefix, replace underscores, title case
        std::string raw_name;
        if (led_id.rfind("neopixel ", 0) == 0) {
            raw_name = led_id.substr(9);
            strip.supports_color = true; // Addressable: RGB(W); no rgb pins in configfile
            strip.supports_white = true; // Neopixel supports RGBW
        } else if (led_id.rfind("dotstar ", 0) == 0) {
            raw_name = led_id.substr(8);
            strip.supports_color = true; // Addressable: RGB(W); no rgb pins in configfile
            strip.supports_white = true; // Dotstar supports RGBW
        } else if (led_id.rfind("led ", 0) == 0) {
            raw_name = led_id.substr(4);
            // Generic [led]: treat as white-only until the configfile parse proves
            // RGB pins exist (update_pin_config upgrades supports_color when red/
            // green/blue_pin are present). Leaves supports_color at the false default.
            strip.supports_white = false;
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

    // Repopulate the macro backend — the clear() above wiped what load_config()
    // installed. Same single rebuild path, so the two lists cannot drift.
    rebuild_macro_backend();

    if (macro_.is_available()) {
        spdlog::info("[LedController] Loaded {} configured macro device(s)",
                     macro_.macros().size());
    }

    // Prune selected strips that don't match any discovered hardware.
    // Presets (e.g. AD5M) may specify LED names that don't exist on community
    // firmware variants (Zmod names it "chamber_LED" vs stock "chamber_light").
    // After pruning, the auto-select below kicks in and picks the real hardware.
    // NOTE: Do NOT persist the pruned list here. If all strips get pruned
    // (e.g., firmware name mismatch, incomplete discovery), persisting the
    // empty list makes the loss permanent across restarts (#373). Let
    // auto-select below save if it picks new strips instead.
    if (!selected_strips_.empty()) {
        auto all_strips = all_selectable_strips();
        auto it = std::remove_if(
            selected_strips_.begin(), selected_strips_.end(),
            [&all_strips](const std::string& id) { return find_strip(all_strips, id) == nullptr; });
        if (it != selected_strips_.end()) {
            std::vector<std::string> pruned(it, selected_strips_.end());
            selected_strips_.erase(it, selected_strips_.end());
            for (const auto& p : pruned) {
                spdlog::info("[LedController] Pruned stale LED strip '{}' (not found in "
                             "discovered hardware)",
                             p);
            }
        }
    }

    // Auto-select all discoverable strips if nothing is selected yet.
    // Covers native (neopixel/dotstar/led), WLED, output_pin, and macro backends so
    // printers whose only LED is e.g. `[output_pin LED]` (K2 Plus, K1C) get a working
    // light toggle out-of-the-box without forcing the user into Settings.
    if (selected_strips_.empty()) {
        auto picks = all_selectable_strips();
        if (!picks.empty()) {
            for (const auto& strip : picks) {
                selected_strips_.push_back(strip.id);
            }
            save_config();
            spdlog::info("[LedController] Auto-selected {} strip(s) (first run)",
                         selected_strips_.size());
        }
    }

    // Bump version to notify UI widgets to rebind
    if (version_subject_initialized_) {
        lv_subject_set_int(&led_config_version_, lv_subject_get_int(&led_config_version_) + 1);
        spdlog::debug("[LedController] LED config version bumped to {}",
                      lv_subject_get_int(&led_config_version_));
    }
    publish_controllable_state();
}

void LedController::discover_wled_strips() {
    if (!api_) {
        spdlog::warn("[LedController] discover_wled_strips called with no API");
        return;
    }

    spdlog::debug("[LedController] Starting WLED strip discovery via Moonraker");

    auto token = lifetime_.token();
    api_->rest().wled_get_strips(
        [this, token](const RestResponse& resp) {
            // === BG THREAD: parse, validate, build local strip list ===
            // Response format: {"result": {"strips": {strip_name: {details...}, ...}}}
            if (!resp.data.is_object()) {
                spdlog::warn("[LedController] WLED strips response is not a JSON object");
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
                    spdlog::debug("[LedController] Discovered WLED strip: {} ({})", strip.name,
                                  strip.id);
                    discovered.push_back(std::move(strip));
                }
            }

            if (discovered.empty()) {
                spdlog::debug("[LedController] No WLED strips found");
                return;
            }

            // === MAIN THREAD: apply discovered strips, then chain server config fetch ===
            token.defer("LedController::wled_strips_apply", [this, token,
                                                             discovered =
                                                                 std::move(discovered)]() mutable {
                spdlog::info("[LedController] Discovered {} WLED strip(s)", discovered.size());
                for (auto& strip : discovered) {
                    wled_.add_strip(strip);
                }

                // Fetch server config to get WLED device addresses
                this->api_->rest().get_server_config(
                    [this, token](const RestResponse& cfg_resp) {
                        // === BG THREAD: parse server config, build local addr map ===
                        if (!cfg_resp.data.is_object())
                            return;

                        const json& config_data = cfg_resp.data.contains("result")
                                                      ? cfg_resp.data["result"]
                                                      : cfg_resp.data;

                        const json& cfg =
                            config_data.contains("config") ? config_data["config"] : config_data;

                        if (!cfg.is_object())
                            return;

                        std::vector<std::pair<std::string, std::string>> wled_addrs;
                        for (auto it = cfg.begin(); it != cfg.end(); ++it) {
                            const std::string& key = it.key();
                            if (key.rfind("wled ", 0) != 0)
                                continue;

                            std::string strip_name = key.substr(5); // strip "wled " prefix
                            if (it.value().is_object() && it.value().contains("address")) {
                                wled_addrs.emplace_back(std::move(strip_name),
                                                        it.value()["address"].get<std::string>());
                            }
                        }

                        if (wled_addrs.empty())
                            return;

                        // === MAIN THREAD: apply addresses, chain preset fetches ===
                        token.defer(
                            "LedController::wled_addrs_apply",
                            [this, token, wled_addrs = std::move(wled_addrs)]() mutable {
                                for (auto& wled_pair : wled_addrs) {
                                    const std::string& strip_name = wled_pair.first;
                                    const std::string& addr = wled_pair.second;
                                    wled_.set_strip_address(strip_name, addr);
                                    // Attempt to fetch preset names from WLED device
                                    wled_.fetch_presets_from_device(strip_name, [this, strip_name,
                                                                                 token]() {
                                        // === MAIN THREAD: defer presets check ===
                                        token.defer(
                                            "LedController::wled_presets_apply",
                                            [this, strip_name]() {
                                                // If fetch didn't populate
                                                // presets (mock/offline), set
                                                // defaults
                                                if (wled_.get_strip_presets(strip_name).empty()) {
                                                    wled_.set_strip_presets(strip_name,
                                                                            {{1, "Preset 1"},
                                                                             {2, "Preset 2"},
                                                                             {3, "Preset 3"},
                                                                             {4, "Preset 4"},
                                                                             {5, "Preset 5"}});
                                                    spdlog::debug("[LedController] Set "
                                                                  "default presets for "
                                                                  "'{}'",
                                                                  strip_name);
                                                }
                                            });
                                    });
                                }
                            });
                    },
                    [](const MoonrakerError& err) {
                        spdlog::warn("[LedController] Failed to fetch server config "
                                     "for WLED addresses: {}",
                                     err.message);
                    });

                // Poll initial status
                wled_.poll_status();
            });
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

void LedController::update_led_pin_config(const nlohmann::json& configfile_config) {
    if (!configfile_config.is_object()) {
        return;
    }

    native_.update_pin_config(configfile_config);
}

// ============================================================================
// NativeBackend
// ============================================================================

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

void NativeBackend::set_color(const std::string& strip_id, double r, double g, double b, double w,
                              SuccessCallback on_success, ErrorCallback on_error,
                              SuccessCallback on_queued) {
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

    // Determine if this strip is white-only based on actual pin configuration
    // (from configfile) or fall back to prefix-based detection.
    bool is_white_only = false;
    if (const auto* s = find_strip(strips_, strip_id)) {
        if (s->pin_config_known) {
            // White-only if only white_pin is defined (no RGB pins)
            is_white_only =
                s->has_white_pin && !s->has_red_pin && !s->has_green_pin && !s->has_blue_pin;
        } else {
            // Fall back: generic "led " prefix = white-only, neopixel/dotstar = RGB(W)
            is_white_only = (strip_id.rfind("led ", 0) == 0);
        }
    }

    if (is_white_only) {
        // Convert RGB to luminance for the white channel
        double luminance = 0.299 * r + 0.587 * g + 0.114 * b;
        w = std::max(luminance, w);
        r = 0.0;
        g = 0.0;
        b = 0.0;
    }

    // Cache the color we're sending
    strip_colors_[strip_id] = {r, g, b, w};

    spdlog::debug("[NativeBackend] set_color: {} r={:.2f} g={:.2f} b={:.2f} w={:.2f}", strip_id, r,
                  g, b, w);

    // Captured before the wrapper below, which is non-null on every call and
    // would otherwise read our own bookkeeping as a promise the caller never
    // made — silencing GcodeErrorRouter's `!!` copy of the same rejection.
    // See include/rpc_error_policy.h.
    const bool caller_surfaces = (on_error != nullptr);

    api_->set_led(
        strip_id, r, g, b, w, std::move(on_success),
        [on_error](const MoonrakerError& err) {
            if (on_error) {
                on_error(err.message);
            }
        },
        std::move(on_queued), caller_surfaces);
}

void NativeBackend::set_brightness(const std::string& strip_id, int brightness_pct, double r,
                                   double g, double b, double w, SuccessCallback on_success,
                                   ErrorCallback on_error, SuccessCallback on_queued) {
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

    set_color(strip_id, r * scale, g * scale, b * scale, w * scale, std::move(on_success),
              std::move(on_error), std::move(on_queued));
}

void NativeBackend::turn_on(const std::string& strip_id, SuccessCallback on_success,
                            ErrorCallback on_error, SuccessCallback on_queued) {
    if (!api_) {
        spdlog::warn("[NativeBackend] turn_on called with no API (strip={})", strip_id);
        if (on_error) {
            on_error("NativeBackend: no API available");
        }
        return;
    }

    spdlog::debug("[NativeBackend] turn_on: {}", strip_id);
    // Use cached color if available, otherwise default to full white
    StripColor cached = get_strip_color(strip_id);
    set_color(strip_id, cached.r, cached.g, cached.b, cached.w, std::move(on_success),
              std::move(on_error), std::move(on_queued));
}

void NativeBackend::turn_off(const std::string& strip_id, SuccessCallback on_success,
                             ErrorCallback on_error, SuccessCallback on_queued) {
    if (!api_) {
        spdlog::warn("[NativeBackend] turn_off called with no API (strip={})", strip_id);
        if (on_error) {
            on_error("NativeBackend: no API available");
        }
        return;
    }

    spdlog::debug("[NativeBackend] turn_off: {}", strip_id);
    // Set all channels to zero
    set_color(strip_id, 0.0, 0.0, 0.0, 0.0, std::move(on_success), std::move(on_error),
              std::move(on_queued));
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

void NativeBackend::update_from_status(const nlohmann::json& status) {
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

        // Detect RGBW capability from actual color_data size (overrides prefix guess)
        const bool has_white = (parsed.channels >= 4);
        if (strip.supports_white != has_white) {
            spdlog::info("[NativeBackend] Strip '{}' RGBW detection updated: {} -> {}", strip.id,
                         strip.supports_white, has_white);
            strip.supports_white = has_white;
        }

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
                                       NativeBackend::ErrorCallback on_error,
                                       NativeBackend::SuccessCallback on_queued,
                                       bool caller_surfaces_errors) {
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

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = caller_surfaces_errors && (on_error != nullptr);

    api_->execute_gcode(
        gcode, std::move(on_success),
        [on_error](const MoonrakerError& err) {
            if (on_error) {
                on_error(err.message);
            }
        },
        /*timeout_ms=*/0, /*silent=*/false, std::move(on_queued), caller_surfaces);
}

void LedEffectBackend::stop_all_effects(NativeBackend::SuccessCallback on_success,
                                        NativeBackend::ErrorCallback on_error,
                                        NativeBackend::SuccessCallback on_queued,
                                        bool caller_surfaces_errors) {
    if (!api_) {
        spdlog::warn("[LedEffectBackend] stop_all_effects called with no API");
        if (on_error) {
            on_error("No API connection available");
        }
        return;
    }

    spdlog::debug("[LedEffectBackend] stop_all_effects: gcode: STOP_LED_EFFECTS");

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = caller_surfaces_errors && (on_error != nullptr);

    api_->execute_gcode(
        "STOP_LED_EFFECTS", std::move(on_success),
        [on_error](const MoonrakerError& err) {
            if (on_error) {
                on_error(err.message);
            }
        },
        /*timeout_ms=*/0, /*silent=*/false, std::move(on_queued), caller_surfaces);
}

void LedEffectBackend::stop_effect(const std::string& effect_name,
                                   NativeBackend::SuccessCallback on_success,
                                   NativeBackend::ErrorCallback on_error,
                                   NativeBackend::SuccessCallback on_queued) {
    if (!api_) {
        spdlog::warn("[LedEffectBackend] stop_effect called with no API (effect={})", effect_name);
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

    std::string gcode = "SET_LED_EFFECT EFFECT=" + bare_name + " STOP=1";
    spdlog::debug("[LedEffectBackend] stop_effect: {} -> gcode: {}", effect_name, gcode);

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = (on_error != nullptr);

    api_->execute_gcode(
        gcode, std::move(on_success),
        [on_error](const MoonrakerError& err) {
            if (on_error) {
                on_error(err.message);
            }
        },
        /*timeout_ms=*/0, /*silent=*/false, std::move(on_queued), caller_surfaces);
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
            const json& strips_data = detail::wled_strip_map(resp.data);

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
    if (macro.display_name.empty()) {
        spdlog::warn("[MacroBackend] Rejecting macro with empty display name");
        return;
    }
    macros_.push_back(macro);
}

void MacroBackend::clear() {
    macros_.clear();
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
            // Captured before the wrapper below, which is non-null on every call
            // and would otherwise claim the report on the caller's behalf. See
            // include/rpc_error_policy.h.
            const bool caller_surfaces = (on_error != nullptr);
            api_->execute_gcode(
                gcode, on_success,
                [on_error](const MoonrakerError& err) {
                    if (on_error) {
                        on_error(err.message);
                    }
                },
                /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr, caller_surfaces);
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
            const bool caller_surfaces = (on_error != nullptr);
            api_->execute_gcode(
                gcode, on_success,
                [on_error](const MoonrakerError& err) {
                    if (on_error) {
                        on_error(err.message);
                    }
                },
                /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr, caller_surfaces);
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
                const bool caller_surfaces = (on_error != nullptr);
                api_->execute_gcode(
                    macro.toggle_macro, on_success,
                    [on_error](const MoonrakerError& err) {
                        if (on_error) {
                            on_error(err.message);
                        }
                    },
                    /*timeout_ms=*/0, /*silent=*/false, /*on_queued=*/nullptr, caller_surfaces);
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
    const bool caller_surfaces = (on_error != nullptr);
    api_->execute_gcode(
        macro_gcode, on_success,
        [on_error](const MoonrakerError& err) {
            if (on_error) {
                on_error(err.message);
            }
        },
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
    pin_values_.clear();
}

void OutputPinBackend::set_value(const std::string& pin_id, double value,
                                 NativeBackend::SuccessCallback on_success,
                                 NativeBackend::ErrorCallback on_error,
                                 NativeBackend::SuccessCallback on_queued) {
    if (!api_) {
        spdlog::warn("[OutputPinBackend] set_value called with no API (pin={})", pin_id);
        if (on_error) {
            on_error("OutputPinBackend: no API available");
        }
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

    // Captured before the wrapper below, which is non-null on every call.
    const bool caller_surfaces = (on_error != nullptr);

    api_->execute_gcode(
        gcode, std::move(on_success),
        [on_error](const MoonrakerError& err) {
            if (on_error) {
                on_error(err.message);
            }
        },
        /*timeout_ms=*/0, /*silent=*/false, std::move(on_queued), caller_surfaces);
}

void OutputPinBackend::turn_on(const std::string& pin_id, NativeBackend::SuccessCallback on_success,
                               NativeBackend::ErrorCallback on_error,
                               NativeBackend::SuccessCallback on_queued) {
    set_value(pin_id, 1.0, std::move(on_success), std::move(on_error), std::move(on_queued));
}

void OutputPinBackend::turn_off(const std::string& pin_id,
                                NativeBackend::SuccessCallback on_success,
                                NativeBackend::ErrorCallback on_error,
                                NativeBackend::SuccessCallback on_queued) {
    set_value(pin_id, 0.0, std::move(on_success), std::move(on_error), std::move(on_queued));
}

void OutputPinBackend::set_brightness(const std::string& pin_id, int brightness_pct,
                                      NativeBackend::SuccessCallback on_success,
                                      NativeBackend::ErrorCallback on_error,
                                      NativeBackend::SuccessCallback on_queued) {
    double value = std::clamp(brightness_pct, 0, 100) / 100.0;
    // Non-PWM pins only accept 0 or 1 — clamp to avoid Klipper errors
    if (!is_pwm(pin_id)) {
        value = (value > 0.0) ? 1.0 : 0.0;
    }
    set_value(pin_id, value, std::move(on_success), std::move(on_error), std::move(on_queued));
}

// Called from UI thread (via UpdateQueue dispatch in printer_state.cpp)
void OutputPinBackend::update_from_status(const nlohmann::json& status) {
    for (const auto& pin : pins_) {
        if (!status.contains(pin.id))
            continue;
        const auto& pin_status = status[pin.id];
        if (pin_status.contains("value") && pin_status["value"].is_number()) {
            pin_values_[pin.id] = pin_status["value"].get<double>();
        }
    }
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
    if (!cfg) {
        // No config available — apply defaults
        color_presets_.assign(DEFAULT_COLOR_PRESETS,
                              DEFAULT_COLOR_PRESETS + DEFAULT_COLOR_PRESETS_COUNT);
        return;
    }

    // NOTE: the one-time fold of the legacy top-level /led block into the
    // active printer's leds/ section lives in migrate_v19_to_v20() (config.cpp).
    // It used to run here on every load_config(), and its get_json() probes
    // re-created the /led orphan on every boot (#1129).

    // Selected strips
    selected_strips_ = cfg->get_string_array(cfg->df() + "leds/selected_strips");

    // Legacy migration: leds/selected (JSON array from old SettingsManager)
    if (selected_strips_.empty()) {
        for (auto& s : cfg->get_string_array(cfg->df() + "leds/selected")) {
            if (!s.empty()) {
                selected_strips_.push_back(std::move(s));
            }
        }
        if (!selected_strips_.empty()) {
            spdlog::info("[LedController] Migrated {} strip(s) from leds/selected",
                         selected_strips_.size());
        }
    }

    // Legacy migration: leds/strip (single string, oldest format)
    if (selected_strips_.empty()) {
        const nlohmann::json* legacy_strip_json = cfg->try_get_json(cfg->df() + "leds/strip");
        std::string legacy_strip = (legacy_strip_json != nullptr && legacy_strip_json->is_string())
                                       ? legacy_strip_json->get<std::string>()
                                       : "";
        if (!legacy_strip.empty()) {
            selected_strips_.push_back(legacy_strip);
            spdlog::info("[LedController] Migrated legacy single strip: {}", legacy_strip);
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
    if (color_presets_.empty()) {
        color_presets_.assign(DEFAULT_COLOR_PRESETS,
                              DEFAULT_COLOR_PRESETS + DEFAULT_COLOR_PRESETS_COUNT);
    }

    // Configured macros
    configured_macros_.clear();
    const nlohmann::json* macros_json = cfg->try_get_json(cfg->df() + "leds/macro_devices");
    if (macros_json != nullptr && macros_json->is_array()) {
        for (const auto& m : *macros_json) {
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

    // The persisted list is the source of truth; mirror it into the backend now.
    // Without this, macro devices exist for the overlay (which renders chips from
    // configured_macros()) but not for MacroBackend, whose macros_ was only ever
    // filled by discover_from_hardware(). Between init() and discovery — and
    // forever on a printer where discovery never runs — every macro chip rendered
    // a button whose execute_toggle() found nothing and silently no-opped.
    rebuild_macro_backend();

    // Config migration: ensure macro strips have "macro:" prefix
    bool needs_resave = false;
    for (auto& s : selected_strips_) {
        // Check if this matches a configured macro display_name but lacks the prefix
        if (!is_macro_strip_id(s) && find_macro(configured_macros_, s) != nullptr) {
            spdlog::info("[LedController] Migrating unprefixed macro strip '{}' -> 'macro:{}'", s,
                         s);
            s = MACRO_STRIP_PREFIX + s;
            needs_resave = true;
        }
    }

    // Clean up stale macro strip entries that no longer have matching macros
    auto before_size = selected_strips_.size();
    selected_strips_.erase(
        std::remove_if(selected_strips_.begin(), selected_strips_.end(),
                       [this](const std::string& s) {
                           if (!is_macro_strip_id(s))
                               return false;
                           if (find_macro(configured_macros_, s) != nullptr)
                               return false;
                           spdlog::info("[LedController] Removing stale macro strip '{}'", s);
                           return true;
                       }),
        selected_strips_.end());
    if (selected_strips_.size() != before_size) {
        needs_resave = true;
    }

    if (needs_resave) {
        // Save just the strips (can't call save_config() since we haven't finished loading)
        auto* cfg2 = Config::get_instance();
        if (cfg2) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& s2 : selected_strips_)
                arr.push_back(s2);
            cfg2->set(cfg2->df() + "leds/selected_strips", arr);
            cfg2->save();
        }
    }

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
    cfg->set(cfg->df() + "leds/selected_strips", strips_arr);

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

LedController::ScaledColor LedController::compute_scaled_last_color(int brightness_pct) const {
    const bool has_color = (last_color_.rgb != 0 || last_color_.white != 0.0);
    if (!has_color) {
        // Safety net: saved state would produce no visible light at all.
        // Fall back to full white so "turn on" always produces light. Honor
        // the requested brightness — a slider drag to 75% should still land at
        // 75% white, not 100%. If brightness is zero here too, default to 100%.
        const int effective = brightness_pct > 0 ? brightness_pct : 100;
        const double scale = effective / 100.0;
        spdlog::info("[LedController] compute_scaled_last_color: no saved color, using full white "
                     "at {}%",
                     effective);
        return {scale, scale, scale, 0.0};
    }
    // Preserve saved color; if brightness is zero but we have color, treat as 100%
    // so toggling on from a dimmed-to-zero state lights at full intensity rather
    // than silently falling back to white.
    const int effective = brightness_pct > 0 ? brightness_pct : 100;
    const double scale = effective / 100.0;
    double r = 0.0, g = 0.0, b = 0.0;
    unpack_rgb(last_color_.rgb, r, g, b);
    return {r * scale, g * scale, b * scale, last_color_.white * scale};
}

void LedController::toggle_all(bool on) {
    if (selected_strips_.empty()) {
        spdlog::debug("[LedController] toggle_all({}) - no strips selected", on);
        return;
    }

    spdlog::info("[LedController] toggle_all({}) for {} strip(s)", on, selected_strips_.size());

    // When turning off, stop active LED effects on selected strips.  LED effects
    // continuously write their own color values to the neopixels, so a bare
    // SET_LED RED=0 will be immediately overridden if effects are still running.
    // Only stop effects that target selected strips — not a blanket STOP_LED_EFFECTS
    // which would affect deselected strips too (issue #329).
    if (!on && effects_.is_available()) {
        std::set<std::string> stopped;
        for (const auto& strip_id : selected_strips_) {
            for (const auto& effect : effects_.effects_for_strip(strip_id)) {
                if (stopped.insert(effect.name).second) {
                    effects_.stop_effect(effect.name);
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

    for (const auto& strip_id : selected_strips_) {
        auto backend_type = backend_for_strip(strip_id);

        switch (backend_type) {
        case LedBackendType::NATIVE:
            if (on) {
                // Shared helper handles the no-saved-color safety floor and the
                // brightness==0-but-color-nonzero restore-at-100% semantics.
                auto c = compute_scaled_last_color(last_brightness_);
                auto cbs = make_settle();
                native_.set_color(strip_id, c.r, c.g, c.b, c.w, cbs.done, cbs.fail, cbs.queued);
            } else {
                auto cbs = make_settle();
                native_.turn_off(strip_id, cbs.done, cbs.fail, cbs.queued);
            }
            break;

        case LedBackendType::WLED: {
            auto cbs = make_settle();
            if (on) {
                wled_.set_on(strip_id, cbs.done, cbs.fail);
            } else {
                wled_.set_off(strip_id, cbs.done, cbs.fail);
            }
            break;
        }

        case LedBackendType::MACRO: {
            if (strip_macro_name(strip_id).empty()) {
                spdlog::warn("[LedController] toggle_all: skipping macro strip with empty name");
                break;
            }
            const auto* macro = find_macro(configured_macros_, strip_id);
            if (macro == nullptr) {
                break;
            }
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
                output_pin_.turn_on(strip_id, cbs.done, cbs.fail, cbs.queued);
            } else {
                output_pin_.turn_off(strip_id, cbs.done, cbs.fail, cbs.queued);
            }
            break;
        }

        case LedBackendType::LED_EFFECT:
            // Effects are controlled separately via activate/stop
            break;
        }
    }
}

LedBackendType LedController::backend_for_strip(const std::string& strip_id) const {
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
        info.id = MACRO_STRIP_PREFIX + macro.display_name;
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
            return MACRO_STRIP_PREFIX + macro.display_name;
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

void LedController::light_set(bool on) {
    spdlog::info("[LedController] light_set({})", on);
    light_on_ = on;
    toggle_all(on);
    if (in_flight_count_ == 0) {
        // No async gcode dispatched (instant/REST backend) — refresh state now.
        query_tracked_led_state();
    }
    // Otherwise query_tracked_led_state() fires from note_command_settled()
    // once every dispatched command's ACK has landed.
}

void LedController::query_tracked_led_state() {
    if (!client_ || selected_strips_.empty()) {
        return;
    }

    // After toggling LEDs, explicitly query the tracked LED's state.
    // Moonraker subscriptions only send diffs.  STOP_LED_EFFECTS may not
    // trigger a neopixel status update, and SET_LED is a no-op if Klipper
    // already had that value.  An explicit query guarantees the subject
    // reflects the actual hardware state.
    std::string tracked = selected_strips_.front();
    nlohmann::json query_objects = nlohmann::json::object();
    query_objects[tracked] = nullptr;
    client_->send_jsonrpc(
        "printer.objects.query", {{"objects", query_objects}}, [tracked](nlohmann::json response) {
            if (!response.contains("result") || !response["result"].contains("status")) {
                spdlog::warn(
                    "[LedController] query_tracked_led_state: no result/status in response");
                return;
            }
            const auto& status = response["result"]["status"];
            if (!status.contains(tracked)) {
                spdlog::warn(
                    "[LedController] query_tracked_led_state: '{}' not in response (keys: {})",
                    tracked, nlohmann::json(status).dump().substr(0, 200));
                return;
            }
            spdlog::debug("[LedController] query_tracked_led_state: got {} = {}", tracked,
                          status[tracked].dump().substr(0, 200));
            helix::ui::queue_update([status]() { get_printer_state().update_from_status(status); });
        });
}

void LedController::turn_off_all() {
    light_set(false);
}

void LedController::set_color_all(double r, double g, double b, double w) {
    light_on_ = (r > 0.0 || g > 0.0 || b > 0.0 || w > 0.0);
    // Cache the white channel for toggle restore
    last_color_.white = w;
    for (const auto& strip_id : selected_strips_) {
        auto backend_type = backend_for_strip(strip_id);
        if (backend_type == LedBackendType::NATIVE) {
            native_.set_color(strip_id, r, g, b, w);
        } else if (backend_type == LedBackendType::OUTPUT_PIN) {
            // Use luminance approximation for single-channel pins
            double lum = 0.299 * r + 0.587 * g + 0.114 * b;
            output_pin_.set_value(strip_id, lum);
        }
    }
}

void LedController::set_brightness_all(int brightness_pct) {
    light_on_ = (brightness_pct > 0);
    // Use shared helper so slider drags honor the same safety floor and
    // RGBW white-channel preservation as toggle_all(true).
    auto c = compute_scaled_last_color(brightness_pct);
    for (const auto& strip_id : selected_strips_) {
        auto backend_type = backend_for_strip(strip_id);
        if (backend_type == LedBackendType::NATIVE) {
            native_.set_color(strip_id, c.r, c.g, c.b, c.w);
        } else if (backend_type == LedBackendType::OUTPUT_PIN) {
            output_pin_.set_brightness(strip_id, brightness_pct);
        }
    }
}

void LedController::light_toggle() {
    light_set(!light_on_);
}

bool LedController::light_is_on() const {
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

int LedController::get_startup_brightness() const {
    return startup_brightness_;
}

void LedController::set_startup_brightness(int brightness_pct) {
    startup_brightness_ = std::clamp(brightness_pct, 0, 100);
    // Also update last_brightness_ so subsequent LED commands use the new value
    last_brightness_ = startup_brightness_;
}

void LedController::apply_startup_preference() {
    if (startup_preference_applied_) {
        spdlog::debug("[LedController] Startup preference already applied this session - skipping");
        return;
    }

    if (selected_strips_.empty()) {
        // Not our shot yet. WLED strips are discovered asynchronously, so an early
        // discovery can legitimately have nothing to act on — leave the latch clear
        // so a later discovery still gets its one chance.
        spdlog::debug("[LedController] No strips selected - startup preference deferred");
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
    light_set(true);
}

void LedController::set_selected_strips(const std::vector<std::string>& strips) {
    selected_strips_ = strips;

    // Bump version to notify UI widgets to rebind
    if (version_subject_initialized_) {
        lv_subject_set_int(&led_config_version_, lv_subject_get_int(&led_config_version_) + 1);
    }
    publish_controllable_state();
}

void LedController::publish_controllable_state() {
    if (!version_subject_initialized_) {
        return;
    }
    int desired = selected_strips_.empty() ? 0 : 1;
    if (lv_subject_get_int(&led_controllable_) != desired) {
        lv_subject_set_int(&led_controllable_, desired);
        spdlog::debug("[LedController] led_controllable={} ({} strip(s) selected)", desired,
                      selected_strips_.size());
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
        query_tracked_led_state();
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
    configured_macros_.clear();
    configured_macros_.reserve(macros.size());
    for (const auto& m : macros) {
        if (!m.display_name.empty()) {
            configured_macros_.push_back(m);
        } else {
            spdlog::warn("[LedController] Skipping macro with empty display name");
        }
    }
    // Keep MacroBackend in lockstep so no caller can leave the two out of sync.
    rebuild_macro_backend();
}

void LedController::rebuild_macro_backend() {
    macro_.clear();
    for (const auto& m : configured_macros_) {
        macro_.add_macro(m);
    }
}

} // namespace helix::led
