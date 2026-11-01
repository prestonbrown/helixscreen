// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "ui_settings_display.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/screensaver_manager_test_access.h"
#include "../test_helpers/screensaver_test_access.h"
#include "config.h"
#include "display_settings_manager.h"
#include "helix_version.h"
#include "platform_capabilities.h"
#include "refresh_period_hold.h"
#include "screen_hide_hold.h"
#include "screensaver.h"
#include "screensaver_base.h"
#include "screensaver_gate.h"
#include "screensaver_level_store.h"
#include "screensaver_usage.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ScreensaverManagerTestAccess;
using helix::ui::CpuSample;
using helix::ui::SaverHost;
using helix::ui::SaverLevelEntry;

namespace {

constexpr const char* kLevelsPath = "/display/screensaver_levels";

/// Drives the shared manager's gate with a scripted CPU clock, and puts the clock, the host,
/// the stored levels and the gate environment back however the test ends.
class GateHarness {
  public:
    GateHarness() {
        mgr.stop();
        helix::Config* config = helix::Config::get_instance();
        if (const nlohmann::json* levels = config->try_get_json(kLevelsPath)) {
            saved_levels_ = *levels;
        }
        clear_levels();
        unsetenv("HELIX_SCREENSAVER_BUDGET_PCT");
        unsetenv("HELIX_SCREENSAVER_LEVEL");
        ScreensaverManagerTestAccess::set_cpu_clock(mgr,
                                                    [this] { return CpuSample{cpu_ns, wall_ns}; });
        ScreensaverManagerTestAccess::reset_baseline(mgr);
        mgr.set_host(SaverHost{[this] { return printing; }, "sdl"});
    }

    ~GateHarness() {
        mgr.stop();
        ScreensaverManagerTestAccess::set_cpu_clock(mgr, helix::ui::read_process_cpu_clock);
        ScreensaverManagerTestAccess::reset_baseline(mgr);
        mgr.set_host(SaverHost{});
        clear_levels();
        if (saved_levels_) {
            helix::Config::get_instance()->set<nlohmann::json>(kLevelsPath, *saved_levels_);
        }
    }

    GateHarness(const GateHarness&) = delete;
    GateHarness& operator=(const GateHarness&) = delete;

    /// Advances both clocks by `seconds` in 250 ms steps with the process using `cores` of CPU,
    /// calling the idle-check tick after each step.
    void run(double seconds, double cores) {
        constexpr uint64_t kStepNs = 250000000ULL;
        const int steps = static_cast<int>(seconds * 4.0 + 0.5);
        for (int i = 0; i < steps; i++) {
            wall_ns += kStepNs;
            cpu_ns += static_cast<uint64_t>(cores * static_cast<double>(kStepNs));
            mgr.on_idle_check_tick();
        }
    }

    std::optional<SaverLevelEntry> stored(const char* name) const {
        return helix::ui::load_level_entry(*helix::Config::get_instance(), name);
    }

    void store(const char* name, const SaverLevelEntry& entry) {
        helix::ui::save_level_entry(*helix::Config::get_instance(), name, entry);
    }

    helix::ui::SaverBase* running() const {
        return ScreensaverManagerTestAccess::active(mgr);
    }

    ScreensaverManager& mgr = ScreensaverManager::instance();
    helix::ScopedEnv budget_env{"HELIX_SCREENSAVER_BUDGET_PCT"};
    helix::ScopedEnv level_env{"HELIX_SCREENSAVER_LEVEL"};
    uint64_t cpu_ns = 0;
    uint64_t wall_ns = 1000ULL * 1000000000ULL;
    bool printing = false;

  private:
    static void clear_levels() {
        helix::Config* config = helix::Config::get_instance();
        if (config->try_get_json(kLevelsPath) != nullptr) {
            config->get_json("/display").erase("screensaver_levels");
        }
    }

    std::optional<nlohmann::json> saved_levels_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "the gate subtracts the idle baseline from a running saver's windows",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "20", 1);
    gate.run(10.0, 0.05);
    CHECK(ScreensaverManagerTestAccess::baseline_rate(gate.mgr) ==
          Catch::Approx(0.05).margin(0.001));

    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    REQUIRE(gate.running()->level() == 0);

    // 24% of a core with 5% of idle work is the saver using 19%, within the 20% budget.
    gate.run(6.25, 0.24);
    CHECK(gate.running()->level() == 0);
    CHECK_FALSE(gate.stored("toasters").has_value());

    // With no idle stretch behind it the baseline is 0, and the same load is over budget.
    gate.mgr.stop();
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    gate.run(6.25, 0.24);
    CHECK(gate.running()->level() == 1);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "an over-budget saver steps down one level, stores it, and the next run starts there",
    "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "20", 1);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);

    gate.run(0.75, 0.9); // the warm-up second
    CHECK(gate.running()->level() == 0);
    gate.run(5.5, 0.5);
    REQUIRE(gate.running()->level() == 1);
    CHECK(SaverTestAccess::timer(*gate.running())->period == 33);
    const std::optional<SaverLevelEntry> entry = gate.stored("toasters");
    REQUIRE(entry.has_value());
    CHECK(entry->level == 1);
    CHECK_FALSE(entry->too_heavy);
    CHECK(entry->version == helix_version_full());
    CHECK(entry->board == ScreensaverManagerTestAccess::current_board(gate.mgr));

    // Marginal comfort does not raise the level: 15% against a 20% budget
    // keeps the saver, but each window sits above the comfort line.
    gate.run(20.0, 0.15);
    CHECK(gate.running()->level() == 1);

    gate.mgr.stop();
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->level() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "the bouncing printer runs under the gate like every other saver",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "20", 1);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::BOUNCING_PRINTER);
    // The manager holds the bouncing printer in its gated saver list: active() is what
    // stayed nullptr while it ran off SaverBase.
    helix::ui::SaverBase* bounce = gate.running();
    REQUIRE(bounce != nullptr);
    CHECK(bounce->type() == ScreensaverType::BOUNCING_PRINTER);
    CHECK(bounce->level() == 0);

    gate.run(0.75, 0.9); // the warm-up second
    CHECK(gate.running()->level() == 0);
    gate.run(5.5, 0.5);
    REQUIRE(gate.running()->level() == 1);
    CHECK(SaverTestAccess::timer(*gate.running())->period == 33);
    const std::optional<SaverLevelEntry> entry = gate.stored("bounce");
    REQUIRE(entry.has_value());
    CHECK(entry->level == 1);
    CHECK_FALSE(entry->too_heavy);

    gate.mgr.stop();
    gate.mgr.start(ScreensaverType::BOUNCING_PRINTER);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->level() == 1);
}

namespace {

/// Runs `saver` down its ladder and one window past the bottom, over a 10% budget.
void run_past_bottom(GateHarness& gate, helix::ui::SaverBase& saver) {
    // Walk to whatever the bottom rung is rather than naming it: the ladder's depth is the
    // saver's business, and hardcoding it here breaks every time a rung is added.
    const size_t bottom = saver.level_count() - 1;
    for (size_t expected = 1; expected <= bottom; expected++) {
        gate.run(6.25, 0.9);
        CAPTURE(expected);
        REQUIRE(saver.level() == expected);
    }
    // One more window over budget with nowhere left to step.
    gate.run(5.5, 0.9);
}

/// Stores every registered saver except `except` as too heavy on this board and version.
void store_all_too_heavy_except(GateHarness& gate, ScreensaverType except) {
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    for (const helix::ui::ScreensaverInfo& info : helix::ui::kScreensavers) {
        if (info.type != except) {
            gate.store(info.name, {0, true, helix_version_full(), board});
        }
    }
}

} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "over budget at the lowest level stores the board as too heavy and runs the fallback saver",
    "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "10", 1);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* maze = gate.running();
    REQUIRE(maze != nullptr);

    run_past_bottom(gate, *maze);

    const std::optional<SaverLevelEntry> entry = gate.stored("maze");
    REQUIRE(entry.has_value());
    CHECK(entry->level == maze->level_count() - 1);
    CHECK(entry->too_heavy);
    CHECK_FALSE(maze->is_active());
    CHECK(SaverTestAccess::timer(*maze) == nullptr);

    // The default saver takes over at once, measured from its own level 0.
    helix::ui::SaverBase* fallback = gate.running();
    REQUIRE(fallback != nullptr);
    CHECK(fallback->type() == helix::ui::kDefaultScreensaverType);
    CHECK(fallback->is_active());
    CHECK(fallback->level() == 0);
    CHECK_FALSE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    CHECK(helix::active_screen_hide_hold().is_held());
    CHECK(helix::active_refresh_period_hold().is_held());

    // The next run of the maze starts the fallback directly.
    gate.mgr.stop();
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
    gate.mgr.start(ScreensaverType::MAZE_3D);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->type() == helix::ui::kDefaultScreensaverType);
    CHECK_FALSE(maze->is_active());
    CHECK(helix::active_screen_hide_hold().is_held());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver stored as too heavy starts the default, or the first runnable saver",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    const std::string version = helix_version_full();
    gate.store("maze", {3, true, version, board});

    SECTION("the default runs in its place") {
        CHECK(gate.mgr.runnable_type(ScreensaverType::MAZE_3D) ==
              helix::ui::kDefaultScreensaverType);
        gate.mgr.start(ScreensaverType::MAZE_3D);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->type() == helix::ui::kDefaultScreensaverType);
    }
    SECTION("with the default too heavy as well, the first registry saver that is not") {
        gate.store(helix::ui::find_screensaver(helix::ui::kDefaultScreensaverType)->name,
                   {2, true, version, board});
        CHECK(gate.mgr.runnable_type(ScreensaverType::MAZE_3D) == ScreensaverType::FLYING_TOASTERS);
        gate.mgr.start(ScreensaverType::MAZE_3D);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->type() == ScreensaverType::FLYING_TOASTERS);
    }
    SECTION("the mark from another board or version is not too heavy here") {
        gate.store("maze", {3, true, version, "fbdev/1c/0bm/1x1/16bpp"});
        CHECK_FALSE(gate.mgr.too_heavy_here(ScreensaverType::MAZE_3D));
        gate.mgr.start(ScreensaverType::MAZE_3D);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->type() == ScreensaverType::MAZE_3D);
    }
    CHECK(gate.mgr.too_heavy_here(ScreensaverType::MAZE_3D) ==
          (gate.mgr.runnable_type(ScreensaverType::MAZE_3D) != ScreensaverType::MAZE_3D));
    CHECK_FALSE(gate.mgr.too_heavy_here(ScreensaverType::OFF));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "with every saver too heavy the last one shows a static black screen",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "10", 1);
    store_all_too_heavy_except(gate, ScreensaverType::FLYING_TOASTERS);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* toasters = gate.running();
    REQUIRE(toasters != nullptr);
    REQUIRE(toasters->type() == ScreensaverType::FLYING_TOASTERS);

    run_past_bottom(gate, *toasters);

    CHECK(gate.running() == nullptr);
    CHECK_FALSE(toasters->is_active());
    CHECK(gate.mgr.is_active());
    REQUIRE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    lv_obj_t* black = ScreensaverManagerTestAccess::black_screen(gate.mgr);
    REQUIRE(black != nullptr);
    CHECK(lv_obj_get_parent(black) == lv_layer_top());
    CHECK(lv_obj_get_style_bg_opa(black, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(helix::active_screen_hide_hold().is_held());
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    const std::optional<SaverLevelEntry> entry = gate.stored("toasters");
    REQUIRE(entry.has_value());
    CHECK(entry->too_heavy);

    gate.mgr.stop();
    CHECK_FALSE(gate.mgr.is_active());
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());

    // The next run of any saver goes straight to the black screen.
    CHECK(gate.mgr.runnable_type(ScreensaverType::MAZE_3D) == ScreensaverType::MAZE_3D);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    CHECK(gate.mgr.is_active());
    CHECK(gate.running() == nullptr);
    CHECK(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    CHECK(helix::active_screen_hide_hold().is_held());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "HELIX_SCREENSAVER_LEVEL runs a saver stored as too heavy, with no fallback",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    gate.store("maze", {3, true, helix_version_full(),
                        ScreensaverManagerTestAccess::current_board(gate.mgr)});
    setenv("HELIX_SCREENSAVER_LEVEL", "1", 1);
    CHECK_FALSE(gate.mgr.too_heavy_here(ScreensaverType::MAZE_3D));
    CHECK(gate.mgr.runnable_type(ScreensaverType::MAZE_3D) == ScreensaverType::MAZE_3D);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->type() == ScreensaverType::MAZE_3D);
    CHECK(gate.running()->level() == 1);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "a stored level from another app version or board, or a malformed one, starts at level 0",
    "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    const std::string version = helix_version_full();

    SECTION("the same version and board start at the stored level") {
        gate.store("toasters", {1, false, version, board});
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 1);
    }
    SECTION("another app version") {
        gate.store("toasters", {1, true, "0.0.0 (other)", board});
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 0);
        CHECK_FALSE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    }
    SECTION("another board") {
        gate.store("toasters", {1, true, version, "fbdev/1c/0bm/1x1/16bpp"});
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 0);
        CHECK_FALSE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    }
    SECTION("a malformed entry") {
        helix::Config::get_instance()->set<nlohmann::json>("/display/screensaver_levels/toasters",
                                                           nlohmann::json("level one"));
        gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(gate.running() != nullptr);
        CHECK(gate.running()->level() == 0);
    }
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "HELIX_SCREENSAVER_LEVEL forces a level with the gate off, and past the ladder is ignored",
    "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "1", 1);
    setenv("HELIX_SCREENSAVER_LEVEL", "1", 1);
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->level() == 1);

    gate.run(30.0, 1.0);
    CHECK(gate.running() != nullptr);
    CHECK_FALSE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    CHECK_FALSE(gate.stored("toasters").has_value());

    gate.mgr.stop();
    unsetenv("HELIX_SCREENSAVER_BUDGET_PCT");
    setenv("HELIX_SCREENSAVER_LEVEL", "7", 1);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    // Toasters have levels 0 to 2; a clamp would give 2, ignoring gives the fresh level 0.
    CHECK(gate.running()->level() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the budget follows the core count and drops when a print starts mid-run",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const double full =
        helix::ui::saver_budget_share(helix::PlatformCapabilities::detect().cpu_cores,
                                      helix::PlatformCapabilities::detect().bogomips, false);
    const double load = full * 0.75;
    gate.run(10.0, 0.0);
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);

    gate.run(6.25, load);
    CHECK(gate.running()->level() == 0);

    gate.printing = true;
    gate.run(5.0, load);
    CHECK(gate.running()->level() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "a step-down taken while printing lasts only for that run",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "20", 1);
    gate.run(10.0, 0.0);
    gate.printing = true;
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);

    gate.run(6.25, 0.5);
    REQUIRE(gate.running()->level() == 1);
    CHECK_FALSE(gate.stored("toasters").has_value());

    // The print ends; the run keeps its level, and the next run is measured from level 0.
    gate.printing = false;
    gate.run(10.0, 0.01);
    CHECK(gate.running()->level() == 1);
    gate.mgr.stop();
    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->level() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "too heavy while printing blanks that run and marks nothing",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "10", 1);
    gate.run(10.0, 0.0);
    gate.printing = true;
    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* maze = gate.running();
    REQUIRE(maze != nullptr);

    run_past_bottom(gate, *maze);

    CHECK_FALSE(maze->is_active());
    CHECK(gate.running() == nullptr);
    CHECK(gate.mgr.is_active());
    CHECK(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    CHECK(helix::active_screen_hide_hold().is_held());
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK_FALSE(gate.stored("maze").has_value());
    CHECK_FALSE(gate.mgr.too_heavy_here(ScreensaverType::MAZE_3D));

    gate.mgr.stop();
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
    gate.printing = false;
    gate.mgr.start(ScreensaverType::MAZE_3D);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.running()->type() == ScreensaverType::MAZE_3D);
    CHECK(gate.running()->level() == 0);
}

namespace {

std::vector<std::string> dropdown_lines(lv_obj_t* dropdown) {
    std::vector<std::string> lines;
    std::string line;
    for (const char* p = lv_dropdown_get_options(dropdown); *p; ++p) {
        if (*p == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += *p;
        }
    }
    lines.push_back(line);
    return lines;
}

/// Puts the stored screensaver type back however the test ends.
struct StoredTypeRestore {
    int saved = helix::DisplaySettingsManager::instance().get_screensaver_type();
    ~StoredTypeRestore() {
        helix::DisplaySettingsManager::instance().set_screensaver_type(saved);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the screensaver dropdown hides too-heavy savers and shows the one that runs",
                 "[screensaver][screensaver_gate][ui]") {
    GateHarness gate;
    StoredTypeRestore restore;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    gate.store("maze", {3, true, helix_version_full(), board});
    gate.store("hyperspace", {4, true, helix_version_full(), board});
    auto& display = helix::DisplaySettingsManager::instance();
    display.set_screensaver_type(static_cast<int>(ScreensaverType::MAZE_3D));

    helix::settings::DisplaySettingsOverlay overlay;
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    overlay.init_screensaver_dropdown();
    lv_obj_t* dropdown =
        lv_obj_find_by_name(lv_obj_find_by_name(root, "row_screensaver"), "dropdown");
    REQUIRE(dropdown != nullptr);

    std::vector<std::string> expected{helix::ui::kScreensaverOffLabelKey};
    for (const helix::ui::ScreensaverInfo& info : helix::ui::kScreensavers) {
        if (info.type != ScreensaverType::MAZE_3D && info.type != ScreensaverType::HYPERSPACE) {
            expected.emplace_back(info.label_key);
        }
    }
    expected.emplace_back(helix::ui::kScreensaverRandomLabelKey);
    CHECK(dropdown_lines(dropdown) == expected);

    // The stored maze shows as the saver that runs in its place, and stays stored.
    const helix::ui::ScreensaverInfo* fallback =
        helix::ui::find_screensaver(helix::ui::kDefaultScreensaverType);
    REQUIRE(fallback != nullptr);
    const uint32_t selected = lv_dropdown_get_selected(dropdown);
    REQUIRE(selected < expected.size());
    CHECK(expected[selected] == fallback->label_key);
    CHECK(display.get_screensaver_type() == static_cast<int>(ScreensaverType::MAZE_3D));

    // Picking a row stores that row's type, not its index.
    const auto flip = std::find(expected.begin(), expected.end(), "Flip Clock");
    REQUIRE(flip != expected.end());
    overlay.handle_screensaver_changed(static_cast<int>(flip - expected.begin()));
    CHECK(display.get_screensaver_type() == static_cast<int>(ScreensaverType::FLIP_CLOCK));
    overlay.handle_screensaver_changed(static_cast<int>(expected.size() - 1));
    CHECK(display.get_screensaver_type() == static_cast<int>(ScreensaverType::RANDOM));
    CHECK(lv_dropdown_get_selected(dropdown) == expected.size() - 1);
    overlay.handle_screensaver_changed(0);
    CHECK(display.get_screensaver_type() == static_cast<int>(ScreensaverType::OFF));
    overlay.handle_screensaver_changed(static_cast<int>(expected.size()));
    CHECK(display.get_screensaver_type() == static_cast<int>(ScreensaverType::OFF));

    lv_obj_delete(root);
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "with every saver too heavy the dropdown keeps the stored one so Off can be chosen",
    "[screensaver][screensaver_gate][ui]") {
    GateHarness gate;
    StoredTypeRestore restore;
    store_all_too_heavy_except(gate, ScreensaverType::OFF);
    auto& display = helix::DisplaySettingsManager::instance();
    display.set_screensaver_type(static_cast<int>(ScreensaverType::FLYING_TOASTERS));

    helix::settings::DisplaySettingsOverlay overlay;
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    overlay.init_screensaver_dropdown();
    lv_obj_t* dropdown =
        lv_obj_find_by_name(lv_obj_find_by_name(root, "row_screensaver"), "dropdown");
    REQUIRE(dropdown != nullptr);

    const std::vector<std::string> expected{"Off", "Flying Toasters (too slow)"};
    CHECK(dropdown_lines(dropdown) == expected);
    REQUIRE(lv_dropdown_get_selected(dropdown) == 1);
    REQUIRE(lv_dropdown_get_text(dropdown) != nullptr);
    CHECK(std::string(lv_dropdown_get_text(dropdown)) == "Flying Toasters");

    // Off is a different row from the one selected, so choosing it is a change that stores.
    overlay.handle_screensaver_changed(0);
    CHECK(helix::Config::get_instance()->get<std::string>("/display/screensaver_type", "") ==
          "off");
    CHECK(dropdown_lines(dropdown) == std::vector<std::string>{"Off"});
    CHECK(lv_dropdown_get_text(dropdown) == nullptr);

    lv_obj_delete(root);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a language change keeps the screensaver dropdown's hidden savers hidden",
                 "[screensaver][screensaver_gate][ui]") {
    GateHarness gate;
    StoredTypeRestore restore;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    gate.store("maze", {3, true, helix_version_full(), board});
    auto& display = helix::DisplaySettingsManager::instance();
    display.set_screensaver_type(static_cast<int>(ScreensaverType::FLIP_CLOCK));

    helix::settings::DisplaySettingsOverlay overlay;
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    overlay.init_screensaver_dropdown();
    overlay.init_screensaver_dropdown();
    lv_obj_t* dropdown =
        lv_obj_find_by_name(lv_obj_find_by_name(root, "row_screensaver"), "dropdown");
    REQUIRE(dropdown != nullptr);
    const std::vector<std::string> before = dropdown_lines(dropdown);
    const uint32_t selected = lv_dropdown_get_selected(dropdown);
    REQUIRE(std::find(before.begin(), before.end(), "3D Maze") == before.end());

    // The row's options_tag puts the full XML list back on a language change; the overlay
    // rebuilds its own list after it.
    lv_obj_send_event(dropdown, LV_EVENT_TRANSLATION_LANGUAGE_CHANGED, nullptr);
    CHECK(dropdown_lines(dropdown) == before);
    CHECK(lv_dropdown_get_selected(dropdown) == selected);

    lv_obj_delete(root);
}

namespace {

/// The manager's usage window as of now.
helix::SaverUsageWindow usage_now(ScreensaverManager& mgr) {
    return mgr.usage().window(std::chrono::steady_clock::now());
}

/// Empties the manager's usage window, which carries over from earlier tests.
void clear_usage(ScreensaverManager& mgr) {
    mgr.stop();
    mgr.usage().reset_window(std::chrono::steady_clock::now());
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "usage counts a run's start, how it ended and the level it settled at",
                 "[screensaver][screensaver_gate][screensaver_usage]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "20", 1);
    gate.run(10.0, 0.0);
    clear_usage(gate.mgr);

    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    CHECK(gate.mgr.usage().running());
    gate.run(0.75, 0.9);
    gate.run(5.5, 0.5);
    REQUIRE(gate.running()->level() == 1);
    gate.mgr.stop(helix::SaverRunEnd::TOUCH);

    gate.mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(gate.running() != nullptr);
    gate.mgr.stop();

    const helix::SaverUsageWindow w = usage_now(gate.mgr);
    CHECK(w.starts == std::map<std::string, int>{{"toasters", 2}});
    CHECK(w.woke_by_touch == std::map<std::string, int>{{"toasters", 1}});
    CHECK(w.stopped_other == std::map<std::string, int>{{"toasters", 1}});
    CHECK(w.too_heavy.empty());
    CHECK(w.settled_levels == std::map<std::string, int>{{"toasters:1", 2}});
    CHECK_FALSE(gate.mgr.usage().running());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "usage counts a too-heavy run under its own name and its fallback as a new run",
                 "[screensaver][screensaver_gate][screensaver_usage]") {
    GateHarness gate;
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "10", 1);
    gate.run(10.0, 0.0);
    clear_usage(gate.mgr);

    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* maze = gate.running();
    REQUIRE(maze != nullptr);
    run_past_bottom(gate, *maze);
    REQUIRE(gate.running() != nullptr);
    REQUIRE(gate.running()->type() == helix::ui::kDefaultScreensaverType);
    gate.mgr.stop(helix::SaverRunEnd::TOUCH);

    const std::string fallback =
        helix::ui::find_screensaver(helix::ui::kDefaultScreensaverType)->name;
    const std::string bottom = "maze:" + std::to_string(maze->level_count() - 1);
    const helix::SaverUsageWindow w = usage_now(gate.mgr);
    CHECK(w.starts == std::map<std::string, int>{{"maze", 1}, {fallback, 1}});
    CHECK(w.too_heavy == std::map<std::string, int>{{"maze", 1}});
    CHECK(w.woke_by_touch == std::map<std::string, int>{{fallback, 1}});
    CHECK(w.stopped_other.empty());
    CHECK(w.settled_levels == std::map<std::string, int>{{bottom, 1}, {fallback + ":0", 1}});
}

TEST_CASE_METHOD(LVGLTestFixture, "usage counts the black screen under its own name",
                 "[screensaver][screensaver_gate][screensaver_usage]") {
    GateHarness gate;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    for (const helix::ui::ScreensaverInfo& row : helix::ui::kScreensavers) {
        gate.store(row.name, {0, true, helix_version_full(), board});
    }
    clear_usage(gate.mgr);

    gate.mgr.start(ScreensaverType::MAZE_3D);
    REQUIRE(ScreensaverManagerTestAccess::showing_black_screen(gate.mgr));
    gate.mgr.stop(helix::SaverRunEnd::TOUCH);

    const helix::SaverUsageWindow w = usage_now(gate.mgr);
    CHECK(w.starts == std::map<std::string, int>{{helix::kSaverUsageBlackScreen, 1}});
    CHECK(w.woke_by_touch == std::map<std::string, int>{{helix::kSaverUsageBlackScreen, 1}});
    CHECK(w.settled_levels.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "usage leaves a settings preview out",
                 "[screensaver][screensaver_gate][screensaver_usage]") {
    GateHarness gate;
    clear_usage(gate.mgr);

    gate.mgr.start(ScreensaverType::STARFIELD, /*preview=*/true);
    REQUIRE(gate.running() != nullptr);
    CHECK_FALSE(gate.mgr.usage().running());
    gate.mgr.stop(helix::SaverRunEnd::TOUCH);
    CHECK(usage_now(gate.mgr).empty());

    // The next real run counts again.
    gate.mgr.start(ScreensaverType::STARFIELD);
    REQUIRE(gate.running() != nullptr);
    gate.mgr.stop(helix::SaverRunEnd::TOUCH);
    CHECK(usage_now(gate.mgr).starts == std::map<std::string, int>{{"starfield", 1}});
}

#endif // HELIX_ENABLE_SCREENSAVER

TEST_CASE_METHOD(LVGLTestFixture,
                 "every saver the registry marks for this colour depth is registered",
                 "[screensaver][screensaver_gate][depth_wiring]") {
    // Four hand-kept lists decide this: kScreensavers::depths, the Makefile source filter, and
    // the manager's include and registration guards. A saver missing here is selectable and
    // starts nothing.
    const std::vector<const char*> missing =
        ScreensaverManager::instance().savers_missing_for_build_depth();
    std::string names;
    for (const char* n : missing) {
        names += names.empty() ? n : std::string(", ") + n;
    }
    CAPTURE(names);
    CHECK(missing.empty());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a comfortable run steps the level back up and records it, capped at the top",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    const std::string version = helix_version_full();
    gate.store("maze", {2, false, version, board});
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "100", 1);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* maze = gate.running();
    REQUIRE(maze != nullptr);
    REQUIRE(maze->level() == 2);

    // Two comfortable windows are not enough; the third raises the level.
    gate.run(12.5, 0.5);
    CHECK(maze->level() == 2);
    gate.run(5.5, 0.5);
    CHECK(maze->level() == 1);
    const std::optional<SaverLevelEntry> raised = gate.stored("maze");
    REQUIRE(raised.has_value());
    CHECK(raised->level == 1);
    CHECK_FALSE(raised->too_heavy);

    // Uncomfortable windows (under budget, above comfort) reset the streak -
    // and a rate change shows up in the window that straddles it, so the
    // second 0.8 window is needed before the climb can start clean.
    gate.run(11.0, 0.8);
    gate.run(21.0, 0.5);
    CHECK(maze->level() == 0);
    const std::optional<SaverLevelEntry> top = gate.stored("maze");
    REQUIRE(top.has_value());
    CHECK(top->level == 0);

    // At the top rung the level stays however comfortable the run.
    gate.run(25.0, 0.1);
    CHECK(maze->level() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "a step-up while printing is not recorded",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    gate.printing = true;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    const std::string version = helix_version_full();
    gate.store("maze", {2, false, version, board});
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "100", 1);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* maze = gate.running();
    REQUIRE(maze != nullptr);

    gate.run(18.5, 0.5);
    CHECK(maze->level() == 1);
    const std::optional<SaverLevelEntry> entry = gate.stored("maze");
    REQUIRE(entry.has_value());
    CHECK(entry->level == 2);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a rung that stepped down is not climbed back into for the rest of the run",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    const std::string version = helix_version_full();
    gate.store("maze", {2, false, version, board});
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "100", 1);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* maze = gate.running();
    REQUIRE(maze != nullptr);
    REQUIRE(maze->level() == 2);

    // Comfortable at level 2: climb to 1, which runs over budget and steps back down.
    gate.run(18.0, 0.5);
    REQUIRE(maze->level() == 1);
    gate.run(11.0, 1.2);
    REQUIRE(maze->level() == 2);
    const std::optional<SaverLevelEntry> down = gate.stored("maze");
    REQUIRE(down.has_value());
    REQUIRE(down->level == 2);

    // However long the comfort lasts, level 1 stays closed and nothing is saved again.
    gate.store("maze", {7, false, version, board});
    gate.run(60.0, 0.5);
    CHECK(maze->level() == 2);
    const std::optional<SaverLevelEntry> after = gate.stored("maze");
    REQUIRE(after.has_value());
    CHECK(after->level == 7);

    // A new run measures from the stored level with every rung open again.
    gate.mgr.stop();
    gate.store("maze", {2, false, version, board});
    gate.mgr.start(ScreensaverType::MAZE_3D);
    maze = gate.running();
    REQUIRE(maze != nullptr);
    gate.run(18.0, 0.5);
    CHECK(maze->level() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a raised level earns its own comfort streak before the next raise",
                 "[screensaver][screensaver_gate]") {
    GateHarness gate;
    const std::string board = ScreensaverManagerTestAccess::current_board(gate.mgr);
    const std::string version = helix_version_full();
    gate.store("maze", {2, false, version, board});
    setenv("HELIX_SCREENSAVER_BUDGET_PCT", "100", 1);
    gate.mgr.start(ScreensaverType::MAZE_3D);
    helix::ui::SaverBase* maze = gate.running();
    REQUIRE(maze != nullptr);

    gate.run(18.0, 0.5);
    REQUIRE(maze->level() == 1);

    // One comfortable window at the new level is not three.
    gate.run(5.0, 0.5);
    CHECK(maze->level() == 1);
    gate.run(10.0, 0.5);
    CHECK(maze->level() == 0);
}
