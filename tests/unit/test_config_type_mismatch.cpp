// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_config_type_mismatch.cpp
 * @brief Config must survive a settings.json whose values have the wrong type.
 *
 * settings.json is user-editable, hand-edited in support threads, and can be
 * left half-written by a power cut. A value of the right name but the wrong
 * JSON type ("true" instead of true) made nlohmann's get<T>() throw
 * type_error.302 straight out of Config::get(json_ptr, default). Nothing in the
 * app catches that: it surfaced inside manager init_subjects() calls, partway
 * through registering subjects, so one mistyped key crashed startup instead of
 * costing the user one setting.
 *
 * The contract these cases pin down:
 *   - a wrongly typed value reads as the caller's default, and says so in the log
 *   - a correctly typed value is untouched, including false / 0 / ""
 *   - writing the right type over a wrong one repairs the node
 *   - a scalar sitting where a parent object belongs cannot make set() throw
 */

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "input_settings_manager.h"
#include "input_settings_test_helpers.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Owns a private Config (not the singleton) whose document the case seeds
/// directly, so nothing here depends on a file on disk or on what another test
/// left in the singleton.
class ConfigTypeFixture : public HelixTestFixture {
  protected:
    Config config;

    json& data() {
        return ConfigTestAccess::data(config);
    }

    void seed(const json& doc) {
        ConfigTestAccess::data(config) = doc;
    }
};

/// RAII spdlog capture, so "the user is told which key was ignored" is an
/// assertion rather than an intention.
class LogCapture {
  public:
    LogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)) {
        logger_ = spdlog::default_logger();
        prev_level_ = logger_->level();
        sink_->set_level(spdlog::level::trace);
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);
    }

    ~LogCapture() {
        auto& sinks = logger_->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == sink_) {
                sinks.erase(it);
                break;
            }
        }
        logger_->set_level(prev_level_);
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    /// True when some captured line contains every one of @p needles.
    bool has_line_with(const std::vector<std::string>& needles) const {
        for (const auto& line : sink_->last_formatted(256)) {
            bool all = true;
            for (const auto& needle : needles) {
                if (line.find(needle) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
        }
        return false;
    }

  private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> logger_;
    spdlog::level::level_enum prev_level_;
};

} // namespace

// ============================================================================
// Wrongly typed values fall back to the default
// ============================================================================

TEST_CASE_METHOD(ConfigTypeFixture, "Config: string where a bool belongs reads as the default",
                 "[core][config][get][type_mismatch]") {
    // The exact shape from a hand-edited settings.json: quoted "true".
    seed({{"input", {{"home_edit_mode_enabled", "true"}}}});

    // Both defaults are exercised: a fallback that happened to agree with the
    // string would still be wrong.
    REQUIRE(config.get<bool>("/input/home_edit_mode_enabled", true) == true);
    REQUIRE(config.get<bool>("/input/home_edit_mode_enabled", false) == false);
}

TEST_CASE_METHOD(ConfigTypeFixture, "Config: string where an int belongs reads as the default",
                 "[core][config][get][type_mismatch]") {
    seed({{"input", {{"jitter_threshold", "5"}}}});

    // "5" is a parseable number as text; it must still be rejected rather than
    // coerced, because nothing else in the app would tolerate the ambiguity.
    REQUIRE(config.get<int>("/input/jitter_threshold", 9) == 9);
}

TEST_CASE_METHOD(ConfigTypeFixture, "Config: object where a scalar belongs reads as the default",
                 "[core][config][get][type_mismatch]") {
    seed({{"input", {{"scroll_throw", {{"value", 12}}}}}});

    REQUIRE(config.get<int>("/input/scroll_throw", 30) == 30);
    REQUIRE(config.get<std::string>("/input/scroll_throw", "fallback") == "fallback");
    REQUIRE(config.get<bool>("/input/scroll_throw", true) == true);
}

TEST_CASE_METHOD(ConfigTypeFixture, "Config: array where a scalar belongs reads as the default",
                 "[core][config][get][type_mismatch]") {
    seed({{"display", {{"rotate", json::array({90, 180})}}}});

    REQUIRE(config.get<int>("/display/rotate", 0) == 0);
    REQUIRE(config.get<std::string>("/display/rotate", "none") == "none");
}

TEST_CASE_METHOD(ConfigTypeFixture, "Config: number where a string belongs reads as the default",
                 "[core][config][get][type_mismatch]") {
    seed({{"printer", {{"moonraker_host", 192168}}}});

    REQUIRE(config.get<std::string>("/printer/moonraker_host", "127.0.0.1") == "127.0.0.1");
}

TEST_CASE_METHOD(ConfigTypeFixture, "Config: wrongly typed container reads as the default",
                 "[core][config][get][type_mismatch]") {
    // PluginManager reads /plugins/enabled as a vector<string>; a scalar there,
    // or an array of the wrong element type, are both unconvertible.
    const std::vector<std::string> fallback{"builtin"};

    seed({{"plugins", {{"enabled", "all"}}}});
    REQUIRE(config.get<std::vector<std::string>>("/plugins/enabled", fallback) == fallback);

    seed({{"plugins", {{"enabled", json::array({1, 2})}}}});
    REQUIRE(config.get<std::vector<std::string>>("/plugins/enabled", fallback) == fallback);
}

// ============================================================================
// The fallback must not be over-broad
// ============================================================================

TEST_CASE_METHOD(ConfigTypeFixture, "Config: correctly typed values are returned unchanged",
                 "[core][config][get][type_mismatch]") {
    seed({{"input",
           {{"home_edit_mode_enabled", false},
            {"jitter_threshold", 0},
            {"touch_device", ""},
            {"scroll_throw", 12}}},
          {"printer", {{"moonraker_host", "192.168.1.100"}}},
          {"plugins", {{"enabled", json::array({"a", "b"})}}}});

    // false, 0 and "" are the values a fallback-happy implementation is most
    // likely to swallow, so each is read back against an opposite default.
    REQUIRE(config.get<bool>("/input/home_edit_mode_enabled", true) == false);
    REQUIRE(config.get<int>("/input/jitter_threshold", 42) == 0);
    REQUIRE(config.get<std::string>("/input/touch_device", "/dev/input/event0").empty());
    REQUIRE(config.get<int>("/input/scroll_throw", 30) == 12);
    REQUIRE(config.get<std::string>("/printer/moonraker_host", "127.0.0.1") == "192.168.1.100");
    REQUIRE(config.get<std::vector<std::string>>("/plugins/enabled", {}) ==
            std::vector<std::string>{"a", "b"});
}

TEST_CASE_METHOD(ConfigTypeFixture, "Config: missing and null keys still read as the default",
                 "[core][config][get][type_mismatch]") {
    seed({{"input", {{"scroll_guard", nullptr}}}});

    REQUIRE(config.get<bool>("/input/scroll_guard", true) == true);
    REQUIRE(config.get<int>("/input/never_written", 7) == 7);
    REQUIRE(config.get<int>("/nothing/here/at/all", 7) == 7);
    // Probing must not have vivified anything along the way (#1129).
    REQUIRE_FALSE(config.exists("/input/never_written"));
    REQUIRE_FALSE(config.exists("/nothing"));
}

// ============================================================================
// The user is told, and can repair the value
// ============================================================================

TEST_CASE_METHOD(ConfigTypeFixture, "Config: a rejected value is logged with pointer and type",
                 "[core][config][get][type_mismatch]") {
    seed({{"input", {{"home_edit_mode_enabled", "true"}}}});

    LogCapture log;
    (void)config.get<bool>("/input/home_edit_mode_enabled", true);

    // A user reading the log needs the key, what is stored, and what was wanted.
    REQUIRE(log.has_line_with({"/input/home_edit_mode_enabled", "string", "boolean"}));
}

TEST_CASE_METHOD(ConfigTypeFixture, "Config: writing the right type over a wrong one repairs it",
                 "[core][config][get][set][type_mismatch]") {
    seed({{"input", {{"home_edit_mode_enabled", "true"}}}});
    REQUIRE(config.get<bool>("/input/home_edit_mode_enabled", true) == true); // default

    // This is what happens when the user toggles the setting in the UI.
    config.set<bool>("/input/home_edit_mode_enabled", false);

    REQUIRE(data()["input"]["home_edit_mode_enabled"].is_boolean());
    REQUIRE(config.get<bool>("/input/home_edit_mode_enabled", true) == false);
}

// ============================================================================
// set() through a corrupted parent
// ============================================================================

TEST_CASE_METHOD(ConfigTypeFixture, "Config: set() through a scalar parent fails without throwing",
                 "[core][config][set][type_mismatch]") {
    // /input is a string, so no child path can be created under it. nlohmann
    // reports out_of_range.404 from operator[]; the setting is dropped rather
    // than taking down whichever panel the user was touching.
    seed({{"input", "oops"}});

    REQUIRE_NOTHROW(config.set<bool>("/input/home_edit_mode_enabled", true));
    REQUIRE(config.set<int>("/input/jitter_threshold", 5) == 5);

    // The failed write must not have mangled what was there.
    REQUIRE(data()["input"].is_string());
    REQUIRE(data()["input"].get<std::string>() == "oops");

    // And reading back through the same broken parent is quiet too.
    REQUIRE(config.get<bool>("/input/home_edit_mode_enabled", true) == true);
    REQUIRE_FALSE(config.exists("/input/home_edit_mode_enabled"));
}

// ============================================================================
// End-to-end: the crash path this defect actually took
// ============================================================================

namespace {

/// InputSettingsManager reads every one of its values through
/// Config::get(ptr, default) inside init_subjects(). Mirrors the fixture in
/// test_input_settings_manager.cpp: leave the manager initialized on the way
/// out, or later tests that build the Touch & Input overlay lose their subjects.
class MistypedInputSettingsFixture : public LVGLTestFixture {
  public:
    MistypedInputSettingsFixture() {
        helix_test::reset_input_settings_to_defaults();
    }
    ~MistypedInputSettingsFixture() override {
        helix_test::reset_input_settings_to_defaults();
    }
};

} // namespace

TEST_CASE_METHOD(MistypedInputSettingsFixture,
                 "InputSettingsManager: a mistyped /input node loads defaults, not a crash",
                 "[core][config][input_settings][type_mismatch]") {
    json& data = ConfigTestAccess::data(*Config::get_instance());
    data["input"] = {{"home_edit_mode_enabled", "true"}, // string where bool belongs
                     {"scroll_guard", "yes"},
                     {"jitter_threshold", "5"},               // string where int belongs
                     {"scroll_throw", {{"value", 12}}},       // object where int belongs
                     {"debug_touches", json::array({true})}}; // array where bool belongs

    // Before the fix this threw type_error.302 out of init_subjects(), after
    // some subjects had been pushed into subjects_ but before
    // subjects_initialized_ was set.
    REQUIRE_NOTHROW(helix_test::reload_input_settings());

    InputSettingsManager& input = InputSettingsManager::instance();
    REQUIRE(input.get_home_edit_mode_enabled() == true); // compiled-in default
    REQUIRE(input.get_scroll_guard() == false);
    REQUIRE(input.get_jitter_threshold() == 5);
    REQUIRE(input.get_scroll_throw() == 25);
    REQUIRE(input.get_debug_touches() == false);
}
