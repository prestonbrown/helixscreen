// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace helix {

/**
 * @brief Liveness signal for the LVGL main loop.
 *
 * Only a live lv_timer_handler() pass advances this, and that is the entire
 * point. When the UI thread deadlocks, the background threads carry on looking
 * perfectly healthy — during the wake-path deadlock that motivated this, the
 * websocket thread kept accumulating CPU, the memory monitor kept firing, and
 * the render thread sat in its normal wait. Anything sampled off a background
 * thread would have reported the app as fine for the entire 36 minutes it was
 * frozen.
 *
 * beat() has to stay trivially cheap: it runs every main-loop iteration, so it
 * is a relaxed increment with no clock read and no lock.
 */
class MainLoopHeartbeat {
  public:
    /// Called once per main-loop iteration.
    static void beat() noexcept {
        counter_.fetch_add(1, std::memory_order_relaxed);
        armed_.store(true, std::memory_order_relaxed);
    }

    static uint64_t count() noexcept {
        return counter_.load(std::memory_order_relaxed);
    }

    /// False until the loop has ticked at least once, so a slow startup (the
    /// XML parse alone runs ~8s on AD5M) can never look like a stall.
    static bool armed() noexcept {
        return armed_.load(std::memory_order_relaxed);
    }

    /// Test seam: forget that the loop ever ran.
    static void reset() noexcept {
        counter_.store(0, std::memory_order_relaxed);
        armed_.store(false, std::memory_order_relaxed);
    }

  private:
    static inline std::atomic<uint64_t> counter_{0};
    static inline std::atomic<bool> armed_{false};
};

/**
 * @brief Decides when a stalled heartbeat counts as a hang.
 *
 * Pure state machine, deliberately separated from the thread that drives it so
 * the decision is unit-testable without sleeping for a minute. Feed it the
 * current counter and a monotonic timestamp; it tells you when the loop has
 * first been stuck past the threshold.
 */
class MainLoopHangDetector {
  public:
    static constexpr uint32_t DEFAULT_THRESHOLD_MS = 60000;

    explicit MainLoopHangDetector(uint32_t threshold_ms = DEFAULT_THRESHOLD_MS) noexcept
        : threshold_ms_(threshold_ms) {}

    void set_threshold_ms(uint32_t ms) noexcept {
        threshold_ms_ = ms;
    }

    uint32_t threshold_ms() const noexcept {
        return threshold_ms_;
    }

    /// True between crossing the threshold and the loop moving again.
    bool stalled() const noexcept {
        return reported_;
    }

    /**
     * @brief Feed one sample.
     *
     * @param count  Current MainLoopHeartbeat::count().
     * @param now_ms Monotonic milliseconds; only differences are used.
     * @return The stall duration in ms on the sample that first crosses the
     *         threshold, otherwise 0. Reports once per stall so a wedged loop
     *         cannot spam the log or the telemetry queue; the loop recovering
     *         re-arms it.
     */
    uint32_t sample(uint64_t count, uint64_t now_ms) noexcept {
        if (!seeded_) {
            seeded_ = true;
            last_count_ = count;
            last_change_ms_ = now_ms;
            return 0;
        }

        if (count != last_count_) {
            last_count_ = count;
            last_change_ms_ = now_ms;
            reported_ = false;
            return 0;
        }

        if (reported_ || threshold_ms_ == 0 || now_ms < last_change_ms_) {
            return 0;
        }

        const uint64_t stalled_ms = now_ms - last_change_ms_;
        if (stalled_ms < threshold_ms_) {
            return 0;
        }

        reported_ = true;
        return static_cast<uint32_t>(stalled_ms);
    }

  private:
    uint64_t last_count_ = 0;
    uint64_t last_change_ms_ = 0;
    uint32_t threshold_ms_ = DEFAULT_THRESHOLD_MS;
    bool seeded_ = false;
    bool reported_ = false;
};

} // namespace helix
