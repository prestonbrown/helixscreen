// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <string>

namespace helix {

/// RAII guard for one environment variable: records the original value at
/// construction and restores it (or unsets it) on destruction. For tests
/// that setenv() a knob under test — a variable left set leaks into every
/// later test in the same shard.
class ScopedEnv {
  public:
    explicit ScopedEnv(const char* name) : name_(name) {
        const char* val = std::getenv(name);
        was_set_ = (val != nullptr);
        if (was_set_) {
            original_ = val;
        }
    }

    ~ScopedEnv() {
        if (was_set_) {
            setenv(name_.c_str(), original_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    std::string name_;
    std::string original_;
    bool was_set_ = false;
};

} // namespace helix
