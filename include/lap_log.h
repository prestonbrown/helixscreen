// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <spdlog/spdlog.h>

#include <chrono>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace helix {

/// Logs how long each step of a multi-step UI-thread operation took, so a slow cycle on
/// a device can be broken down from its log alone. On the ESP32 each step also logs the
/// internal heap, so a step that keeps or splits internal RAM shows in the same line.
class LapLog {
  public:
    explicit LapLog(const char* scope) : scope_(scope), start_(now()), last_(start_) {}

    void lap(const char* step) {
        const auto t = now();
#ifdef ESP_PLATFORM
        spdlog::info("[{}] {} {} ms (total {} ms) | internal free={} largest={}", scope_, step,
                     ms(t - last_), ms(t - start_), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#else
        spdlog::info("[{}] {} {} ms (total {} ms)", scope_, step, ms(t - last_), ms(t - start_));
#endif
        last_ = t;
    }

  private:
    using clock = std::chrono::steady_clock;
    static clock::time_point now() {
        return clock::now();
    }
    static long long ms(clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    }

    const char* scope_;
    clock::time_point start_;
    clock::time_point last_;
};

} // namespace helix
