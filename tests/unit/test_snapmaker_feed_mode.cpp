// tests/unit/test_snapmaker_feed_mode.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_feed_mode.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::snapmaker::feed_mode;

TEST_CASE("feed mode: the stock wiring names each head's module and channel",
          "[snapmaker][feed_mode]") {
    CHECK(stock_channel(0) == FeedChannel{"left", 1});
    CHECK(stock_channel(1) == FeedChannel{"left", 0});
    CHECK(stock_channel(2) == FeedChannel{"right", 0});
    CHECK(stock_channel(3) == FeedChannel{"right", 1});
    CHECK_FALSE(stock_channel(-1).has_value());
    CHECK_FALSE(stock_channel(4).has_value());
}

TEST_CASE("feed mode: the macro's sequences decide the map", "[snapmaker][feed_mode]") {
    const nlohmann::json macro = {{"module_sequence", {"right", "right", "left", "left"}},
                                  {"channel_sequence", {0, 1, 1, 0}}};
    const FeedChannelMap map = read_channel_map(&macro);
    CHECK(map[0] == FeedChannel{"right", 0});
    CHECK(map[1] == FeedChannel{"right", 1});
    CHECK(map[2] == FeedChannel{"left", 1});
    CHECK(map[3] == FeedChannel{"left", 0});
}

TEST_CASE("feed mode: a head the macro leaves out or misstates takes the stock wiring",
          "[snapmaker][feed_mode]") {
    SECTION("no macro") {
        const FeedChannelMap map = read_channel_map(nullptr);
        for (int h = 0; h < kHeadCount; ++h) {
            CHECK(map[static_cast<size_t>(h)] == stock_channel(h));
        }
    }
    SECTION("a macro with neither variable") {
        const nlohmann::json macro = nlohmann::json::object();
        const FeedChannelMap map = read_channel_map(&macro);
        CHECK(map[0] == stock_channel(0));
        CHECK(map[3] == stock_channel(3));
    }
    SECTION("short lists cover only the heads they reach") {
        const nlohmann::json macro = {{"module_sequence", {"right"}}, {"channel_sequence", {0, 1}}};
        const FeedChannelMap map = read_channel_map(&macro);
        CHECK(map[0] == FeedChannel{"right", 0});
        CHECK(map[1] == FeedChannel{"left", 1});
        CHECK(map[2] == stock_channel(2));
    }
    SECTION("a delta naming one list keeps the other from the map it updates") {
        const nlohmann::json first = {{"module_sequence", {"right", "right", "left", "left"}},
                                      {"channel_sequence", {0, 1, 1, 0}}};
        const nlohmann::json delta = {{"channel_sequence", {1, 0, 0, 1}}};
        const FeedChannelMap map = read_channel_map(&delta, read_channel_map(&first));
        CHECK(map[0] == FeedChannel{"right", 1});
        CHECK(map[3] == FeedChannel{"left", 1});
    }
    SECTION("unusable entries fall back one head at a time") {
        const nlohmann::json macro = {{"module_sequence", {"left x", "", 7, "right"}},
                                      {"channel_sequence", {1, 0, 0, 2}}};
        const FeedChannelMap map = read_channel_map(&macro);
        CHECK(map[0] == stock_channel(0)); // a space would split the parameter
        CHECK(map[1] == stock_channel(1)); // empty module
        CHECK(map[2] == stock_channel(2)); // not a string
        CHECK(map[3] == stock_channel(3)); // channel out of range
    }
}

TEST_CASE("feed mode: the command names the module, the channel and the mode",
          "[snapmaker][feed_mode]") {
    const int head = GENERATE(0, 1, 2, 3);
    const bool on = GENERATE(true, false);
    const FeedChannel ch = *stock_channel(head);
    CHECK(auto_mode_gcode(ch, on) == "FEED_AUTO MODULE=" + ch.module + " CHANNEL=" +
                                         std::to_string(ch.channel) + " AUTO=" + (on ? "1" : "0"));
    CHECK(auto_mode_gcode({"left", 1}, true) == "FEED_AUTO MODULE=left CHANNEL=1 AUTO=1");
    CHECK(auto_mode_gcode({"right", 0}, false) == "FEED_AUTO MODULE=right CHANNEL=0 AUTO=0");
}
