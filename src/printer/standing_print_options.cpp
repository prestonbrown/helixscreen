// SPDX-License-Identifier: GPL-3.0-or-later

#include "standing_print_options.h"

#include "ams_backend.h"

#include <algorithm>
#include <iterator>

#include "hv/json.hpp"

namespace helix {

std::string render_standing_gcode(const std::vector<const AmsBackend*>& backends) {
    std::string out;
    for (const AmsBackend* backend : backends) {
        if (backend == nullptr) {
            continue;
        }
        const std::string gcode = backend->standing_print_gcode();
        if (gcode.empty()) {
            continue;
        }
        out += (out.empty() ? "" : "\n") + gcode;
    }
    return out;
}

nlohmann::json standing_options_record(const std::string& gcode) {
    return nlohmann::json{{"version", kStandingOptionsVersion}, {"gcode", gcode}};
}

StandingOptionsSender standing_options_sender(PluginFeatureState hooks, bool opted_in,
                                              bool screen_started, bool has_gcode,
                                              FallbackSenderRole role) {
    if (screen_started || !has_gcode) {
        return StandingOptionsSender::None;
    }
    switch (hooks) {
    case PluginFeatureState::Ready:
        return StandingOptionsSender::Plugin;
    case PluginFeatureState::Disabled:
        // The plugin can run it and the user switched it off.
        return StandingOptionsSender::None;
    case PluginFeatureState::Absent:
        return opted_in && role == FallbackSenderRole::Sender ? StandingOptionsSender::App
                                                              : StandingOptionsSender::None;
    }
    return StandingOptionsSender::None;
}

bool writes_standing_record(FallbackSenderRole role, const std::string& gcode) {
    return role == FallbackSenderRole::Sender && !gcode.empty();
}

bool announce_screen_start(PluginFeatureState hooks) {
    return hooks == PluginFeatureState::Ready;
}

void ScreenStartLedger::expire(Clock::time_point now) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [now](const Entry& e) { return now > e.expires; }),
                   entries_.end());
}

void ScreenStartLedger::note(const std::string& filename, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire(now);
    entries_.push_back(Entry{filename, now + kWindow});
}

void ScreenStartLedger::note_any(Clock::time_point now, Clock::duration window) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire(now);
    entries_.push_back(Entry{std::nullopt, now + window});
}

bool ScreenStartLedger::claim(const std::string& filename, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire(now);
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&filename](const Entry& e) {
        return !e.filename || *e.filename == filename;
    });
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it);
    return true;
}

bool ScreenStartLedger::withdraw(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->filename && *it->filename == filename) {
            entries_.erase(std::next(it).base());
            return true;
        }
    }
    return false;
}

bool ScreenStartLedger::withdraw_any() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (!it->filename) {
            entries_.erase(std::next(it).base());
            return true;
        }
    }
    return false;
}

void ScreenStartLedger::set_announce(bool announce) {
    std::lock_guard<std::mutex> lock(mutex_);
    announce_ = announce;
}

bool ScreenStartLedger::announce() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return announce_;
}

ScreenStartLedger& screen_start_ledger() {
    static ScreenStartLedger ledger;
    return ledger;
}

} // namespace helix
