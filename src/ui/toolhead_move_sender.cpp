// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolhead_move_sender.h"

#include "ui_error_reporting.h"

#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix {

std::string clean_gcode_error(const std::string& msg) {
    std::string cleaned = MoonrakerError::friendly_error_line(msg);

    // Provide friendly messages for common error patterns
    if (cleaned.find("Must home axis") != std::string::npos ||
        cleaned.find("must home") != std::string::npos) {
        return lv_tr("Must home axes first");
    }
    // "Move out of range" — let through with truncation so user sees the limits

    // Truncate long messages for toast display
    constexpr size_t kMaxLen = 80;
    if (cleaned.size() > kMaxLen) {
        cleaned = cleaned.substr(0, kMaxLen - 3) + "...";
    }

    return cleaned;
}

ToolheadMoveSender::ToolheadMoveSender(AsyncLifetimeGuard& lifetime, const char* tag, Hooks hooks)
    : lifetime_(lifetime), tag_(tag), hooks_(std::move(hooks)) {}

bool ToolheadMoveSender::send_target(IMoonrakerAPI* api, const AxisTarget& target,
                                     const AxisBounds& clamp_bounds, double current_z) {
    // A set axis without a known envelope cannot be clamped, and sending it unclamped would
    // trust exactly the value that is missing.
    if ((target.x && !clamp_bounds.has_x) || (target.y && !clamp_bounds.has_y) ||
        (target.z && !clamp_bounds.has_z)) {
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return false;
    }
    if (!api) {
        return false;
    }
    std::optional<std::pair<double, double>> z_range;
    if (clamp_bounds.has_z) {
        z_range = std::make_pair(static_cast<double>(clamp_bounds.z_min),
                                 static_cast<double>(clamp_bounds.z_max));
    }
    const AxisTarget clamped = clamp_target_to_bounds(
        target, static_cast<double>(clamp_bounds.x_min), static_cast<double>(clamp_bounds.x_max),
        static_cast<double>(clamp_bounds.y_min), static_cast<double>(clamp_bounds.y_max), z_range);

    target_start_z_ = coalescer_.target_start_z(current_z);
    if (auto immediate = coalescer_.on_target(clamped)) {
        return send(api, *immediate);
    }
    spdlog::debug("[{}] Target coalesced behind the move in flight", tag_);
    return true;
}

bool ToolheadMoveSender::send_delta(IMoonrakerAPI* api, const AxisMove& delta) {
    if (!api) {
        return false;
    }
    if (auto immediate = coalescer_.on_tap(delta)) {
        return send(api, *immediate);
    }
    spdlog::debug("[{}] Jog coalesced behind the move in flight", tag_);
    return true;
}

double ToolheadMoveSender::target_start_z(double current_z) const {
    return coalescer_.target_start_z(current_z);
}

void ToolheadMoveSender::reset() {
    coalescer_.reset();
}

bool ToolheadMoveSender::send(IMoonrakerAPI* api, const JogCoalescer::CoalescedMove& move) {
    if (!api) {
        coalescer_.on_error();
        return false;
    }
    auto& settings = SettingsManager::instance();
    // Storage keeps the user's choice; emission is clamped to what the printer currently
    // permits, or move_relative/move_to would reject the move outright.
    const SafetyLimits& limits = api->get_safety_limits();
    const double xy_feedrate = static_cast<double>(effective_jog_speed_mm_min(
        settings.get_jog_speed_xy(), limits.min_feedrate_mm_min, limits.max_feedrate_mm_min));
    const double z_feedrate = static_cast<double>(effective_jog_speed_mm_min(
        settings.get_jog_speed_z(), limits.min_feedrate_mm_min, limits.max_feedrate_mm_min));

    auto on_ack = lifetime_.bg_cb("ToolheadMoveSender::on_ack", [this, api]() {
        if (auto flush = coalescer_.on_ack()) {
            if (hooks_.may_flush && !hooks_.may_flush(*flush)) {
                spdlog::debug("[{}] Moves no longer allowed, dropping the coalesced move", tag_);
                coalescer_.reset();
            } else {
                send(api, *flush);
            }
        }
        if (hooks_.on_ack) {
            hooks_.on_ack();
        }
    });
    auto on_error =
        lifetime_.bg_cb("ToolheadMoveSender::on_error", [this](const MoonrakerError& err) {
            coalescer_.on_error();
            NOTIFY_ERROR(lv_tr("Jog failed: {}"), clean_gcode_error(err.localized_message()));
            if (hooks_.on_error) {
                hooks_.on_error(err);
            }
        });

    if (const auto* delta = std::get_if<AxisMove>(&move)) {
        api->motion().move_relative(delta->dx, delta->dy, delta->dz, xy_feedrate, z_feedrate,
                                    std::move(on_ack), std::move(on_error));
    } else if (const auto* target = std::get_if<AxisTarget>(&move)) {
        api->motion().move_to(*target, xy_feedrate, z_feedrate, std::move(on_ack),
                              std::move(on_error), target_start_z_);
    }
    return true;
}

} // namespace helix
