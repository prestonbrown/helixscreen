// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guards the test binary's config sandbox.
//
// Config is a process singleton and the test binary runs many test cases per
// process, so without an explicit reset a test inherits whatever the previous
// one left behind — and, worse, whatever path the previous one pointed it at.
// Before the sandbox existed, `make test-run` deleted and rewrote the
// developer's real $HOME/.helixscreen/settings.json.backup and
// helixscreen.env.backup on every run, and dropped a tool_spools.json into the
// repo's own config/ directory.
//
// These assertions are the tripwire for that whole class of leak: they fail if
// the sandbox stops being installed, if the per-test reset stops running, or if
// a config path starts resolving outside the sandbox again.

#include "../helix_test_fixture.h"
#include "app_constants.h"
#include "config.h"
#include "core/data_root_resolver.h"
#include "tool_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Config;
using nlohmann::json;

namespace {

bool under_sandbox(const std::string& path) {
    const std::string& sandbox = helix::test::config_sandbox_dir();
    return !sandbox.empty() && path.rfind(sandbox, 0) == 0;
}

constexpr const char* kProbeStrip = "neopixel:config_isolation_probe";

std::string probe_key(Config* cfg) {
    return cfg->df() + "leds/selected_strips";
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "Config singleton is sandboxed and reset per test",
                 "[config][isolation]") {
    auto* cfg = Config::get_instance();

    SECTION("the singleton persists inside the sandbox, never the repo or $HOME") {
        const std::string path = cfg->get_path();
        REQUIRE_FALSE(path.empty());
        REQUIRE(under_sandbox(path));
        // The two real locations this used to reach.
        REQUIRE(path.find("/.helixscreen/") == std::string::npos);
        REQUIRE(path.find("/helixscreen/config/") == std::string::npos);
    }

    SECTION("the rolling-backup tiers are redirected off /var/lib and $HOME") {
        // Config::init()/save() write rolling backups through these, and two
        // fixtures delete the fallback outright. Before the sandbox that meant
        // the developer's real $HOME/.helixscreen/*.backup, every test run.
        REQUIRE(under_sandbox(helix::app_constants::update::state_dir()));
        REQUIRE(under_sandbox(helix::app_constants::update::backup_fallback_dir()));
        REQUIRE(under_sandbox(helix::app_constants::update::config_backup_fallback()));
        REQUIRE(under_sandbox(helix::app_constants::update::env_backup_fallback()));
        REQUIRE(under_sandbox(helix::app_constants::update::config_backup_primary()));
        REQUIRE(under_sandbox(helix::app_constants::update::env_backup_primary()));
    }

    SECTION("ToolState persists tool_spools.json inside the sandbox, not the repo") {
        // helix::get_user_config_dir() defaults to the RELATIVE dir "config",
        // so an un-redirected ToolState writes into the checkout itself.
        REQUIRE(under_sandbox(helix::ToolState::instance().get_config_dir()));
    }

    // Catch2 re-runs the test body once per leaf section, in declaration order,
    // in the SAME process — so "writer" is guaranteed to run before "reader"
    // with a fresh fixture in between. That is the cross-test contamination
    // this pair proves is gone.
    SECTION("writer: persists a distinctive LED selection") {
        cfg->set(probe_key(cfg), json::array({kProbeStrip}));
        REQUIRE(cfg->get_string_array(probe_key(cfg)).size() == 1);
        REQUIRE(cfg->save());
    }

    SECTION("reader: does not observe the writer's value") {
        const auto strips = cfg->get_string_array(probe_key(cfg));
        REQUIRE(strips.empty());
        // Nor is it readable back off disk — the reset unlinks the file the
        // writer's save() produced.
        REQUIRE(cfg->get<json>(probe_key(cfg), json()).is_null());
    }
}

// A fixture-less TEST_CASE gets the same guarantee, via the isolation listener
// rather than HelixTestFixture. Most config-touching tests in the suite
// (test_led_config.cpp is entirely fixture-less) rely on this path.
TEST_CASE("Config sandbox applies to fixture-less test cases", "[config][isolation]") {
    auto* cfg = Config::get_instance();
    REQUIRE(under_sandbox(cfg->get_path()));
    REQUIRE(cfg->get_string_array(probe_key(cfg)).empty());
}
