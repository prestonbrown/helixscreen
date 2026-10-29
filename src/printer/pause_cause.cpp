// SPDX-License-Identifier: GPL-3.0-or-later
#include "pause_cause.h"

#include "text_io.h"

#include <algorithm>
#include <cctype>

namespace helix {
namespace {

// A matcher with no active criterion must never match (guards against an
// all-wildcard {} matcher classifying every pause as Terminal).
bool matcher_is_active(const TerminalMatcher& m) {
    return !m.message_substr.empty() || m.exception_id >= 0 || m.require_sdcard_inactive ||
           m.exception_code >= 0;
}

bool matches(const PauseSignals& s, const TerminalMatcher& m) {
    if (!matcher_is_active(m)) {
        return false;
    }
    if (!m.message_substr.empty() && !text_io::contains_ignore_case(s.message, m.message_substr)) {
        return false;
    }
    if (m.exception_id >= 0 && s.exception_id != m.exception_id) {
        return false;
    }
    if (m.exception_code >= 0 && s.exception_code != m.exception_code) {
        return false;
    }
    if (m.require_sdcard_inactive && s.sdcard_active) {
        return false;
    }
    return true;
}

} // namespace

PauseCause classify_pause(const PauseSignals& signals,
                          const std::vector<TerminalMatcher>& terminal_matchers) {
    for (const auto& m : terminal_matchers) {
        if (matches(signals, m)) {
            return PauseCause::Terminal;
        }
    }
    if (signals.runout_tripped || signals.exception_id >= 0 || !signals.message.empty()) {
        return PauseCause::Recoverable;
    }
    return PauseCause::Unknown;
}

} // namespace helix
