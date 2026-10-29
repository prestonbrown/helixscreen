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

TEST_CASE("other tools: none climbing lists every head in order", "[print_status_other_tools]") {
    const ChipContent c =
        other_tools_chip({tool("1", 1500, 1500), tool("3", 250, 0), tool("4", 250, 0)}, always);
    REQUIRE(c.shown);
    CHECK(c.text == "1 150° · 3 off · 4 off");
    CHECK(c.tone == ChipTone::Normal);
    CHECK(c.expendable);
    CHECK(c.progress_pct == -1);
    CHECK_FALSE(c.glow);
}

TEST_CASE("other tools: every head off is Dim", "[print_status_other_tools]") {
    const ChipContent c = other_tools_chip({tool("2", 250, 0), tool("3", 250, 0)}, always);
    CHECK(c.text == "2 off · 3 off");
    CHECK(c.tone == ChipTone::Dim);
    CHECK(c.expendable);
}

TEST_CASE("other tools: a parked head still hot shows its reading", "[print_status_other_tools]") {
    CHECK(other_tools_chip({tool("2", 1200, 0)}, always).text == "2 120°");
}

TEST_CASE("other tools: a climbing head leads with value, target and bar",
          "[print_status_other_tools]") {
    const ChipContent c = other_tools_chip(
        {tool("1", 1500, 1500), tool("3", 1840, 2200, 300), tool("4", 250, 0)}, always);
    REQUIRE(c.shown);
    CHECK(c.text == "3 184 / 220° · 1 150° · 4 off");
    CHECK(c.tone == ChipTone::Active);
    CHECK_FALSE(c.expendable);
    CHECK(c.progress_pct == 81);
}

TEST_CASE("other tools: two climbing heads both lead, the first sets the bar",
          "[print_status_other_tools]") {
    const ChipContent c = other_tools_chip(
        {tool("1", 250, 0), tool("2", 1100, 2000, 200), tool("3", 1840, 2200, 300)}, always);
    CHECK(c.text == "2 110 / 200° · 3 184 / 220° · 1 off");
    CHECK(c.progress_pct == 50);
    CHECK(c.tone == ChipTone::Active);
}

TEST_CASE("other tools: a head cooling to its standby shows the target and does not lead",
          "[print_status_other_tools]") {
    const ChipContent c = other_tools_chip({tool("1", 250, 0), tool("2", 2000, 1500)}, always);
    CHECK(c.text == "1 off · 2 200 / 150°");
    CHECK(c.tone == ChipTone::Normal);
    CHECK(c.expendable);
}

TEST_CASE("other tools: too narrow, the climbing head keeps its place and the rest count",
          "[print_status_other_tools]") {
    const std::vector<OtherToolReading> t = {tool("1", 1500, 1500), tool("3", 1840, 2200),
                                             tool("4", 250, 0)};
    const std::string full = other_tools_chip(t, always).text;
    const ChipContent c = other_tools_chip(t, room(full.size() - 1));
    CHECK(c.text == "3 184 / 220° · +2");
    CHECK(c.tone == ChipTone::Active);
    CHECK(c.progress_pct >= 0);
    // A room smaller than even that still shows the climbing head: a pre-heat is never hidden.
    CHECK(other_tools_chip(t, room(3)).text == "3 184 / 220° · +2");
}

TEST_CASE("other tools: too narrow for two climbing heads keeps as many as fit",
          "[print_status_other_tools]") {
    const std::vector<OtherToolReading> t = {tool("1", 250, 0), tool("2", 1100, 2000),
                                             tool("3", 1840, 2200)};
    const std::string two = "2 110 / 200° · 3 184 / 220° · +1";
    CHECK(other_tools_chip(t, room(two.size())).text == two);
    CHECK(other_tools_chip(t, room(two.size() - 1)).text == "2 110 / 200° · +2");
}

TEST_CASE("other tools: too narrow with none climbing counts the tools",
          "[print_status_other_tools]") {
    const std::vector<OtherToolReading> t = {tool("1", 1500, 1500), tool("3", 250, 0),
                                             tool("4", 250, 0)};
    CHECK(other_tools_chip(t, room(10)).text == "+3 tools");
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

TEST_CASE("other heads: the active head is left out and each head is named by its tool",
          "[print_status_other_tools]") {
    const std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder1"),
                                         tool_on(2, "extruder2")};
    const auto others = other_heads(physical_heads(tools), 1, false);
    REQUIRE(others.size() == 2);
    CHECK(others[0].tool == 0);
    CHECK(others[0].extruder == "extruder");
    CHECK(others[1].tool == 2);
    CHECK(others[1].extruder == "extruder2");
}

TEST_CASE("other heads: nothing mounted lists every head", "[print_status_other_tools]") {
    const std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder1")};
    CHECK(other_heads(physical_heads(tools), -1, false).size() == 2);
}

TEST_CASE("other heads: lanes sharing one extruder are one head", "[print_status_other_tools]") {
    // An AFC/Happy Hare/ERCF unit: four lanes through one nozzle.
    const std::vector<ToolInfo> lanes = {tool_on(0, "extruder"), tool_on(1, "extruder"),
                                         tool_on(2, "extruder"), tool_on(3, "extruder")};
    CHECK(other_heads(physical_heads(lanes), 2, false).empty());

    // Two heads, one fed by a hub of three lanes: the hub head is listed once.
    const std::vector<ToolInfo> mixed = {tool_on(0, "extruder"), tool_on(1, "extruder"),
                                         tool_on(2, "extruder"), tool_on(3, "extruder1")};
    const auto others = other_heads(physical_heads(mixed), 3, false);
    REQUIRE(others.size() == 1);
    CHECK(others[0].extruder == "extruder");
}

TEST_CASE("other heads: heads on one shared heater list none", "[print_status_other_tools]") {
    const std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder")};
    CHECK(other_heads(physical_heads(tools, true), 0, true).empty());
}

TEST_CASE("other heads: a head naming no extruder has no reading to list",
          "[print_status_other_tools]") {
    std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder1")};
    tools[1].extruder_name = std::nullopt;
    CHECK(other_heads(physical_heads(tools), 0, false).empty());
}
