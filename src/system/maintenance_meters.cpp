// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "maintenance_meters.h"

#include "json_utils.h"
#include "wall_clock_trust.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

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

/// A stored wall-clock stamp; only one that can be a real moment counts.
std::optional<int64_t> stored_stamp(const json* v) {
    if (!v || !v->is_number())
        return std::nullopt;
    const double s = json_util::as_double(*v, -1.0);
    if (!std::isfinite(s) || s >= 1e12 || !wall_clock_trusted(static_cast<int64_t>(s)))
        return std::nullopt;
    return static_cast<int64_t>(s);
}

/// The Z meter takes the rails hours: the last rails service is the best evidence of the
/// last Z service. Its interval stays unseeded, so the printer's default applies.
void start_z_from_rails(State& s) {
    const Meter& xy = s.meters[static_cast<size_t>(MeterId::XyAxis)];
    Meter& z = s.meters[static_cast<size_t>(MeterId::ZAxis)];
    z.used_h = xy.used_h;
    z.reset_at_h = xy.reset_at_h;
}

constexpr double kSecondsPerDay = 86400.0;

} // namespace

State default_state() {
    State s;
    for (Meter& m : s.meters)
        m.seeded = false;
    return s;
}

bool applies(MeterId m, const Applicability& a) {
    switch (m) {
    case MeterId::Filter:
        return a.enclosed || a.chamber_filter_fan || a.firmware_filter;
    case MeterId::ElectronicsFans:
        return a.controller_fan;
    case MeterId::BeltTension:
        return false;
    default:
        return true;
    }
}

MeterDefault default_for(MeterId m, const Applicability& a, const ModelDefaults& model) {
    const auto i = static_cast<size_t>(m);
    MeterDefault out;
    out.hours = model[i].hours.value_or(applies(m, a) ? kDefaultIntervals[i] : 0);
    out.days = model[i].days.value_or(0);
    return out;
}

bool seed_defaults(State& s, const Applicability& a, const ModelDefaults& model) {
    bool changed = false;
    for (int i = 0; i < kMeterCount; ++i) {
        Meter& m = s.meters[static_cast<size_t>(i)];
        if (m.seeded)
            continue;
        const MeterDefault d = default_for(static_cast<MeterId>(i), a, model);
        m.interval_h = snap_interval(d.hours.value_or(0));
        m.interval_d = snap_days(d.days.value_or(0));
        m.seeded = true;
        changed = true;
    }
    return changed;
}

bool stamp_calendar_starts(State& s, std::optional<int64_t> now_s) {
    if (!now_s || !wall_clock_trusted(*now_s))
        return false;
    bool changed = false;
    for (Meter& m : s.meters) {
        if (enabled(m) && m.interval_d > 0 && !m.reset_at_s) {
            m.reset_at_s = *now_s;
            changed = true;
        }
    }
    return changed;
}

namespace {

/// Days since the limit's stamp; nullopt while the limit is idle.
std::optional<double> days_elapsed(const Meter& m, std::optional<int64_t> now_s) {
    if (m.interval_d <= 0 || !m.reset_at_s || !now_s || !wall_clock_trusted(*now_s))
        return std::nullopt;
    return static_cast<double>(*now_s - *m.reset_at_s) / kSecondsPerDay;
}

double day_fraction(const Meter& m, std::optional<int64_t> now_s) {
    const auto elapsed = days_elapsed(m, now_s);
    return elapsed ? std::clamp(*elapsed / static_cast<double>(m.interval_d), 0.0, 1.0) : 0.0;
}

double hour_fraction(const Meter& m) {
    return std::clamp(m.used_h / static_cast<double>(m.interval_h), 0.0, 1.0);
}

} // namespace

double used_fraction(const Meter& m, std::optional<int64_t> now_s) {
    if (!enabled(m))
        return 0.0;
    return std::max(hour_fraction(m), day_fraction(m, now_s));
}

int hours_left(const Meter& m) {
    if (!enabled(m))
        return 0;
    return std::max(0, static_cast<int>(std::ceil(static_cast<double>(m.interval_h) - m.used_h)));
}

int hours_used(const Meter& m) {
    return static_cast<int>(std::lround(m.used_h));
}

std::optional<int> days_left(const Meter& m, std::optional<int64_t> now_s) {
    const auto elapsed = days_elapsed(m, now_s);
    if (!elapsed)
        return std::nullopt;
    return std::max(0, static_cast<int>(std::ceil(static_cast<double>(m.interval_d) - *elapsed)));
}

bool days_are_closer(const Meter& m, std::optional<int64_t> now_s) {
    return enabled(m) && day_fraction(m, now_s) > hour_fraction(m);
}

ValueStatus meter_status(const Meter& m, std::optional<int64_t> now_s) {
    return status::maintenance_status(used_fraction(m, now_s));
}

std::vector<int> due_order(const State& s, std::optional<int64_t> now_s) {
    std::vector<int> order;
    for (int i = 0; i < kMeterCount; ++i) {
        if (enabled(s.meters[static_cast<size_t>(i)]))
            order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return used_fraction(s.meters[static_cast<size_t>(a)], now_s) >
               used_fraction(s.meters[static_cast<size_t>(b)], now_s);
    });
    return order;
}

std::optional<int> most_due(const State& s, std::optional<int64_t> now_s) {
    const std::vector<int> order = due_order(s, now_s);
    if (order.empty())
        return std::nullopt;
    return order.front();
}

std::vector<int> take_pending_notices(State& s, std::optional<int64_t> now_s) {
    std::vector<int> due;
    for (int i = 0; i < kMeterCount; ++i) {
        Meter& m = s.meters[static_cast<size_t>(i)];
        if (enabled(m) && !m.notified && used_fraction(m, now_s) >= 1.0) {
            m.notified = true;
            due.push_back(i);
        }
    }
    return due;
}

int longest_interval(const State& s) {
    int longest = 0;
    for (const Meter& m : s.meters)
        longest = std::max(longest, m.interval_h);
    return longest;
}

void accrue(State& s, double hours, bool skip_filter) {
    if (!(hours > 0.0) || !std::isfinite(hours))
        return;
    s.total_h = std::min(s.total_h + hours, kMaxStoredHours);
    for (int i = 0; i < kMeterCount; ++i) {
        Meter& m = s.meters[static_cast<size_t>(i)];
        if (!enabled(m) || (skip_filter && i == static_cast<int>(MeterId::Filter)))
            continue;
        m.used_h = std::min(m.used_h + hours, kMaxStoredHours);
    }
}

void apply_firmware_filter_hours(State& s, double hours) {
    Meter& m = s.meters[static_cast<size_t>(MeterId::Filter)];
    if (!enabled(m) || !std::isfinite(hours) || hours < 0.0)
        return;
    // A counter that went down was reset on the printer, from here or anywhere else:
    // the interval starts over and its due notice is armed again.
    if (hours < m.used_h) {
        m.reset_at_h = s.total_h;
        m.notified = false;
    }
    m.used_h = std::min(hours, kMaxStoredHours);
}

void reset_meter(State& s, int meter, std::optional<int64_t> now_s) {
    if (!valid_meter(meter))
        return;
    Meter& m = s.meters[static_cast<size_t>(meter)];
    m.used_h = 0.0;
    m.reset_at_h = s.total_h;
    m.notified = false;
    m.reset_at_s = now_s && wall_clock_trusted(*now_s) ? now_s : std::nullopt;
}

namespace {

void rearm_if_not_due(Meter& m, std::optional<int64_t> now_s) {
    if (m.notified && used_fraction(m, now_s) < 1.0)
        m.notified = false;
}

} // namespace

void set_interval(State& s, int meter, int hours, std::optional<int64_t> now_s) {
    if (!valid_meter(meter))
        return;
    Meter& m = s.meters[static_cast<size_t>(meter)];
    m.interval_h = snap_interval(hours);
    m.seeded = true;
    rearm_if_not_due(m, now_s);
}

void set_interval_days(State& s, int meter, int days, std::optional<int64_t> now_s) {
    if (!valid_meter(meter))
        return;
    Meter& m = s.meters[static_cast<size_t>(meter)];
    m.interval_d = snap_days(days);
    stamp_calendar_starts(s, now_s);
    rearm_if_not_due(m, now_s);
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

int snap_days(int days) {
    if (days <= 0)
        return 0;
    int best = kDayChoices[1];
    for (int choice : kDayChoices) {
        if (choice > 0 && std::abs(choice - days) < std::abs(best - days))
            best = choice;
    }
    return best;
}

int day_choice_index(int days) {
    const int snapped = snap_days(days);
    const auto it = std::find(kDayChoices.begin(), kDayChoices.end(), snapped);
    return static_cast<int>(std::distance(kDayChoices.begin(), it));
}

json to_json(const State& s) {
    json meters = json::object();
    for (int i = 0; i < kMeterCount; ++i) {
        const Meter& m = s.meters[static_cast<size_t>(i)];
        json entry = {{"used_h", m.used_h}, {"notified", m.notified}};
        if (m.seeded)
            entry["interval_h"] = m.interval_h;
        if (m.interval_d > 0)
            entry["interval_d"] = m.interval_d;
        if (m.reset_at_h)
            entry["reset_at_h"] = *m.reset_at_h;
        if (m.reset_at_s)
            entry["reset_at_s"] = *m.reset_at_s;
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
        if (auto interval = stored_interval(json_util::find_member(*entry, "interval_h"))) {
            m.interval_h = *interval;
            m.seeded = true;
        }
        if (auto days = stored_hours(json_util::find_member(*entry, "interval_d")))
            m.interval_d = snap_days(static_cast<int>(std::lround(*days)));
        m.reset_at_s = stored_stamp(json_util::find_member(*entry, "reset_at_s"));
        m.used_h = stored_hours(json_util::find_member(*entry, "used_h")).value_or(0.0);
        m.reset_at_h = stored_hours(json_util::find_member(*entry, "reset_at_h"));
        const json* notified = json_util::find_member(*entry, "notified");
        m.notified = notified && notified->is_boolean() && notified->get<bool>();
    }
    if (!json_util::find_member(*meters, kMeterKeys[static_cast<size_t>(MeterId::ZAxis)]))
        start_z_from_rails(s);
    return s;
}

ModelDefaults model_defaults_from_json(const json& maintenance_field) {
    ModelDefaults out{};
    if (!maintenance_field.is_object())
        return out;
    for (int i = 0; i < kMeterCount; ++i) {
        const json* entry =
            json_util::find_member(maintenance_field, kMeterKeys[static_cast<size_t>(i)]);
        if (!entry || !entry->is_object())
            continue;
        MeterDefault& d = out[static_cast<size_t>(i)];
        d.hours = stored_interval(json_util::find_member(*entry, "hours"));
        if (auto days = stored_hours(json_util::find_member(*entry, "days")))
            d.days = static_cast<int>(std::lround(*days));
    }
    return out;
}

std::optional<State> from_plugin(const json& storage, const json& settings) {
    // The meters the plugin kept, and the interval keys its manifest declares.
    static constexpr std::array<std::pair<MeterId, const char*>, 3> kPluginMeters = {{
        {MeterId::Filter, "filter_interval_h"},
        {MeterId::Nozzle, "nozzle_interval_h"},
        {MeterId::XyAxis, "rails_interval_h"},
    }};

    State s = default_state();
    bool found = false;
    for (const auto& [id, key] : kPluginMeters) {
        Meter& m = s.meters[static_cast<size_t>(id)];
        m.interval_h = kDefaultIntervals[static_cast<size_t>(id)];
        m.seeded = true;
        if (auto interval = stored_interval(json_util::find_member(settings, key))) {
            m.interval_h = *interval;
            found = true;
        }
    }

    const json* state = json_util::find_member(storage, "state");
    if (state && state->is_object()) {
        found = true;
        s.total_h = stored_hours(json_util::find_member(*state, "total_h")).value_or(0.0);
        const json* meters = json_util::find_member(*state, "meters");
        for (const auto& plugin_meter : kPluginMeters) {
            if (!meters || !meters->is_object())
                break;
            const auto id = static_cast<size_t>(plugin_meter.first);
            const json* entry = json_util::find_member(*meters, kMeterKeys[id]);
            if (!entry || !entry->is_object())
                continue;
            Meter& m = s.meters[id];
            m.used_h = stored_hours(json_util::find_member(*entry, "used")).value_or(0.0);
            m.reset_at_h = stored_hours(json_util::find_member(*entry, "reset_at"));
            const json* notified = json_util::find_member(*entry, "notified");
            m.notified = notified && notified->is_boolean() && notified->get<bool>();
        }
    }
    if (!found)
        return std::nullopt;
    start_z_from_rails(s);
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
