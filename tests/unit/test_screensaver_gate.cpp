// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/scoped_env.h"
#include "config.h"
#include "display_backend.h"
#include "screensaver_cpu_clock.h"
#include "screensaver_gate.h"
#include "screensaver_level_store.h"

#include <cstdint>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::BoardFacts;
using helix::ui::CpuSample;
using helix::ui::display_backend_key;
using helix::ui::GateDecision;
using helix::ui::IdleBaseline;
using helix::ui::SaverGateSession;
using helix::ui::SaverLevelEntry;

namespace {

constexpr uint64_t NS = 1000000000ULL;

CpuSample at(double wall_s, double cpu_s) {
    return {static_cast<uint64_t>(cpu_s * NS), static_cast<uint64_t>(wall_s * NS)};
}

} // namespace

// ============================================================================
// Budget and decision
// ============================================================================

TEST_CASE("the saver budget follows the core count and drops while printing",
          "[screensaver][screensaver_gate]") {
    using helix::ui::saver_budget_share;
    CHECK(saver_budget_share(8, false, false) == Catch::Approx(1.00));
    CHECK(saver_budget_share(4, false, false) == Catch::Approx(1.00));
    CHECK(saver_budget_share(4, true, false) == Catch::Approx(0.50)); // every core in-order
    CHECK(saver_budget_share(3, false, false) == Catch::Approx(0.75));
    CHECK(saver_budget_share(2, false, false) == Catch::Approx(0.50));
    CHECK(saver_budget_share(1, false, false) == Catch::Approx(0.10));
    CHECK(saver_budget_share(0, false, false) == Catch::Approx(0.10));
    // klippy shares the SoC while printing: these stay where they are on purpose.
    CHECK(saver_budget_share(4, true, true) == Catch::Approx(0.25));
    CHECK(saver_budget_share(3, false, true) == Catch::Approx(0.185));
    CHECK(saver_budget_share(2, false, true) == Catch::Approx(0.125));
    CHECK(saver_budget_share(1, false, true) == Catch::Approx(0.05));
    CHECK(saver_budget_share(0, false, true) == Catch::Approx(0.05));
}

TEST_CASE("over budget steps down one level, at the bottom it is too heavy",
          "[screensaver][screensaver_gate]") {
    using helix::ui::decide_saver_level;
    CHECK(decide_saver_level(0.51, 0.50, 0, 2) == GateDecision::STEP_DOWN);
    CHECK(decide_saver_level(0.51, 0.50, 1, 2) == GateDecision::TOO_HEAVY);
    CHECK(decide_saver_level(0.90, 0.50, 2, 4) == GateDecision::STEP_DOWN);
    CHECK(decide_saver_level(0.90, 0.50, 3, 4) == GateDecision::TOO_HEAVY);
    CHECK(decide_saver_level(0.50, 0.50, 0, 2) == GateDecision::KEEP);
    // Far under budget keeps the level: a raise is decided by the comfort streak.
    CHECK(decide_saver_level(0.01, 0.50, 1, 2) == GateDecision::KEEP);
    CHECK(decide_saver_level(0.51, 0.50, 0, 1) == GateDecision::TOO_HEAVY);
}

TEST_CASE("a comfort streak rises only on windows comfortably under budget",
          "[screensaver][screensaver_gate]") {
    using helix::ui::SaverComfortStreak;
    SaverComfortStreak streak;
    CHECK(streak.streak() == 0);
    streak.add(0.59, 1.0);
    CHECK(streak.streak() == 1);
    streak.add(0.6, 1.0);
    CHECK(streak.streak() == 2);
    streak.add(0.61, 1.0);
    CHECK(streak.streak() == 0);
    streak.add(0.1, 1.0);
    streak.reset();
    CHECK(streak.streak() == 0);
}

TEST_CASE("three comfortable windows step the level back up, never past the top",
          "[screensaver][screensaver_gate]") {
    using helix::ui::decide_saver_level_up;
    CHECK(decide_saver_level_up(3, 3, 0) == GateDecision::STEP_UP);
    CHECK(decide_saver_level_up(1, 3, 0) == GateDecision::STEP_UP);
    CHECK(decide_saver_level_up(0, 10, 0) == GateDecision::KEEP);
    CHECK(decide_saver_level_up(3, 2, 0) == GateDecision::KEEP);
}

TEST_CASE("a raise never reaches a rung above the ceiling", "[screensaver][screensaver_gate]") {
    using helix::ui::decide_saver_level_up;
    CHECK(decide_saver_level_up(2, 10, 2) == GateDecision::KEEP);
    CHECK(decide_saver_level_up(2, 10, 1) == GateDecision::STEP_UP);
    CHECK(decide_saver_level_up(3, 10, 2) == GateDecision::STEP_UP);
}

// ============================================================================
// Idle baseline and windows
// ============================================================================

TEST_CASE("the idle baseline is 0 under 3 s of samples and the rate over the last 10 s after",
          "[screensaver][screensaver_gate]") {
    IdleBaseline baseline;
    baseline.add(at(0.0, 0.0));
    baseline.add(at(1.0, 0.1));
    baseline.add(at(2.9, 0.29));
    CHECK(baseline.rate() == 0.0);

    baseline.add(at(3.0, 0.30));
    CHECK(baseline.rate() == Catch::Approx(0.10));

    // Ten busy seconds, then ten quiet ones: only the last ten count.
    IdleBaseline stretch;
    double cpu = 0.0;
    for (int s = 0; s <= 20; s++) {
        stretch.add(at(s, cpu));
        cpu += s < 10 ? 0.9 : 0.05;
    }
    CHECK(stretch.size() == 11);
    CHECK(stretch.rate() == Catch::Approx(0.05));

    stretch.reset();
    CHECK(stretch.size() == 0);
    CHECK(stretch.rate() == 0.0);
}

TEST_CASE("a gate session ignores the first second, then measures 5 s windows net of the baseline",
          "[screensaver][screensaver_gate]") {
    SaverGateSession session;
    session.begin(at(0.0, 0.0), 0.1);

    // A full core during the warm-up second is not counted.
    CHECK_FALSE(session.add(at(0.5, 0.5)).has_value());
    CHECK_FALSE(session.add(at(1.0, 1.0)).has_value());
    CHECK_FALSE(session.add(at(3.0, 1.8)).has_value());

    // 0.4 of a core for 5 s, less 0.1 of baseline.
    const auto first = session.add(at(6.0, 3.0));
    REQUIRE(first.has_value());
    CHECK(*first == Catch::Approx(0.3));

    const auto second = session.add(at(11.0, 4.0));
    REQUIRE(second.has_value());
    CHECK(*second == Catch::Approx(0.1));

    // A restarted window measures from the restart, with no warm-up.
    session.restart_window(at(12.0, 4.5));
    CHECK_FALSE(session.add(at(16.9, 5.9)).has_value());
    const auto third = session.add(at(17.0, 6.0));
    REQUIRE(third.has_value());
    CHECK(*third == Catch::Approx(0.2));
}

TEST_CASE("the process CPU clock moves forward with work and time",
          "[screensaver][screensaver_gate]") {
    const CpuSample before = helix::ui::read_process_cpu_clock();
    volatile uint64_t sum = 0;
    for (uint64_t i = 0; i < 30000000ULL; i++) {
        sum += i;
    }
    const CpuSample after = helix::ui::read_process_cpu_clock();
    CHECK(after.cpu_ns > before.cpu_ns);
    CHECK(after.wall_ns > before.wall_ns);
}

// ============================================================================
// Environment switches
// ============================================================================

TEST_CASE("screensaver gate switches parse strictly and ignore malformed values",
          "[screensaver][screensaver_gate]") {
    helix::ScopedEnv budget{"HELIX_SCREENSAVER_BUDGET_PCT"};
    helix::ScopedEnv level{"HELIX_SCREENSAVER_LEVEL"};

    unsetenv("HELIX_SCREENSAVER_BUDGET_PCT");
    unsetenv("HELIX_SCREENSAVER_LEVEL");
    CHECK_FALSE(helix::ui::saver_env_overrides().budget_pct.has_value());
    CHECK_FALSE(helix::ui::saver_env_overrides().level.has_value());

    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "25", 1);
    setenv("HELIX_SCREENSAVER_LEVEL", "2", 1);
    CHECK(helix::ui::saver_env_overrides().budget_pct == 25u);
    CHECK(helix::ui::saver_env_overrides().level == 2u);

    for (const char* bad : {"", "abc", "25%", "-5", "0", "401", " 25", "2.5"}) {
        CAPTURE(bad);
        setenv("HELIX_SCREENSAVER_BUDGET_PCT", bad, 1);
        CHECK_FALSE(helix::ui::saver_env_overrides().budget_pct.has_value());
    }
    for (const char* bad : {"", "x", "-1", "100", "1e1"}) {
        CAPTURE(bad);
        setenv("HELIX_SCREENSAVER_LEVEL", bad, 1);
        CHECK_FALSE(helix::ui::saver_env_overrides().level.has_value());
    }
}

// ============================================================================
// Level store
// ============================================================================

TEST_CASE("the display backend key names the running display path",
          "[screensaver][screensaver_gate]") {
    CHECK(std::string(display_backend_key(DisplayBackendType::SDL, false)) == "sdl");
    CHECK(std::string(display_backend_key(DisplayBackendType::FBDEV, false)) == "fbdev");
    CHECK(std::string(display_backend_key(DisplayBackendType::DRM, false)) == "drm");
    CHECK(std::string(display_backend_key(DisplayBackendType::DRM, true)) == "egl");
}

TEST_CASE("the board fingerprint changes with every component and rounds bogomips to 100",
          "[screensaver][screensaver_gate]") {
    const BoardFacts base{"egl", 4, 1234.0f, 800, 480, 32};
    const std::string print = helix::ui::board_fingerprint(base);
    CHECK(print == "egl/4c/1200bm/800x480/32bpp");

    BoardFacts same_bogomips = base;
    same_bogomips.bogomips = 1249.0f;
    CHECK(helix::ui::board_fingerprint(same_bogomips) == print);

    BoardFacts changed = base;
    changed.display_backend = "drm";
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.cores = 2;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.bogomips = 1251.0f;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.width = 1024;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.height = 600;
    CHECK(helix::ui::board_fingerprint(changed) != print);
    changed = base;
    changed.color_depth = 16;
    CHECK(helix::ui::board_fingerprint(changed) != print);
}

TEST_CASE("a stored level is used only for the same version and board, clamped to the ladder",
          "[screensaver][screensaver_gate]") {
    const SaverLevelEntry stored{2, false, "1.2.3 (abc)", "egl/4c/0bm/800x480/32bpp"};

    SaverLevelEntry start = helix::ui::start_entry(stored, stored.version, stored.board, 4);
    CHECK(start.level == 2);
    CHECK_FALSE(start.too_heavy);

    start = helix::ui::start_entry(stored, stored.version, stored.board, 2);
    CHECK(start.level == 1);

    const SaverLevelEntry heavy{1, true, stored.version, stored.board};
    CHECK(helix::ui::start_entry(heavy, stored.version, stored.board, 2).too_heavy);

    for (const auto& [version, board] :
         {std::pair<std::string, std::string>{"1.2.4 (abd)", stored.board},
          std::pair<std::string, std::string>{stored.version, "drm/4c/0bm/800x480/32bpp"}}) {
        INFO("version " << version << ", board " << board);
        const SaverLevelEntry fresh = helix::ui::start_entry(heavy, version, board, 2);
        CHECK(fresh.level == 0);
        CHECK_FALSE(fresh.too_heavy);
        CHECK(fresh.version == version);
        CHECK(fresh.board == board);
    }
    CHECK(helix::ui::start_entry(std::nullopt, stored.version, stored.board, 2).level == 0);
}

TEST_CASE("malformed level entries are ignored and a written entry parses back",
          "[screensaver][screensaver_gate]") {
    using nlohmann::json;
    const SaverLevelEntry entry{1, true, "1.2.3 (abc)", "sdl/8c/0bm/800x480/32bpp"};
    const json written = helix::ui::level_entry_json(entry);
    CHECK(helix::ui::parse_level_entry(&written) == entry);

    // The version string contains ")" before a quote, so the raw string needs its own
    // delimiter or it would end at "(abc)".
    const json parsed_from_text = json::parse(
        R"json({"level": 1, "too_heavy": true, "version": "1.2.3 (abc)", "board": "sdl/8c/0bm/800x480/32bpp"})json");
    CHECK(helix::ui::parse_level_entry(&parsed_from_text) == entry);

    CHECK_FALSE(helix::ui::parse_level_entry(nullptr).has_value());
    for (const char* bad : {
             R"("level one")",
             R"({"too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": -1, "too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": 1.5, "too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": "1", "too_heavy": false, "version": "v", "board": "b"})",
             R"({"level": 1, "too_heavy": "no", "version": "v", "board": "b"})",
             R"({"level": 1, "too_heavy": false, "board": "b"})",
             R"({"level": 1, "too_heavy": false, "version": "v", "board": 7})",
         }) {
        CAPTURE(bad);
        const json node = json::parse(bad);
        CHECK_FALSE(helix::ui::parse_level_entry(&node).has_value());
    }
}

TEST_CASE("a level entry saved to config loads back from its path",
          "[screensaver][screensaver_gate]") {
    helix::Config* config = helix::Config::get_instance();
    constexpr const char* NAME = "gate_store_probe";
    CHECK(helix::ui::level_store_path(NAME) == "/display/screensaver_levels/gate_store_probe");

    const SaverLevelEntry entry{3, false, "9.9.9 (probe)", "fbdev/2c/1000bm/480x272/16bpp"};
    helix::ui::save_level_entry(*config, NAME, entry);
    CHECK(helix::ui::load_level_entry(*config, NAME) == entry);

    config->get_json("/display/screensaver_levels").erase(NAME);
    CHECK_FALSE(helix::ui::load_level_entry(*config, NAME).has_value());
}

#endif // HELIX_ENABLE_SCREENSAVER
