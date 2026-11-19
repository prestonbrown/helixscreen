// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// A test case that reports one of the strict anomaly codes below through
/// helix_lvgl_anomaly() fails, with the code and its context in the failure
/// message. The listener in tests/unit/test_strict_anomalies.cpp does the
/// checking; this header is the opt-out for a case whose purpose is to provoke
/// one:
///
///     helix::test::AllowAnomaly allow("subject_reinit_while_live");
///     subject.init_int(0);  // the re-init under test
///
/// The allowance covers the guard's scope only, and only the named code.

#include <cstring>
#include <string>
#include <vector>

namespace helix::test {

/// Codes that mean a test misused a subject: each one is a binding that silently
/// does nothing or an observer left on a dead subject. observer_refused_deinit_subject
/// belongs here too and joins once the suite stops reporting it (#2116).
inline constexpr const char* kStrictAnomalies[] = {
    "subject_reinit_while_live",
    "observer_null_subject",
};

inline bool is_strict_anomaly(const char* code) {
    if (code == nullptr) {
        return false;
    }
    for (const char* strict : kStrictAnomalies) {
        if (std::strcmp(code, strict) == 0) {
            return true;
        }
    }
    return false;
}

/// Codes currently allowed, one entry per live AllowAnomaly. Test thread only.
inline std::vector<std::string>& allowed_anomalies() {
    static std::vector<std::string> allowed;
    return allowed;
}

inline bool is_anomaly_allowed(const char* code) {
    for (const auto& allowed : allowed_anomalies()) {
        if (allowed == code) {
            return true;
        }
    }
    return false;
}

class AllowAnomaly {
  public:
    explicit AllowAnomaly(const char* code) : code_(code) {
        allowed_anomalies().push_back(code_);
    }
    ~AllowAnomaly() {
        auto& allowed = allowed_anomalies();
        for (auto it = allowed.rbegin(); it != allowed.rend(); ++it) {
            if (*it == code_) {
                allowed.erase(std::next(it).base());
                break;
            }
        }
    }
    AllowAnomaly(const AllowAnomaly&) = delete;
    AllowAnomaly& operator=(const AllowAnomaly&) = delete;

  private:
    std::string code_;
};

} // namespace helix::test
