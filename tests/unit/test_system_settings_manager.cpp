// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "logging_init.h"
#include "system_settings_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// SystemSettingsManager Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager default values after init",
                 "[system_settings]") {
    Config::get_instance();
    SystemSettingsManager::instance().init_subjects();

    SECTION("language defaults to English (index 0)") {
        REQUIRE(SystemSettingsManager::instance().get_language() == "en");
        REQUIRE(SystemSettingsManager::instance().get_language_index() == 0);
    }

    SECTION("update_channel defaults to Stable (0)") {
        REQUIRE(SystemSettingsManager::instance().get_update_channel() == 0);
    }

    SECTION("telemetry defaults to disabled") {
        REQUIRE(SystemSettingsManager::instance().get_telemetry_enabled() == false);
    }

    SystemSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager language index/code conversion",
                 "[system_settings]") {
    SECTION("language_index_to_code valid indices") {
        const char* codes[] = {"en", "en-GB", "de", "fr", "es", "ru", "pt", "it", "zh", "ja"};
        for (int i = 0; i < 10; ++i) {
            CAPTURE(i);
            REQUIRE(SystemSettingsManager::language_index_to_code(i) == codes[i]);
        }
    }

    SECTION("language_index_to_code out-of-range defaults to en") {
        REQUIRE(SystemSettingsManager::language_index_to_code(-1) == "en");
        REQUIRE(SystemSettingsManager::language_index_to_code(99) == "en");
    }

    SECTION("language_code_to_index valid codes") {
        REQUIRE(SystemSettingsManager::language_code_to_index("en") == 0);
        REQUIRE(SystemSettingsManager::language_code_to_index("en-GB") == 1);
        REQUIRE(SystemSettingsManager::language_code_to_index("de") == 2);
        REQUIRE(SystemSettingsManager::language_code_to_index("ja") == 9);
    }

    SECTION("language_code_to_index unknown code defaults to 0") {
        REQUIRE(SystemSettingsManager::language_code_to_index("xx") == 0);
        REQUIRE(SystemSettingsManager::language_code_to_index("") == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager update channel set/get",
                 "[system_settings]") {
    Config::get_instance();
    SystemSettingsManager::instance().init_subjects();

    SECTION("set/get round trip") {
        SystemSettingsManager::instance().set_update_channel(1);
        REQUIRE(SystemSettingsManager::instance().get_update_channel() == 1);

        SystemSettingsManager::instance().set_update_channel(2);
        REQUIRE(SystemSettingsManager::instance().get_update_channel() == 2);

        SystemSettingsManager::instance().set_update_channel(0);
        REQUIRE(SystemSettingsManager::instance().get_update_channel() == 0);
    }

    SECTION("clamping out-of-range values") {
        SystemSettingsManager::instance().set_update_channel(-1);
        REQUIRE(SystemSettingsManager::instance().get_update_channel() == 0);

        SystemSettingsManager::instance().set_update_channel(99);
        REQUIRE(SystemSettingsManager::instance().get_update_channel() == 2);
    }

    SystemSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager telemetry set/get", "[system_settings]") {
    Config::get_instance();
    SystemSettingsManager::instance().init_subjects();

    SECTION("set/get round trip") {
        SystemSettingsManager::instance().set_telemetry_enabled(true);
        REQUIRE(SystemSettingsManager::instance().get_telemetry_enabled() == true);

        SystemSettingsManager::instance().set_telemetry_enabled(false);
        REQUIRE(SystemSettingsManager::instance().get_telemetry_enabled() == false);
    }

    SystemSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager subject values match getters",
                 "[system_settings]") {
    Config::get_instance();
    SystemSettingsManager::instance().init_subjects();

    SECTION("update_channel subject reflects setter") {
        SystemSettingsManager::instance().set_update_channel(2);
        REQUIRE(lv_subject_get_int(SystemSettingsManager::instance().subject_update_channel()) ==
                2);
    }

    SECTION("telemetry subject reflects setter") {
        SystemSettingsManager::instance().set_telemetry_enabled(true);
        REQUIRE(lv_subject_get_int(SystemSettingsManager::instance().subject_telemetry_enabled()) ==
                1);

        SystemSettingsManager::instance().set_telemetry_enabled(false);
        REQUIRE(lv_subject_get_int(SystemSettingsManager::instance().subject_telemetry_enabled()) ==
                0);
    }

    SECTION("language subject reflects default") {
        // Default is English = index 0
        REQUIRE(lv_subject_get_int(SystemSettingsManager::instance().subject_language()) == 0);
    }

    SystemSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager language options string",
                 "[system_settings]") {
    const char* options = SystemSettingsManager::get_language_options();
    REQUIRE(options != nullptr);
    // Verify it starts with English and contains multiple entries
    std::string opts(options);
    REQUIRE(opts.find("English") == 0);
    REQUIRE(opts.find("Deutsch") != std::string::npos);

    // Each option sits at its code's index: a regional variant is named in its own words.
    std::vector<std::string> names;
    for (size_t pos = 0, end; pos <= opts.size(); pos = end + 1) {
        end = std::min(opts.find('\n', pos), opts.size());
        names.push_back(opts.substr(pos, end - pos));
    }
    REQUIRE(names.at(SystemSettingsManager::language_code_to_index("en")) == "English");
    REQUIRE(names.at(SystemSettingsManager::language_code_to_index("en-GB")) == "English (UK)");
    REQUIRE(names.at(SystemSettingsManager::language_code_to_index("de")) == "Deutsch");
}

// ============================================================================
// Log level subject
// ============================================================================

namespace {

// Put the logger in the shape a device runs in: persistent sinks at `level`,
// logger floor held at debug so the ring buffer keeps feeding the debug bundle.
void init_logging_at(spdlog::level::level_enum level) {
    helix::logging::LogConfig cfg;
    cfg.target = helix::logging::LogTarget::Console;
    cfg.enable_console = false;
    cfg.level = spdlog::level::info;
    helix::logging::init(cfg);
    helix::logging::set_runtime_level(level);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager log level reports the sink level",
                 "[system_settings]") {
    init_logging_at(spdlog::level::warn);

    // The condition that makes this test worth running: the logger floor is
    // debug (the ring buffer's), so anything reading spdlog::get_level() sees
    // "debug" on a device whose sinks are at warn.
    REQUIRE(spdlog::get_level() == spdlog::level::debug);
    REQUIRE(helix::logging::effective_log_level() == spdlog::level::warn);

    auto* config = Config::get_instance();
    config->set<std::string>("/log_level", "");

    SystemSettingsManager::instance().deinit_subjects();
    SystemSettingsManager::instance().init_subjects();

    // Index 0 is warn, 2 is debug.
    REQUIRE(SystemSettingsManager::instance().get_log_level_index() == 0);

    SystemSettingsManager::instance().deinit_subjects();
    helix::logging::set_runtime_level(spdlog::level::info);
}

TEST_CASE_METHOD(LVGLTestFixture, "SystemSettingsManager log level prefers a saved value",
                 "[system_settings]") {
    init_logging_at(spdlog::level::warn);

    auto* config = Config::get_instance();
    config->set<std::string>("/log_level", "trace");

    SystemSettingsManager::instance().deinit_subjects();
    SystemSettingsManager::instance().init_subjects();

    // Index 3 is trace: a saved choice outranks whatever the sinks run at.
    REQUIRE(SystemSettingsManager::instance().get_log_level_index() == 3);

    config->set<std::string>("/log_level", "");
    SystemSettingsManager::instance().deinit_subjects();
    helix::logging::set_runtime_level(spdlog::level::info);
}
