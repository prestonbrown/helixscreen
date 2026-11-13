// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The U1's per-head Auto loading: its Print Behavior rows and the FEED_AUTO
// command a row sends (docs/devel/FILAMENT_BACKEND_SNAPMAKER_U1.md, Auto Loading).

#include "ams_backend_snapmaker.h"
#include "ams_device_action_value.h"
#include "json_utils.h"
#include "snapmaker_channel_state.h"
#include "snapmaker_feed_mode.h"
#include "translation_loader.h"

#include "hv/json.hpp"

namespace helix {

bool AmsBackendSnapmaker::auto_load_reported() const {
    for (int h = 0; h < kNumTools; ++h) {
        if (channel_snapshot(h).auto_reported) {
            return true;
        }
    }
    return false;
}

void AmsBackendSnapmaker::append_auto_load_actions(
    std::vector<helix::printer::DeviceAction>& out) const {
    using helix::printer::DeviceAction;
    // Whether each head's feeder takes a spool pushed into its port and feeds it.
    for (int h = 0; h < kNumTools; ++h) {
        const ChannelSnapshot snap = channel_snapshot(h);
        if (!snap.auto_reported) {
            continue;
        }
        DeviceAction a =
            DeviceAction::toggle(std::string(kAutoLoadActionPrefix) + std::to_string(h),
                                 TR_NOOP("Auto loading"), kPrintPrefsSection, !snap.disable_auto);
        a.slot_index = h;
        a.tool_index = h;
        if (snap.module_reported && !snap.module_exist) {
            a.enabled = false;
            a.disable_reason = TR_NOOP("No feeder attached");
        } else if (snapmaker::channel_state_in_progress(snap.state)) {
            // The firmware holds the switch until the feed ends.
            a.enabled = false;
            a.disable_reason = TR_NOOP("Wait for the load or unload to finish");
        }
        out.push_back(std::move(a));
    }
}

std::string AmsBackendSnapmaker::auto_load_gcode(size_t head, const std::any& value) const {
    const auto [on, ok] = printer::device_action_value<bool>(value, "setting");
    if (!ok || head >= static_cast<size_t>(kNumTools)) {
        return {};
    }
    std::optional<snapmaker::feed_mode::FeedChannel> channel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel = feed_channels_[head];
    }
    return channel ? snapmaker::feed_mode::auto_mode_gcode(*channel, on) : std::string{};
}

void AmsBackendSnapmaker::update_feed_channels_locked(const nlohmann::json& status) {
    if (feed_variable_object_.empty()) {
        return;
    }
    if (const nlohmann::json* fv = json_util::find_member(status, feed_variable_object_.c_str())) {
        feed_channels_ = snapmaker::feed_mode::read_channel_map(fv, feed_channels_);
    }
}

} // namespace helix
