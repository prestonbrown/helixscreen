// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cold_pull_flow.h"

#include "filament_op_router.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace helix {

namespace {

bool within(int a, int b, int tolerance) {
    return std::abs(a - b) <= tolerance;
}

} // namespace

int cold_pull_ring_percent(ColdPullStage stage, int from_c, int current_c, int goal_c) {
    if (stage == ColdPullStage::Feed || stage == ColdPullStage::Pull || goal_c == from_c) {
        return 100;
    }
    return std::clamp((current_c - from_c) * 100 / (goal_c - from_c), 0, 100);
}

ColdPullFlow::ColdPullFlow(ColdPullParams params) : params_(std::move(params)) {}

bool ColdPullFlow::ended() const {
    return stage_ == ColdPullStage::Finished || stage_ == ColdPullStage::Cancelled ||
           stage_ == ColdPullStage::Abandoned;
}

void ColdPullFlow::track_command(ColdPullActions& a, InFlight kind) {
    in_flight_ = kind;
    in_flight_command_ = ++last_command_;
    a.command_id = in_flight_command_;
}

void ColdPullFlow::clear_in_flight() {
    in_flight_ = InFlight::None;
    in_flight_command_ = 0;
}

GuidedLoadFlow ColdPullFlow::new_feed() const {
    // Filament already in the nozzle has nothing to push: only the purge is left.
    return GuidedLoadFlow(hand_feed_steps(),
                          /*waits_for_push=*/feed_mode() != ColdPullFeedMode::Loaded);
}

bool ColdPullFlow::live() const {
    return stage_ != ColdPullStage::Setup && !ended();
}

bool ColdPullFlow::busy() const {
    return in_flight_ != InFlight::None;
}

int ColdPullFlow::goal_c() const {
    switch (stage_) {
    case ColdPullStage::Heating:
    case ColdPullStage::Feed:
        return sent_target_c_.value_or(params_.temps.soak_c);
    case ColdPullStage::Cooling:
    case ColdPullStage::Pull:
        return params_.temps.pull_c;
    default:
        return 0;
    }
}

GuidedLoadStage ColdPullFlow::feed_stage() const {
    return feed_ ? feed_->stage() : GuidedLoadStage::AwaitPush;
}

ColdPullFeedMode ColdPullFlow::feed_mode() const {
    if (!params_.toolhead_loaded) {
        return ColdPullFeedMode::ByHand;
    }
    return filament_out_ ? ColdPullFeedMode::Refeed : ColdPullFeedMode::Loaded;
}

std::optional<int> ColdPullFlow::owned_target_c() const {
    return sent_target_c_;
}

ColdPullActions ColdPullFlow::exit_actions(bool job_holds_machine) const {
    ColdPullActions a;
    if (job_holds_machine) {
        return a;
    }
    if (sent_target_c_ && *sent_target_c_ != 0) {
        a.heater_target_c = 0;
    }
    if (fan_changed_ && !params_.part_fan.empty()) {
        a.fan_percent = params_.prior_fan_percent;
    }
    return a;
}

ColdPullActions ColdPullFlow::end(ColdPullStage stage, ColdPullEndReason reason,
                                  bool send_cleanup) {
    if (ended()) {
        return {};
    }
    ColdPullActions a = send_cleanup ? exit_actions(false) : ColdPullActions{};
    stage_ = stage;
    end_reason_ = reason;
    sent_target_c_.reset();
    prev_target_c_.reset();
    fan_changed_ = false;
    clear_in_flight();
    return a;
}

ColdPullActions ColdPullFlow::start(TimePoint now) {
    if (stage_ != ColdPullStage::Setup) {
        return {};
    }
    stage_started_ = now;
    stage_ = ColdPullStage::Preparing;
    ColdPullActions a;
    // Nothing moves an unhomed machine on its own: homing is the user's tap.
    if (params_.homed && params_.has_park_macro) {
        a.run_park = true;
        parked_ = true;
    }
    return a;
}

ColdPullActions ColdPullFlow::home_and_park(TimePoint /*now*/) {
    if (stage_ != ColdPullStage::Preparing || !params_.has_park_macro || parked_) {
        return {};
    }
    parked_ = true;
    ColdPullActions a;
    a.home_then_park = true;
    return a;
}

ColdPullActions ColdPullFlow::toolhead_ready(TimePoint now) {
    if (stage_ != ColdPullStage::Preparing) {
        return {};
    }
    return enter_heating(now);
}

ColdPullActions ColdPullFlow::enter_heating(TimePoint now) {
    stage_ = ColdPullStage::Heating;
    stage_started_ = now;
    prev_target_c_ = sent_target_c_;
    sent_target_c_ = params_.temps.soak_c;
    target_seen_ = false;
    feed_.reset();
    ColdPullActions a;
    a.heater_target_c = params_.temps.soak_c;
    a.keep_previous_hot = true;
    return a;
}

ColdPullActions ColdPullFlow::enter_cooling(TimePoint now) {
    stage_ = ColdPullStage::Cooling;
    stage_started_ = now;
    prev_target_c_ = sent_target_c_;
    sent_target_c_ = params_.temps.pull_c;
    target_seen_ = false;
    was_above_pull_ = false;
    hold_stopped_ = !params_.hold_pressure;
    next_hold_at_ = now + kHoldPressureInterval;
    ColdPullActions a;
    a.heater_target_c = params_.temps.pull_c;
    if (!params_.part_fan.empty()) {
        a.fan_percent = 100;
        fan_changed_ = true;
    }
    return a;
}

ColdPullActions ColdPullFlow::enter_pull(TimePoint now) {
    stage_ = ColdPullStage::Pull;
    stage_started_ = now;
    ColdPullActions a;
    // The heater holds the pull temperature now; the fan would only fight it.
    if (fan_changed_) {
        a.fan_percent = params_.prior_fan_percent;
        fan_changed_ = false;
    }
    return a;
}

ColdPullActions ColdPullFlow::extrude(TimePoint now) {
    if (stage_ != ColdPullStage::Feed || !feed_ || busy()) {
        return {};
    }
    last_tap_ = now;
    ColdPullActions a;
    a.gcode = feed_->extrude();
    if (!a.gcode.empty()) {
        track_command(a, InFlight::Feed);
    }
    return a;
}

ColdPullActions ColdPullFlow::filament_detected(TimePoint now) {
    if (stage_ != ColdPullStage::Feed || !feed_ || busy()) {
        return {};
    }
    ColdPullActions a;
    a.gcode = feed_->filament_detected();
    if (!a.gcode.empty()) {
        track_command(a, InFlight::Feed);
        last_tap_ = now;
    }
    return a;
}

ColdPullActions ColdPullFlow::fed(TimePoint now) {
    // Never waits on an extrusion: a clogged nozzle may not flow at all, and the
    // answer to one still in flight no longer matters.
    if (stage_ != ColdPullStage::Feed || !feed_) {
        return {};
    }
    feed_.reset();
    clear_in_flight();
    return enter_cooling(now);
}

ColdPullActions ColdPullFlow::command_finished(uint32_t command_id, bool ok, TimePoint now) {
    if (command_id == 0 || command_id != in_flight_command_) {
        return {};
    }
    const InFlight was = in_flight_;
    clear_in_flight();
    if (was == InFlight::Hold) {
        // A refused hold extrusion is not worth stopping the pull for; it stops holding.
        if (!ok) {
            hold_stopped_ = true;
        }
        return {};
    }
    if (was != InFlight::Feed || !feed_) {
        return {};
    }
    last_tap_ = now;
    // A refused extrusion is what a clog looks like; the pull does not need it.
    if (!ok) {
        feed_ = new_feed();
        return {};
    }
    (void)feed_->command_finished(true);
    filament_out_ = false;
    return {};
}

ColdPullActions ColdPullFlow::pulled(TimePoint now) {
    if (stage_ != ColdPullStage::Pull) {
        return {};
    }
    stage_ = ColdPullStage::Inspect;
    stage_started_ = now;
    filament_out_ = true;
    // Nothing is hot while the user looks at the tip.
    ColdPullActions a;
    a.heater_target_c = 0;
    sent_target_c_.reset();
    prev_target_c_.reset();
    return a;
}

ColdPullActions ColdPullFlow::repeat(TimePoint now) {
    if (stage_ != ColdPullStage::Inspect) {
        return {};
    }
    ++pass_;
    return enter_heating(now);
}

ColdPullActions ColdPullFlow::done(TimePoint /*now*/) {
    if (stage_ != ColdPullStage::Inspect) {
        return {};
    }
    ColdPullActions a = end(ColdPullStage::Finished, ColdPullEndReason::None, true);
    a.reset_meter = true;
    return a;
}

ColdPullActions ColdPullFlow::cancel() {
    if (stage_ == ColdPullStage::Setup) {
        stage_ = ColdPullStage::Cancelled;
        end_reason_ = ColdPullEndReason::UserCancel;
        return {};
    }
    return end(ColdPullStage::Cancelled, ColdPullEndReason::UserCancel, true);
}

ColdPullActions ColdPullFlow::job_started() {
    // The print owns the heater and the fan now.
    if (!live()) {
        return {};
    }
    return end(ColdPullStage::Abandoned, ColdPullEndReason::JobStarted, false);
}

ColdPullActions ColdPullFlow::disconnected() {
    // Nothing reaches the printer; the runner owes the cleanup to the next connection.
    if (!live()) {
        return {};
    }
    return end(ColdPullStage::Abandoned, ColdPullEndReason::Disconnected, false);
}

ColdPullActions ColdPullFlow::temperature(int current_c, int target_c, TimePoint now) {
    if (!live()) {
        return {};
    }
    last_current_c_ = current_c;

    if (sent_target_c_) {
        if (!target_seen_) {
            if (within(target_c, *sent_target_c_, kForeignTargetToleranceC) ||
                (stage_ == ColdPullStage::Heating && target_c > *sent_target_c_)) {
                // keep_previous_hot may raise the soak; the target that landed is the soak.
                target_seen_ = true;
                sent_target_c_ = target_c;
                prev_target_c_.reset();
            } else if (prev_target_c_ &&
                       !within(target_c, *prev_target_c_, kForeignTargetToleranceC)) {
                // Neither the target being replaced nor ours: someone else set it,
                // and whatever they chose stands. Before the first target lands the
                // subject still shows whatever was there; the land timeout covers it.
                sent_target_c_.reset();
                return end(ColdPullStage::Abandoned, ColdPullEndReason::ForeignTarget, true);
            }
        } else if (!within(target_c, *sent_target_c_, kForeignTargetToleranceC)) {
            sent_target_c_.reset();
            return end(ColdPullStage::Abandoned, ColdPullEndReason::ForeignTarget, true);
        }
    }

    switch (stage_) {
    case ColdPullStage::Heating: {
        const int soak = *sent_target_c_;
        if (target_seen_ && current_c >= std::max(soak - kColdPullReachMarginC, params_.floor_c)) {
            stage_ = ColdPullStage::Feed;
            stage_started_ = now;
            last_tap_ = now;
            feed_ = new_feed();
        }
        return {};
    }
    case ColdPullStage::Cooling:
        if (current_c < params_.floor_c + kHoldPressureMarginC) {
            hold_stopped_ = true;
        }
        // Entering on the target change alone would call a 225 °C nozzle ready.
        if (current_c > params_.temps.pull_c + kPullBandC) {
            was_above_pull_ = true;
        } else if (was_above_pull_) {
            return enter_pull(now);
        }
        return {};
    default:
        return {};
    }
}

ColdPullActions ColdPullFlow::tick(TimePoint now) {
    if (!live()) {
        return {};
    }
    const auto elapsed = now - stage_started_;
    switch (stage_) {
    case ColdPullStage::Heating:
        if (!target_seen_ && elapsed >= kColdPullTargetLandTimeout) {
            return end(ColdPullStage::Abandoned, ColdPullEndReason::TargetNeverLanded, true);
        }
        if (elapsed >= kColdPullHeatTimeout) {
            return end(ColdPullStage::Abandoned, ColdPullEndReason::Timeout, true);
        }
        return {};
    case ColdPullStage::Feed:
        // Counted from the last tap or send: a command that never answers still
        // lets the heater go off.
        if (now - last_tap_ >= kFeedIdleTimeout) {
            return end(ColdPullStage::Abandoned, ColdPullEndReason::Timeout, true);
        }
        return {};
    case ColdPullStage::Cooling: {
        const auto limit = params_.part_fan.empty() ? kCoolTimeoutNoFan : kCoolTimeout;
        if (elapsed >= std::chrono::duration_cast<std::chrono::seconds>(limit)) {
            return end(ColdPullStage::Abandoned, ColdPullEndReason::Timeout, true);
        }
        if (hold_stopped_ || busy() || now < next_hold_at_) {
            return {};
        }
        if (last_current_c_ < params_.floor_c + kHoldPressureMarginC) {
            hold_stopped_ = true;
            return {};
        }
        next_hold_at_ = now + kHoldPressureInterval;
        ColdPullActions a;
        a.gcode = ui::extrude_gcode(kHoldPressureMm, kHoldPressureSpeedMmMin);
        track_command(a, InFlight::Hold);
        return a;
    }
    case ColdPullStage::Pull:
        if (elapsed >= kPullHoldTimeout) {
            return end(ColdPullStage::Abandoned, ColdPullEndReason::Timeout, true);
        }
        return {};
    default:
        return {};
    }
}

} // namespace helix
