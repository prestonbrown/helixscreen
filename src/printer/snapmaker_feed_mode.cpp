// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_feed_mode.h"

#include <fmt/format.h>

#include "hv/json.hpp"

namespace helix::snapmaker::feed_mode {
namespace {

/// printer.cfg's `_FILAMENT_FEED_VARIABLE` on a stock U1:
/// module_sequence 'left','left','right','right', channel_sequence 1,0,0,1.
const std::array<FeedChannel, kHeadCount>& stock_table() {
    static const std::array<FeedChannel, kHeadCount> table = {
        FeedChannel{"left", 1}, FeedChannel{"left", 0}, FeedChannel{"right", 0},
        FeedChannel{"right", 1}};
    return table;
}

/// Each module has two channels.
constexpr int kChannelsPerModule = 2;

/// A module name goes into the command line as one parameter value, so it
/// must be one word Klipper will not split or misread.
bool is_module_name(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<FeedChannel> stock_channel(int head) {
    if (head < 0 || head >= kHeadCount) {
        return std::nullopt;
    }
    return stock_table()[static_cast<size_t>(head)];
}

int stock_head(const std::string& module, int channel) {
    for (int h = 0; h < kHeadCount; ++h) {
        if (stock_table()[static_cast<size_t>(h)] == FeedChannel{module, channel}) {
            return h;
        }
    }
    return -1;
}

FeedChannelMap stock_channel_map() {
    FeedChannelMap out;
    for (int h = 0; h < kHeadCount; ++h) {
        out[static_cast<size_t>(h)] = stock_channel(h);
    }
    return out;
}

FeedChannelMap read_channel_map(const nlohmann::json* macro_status, const FeedChannelMap& base) {
    FeedChannelMap out;
    const nlohmann::json* modules = nullptr;
    const nlohmann::json* channels = nullptr;
    if (macro_status != nullptr && macro_status->is_object()) {
        auto m = macro_status->find("module_sequence");
        auto c = macro_status->find("channel_sequence");
        if (m != macro_status->end() && m->is_array()) {
            modules = &*m;
        }
        if (c != macro_status->end() && c->is_array()) {
            channels = &*c;
        }
    }
    for (int h = 0; h < kHeadCount; ++h) {
        const auto idx = static_cast<size_t>(h);
        FeedChannel ch = base[idx] ? *base[idx] : *stock_channel(h);
        if (modules != nullptr && idx < modules->size()) {
            const nlohmann::json& m = (*modules)[idx];
            if (m.is_string() && is_module_name(m.get<std::string>())) {
                ch.module = m.get<std::string>();
            }
        }
        if (channels != nullptr && idx < channels->size()) {
            const nlohmann::json& c = (*channels)[idx];
            if (c.is_number_integer() && c.get<int>() >= 0 && c.get<int>() < kChannelsPerModule) {
                ch.channel = c.get<int>();
            }
        }
        out[idx] = std::move(ch);
    }
    return out;
}

std::string auto_mode_gcode(const FeedChannel& channel, bool on) {
    return fmt::format("FEED_AUTO MODULE={} CHANNEL={} AUTO={}", channel.module, channel.channel,
                       on ? 1 : 0);
}

} // namespace helix::snapmaker::feed_mode
