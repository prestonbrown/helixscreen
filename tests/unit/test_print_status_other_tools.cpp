// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_chips.h"
#include "tool_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

OtherToolReading tool(const char* badge, int cur, int tgt, int start = 300) {
    return {badge, cur, tgt, start};
}

bool always(const std::string&) {
    return true;
}

/// A room that holds `n` bytes of text.
std::function<bool(const std::string&)> room(size_t n) {
    return [n](const std::string& t) { return t.size() <= n; };
}

ToolInfo tool_on(int index, const char* extruder) {
    ToolInfo t;
    t.index = index;
    t.name = "T" + std::to_string(index);
    t.extruder_name = extruder;
    return t;
}

} // namespace

TEST_CASE("other tools: no other heads hides the chip", "[print_status_other_tools]") {
    const ChipContent c = other_tools_chip({}, always);
    CHECK_FALSE(c.shown);
    CHECK(other_tools_budget_text({}).empty());
}

TEST_CASE("other tools: with room for every head, the climbing ones lead and set the bar",
          "[print_status_other_tools]") {
    const auto check = [](const std::vector<OtherToolReading>& heads, const char* text,
                          ChipTone tone, int progress_pct) {
        CAPTURE(text);
        const ChipContent chip = other_tools_chip(heads, always);
        REQUIRE(chip.shown);
        CHECK(chip.text == text);
        CHECK(chip.tone == tone);
        CHECK(chip.progress_pct == progress_pct);
        // Only a pre-heat in progress keeps the chip when the row runs short.
        CHECK(chip.expendable == (tone != ChipTone::Active));
        CHECK_FALSE(chip.glow);
    };
    // clang-format off
    check({tool("1", 1500, 1500), tool("3", 250, 0), tool("4", 250, 0)}, "1 150° · 3 off · 4 off", ChipTone::Normal, -1);
    check({tool("2", 250, 0), tool("3", 250, 0)}, "2 off · 3 off", ChipTone::Dim, -1);
    check({tool("2", 1200, 0)}, "2 120°", ChipTone::Normal, -1); // parked but still hot
    check({tool("1", 1500, 1500), tool("3", 1840, 2200, 300), tool("4", 250, 0)}, "3 184 / 220° · 1 150° · 4 off", ChipTone::Active, 81);
    // Two climbing heads both lead; the first sets the bar.
    check({tool("1", 250, 0), tool("2", 1100, 2000, 200), tool("3", 1840, 2200, 300)}, "2 110 / 200° · 3 184 / 220° · 1 off", ChipTone::Active, 50);
    // A head cooling to its standby shows the target and does not lead.
    check({tool("1", 250, 0), tool("2", 2000, 1500)}, "1 off · 2 200 / 150°", ChipTone::Normal, -1);
    check({tool("Left", 1500, 1500), tool("3", 250, 0)}, "Left 150° · 3 off", ChipTone::Normal, -1);
    // clang-format on
}

TEST_CASE("other tools: too narrow, the heads that fit stay in order and the rest count",
          "[print_status_other_tools]") {
    const std::vector<OtherToolReading> t = {tool("1", 1500, 1500), tool("3", 1840, 2200),
                                             tool("4", 250, 0)};
    const std::string full = other_tools_chip(t, always).text;
    const ChipContent c = other_tools_chip(t, room(full.size() - 1));
    CHECK(c.text == "3 184 / 220° · 1 150° · +1");
    CHECK(c.tone == ChipTone::Active);
    CHECK(c.progress_pct >= 0);
    const std::string one = "3 184 / 220° · +2";
    CHECK(other_tools_chip(t, room(one.size())).text == one);
    // A room smaller than even that still shows the climbing head: a pre-heat is never hidden.
    CHECK(other_tools_chip(t, room(3)).text == "3 184 / 220° · +2");
    const ChipContent lone = other_tools_chip({tool("2", 1840, 2200)}, room(1));
    CHECK(lone.text == "2 184 / 220°");
    CHECK(lone.tone == ChipTone::Active);
}

TEST_CASE("other tools: too narrow for two climbing heads keeps as many as fit",
          "[print_status_other_tools]") {
    const std::vector<OtherToolReading> t = {tool("1", 250, 0), tool("2", 1100, 2000),
                                             tool("3", 1840, 2200)};
    const std::string two = "2 110 / 200° · 3 184 / 220° · +1";
    CHECK(other_tools_chip(t, room(two.size())).text == two);
    CHECK(other_tools_chip(t, room(two.size() - 1)).text == "2 110 / 200° · +2");
}

TEST_CASE("other tools: too narrow with none climbing lists what fits, then counts the tools",
          "[print_status_other_tools]") {
    const std::vector<OtherToolReading> t = {tool("1", 1500, 1500), tool("3", 250, 0),
                                             tool("4", 250, 0)};
    CHECK(other_tools_chip(t, room(std::string("1 150° · 3 off · +1").size())).text ==
          "1 150° · 3 off · +1");
    const std::string first = "1 150° · +2";
    CHECK(other_tools_chip(t, room(first.size())).text == first);
    CHECK(other_tools_chip(t, room(first.size() - 1)).text == "+3 tools");
    CHECK(other_tools_chip({tool("2", 250, 0)}, room(1)).text == "+1 tool");
}

TEST_CASE("other tools: the budget is one climbing head at its widest and the count",
          "[print_status_other_tools]") {
    CHECK(other_tools_budget_text({tool("1", 0, 0), tool("10", 0, 0), tool("3", 0, 0)}) ==
          "10 999 / 999° · +2");
    CHECK(other_tools_budget_text({tool("2", 0, 0)}) == "2 999 / 999°");
    // The climbing form the chip falls back to always fits the budget.
    const std::vector<OtherToolReading> t = {tool("1", 250, 0), tool("2", 1840, 2200),
                                             tool("3", 250, 0)};
    const std::string budget = other_tools_budget_text(t);
    CHECK(other_tools_chip(t, room(budget.size())).text.size() <= budget.size());
}

TEST_CASE("other tools: configured names give way to numbers where they do not fit",
          "[print_status_other_tools]") {
    OtherToolReading left = tool("Left Extruder Hotend", 1500, 1500);
    left.number = "2";
    const std::vector<OtherToolReading> t = {left, tool("3", 250, 0)};
    CHECK(other_tools_chip(t, always).text == "Left Extruder Hotend 150° · 3 off");
    const std::string numbered = "2 150° · 3 off";
    CHECK(other_tools_chip(t, room(numbered.size())).text == numbered);
    // The budget measures the number, so a long name never widens the layout's room.
    CHECK(other_tools_budget_text(t) == "2 999 / 999° · +1");
}

TEST_CASE("other tools: a configured tool name is its badge, a generated one the 1-based number",
          "[print_status_other_tools]") {
    CHECK(other_tool_label(ui::ToolNumberStyle::Physical, "Left", 0).badge == "Left");
    CHECK(other_tool_label(ui::ToolNumberStyle::Physical, "T2", 2).badge == "3");
    CHECK(other_tool_label(ui::ToolNumberStyle::Physical, "T0", 0).badge == "1");
}

TEST_CASE("other tools: each head is labeled in the printer's tool number style",
          "[print_status_other_tools][tool_style]") {
    const std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder1"),
                                         tool_on(2, "extruder2"), tool_on(3, "extruder3")};
    const auto others = other_heads(physical_heads(tools), 1, false);
    REQUIRE(others.size() == 3);
    const auto labels = [&](ui::ToolNumberStyle style) {
        std::vector<std::string> out;
        for (const OtherHead& head : others) {
            const OtherToolReading r = other_tool_label(style, tools[head.tool].name, head.tool);
            // The fallback number is empty where it would only repeat the badge.
            CHECK(r.number.empty());
            out.push_back(r.badge);
        }
        return out;
    };
    CHECK(labels(ui::ToolNumberStyle::Gcode) == std::vector<std::string>{"T0", "T2", "T3"});
    CHECK(labels(ui::ToolNumberStyle::Physical) == std::vector<std::string>{"1", "3", "4"});
}

TEST_CASE("other tools: a configured name falls back to the number in the printer's style",
          "[print_status_other_tools][tool_style]") {
    const OtherToolReading gcode = other_tool_label(ui::ToolNumberStyle::Gcode, "Left", 0);
    CHECK(gcode.badge == "Left");
    CHECK(gcode.number == "T0");
    const OtherToolReading physical = other_tool_label(ui::ToolNumberStyle::Physical, "Left", 0);
    CHECK(physical.badge == "Left");
    CHECK(physical.number == "1");
}

TEST_CASE("other heads: each head besides the printing one, once, named by its tool",
          "[print_status_other_tools]") {
    using Heads = std::vector<std::pair<int, std::string>>;
    const auto others = [](const std::vector<ToolInfo>& tools, int active, bool shared = false) {
        Heads out;
        for (const auto& h : other_heads(physical_heads(tools, shared), active, shared)) {
            out.emplace_back(h.tool, h.extruder);
        }
        return out;
    };
    const ToolInfo t0 = tool_on(0, "extruder");
    const ToolInfo t1 = tool_on(1, "extruder1");
    CHECK(others({t0, t1, tool_on(2, "extruder2")}, 1) == Heads{{0, "extruder"}, {2, "extruder2"}});
    CHECK(others({t0}, -1).empty());
    CHECK(others({t0, t1}, -1) == Heads{{0, "extruder"}, {1, "extruder1"}});
    // An AFC/Happy Hare/ERCF unit: four lanes through one nozzle are one head, and a
    // hub of three lanes beside a second head is listed once.
    std::vector<ToolInfo> lanes = {t0, tool_on(1, "extruder"), tool_on(2, "extruder"),
                                   tool_on(3, "extruder")};
    CHECK(others(lanes, 2).empty());
    lanes[3].extruder_name = "extruder1";
    CHECK(others(lanes, 3) == Heads{{0, "extruder"}});
    CHECK(others({t0, tool_on(1, "extruder")}, 0, true).empty()); // one shared heater
    ToolInfo no_extruder = t1;
    no_extruder.extruder_name = std::nullopt;
    CHECK(others({t0, no_extruder}, 0).empty()); // no reading to list
}
