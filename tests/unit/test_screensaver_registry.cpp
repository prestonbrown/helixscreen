// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_registry.h"
#include "screensaver_setting.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::kScreensaverCount;
using helix::ui::kScreensavers;

namespace {

/// Value of `attr` on the self-closing element whose name is `name`, with &#10; decoded to a
/// newline, or "" when there is no such element or attribute.
std::string attribute_of_named(const std::string& xml, const std::string& name,
                               const std::string& attr) {
    const size_t at = xml.find("name=\"" + name + "\"");
    if (at == std::string::npos) {
        return "";
    }
    const size_t open = xml.rfind('<', at);
    const size_t close = xml.find("/>", at);
    if (open == std::string::npos || close == std::string::npos) {
        return "";
    }
    const std::string element = xml.substr(open, close - open);
    const std::regex pattern("\\s" + attr + "=\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(element, match, pattern)) {
        return "";
    }
    std::string value = match[1].str();
    for (size_t pos = value.find("&#10;"); pos != std::string::npos;
         pos = value.find("&#10;", pos)) {
        value.replace(pos, 5, "\n");
        pos += 1;
    }
    return value;
}

std::vector<std::string> split_option_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST_CASE("screensaver registry rows run in type order from 1 with unique names",
          "[screensaver][screensaver_registry]") {
    REQUIRE(kScreensaverCount > 0);
    std::set<std::string> names;
    for (size_t i = 0; i < kScreensaverCount; i++) {
        CAPTURE(i);
        CHECK(static_cast<int>(kScreensavers[i].type) == static_cast<int>(i) + 1);
        CHECK_FALSE(std::string(kScreensavers[i].name).empty());
        CHECK_FALSE(std::string(kScreensavers[i].label_key).empty());
        CHECK(kScreensavers[i].depths != 0);
        names.insert(kScreensavers[i].name);
    }
    CHECK(names.size() == kScreensaverCount);
    CHECK(helix::ui::screensaver_last_type() == static_cast<int>(kScreensaverCount));
}

TEST_CASE("an out-of-range screensaver type clamps to the last type",
          "[screensaver][screensaver_registry]") {
    const int last = static_cast<int>(kScreensaverCount);
    CHECK(helix::ui::clamp_screensaver_type(-1) == 0);
    CHECK(helix::ui::clamp_screensaver_type(0) == 0);
    CHECK(helix::ui::clamp_screensaver_type(1) == 1);
    CHECK(helix::ui::clamp_screensaver_type(last) == last);
    CHECK(helix::ui::clamp_screensaver_type(last + 1) == last);
    CHECK(helix::ui::clamp_screensaver_type(99) == last);
}

TEST_CASE("Random is a stored choice outside the registry rows",
          "[screensaver][screensaver_registry]") {
    const int random = static_cast<int>(ScreensaverType::RANDOM);
    CHECK(helix::ui::clamp_screensaver_type(random) == random);
    CHECK(helix::ui::clamp_screensaver_type(random + 1) == helix::ui::screensaver_last_type());
    CHECK(helix::ui::find_screensaver(ScreensaverType::RANDOM) == nullptr);
    CHECK(helix::ui::screensaver_label_key(ScreensaverType::RANDOM) ==
          std::string(helix::ui::kScreensaverRandomLabelKey));
    CHECK(helix::ui::screensaver_label_key(ScreensaverType::OFF) ==
          std::string(helix::ui::kScreensaverOffLabelKey));
    CHECK(helix::ui::screensaver_label_key(ScreensaverType::MAZE_3D) == std::string("3D Maze"));
    const nlohmann::json stored = "random";
    CHECK(helix::ui::screensaver_from_setting(&stored, ScreensaverType::STARFIELD) ==
          ScreensaverType::RANDOM);
}

TEST_CASE("every registered screensaver name resolves to its row",
          "[screensaver][screensaver_registry]") {
    for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
        CAPTURE(info.name);
        const helix::ui::ScreensaverInfo* by_name = helix::ui::find_screensaver_by_name(info.name);
        REQUIRE(by_name != nullptr);
        CHECK(by_name->type == info.type);
        CHECK(helix::ui::find_screensaver(info.type) == by_name);
        CHECK(helix::ui::resolve_screensaver_now(info.name, ScreensaverType::OFF) == info.type);
    }
    CHECK(helix::ui::find_screensaver_by_name("") == nullptr);
    CHECK(helix::ui::find_screensaver_by_name("Starfield") == nullptr);
    CHECK(helix::ui::find_screensaver(ScreensaverType::OFF) == nullptr);
}

TEST_CASE("HELIX_SCREENSAVER_NOW falls back to the configured saver, then flying toasters",
          "[screensaver][screensaver_registry]") {
    using helix::ui::resolve_screensaver_now;
    CHECK(resolve_screensaver_now("1", ScreensaverType::STARFIELD) == ScreensaverType::STARFIELD);
    CHECK(resolve_screensaver_now("bogus", ScreensaverType::PIPES_3D) == ScreensaverType::PIPES_3D);
    CHECK(resolve_screensaver_now("1", ScreensaverType::OFF) == ScreensaverType::FLYING_TOASTERS);
    CHECK(resolve_screensaver_now("random", ScreensaverType::OFF) == ScreensaverType::RANDOM);
    CHECK(resolve_screensaver_now("1", ScreensaverType::RANDOM) == ScreensaverType::RANDOM);
}

TEST_CASE(
    "the settings dropdown lists Off, every registered screensaver in type order, then Random",
    "[screensaver][screensaver_registry]") {
    std::ifstream file("ui_xml/display_screensaver_row.xml");
    REQUIRE(file.is_open()); // helix-tests runs from the repo root
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string xml = buffer.str();

    std::vector<std::string> expected{helix::ui::kScreensaverOffLabelKey};
    for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
        expected.emplace_back(info.label_key);
    }
    expected.emplace_back(helix::ui::kScreensaverRandomLabelKey);
    CHECK(split_option_lines(attribute_of_named(xml, "row_screensaver", "options")) == expected);
    CHECK(split_option_lines(attribute_of_named(xml, "row_screensaver", "options_tag")) ==
          expected);
}

TEST_CASE("every screensaver label the dropdown can show has an en.xml translation",
          "[screensaver][screensaver_registry]") {
    std::ifstream file("ui_xml/translations/en.xml");
    REQUIRE(file.is_open()); // helix-tests runs from the repo root
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string xml = buffer.str();

    // en.xml is the master list the other locales are generated from, so a saver
    // missing here ships untranslated everywhere.
    std::vector<std::string> tags{helix::ui::kScreensaverOffLabelKey};
    for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
        tags.emplace_back(info.label_key);
    }
    tags.emplace_back(helix::ui::kScreensaverRandomLabelKey);
    for (const std::string& tag : tags) {
        CAPTURE(tag);
        CHECK(xml.find("<translation tag=\"" + tag + "\"") != std::string::npos);
    }
}

TEST_CASE("fresh installs default to the flip clock, a registered saver",
          "[screensaver][screensaver_registry]") {
    CHECK(helix::ui::kDefaultScreensaverType == ScreensaverType::FLIP_CLOCK);
    CHECK(helix::ui::find_screensaver(helix::ui::kDefaultScreensaverType) != nullptr);
}

TEST_CASE("the fresh-install default is a saver this build can actually draw",
          "[screensaver][screensaver_registry]") {
    using helix::ui::default_screensaver_type;
    using helix::ui::find_screensaver;
    using helix::ui::SAVER_DEPTH_16;
    using helix::ui::SAVER_DEPTH_32;

    SECTION("a 32 bpp build gets the preferred default") {
        CHECK(default_screensaver_type(SAVER_DEPTH_32) == helix::ui::kDefaultScreensaverType);
    }

    SECTION("a 16 bpp build does not get a 32 bpp-only saver") {
        const ScreensaverType chosen = default_screensaver_type(SAVER_DEPTH_16);
        REQUIRE(chosen != ScreensaverType::OFF);
        const helix::ui::ScreensaverInfo* row = find_screensaver(chosen);
        REQUIRE(row != nullptr);
        CAPTURE(row->name);
        CHECK((row->depths & SAVER_DEPTH_16) != 0U);
    }

    SECTION("whatever is chosen draws at the depth it was chosen for") {
        for (const uint8_t depth : {SAVER_DEPTH_16, SAVER_DEPTH_32}) {
            CAPTURE(depth);
            const ScreensaverType chosen = default_screensaver_type(depth);
            const helix::ui::ScreensaverInfo* row = find_screensaver(chosen);
            REQUIRE(row != nullptr);
            CAPTURE(row->name);
            CHECK((row->depths & depth) != 0U);
        }
    }
}

namespace {

/// A too-heavy predicate that rejects exactly `heavy`.
struct HeavySet {
    std::set<ScreensaverType> heavy;
    bool operator()(ScreensaverType type) const {
        return heavy.count(type) != 0;
    }
};

} // namespace

TEST_CASE("a too-heavy saver falls back to the default, then the first runnable registry saver",
          "[screensaver][screensaver_registry]") {
    using helix::ui::kDefaultScreensaverType;
    using helix::ui::runnable_screensaver;

    SECTION("a saver the board can run runs itself") {
        CHECK(runnable_screensaver(ScreensaverType::MAZE_3D, HeavySet{}) ==
              ScreensaverType::MAZE_3D);
        CHECK(runnable_screensaver(ScreensaverType::MAZE_3D,
                                   HeavySet{{ScreensaverType::HYPERSPACE}}) ==
              ScreensaverType::MAZE_3D);
    }
    SECTION("a too-heavy saver gives way to the default") {
        CHECK(
            runnable_screensaver(ScreensaverType::MAZE_3D, HeavySet{{ScreensaverType::MAZE_3D}}) ==
            kDefaultScreensaverType);
        // The preferred saver comes before registry order wherever it sits in the registry.
        CHECK(runnable_screensaver(ScreensaverType::MAZE_3D, HeavySet{{ScreensaverType::MAZE_3D}},
                                   ScreensaverType::FLIP_CLOCK) == ScreensaverType::FLIP_CLOCK);
    }
    SECTION("with the default too heavy as well, the first registry saver that is not") {
        const HeavySet heavy{{ScreensaverType::MAZE_3D, kDefaultScreensaverType}};
        ScreensaverType first = ScreensaverType::OFF;
        for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
            if (!heavy(info.type)) {
                first = info.type;
                break;
            }
        }
        REQUIRE(first != ScreensaverType::OFF);
        CHECK(runnable_screensaver(ScreensaverType::MAZE_3D, heavy) == first);
        // The configured saver is skipped in registry order too, not just at its own row.
        HeavySet early{{kScreensavers[0].type, kScreensavers[1].type, kDefaultScreensaverType}};
        CHECK(runnable_screensaver(kScreensavers[1].type, early) == kScreensavers[2].type);
    }
    SECTION("with every saver too heavy, the configured one stays for the black screen") {
        HeavySet all;
        for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
            all.heavy.insert(info.type);
        }
        CHECK(runnable_screensaver(ScreensaverType::MAZE_3D, all) == ScreensaverType::MAZE_3D);
    }
    SECTION("Off stays off whatever is too heavy") {
        CHECK(runnable_screensaver(ScreensaverType::OFF, HeavySet{{kDefaultScreensaverType}}) ==
              ScreensaverType::OFF);
    }
}

TEST_CASE("the screensaver dropdown offers Off and every saver that is not too heavy",
          "[screensaver][screensaver_registry]") {
    using helix::ui::screensaver_choices;

    SECTION("nothing too heavy: Off, the whole registry so the index is the type, then Random") {
        const std::vector<ScreensaverType> choices =
            screensaver_choices(HeavySet{}, ScreensaverType::FLYING_TOASTERS);
        REQUIRE(choices.size() == kScreensaverCount + 2);
        for (size_t i = 0; i + 1 < choices.size(); i++) {
            CHECK(static_cast<size_t>(choices[i]) == i);
        }
        CHECK(choices.back() == ScreensaverType::RANDOM);
    }
    SECTION("too-heavy savers are left out, the rest keep registry order") {
        const std::vector<ScreensaverType> choices =
            screensaver_choices(HeavySet{{ScreensaverType::MAZE_3D, ScreensaverType::HYPERSPACE}},
                                ScreensaverType::MAZE_3D);
        CHECK(choices.size() == kScreensaverCount);
        CHECK(choices.front() == ScreensaverType::OFF);
        CHECK(choices.back() == ScreensaverType::RANDOM);
        CHECK(std::find(choices.begin(), choices.end(), ScreensaverType::MAZE_3D) == choices.end());
        CHECK(std::find(choices.begin(), choices.end(), ScreensaverType::HYPERSPACE) ==
              choices.end());
        CHECK(std::is_sorted(choices.begin(), choices.end()));
    }
    SECTION("every saver too heavy keeps the configured one, so choosing Off is a change") {
        HeavySet all;
        for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
            all.heavy.insert(info.type);
        }
        CHECK(screensaver_choices(all, ScreensaverType::MAZE_3D) ==
              std::vector<ScreensaverType>{ScreensaverType::OFF, ScreensaverType::MAZE_3D});
        CHECK(screensaver_choices(all, ScreensaverType::OFF) ==
              std::vector<ScreensaverType>{ScreensaverType::OFF});
    }
    SECTION("every saver too heavy drops Random unless it is the stored choice") {
        HeavySet all;
        for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
            all.heavy.insert(info.type);
        }
        CHECK(screensaver_choices(all, ScreensaverType::RANDOM) ==
              std::vector<ScreensaverType>{ScreensaverType::OFF, ScreensaverType::RANDOM});
    }
    SECTION("a stored Random is selected as itself") {
        const std::vector<ScreensaverType> choices =
            screensaver_choices(HeavySet{{ScreensaverType::MAZE_3D}}, ScreensaverType::RANDOM);
        CHECK(helix::ui::runnable_screensaver(ScreensaverType::RANDOM,
                                              HeavySet{{ScreensaverType::MAZE_3D}}) ==
              ScreensaverType::RANDOM);
        CHECK(helix::ui::screensaver_choice_index(choices, ScreensaverType::RANDOM) ==
              choices.size() - 1);
    }
}

TEST_CASE("a Random run picks a runnable saver other than the last one",
          "[screensaver][screensaver_registry][screensaver_random]") {
    using helix::ui::random_screensaver;

    SECTION("every roll lands on a registered saver the board can run, never the previous") {
        const HeavySet heavy{{ScreensaverType::MAZE_3D, ScreensaverType::HYPERSPACE}};
        std::set<ScreensaverType> seen;
        for (uint32_t roll = 0; roll < 4 * kScreensaverCount; roll++) {
            CAPTURE(roll);
            const ScreensaverType pick = random_screensaver(heavy, roll, ScreensaverType::AQUA);
            REQUIRE(helix::ui::find_screensaver(pick) != nullptr);
            CHECK_FALSE(heavy(pick));
            CHECK(pick != ScreensaverType::AQUA);
            seen.insert(pick);
        }
        // Rolls reach every candidate: the registry less two heavy savers and the previous one.
        CHECK(seen.size() == kScreensaverCount - 3);
    }
    SECTION("the previous saver repeats when it is the only one the board can run") {
        HeavySet heavy;
        for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
            if (info.type != ScreensaverType::STARFIELD) {
                heavy.heavy.insert(info.type);
            }
        }
        CHECK(random_screensaver(heavy, 7, ScreensaverType::STARFIELD) ==
              ScreensaverType::STARFIELD);
        CHECK(random_screensaver(heavy, 7, ScreensaverType::OFF) == ScreensaverType::STARFIELD);
    }
    SECTION("with every saver too heavy, the default, which the manager blacks out") {
        HeavySet all;
        for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
            all.heavy.insert(info.type);
        }
        CHECK(random_screensaver(all, 3, ScreensaverType::OFF) ==
              helix::ui::kDefaultScreensaverType);
    }
}

TEST_CASE("the dropdown selects the saver that actually runs",
          "[screensaver][screensaver_registry]") {
    using helix::ui::screensaver_choice_index;
    const std::vector<ScreensaverType> choices{
        ScreensaverType::OFF, ScreensaverType::FLYING_TOASTERS, ScreensaverType::FLIP_CLOCK};
    CHECK(screensaver_choice_index(choices, ScreensaverType::OFF) == 0);
    CHECK(screensaver_choice_index(choices, ScreensaverType::FLIP_CLOCK) == 2);

    HeavySet all;
    for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
        all.heavy.insert(info.type);
    }
    const std::vector<ScreensaverType> all_heavy =
        helix::ui::screensaver_choices(all, ScreensaverType::FLYING_TOASTERS);
    const ScreensaverType runs =
        helix::ui::runnable_screensaver(ScreensaverType::FLYING_TOASTERS, all);
    CHECK(screensaver_choice_index(all_heavy, runs) == 1);
}

TEST_CASE("a stored screensaver name maps to its saver whatever its case",
          "[screensaver][screensaver_registry][screensaver_setting]") {
    using helix::ui::screensaver_from_name;
    using helix::ui::screensaver_name;
    for (const helix::ui::ScreensaverInfo& info : kScreensavers) {
        CAPTURE(info.name);
        CHECK(screensaver_name(info.type) == std::string(info.name));
        CHECK(screensaver_from_name(info.name) == info.type);
    }
    CHECK(screensaver_name(ScreensaverType::OFF) == std::string("off"));
    CHECK(screensaver_from_name("off") == ScreensaverType::OFF);
    CHECK(screensaver_from_name("OFF") == ScreensaverType::OFF);
    CHECK(screensaver_name(ScreensaverType::RANDOM) == std::string("random"));
    CHECK(screensaver_from_name("random") == ScreensaverType::RANDOM);
    CHECK(screensaver_from_name("Random") == ScreensaverType::RANDOM);
    CHECK(screensaver_from_name("Maze") == ScreensaverType::MAZE_3D);
    CHECK(screensaver_from_name("FLIPCLOCK") == ScreensaverType::FLIP_CLOCK);
    CHECK_FALSE(screensaver_from_name("3D Maze").has_value());
    CHECK_FALSE(screensaver_from_name("").has_value());
    CHECK_FALSE(screensaver_from_name("mazes").has_value());
}

TEST_CASE("legacy screensaver type numbers keep the meaning they were written with",
          "[screensaver][screensaver_registry][screensaver_setting]") {
    // Literal, not derived from ScreensaverType or kScreensavers: reordering the registry, adding
    // a saver or compiling one out must never change what an old stored number selects.
    const std::vector<std::string> written{"off",        "toasters",  "starfield", "pipes",
                                           "bounce",     "fireworks", "logo",      "maze",
                                           "hyperspace", "flipclock"};
    REQUIRE(std::size(helix::ui::kLegacyScreensaverTypeNames) == written.size());
    for (size_t i = 0; i < written.size(); i++) {
        CAPTURE(i);
        REQUIRE(helix::ui::legacy_screensaver_name(static_cast<long long>(i)) != nullptr);
        CHECK(helix::ui::legacy_screensaver_name(static_cast<long long>(i)) == written[i]);
        const nlohmann::json number = static_cast<int>(i);
        CHECK(helix::ui::screensaver_name(helix::ui::screensaver_from_setting(
                  &number, ScreensaverType::STARFIELD)) == written[i]);
    }
    CHECK(helix::ui::legacy_screensaver_name(-1) == nullptr);
    CHECK(helix::ui::legacy_screensaver_name(static_cast<long long>(written.size())) == nullptr);
}

TEST_CASE("a stored screensaver setting reads leniently and falls back to the default",
          "[screensaver][screensaver_registry][screensaver_setting]") {
    using helix::ui::screensaver_from_setting;
    const ScreensaverType fallback = ScreensaverType::FLYING_TOASTERS;
    const auto read = [&](const nlohmann::json& value) {
        return screensaver_from_setting(&value, fallback);
    };
    SECTION("a name, in any case") {
        CHECK(read("maze") == ScreensaverType::MAZE_3D);
        CHECK(read("Hyperspace") == ScreensaverType::HYPERSPACE);
        CHECK(read("off") == ScreensaverType::OFF);
    }
    SECTION("a legacy integer") {
        CHECK(read(7) == ScreensaverType::MAZE_3D);
        CHECK(read(0) == ScreensaverType::OFF);
    }
    SECTION("an unknown name, an out-of-range number or another JSON type") {
        CHECK(read("lava lamp") == fallback);
        CHECK(read(10) == fallback);
        CHECK(read(-1) == fallback);
        CHECK(read(7.0) == fallback);
        CHECK(read(true) == fallback);
        CHECK(read(nlohmann::json::array()) == fallback);
        CHECK(read(nullptr) == fallback);
        CHECK(screensaver_from_setting(nullptr, fallback) == fallback);
    }
}

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_manager_test_access.h"
#include "screensaver.h"
#include "screensaver_canvas.h"

TEST_CASE_METHOD(LVGLTestFixture, "every registered saver this build draws starts from the manager",
                 "[screensaver][screensaver_registry]") {
    helix::ScopedGateEnv gate_env;

    auto& mgr = ScreensaverManager::instance();
    size_t started = 0;
    for (const helix::ui::ScreensaverInfo& info : helix::ui::kScreensavers) {
        CAPTURE(info.name);
        if ((info.depths & helix::ui::kSaverBuildDepth) == 0) {
            continue;
        }
        mgr.start(info.type);
        CHECK(mgr.is_active());
        // Every registered saver runs on SaverBase, so the gate can measure it.
        CHECK(helix::ScreensaverManagerTestAccess::active(mgr) != nullptr);
        started += 1;
        mgr.stop();
    }
    CHECK(started > 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Random starts a real saver and a different one next run",
                 "[screensaver][screensaver_random]") {
    helix::ScopedGateEnv gate_env;

    auto& mgr = ScreensaverManager::instance();
    mgr.start(ScreensaverType::RANDOM);
    REQUIRE(mgr.is_active());
    const ScreensaverType first = mgr.running_type();
    REQUIRE(helix::ui::find_screensaver(first) != nullptr);

    // Another start while it runs keeps the saver already drawn.
    mgr.start(ScreensaverType::RANDOM);
    CHECK(mgr.running_type() == first);
    mgr.stop();
    CHECK(mgr.running_type() == ScreensaverType::OFF);

    mgr.start(ScreensaverType::RANDOM);
    REQUIRE(mgr.is_active());
    CHECK(helix::ui::find_screensaver(mgr.running_type()) != nullptr);
    CHECK(mgr.running_type() != first);
    mgr.stop();
}

#endif // HELIX_ENABLE_SCREENSAVER
