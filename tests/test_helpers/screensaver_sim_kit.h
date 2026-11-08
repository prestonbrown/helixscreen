// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_sim_contract.h"

#if HELIX_ENABLE_SCREENSAVER
#include "screensaver_canvas.h"

#include <lvgl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

/**
 * @file screensaver_sim_kit.h
 * @brief What the screensaver sim tests share: frames, diffs, pixel counts, hashes and runs
 *
 * No saver is special here. A sim test builds a TestFrame, drives its sim through SimRun (the
 * same calling shape SimSaver uses), and asks the frame what changed.
 */

namespace helix::test {

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

/// Frame times a run cycles through: uneven, so motion that ignores dt shows.
inline constexpr uint32_t kUnevenFrameMs[] = {33, 16, 50, 7, 70};

inline constexpr uint64_t kFnv1aOffset = 14695981039346656037ULL;
inline constexpr uint64_t kFnv1aPrime = 1099511628211ULL;

/// FNV-1a of `size` bytes, continuing from `hash`.
inline uint64_t fnv1a(uint64_t hash, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= kFnv1aPrime;
    }
    return hash;
}

/// FNV-1a of every pixel byte of `frame`, row by row, leaving out row padding.
inline uint64_t frame_fnv1a(const FrameTarget& frame, uint64_t hash = kFnv1aOffset) {
    const size_t row_bytes = static_cast<size_t>(frame.w) * helix::ui::pixel_bytes(frame.format);
    for (uint32_t y = 0; y < frame.h; y++) {
        hash = fnv1a(hash, frame.data + static_cast<size_t>(y) * frame.stride, row_bytes);
    }
    return hash;
}

/**
 * @brief A heap frame with guarded padding
 *
 * Every row carries `pad_bytes` of padding past its pixels, and one whole guard row follows the
 * last, all holding `canary`, so a write past a row's end or the frame's bottom shows in
 * padding_intact(). Pixels start as `fill` bytes.
 */
class TestFrame {
  public:
    TestFrame(uint32_t w, uint32_t h, PixelFormat format, uint32_t pad_bytes = 12,
              uint8_t canary = 0xA5, uint8_t fill = 0)
        : canary_(canary) {
        const uint32_t row = w * helix::ui::pixel_bytes(format);
        const uint32_t stride = row + pad_bytes;
        bytes.assign(static_cast<size_t>(stride) * (h + 1), canary);
        for (uint32_t y = 0; y < h; y++) {
            std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(y) * stride, row, fill);
        }
        target = {bytes.data(), stride, w, h, format};
    }
    TestFrame(const TestFrame&) = delete;
    TestFrame& operator=(const TestFrame&) = delete;

    /// The colour at (x, y), as PixelWriter reads it.
    Rgb at(int32_t x, int32_t y) const {
        return PixelWriter(target).get(x, y);
    }

    /// True while every padding byte and the guard row still hold the canary.
    bool padding_intact() const {
        const size_t row = static_cast<size_t>(target.w) * helix::ui::pixel_bytes(target.format);
        for (uint32_t y = 0; y <= target.h; y++) {
            const size_t start = static_cast<size_t>(y) * target.stride;
            const size_t from = y < target.h ? start + row : start;
            for (size_t i = from; i < start + target.stride; i++) {
                if (bytes[i] != canary_) {
                    return false;
                }
            }
        }
        return true;
    }

    /// A copy of every byte, to diff a later frame against.
    std::vector<uint8_t> snapshot() const {
        return bytes;
    }

    std::vector<uint8_t> bytes;
    FrameTarget target;

  private:
    uint8_t canary_;
};

/// True when some box in `dirty` covers (x, y).
inline bool covers(const std::vector<DirtyRect>& dirty, int32_t x, int32_t y) {
    return std::any_of(dirty.begin(), dirty.end(), [x, y](const DirtyRect& r) {
        return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
    });
}

/// True when `outer` contains all of `inner`.
inline bool contains(const DirtyRect& outer, const DirtyRect& inner) {
    return outer.x1 <= inner.x1 && outer.y1 <= inner.y1 && outer.x2 >= inner.x2 &&
           outer.y2 >= inner.y2;
}

/// Bytes at which `a` and `b` differ, a length difference counting each missing byte.
/// Compare buffers through this rather than with `==`: Catch2 prints both operands of
/// a decomposed comparison in full, so a frame-sized `==` writes megabytes into the
/// --success report per assertion.
inline size_t bytes_differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t common = std::min(a.size(), b.size());
    size_t n = std::max(a.size(), b.size()) - common;
    for (size_t i = 0; i < common; i++) {
        n += a[i] != b[i] ? 1 : 0;
    }
    return n;
}

/// What changed between a snapshot and a frame, against the boxes a step reported.
struct FrameDiff {
    size_t changed = 0;    ///< pixels whose bytes differ
    size_t uncovered = 0;  ///< of those, pixels no dirty box covers
    DirtyRect changed_box; ///< bounds of the changed pixels
};

/// Compares `frame` with `before` (a snapshot() of it) pixel by pixel.
inline FrameDiff diff_against(const std::vector<uint8_t>& before, const TestFrame& frame,
                              const std::vector<DirtyRect>& dirty) {
    REQUIRE(before.size() == frame.bytes.size());
    const FrameTarget& t = frame.target;
    const uint32_t bpp = helix::ui::pixel_bytes(t.format);
    FrameDiff diff;
    for (uint32_t y = 0; y < t.h; y++) {
        for (uint32_t x = 0; x < t.w; x++) {
            const size_t at = static_cast<size_t>(y) * t.stride + static_cast<size_t>(x) * bpp;
            if (std::equal(before.begin() + static_cast<std::ptrdiff_t>(at),
                           before.begin() + static_cast<std::ptrdiff_t>(at + bpp),
                           frame.bytes.begin() + static_cast<std::ptrdiff_t>(at))) {
                continue;
            }
            const auto xi = static_cast<int32_t>(x);
            const auto yi = static_cast<int32_t>(y);
            diff.changed++;
            diff.changed_box.add(xi, yi, xi, yi);
            if (!covers(dirty, xi, yi)) {
                diff.uncovered++;
            }
        }
    }
    return diff;
}

/// Pixels of `frame` whose colour satisfies `pred(Rgb)`.
template <typename Pred> size_t count_pixels(const FrameTarget& frame, Pred&& pred) {
    const PixelWriter reader(frame);
    size_t n = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(frame.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(frame.w); x++) {
            n += pred(reader.get(x, y)) ? 1 : 0;
        }
    }
    return n;
}

/// Pixels with any channel above 0.
inline size_t lit_pixels(const FrameTarget& frame) {
    return count_pixels(frame, [](Rgb c) { return c.r > 0 || c.g > 0 || c.b > 0; });
}

/// XRGB8888 pixels whose X byte is not 0xFF, which an ARGB8888 display draws see-through.
inline size_t non_opaque_pixels(const FrameTarget& frame) {
    REQUIRE(frame.format == PixelFormat::XRGB8888);
    size_t n = 0;
    for (uint32_t y = 0; y < frame.h; y++) {
        const uint8_t* row = frame.data + static_cast<size_t>(y) * frame.stride;
        for (uint32_t x = 0; x < frame.w; x++) {
            n += row[x * 4 + 3] != 0xFF ? 1 : 0;
        }
    }
    return n;
}

/// True when each channel of `a` is within `tol` of `b`'s.
inline bool near(Rgb a, Rgb b, int tol = 1) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

/// Seconds of the day at h:m:s.
constexpr int32_t hms(int32_t h, int32_t m, int32_t s = 0) {
    return h * 3600 + m * 60 + s;
}

/**
 * @brief A run replays and depends on its seed
 *
 * `run(seed)` returns what the run produced (a hash, a snapshot); the same seed must give the
 * same result and `seed_b` a different one, so a sim whose output ignores the seed (or the
 * input standing in for one) fails.
 */
template <typename Run> void check_replays(Run&& run, uint32_t seed_a, uint32_t seed_b) {
    const auto first = run(seed_a);
    CHECK(run(seed_a) == first);
    CHECK_FALSE(run(seed_b) == first);
}

/// Mean wall time, in ms, of `frames` calls of `step()`.
template <typename Step> double bench_ms_per_frame(Step&& step, int frames) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i++) {
        step();
    }
    const std::chrono::duration<double, std::milli> spent =
        std::chrono::steady_clock::now() - start;
    return frames > 0 ? spent.count() / frames : 0.0;
}

/**
 * @brief A sim, its frame, its rng and its dirty list, driven the way SimSaver drives a sim
 *
 * init() and step() take whatever the sim reads besides time (a clock reading, say); a sim
 * without an rng is driven without one (screensaver_sim_contract.h).
 */
template <typename Sim> struct SimRun {
    SimRun(uint32_t seed, uint32_t w, uint32_t h, PixelFormat format = PixelFormat::XRGB8888)
        : rng(seed), frame(w, h, format) {}

    template <typename... Extra> void init(size_t level, Extra&&... extra) {
        helix::ui::init_sim(sim, frame.target, rng, level, std::forward<Extra>(extra)...);
    }

    template <typename... Extra> void step(uint32_t dt_ms, Extra&&... extra) {
        helix::ui::step_sim(sim, dt_ms, frame.target, rng, dirty, std::forward<Extra>(extra)...);
    }

    std::minstd_rand rng;
    Sim sim;
    TestFrame frame;
    std::vector<DirtyRect> dirty;
};

#if HELIX_ENABLE_SCREENSAVER

/// A clock reading of the time of day `seconds_of_day`, with no print.
inline helix::ui::ClockReading reading_at(int32_t seconds_of_day, bool use_24h = false) {
    helix::ui::ClockReading r;
    r.time = helix::ui::clock_time(seconds_of_day, use_24h);
    r.use_24h = use_24h;
    return r;
}

/**
 * @brief A reading of a print in `mode` at `progress_pct`, at the time of day `seconds_of_day`
 *
 * COUNTDOWN and PAUSED carry `countdown_s` as a real estimate, COUNTDOWN its end minute too;
 * DONE reads 100% whatever `progress_pct` says, as ClockState reads it. CLOCK is reading_at().
 */
inline helix::ui::ClockReading print_reading_for(helix::ui::ClockMode mode, int32_t progress_pct,
                                                 int32_t seconds_of_day = hms(10, 8, 30),
                                                 int32_t countdown_s = 3723) {
    using helix::ui::ClockMode;
    helix::ui::ClockReading r = reading_at(seconds_of_day);
    if (mode == ClockMode::CLOCK) {
        return r;
    }
    r.mode = mode;
    r.progress_pct = mode == ClockMode::DONE ? 100 : progress_pct;
    if (mode == ClockMode::COUNTDOWN || mode == ClockMode::PAUSED) {
        r.countdown_s = countdown_s;
        r.has_countdown = true;
    }
    if (mode == ClockMode::COUNTDOWN) {
        r.eta_minute_of_day = ((seconds_of_day + countdown_s) / 60) % 1440;
    }
    return r;
}

/// An LVGL draw buffer (a canvas's, say) as a frame, for a format PixelWriter reads.
inline FrameTarget frame_of(const lv_draw_buf_t* buf) {
    REQUIRE(buf != nullptr);
    return {buf->data, buf->header.stride, buf->header.w, buf->header.h,
            helix::ui::pixel_format_for(static_cast<lv_color_format_t>(buf->header.cf))};
}

#endif // HELIX_ENABLE_SCREENSAVER

} // namespace helix::test
