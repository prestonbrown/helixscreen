// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pwm_sound_backend.h"

#include <cstdint>
#include <functional>

/**
 * @brief Test-only access to PWMSoundBackend render-loop internals.
 *
 * The park/resume/catch-up logic in render_loop() runs on the PCM render
 * thread paced by the real CLOCK_MONOTONIC — a park cycle costs ~512 ms of
 * wall time and a catch-up stall cannot be injected at all. These seams let
 * tests drive the whole loop on a virtual clock in milliseconds:
 *
 *  - `set_now_fn()` / `set_wait_until_fn()` inject the clock and the sleep.
 *    MUST be called before set_render_source(): render_loop() reads the seams
 *    on the render thread, and the injection has to win that race by
 *    happening before the thread exists.
 *  - `set_park_silent_buffers()` shrinks the park threshold so tests reach
 *    the parked state after 2-3 buffers instead of ~512 ms.
 */
class PWMSoundBackendTestAccess {
  public:
    /// Inject the monotonic-ns clock used for pacing decisions.
    /// Call BEFORE set_render_source() (thread not started).
    static void set_now_fn(PWMSoundBackend& b, std::function<int64_t()> fn) {
        b.now_fn_ = std::move(fn);
    }

    /// Inject the "sleep until deadline_ns" seam. The virtual-clock tests
    /// pass a fake that just advances the clock to the deadline.
    /// Call BEFORE set_render_source() (thread not started).
    static void set_wait_until_fn(PWMSoundBackend& b, std::function<void(int64_t)> fn) {
        b.wait_until_fn_ = std::move(fn);
    }

    /// Test knob: park after N consecutive silent buffers (default
    /// PCM_PARK_SILENT_BUFFERS). Call before set_render_source().
    static void set_park_silent_buffers(PWMSoundBackend& b, int n) {
        b.park_silent_buffers_ = n;
    }

    /// True once the loop has parked the channel (duty 0, enable 0) and is
    /// only polling the render source every PCM_PARK_POLL_NS.
    static bool parked(const PWMSoundBackend& b) {
        return b.parked_.load();
    }

    /// Consecutive exactly-silent buffers counted so far (reset to 0 by any
    /// non-silent buffer).
    static int silent_buffer_run(const PWMSoundBackend& b) {
        return b.silent_buffer_run_.load();
    }

    /// Total duty-cycle writes performed by the render loop.
    static uint64_t duty_writes(const PWMSoundBackend& b) {
        return b.duty_write_count_.load();
    }

    /// Tone sysfs write batches emitted by set_tone() — one per call that
    /// actually writes (deduped re-emits are not counted).
    static uint64_t tone_writes(const PWMSoundBackend& b) {
        return b.tone_write_count_.load();
    }

    /// Policy the render thread managed to apply (SCHED_IDLE on success,
    /// -1 when the kernel refused). Unprivileged sandboxes may refuse.
    static int applied_sched_policy(const PWMSoundBackend& b) {
        return b.applied_sched_policy_;
    }
};
