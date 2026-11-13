// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <array>
#include <optional>
#include <string>

/**
 * @file snapmaker_feed_mode.h
 * @brief The U1's per-head Auto Loading switch: which feeder channel serves a
 * head, and the command that turns its automatic mode on or off.
 *
 * Each feeder module (`filament_feed left` / `filament_feed right`) drives two
 * channels, and each channel feeds one head. The firmware's own macros find a
 * head's module and channel in `gcode_macro _FILAMENT_FEED_VARIABLE`
 * (`module_sequence`, `channel_sequence`, indexed by head). A head that macro
 * does not describe falls back to the stock wiring.
 *
 * `FEED_AUTO MODULE=<m> CHANNEL=<c> AUTO=<0|1>` sets the channel's automatic
 * mode and saves it to the feeder's config file, so it survives a reboot. The
 * channel reports it back as `disable_auto` in the feeder's status. With it off
 * the firmware ignores a spool pushed into the port and answers a load OK
 * without feeding.
 *
 * Pure: no I/O, no LVGL, no Moonraker. The backend owns the plumbing.
 */
namespace helix::snapmaker::feed_mode {

/// The macro whose variables map heads to feeder channels.
inline constexpr const char* kFeedVariableMacro = "_FILAMENT_FEED_VARIABLE";

/// Heads the U1 has.
inline constexpr int kHeadCount = 4;

/// One feeder channel: the module's name as FEED_AUTO's MODULE= takes it, and
/// the channel index within that module.
struct FeedChannel {
    std::string module;
    int channel = 0;

    bool operator==(const FeedChannel& o) const {
        return module == o.module && channel == o.channel;
    }
};

using FeedChannelMap = std::array<std::optional<FeedChannel>, kHeadCount>;

/// The stock wiring for @p head, or nullopt for a head the U1 does not have.
[[nodiscard]] std::optional<FeedChannel> stock_channel(int head);

/// The head the stock wiring feeds from @p module's @p channel, or -1.
[[nodiscard]] int stock_head(const std::string& module, int channel);

/// Every head on the stock wiring.
[[nodiscard]] FeedChannelMap stock_channel_map();

/// @p base with what @p macro_status (the macro's status object, or a delta of
/// it; null when the printer has none) states laid over it. Status frames are
/// deltas, so a list the frame omits, and an entry it states unusably, keep
/// @p base's value for that head.
[[nodiscard]] FeedChannelMap read_channel_map(const nlohmann::json* macro_status,
                                              const FeedChannelMap& base = stock_channel_map());

/// The command that sets @p channel's automatic mode.
[[nodiscard]] std::string auto_mode_gcode(const FeedChannel& channel, bool on);

} // namespace helix::snapmaker::feed_mode
