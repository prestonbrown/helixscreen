// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "maintenance_meters.h"

#include "json_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace helix::maintenance {

using json = nlohmann::json;

namespace {

bool valid_meter(int meter) {
    return meter >= 0 && meter < kMeterCount;
}

/// A stored hour count: a finite, non-negative number below kMaxStoredHours.
std::optional<double> stored_hours(const json* v) {
    if (!v || !(v->is_number() || v->is_string()))
        return std::nullopt;
    const double h = json_util::as_double(*v, -1.0);
    if (h < 0.0 || h >= kMaxStoredHours)
        return std::nullopt;
    return h;
}

/// A stored interval: a whole number of hours, snapped; nullopt when absent or not a number.
std::optional<int> stored_interval(const json* v) {
    if (!v || !(v->is_number() || v->is_string()))
        return std::nullopt;
    const double h = json_util::as_double(*v, -1.0);
    if (h < 0.0 || h >= kMaxStoredHours)
        return std::nullopt;
    return snap_interval(static_cast<int>(std::lround(h)));
}

} // namespace

State default_state() {
    State s;
    for (int i = 0; i < kMeterCount; ++i)
        s.meters[static_cast<size_t>(i)].interval_h = kDefaultIntervals[static_cast<size_t>(i)];
    return s;
}

double used_fraction(const Meter& m) {
    if (!enabled(m))
        return 0.0;
    return std::clamp(m.used_h / static_cast<double>(m.interval_h), 0.0, 1.0);
}

int hours_left(const Meter& m) {
    if (!enabled(m))
        return 0;
    return std::max(0, static_cast<int>(std::ceil(static_cast<double>(m.interval_h) - m.used_h)));
}

int hours_used(const Meter& m) {
    return static_cast<int>(std::lround(m.used_h));
}

ValueStatus meter_status(const Meter& m) {
    return status::maintenance_status(used_fraction(m));
}

std::optional<int> most_due(const State& s) {
    std::optional<int> worst;
    for (int i = 0; i < kMeterCount; ++i) {
        const Meter& m = s.meters[static_cast<size_t>(i)];
        if (!enabled(m))
            continue;
        if (!worst || used_fraction(m) > used_fraction(s.meters[static_cast<size_t>(*worst)]))
            worst = i;
    }
    return worst;
}

int longest_interval(const State& s) {
    int longest = 0;
    for (const Meter& m : s.meters)
        longest = std::max(longest, m.interval_h);
    return longest;
}

std::vector<int> accrue(State& s, double hours, bool skip_filter) {
    std::vector<int> due;
    if (!(hours > 0.0) || !std::isfinite(hours))
        return due;
    s.total_h = std::min(s.total_h + hours, kMaxStoredHours);
    for (int i = 0; i < kMeterCount; ++i) {
        Meter& m = s.meters[static_cast<size_t>(i)];
        if (!enabled(m) || (skip_filter && i == static_cast<int>(MeterId::Filter)))
            continue;
        m.used_h = std::min(m.used_h + hours, kMaxStoredHours);
        if (!m.notified && m.used_h >= static_cast<double>(m.interval_h)) {
            m.notified = true;
            due.push_back(i);
        }
    }
    return due;
}

std::vector<int> apply_firmware_filter_hours(State& s, double hours) {
    std::vector<int> due;
    Meter& m = s.meters[static_cast<size_t>(MeterId::Filter)];
    if (!enabled(m) || !std::isfinite(hours) || hours < 0.0)
        return due;
    m.used_h = std::min(hours, kMaxStoredHours);
    if (!m.notified && m.used_h >= static_cast<double>(m.interval_h)) {
        m.notified = true;
        due.push_back(static_cast<int>(MeterId::Filter));
    }
    return due;
}

void reset_meter(State& s, int meter) {
    if (!valid_meter(meter))
        return;
    Meter& m = s.meters[static_cast<size_t>(meter)];
    m.used_h = 0.0;
    m.reset_at_h = s.total_h;
    m.notified = false;
}

void set_interval(State& s, int meter, int hours) {
    if (!valid_meter(meter))
        return;
    Meter& m = s.meters[static_cast<size_t>(meter)];
    m.interval_h = snap_interval(hours);
    if (m.notified && m.used_h < static_cast<double>(m.interval_h))
        m.notified = false;
}

int snap_interval(int hours) {
    if (hours <= 0)
        return 0;
    // A positive request never snaps to Off.
    int best = kIntervalChoices[1];
    for (int choice : kIntervalChoices) {
        if (choice > 0 && std::abs(choice - hours) < std::abs(best - hours))
            best = choice;
    }
    return best;
}

int interval_choice_index(int hours) {
    const int snapped = snap_interval(hours);
    const auto it = std::find(kIntervalChoices.begin(), kIntervalChoices.end(), snapped);
    return static_cast<int>(std::distance(kIntervalChoices.begin(), it));
}

json to_json(const State& s) {
    json meters = json::object();
    for (int i = 0; i < kMeterCount; ++i) {
        const Meter& m = s.meters[static_cast<size_t>(i)];
        json entry = {{"interval_h", m.interval_h}, {"used_h", m.used_h}, {"notified", m.notified}};
        if (m.reset_at_h)
            entry["reset_at_h"] = *m.reset_at_h;
        meters[kMeterKeys[static_cast<size_t>(i)]] = std::move(entry);
    }
    return json{{"total_h", s.total_h}, {"meters", std::move(meters)}};
}

State from_json(const json& j) {
    State s = default_state();
    s.total_h = stored_hours(json_util::find_member(j, "total_h")).value_or(0.0);
    const json* meters = json_util::find_member(j, "meters");
    if (!meters || !meters->is_object())
        return s;
    for (int i = 0; i < kMeterCount; ++i) {
        const json* entry = json_util::find_member(*meters, kMeterKeys[static_cast<size_t>(i)]);
        if (!entry || !entry->is_object())
            continue;
        Meter& m = s.meters[static_cast<size_t>(i)];
        if (auto interval = stored_interval(json_util::find_member(*entry, "interval_h")))
            m.interval_h = *interval;
        m.used_h = stored_hours(json_util::find_member(*entry, "used_h")).value_or(0.0);
        m.reset_at_h = stored_hours(json_util::find_member(*entry, "reset_at_h"));
        const json* notified = json_util::find_member(*entry, "notified");
        m.notified = notified && notified->is_boolean() && notified->get<bool>();
    }
    return s;
}

std::optional<State> from_plugin(const json& storage, const json& settings) {
    // Interval keys as the plugin's manifest declares them.
    static constexpr std::array<const char*, kMeterCount> kSettingKeys = {
        "filter_interval_h", "nozzle_interval_h", "rails_interval_h"};

    State s = default_state();
    bool found = false;
    for (int i = 0; i < kMeterCount; ++i) {
        if (auto interval = stored_interval(
                json_util::find_member(settings, kSettingKeys[static_cast<size_t>(i)]))) {
            s.meters[static_cast<size_t>(i)].interval_h = *interval;
            found = true;
        }
    }

    const json* state = json_util::find_member(storage, "state");
    if (state && state->is_object()) {
        found = true;
        s.total_h = stored_hours(json_util::find_member(*state, "total_h")).value_or(0.0);
        const json* meters = json_util::find_member(*state, "meters");
        for (int i = 0; meters && meters->is_object() && i < kMeterCount; ++i) {
            const json* entry = json_util::find_member(*meters, kMeterKeys[static_cast<size_t>(i)]);
            if (!entry || !entry->is_object())
                continue;
            Meter& m = s.meters[static_cast<size_t>(i)];
            m.used_h = stored_hours(json_util::find_member(*entry, "used")).value_or(0.0);
            m.reset_at_h = stored_hours(json_util::find_member(*entry, "reset_at"));
            const json* notified = json_util::find_member(*entry, "notified");
            m.notified = notified && notified->is_boolean() && notified->get<bool>();
        }
    }
    if (!found)
        return std::nullopt;
    // A snapped interval can move the due point either way; the latch follows it.
    for (Meter& m : s.meters) {
        if (m.notified && m.used_h < static_cast<double>(m.interval_h))
            m.notified = false;
    }
    return s;
}

int DurationTracker::observe(int duration_s) {
    if (duration_s < 0)
        return 0;
    if (!last_s_) {
        last_s_ = duration_s;
        return 0;
    }
    if (duration_s < *last_s_) {
        // A drop is a new job, or the screen zeroing its copy of the value before
        // Klipper's arrives; either way the next reading is the baseline.
        last_s_.reset();
        return 0;
    }
    const int delta = duration_s - *last_s_;
    last_s_ = duration_s;
    return delta;
}

} // namespace helix::maintenance
