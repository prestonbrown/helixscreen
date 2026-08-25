// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

/**
 * @file touch_calibration_panel.cpp
 * @brief Touch calibration panel state machine implementation
 */

#include "touch_calibration_panel.h"

#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace helix {

// Default screen dimensions used when invalid values are provided
constexpr int DEFAULT_SCREEN_WIDTH = 800;
constexpr int DEFAULT_SCREEN_HEIGHT = 480;

TouchCalibrationPanel::TouchCalibrationPanel()
    : debounce_enabled_(RuntimeConfig::touch_cal_debounce()) {}

TouchCalibrationPanel::~TouchCalibrationPanel() {
    stop_countdown_timer();
    stop_fast_revert_timer();
    stop_stall_timer();
}

void TouchCalibrationPanel::set_completion_callback(CompletionCallback cb) {
    callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_failure_callback(FailureCallback cb) {
    failure_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_countdown_callback(CountdownCallback cb) {
    countdown_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_timeout_callback(TimeoutCallback cb) {
    timeout_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_fast_revert_callback(FastRevertCallback cb) {
    fast_revert_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_sample_progress_callback(SampleProgressCallback cb) {
    sample_progress_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_verify_entry_callback(VerifyEntryCallback cb) {
    verify_entry_callback_ = std::move(cb);
}

void TouchCalibrationPanel::set_verify_timeout_seconds(int seconds) {
    verify_timeout_seconds_ = seconds;
}

void TouchCalibrationPanel::set_screen_size(int width, int height) {
    // Validate screen dimensions - reject zero or negative values
    if (width <= 0 || height <= 0) {
        spdlog::warn("[TouchCalibrationPanel] Invalid screen size {}x{}, using defaults {}x{}",
                     width, height, DEFAULT_SCREEN_WIDTH, DEFAULT_SCREEN_HEIGHT);
        screen_width_ = DEFAULT_SCREEN_WIDTH;
        screen_height_ = DEFAULT_SCREEN_HEIGHT;
        return;
    }
    screen_width_ = width;
    screen_height_ = height;

    // Diagnostic: the target space the 3-point solve will map captured taps into.
    // Combined with the ABS-range log (display backend) and the solve's
    // screen/touch-point dump (touch_calibration.cpp), this pins down whether a
    // coordinate-space/rotation mismatch is producing bad calibration (#986).
    if (helix::is_touch_debug_enabled()) {
        lv_display_t* disp = lv_display_get_default();
        spdlog::warn("[TouchDebug] calibration target space: {}x{} display_rotation={}",
                     screen_width_, screen_height_,
                     disp ? static_cast<int>(lv_display_get_rotation(disp)) : -1);
    }
}

Point TouchCalibrationPanel::compute_target_position(int step) const {
    switch (step) {
    case 0:
        return {static_cast<int>(screen_width_ * TARGET_0_X_RATIO),
                static_cast<int>(screen_height_ * TARGET_0_Y_RATIO)};
    case 1:
        return {static_cast<int>(screen_width_ * TARGET_1_X_RATIO),
                static_cast<int>(screen_height_ * TARGET_1_Y_RATIO)};
    case 2:
        return {static_cast<int>(screen_width_ * TARGET_2_X_RATIO),
                static_cast<int>(screen_height_ * TARGET_2_Y_RATIO)};
    default:
        return Point{0, 0};
    }
}

void TouchCalibrationPanel::start() {
    state_ = State::POINT_1;
    calibration_.valid = false;
    range_fit_ = TouchRangeFit{};
    raw_points_valid_ = true;
    reset_samples();

    // Calculate screen target positions using named constants
    screen_points_[0] = compute_target_position(0);
    screen_points_[1] = compute_target_position(1);
    screen_points_[2] = compute_target_position(2);
}

void TouchCalibrationPanel::capture_point(Point raw, const Point* device_raw) {
    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] capture_point: state={} raw=({},{}) device_raw=({},{}) have={}",
                     static_cast<int>(state_), raw.x, raw.y, device_raw ? device_raw->x : 0,
                     device_raw ? device_raw->y : 0, device_raw != nullptr);
    }

    // A range fit needs all three digitizer readings; one missing point drops the
    // whole session back to the affine-only answer.
    const int slot = state_ == State::POINT_1   ? 0
                     : state_ == State::POINT_2 ? 1
                     : state_ == State::POINT_3 ? 2
                                                : -1;
    if (slot >= 0) {
        if (device_raw) {
            raw_points_[slot] = *device_raw;
        } else {
            raw_points_valid_ = false;
        }
    }

    switch (state_) {
    case State::POINT_1:
        touch_points_[0] = raw;
        state_ = State::POINT_2;
        break;
    case State::POINT_2:
        touch_points_[1] = raw;
        state_ = State::POINT_3;
        break;
    case State::POINT_3:
        touch_points_[2] = raw;

        if (!compute_calibration(screen_points_, touch_points_, calibration_)) {
            spdlog::warn(
                "[TouchCalibrationPanel] Calibration failed (degenerate points), restarting");
            state_ = State::POINT_1;
            calibration_.valid = false;
            // The retry starts a fresh set of three captures, so a point that
            // arrived without a digitizer reading must not veto the next attempt.
            raw_points_valid_ = true;
            if (failure_callback_) {
                failure_callback_("Touch points too close together. Please try again.");
            }
            break;
        }

        // Detect and correct swapped touch axes (e.g., Ender 5 Max screens)
        // This swaps touch_points_ in-place and recomputes calibration_ if needed
        detect_and_correct_axis_swap(calibration_, screen_points_, touch_points_);

        if (is_touch_debug_enabled()) {
            spdlog::warn("[TouchDebug] calibration computed for all 3 points:");
            for (int i = 0; i < 3; i++) {
                spdlog::warn("[TouchDebug]   point[{}]: screen=({},{}) touch=({},{})", i,
                             screen_points_[i].x, screen_points_[i].y, touch_points_[i].x,
                             touch_points_[i].y);
            }
            if (calibration_.axes_swapped) {
                spdlog::warn("[TouchDebug]   axes were auto-swapped");
            }
        }

        // #943 always-on diagnostic. The captured touch points and the on-screen
        // targets both live in panel-pixel space (touches arrive already coarse-
        // scaled by lv_evdev_set_calibration). A user who taps on each target
        // should produce a captured span that matches the target span, so the
        // ratio is ~1.0 when the pipeline is healthy. When the coarse scale over-
        // divides — the digitizer reports a wider ABS range than it actually emits
        // (Goodix/Q2 class) — taps collapse into a fraction of the panel and this
        // ratio drops well below 1.0. Emitted at warn once per completed
        // calibration so it lands in a plain journalctl capture without
        // HELIX_DEBUG_TOUCH; the single line that shows whether touch is
        // compressed and by how much.
        {
            auto axis_ratio = [](const int s[3], const int t[3]) -> double {
                int sd01 = std::abs(s[0] - s[1]);
                int sd02 = std::abs(s[0] - s[2]);
                int sd12 = std::abs(s[1] - s[2]);
                int sspan, tspan;
                if (sd01 >= sd02 && sd01 >= sd12) {
                    sspan = sd01;
                    tspan = std::abs(t[0] - t[1]);
                } else if (sd02 >= sd12) {
                    sspan = sd02;
                    tspan = std::abs(t[0] - t[2]);
                } else {
                    sspan = sd12;
                    tspan = std::abs(t[1] - t[2]);
                }
                return sspan > 0 ? static_cast<double>(tspan) / sspan : 0.0;
            };
            const int sx[3] = {screen_points_[0].x, screen_points_[1].x, screen_points_[2].x};
            const int tx[3] = {touch_points_[0].x, touch_points_[1].x, touch_points_[2].x};
            const int sy[3] = {screen_points_[0].y, screen_points_[1].y, screen_points_[2].y};
            const int ty[3] = {touch_points_[0].y, touch_points_[1].y, touch_points_[2].y};
            const double rx = axis_ratio(sx, tx);
            const double ry = axis_ratio(sy, ty);
            spdlog::warn("[TouchCal] #943 span check: captured/target ratio x={:.2f} y={:.2f} "
                         "(~1.0 healthy; <0.85 => coarse scale over-divides / digitizer over-"
                         "reports ABS range; >1.15 => under-scaled). target space {}x{}",
                         rx, ry, screen_width_, screen_height_);
        }

        // Validate the matrix produces reasonable results
        if (!validate_calibration_result(calibration_, screen_points_, touch_points_, screen_width_,
                                         screen_height_)) {
            spdlog::warn(
                "[TouchCalibrationPanel] Calibration matrix failed validation, restarting");
            state_ = State::POINT_1;
            calibration_.valid = false;
            raw_points_valid_ = true;
            if (failure_callback_) {
                failure_callback_("Calibration produced unusual results. Please try again.");
            }
            break;
        }

        // Solve the evdev stage from the untouched digitizer readings. This is a
        // re-parameterisation of the very matrix just validated, not a second
        // opinion: the range plus range_fit_.residual reproduce calibration_ once
        // the range is programmed, so nothing here can widen what VERIFY approves.
        // Deliberately after validation, so a matrix the user will be asked to
        // retry never leaves a range fit behind.
        if (raw_points_valid_) {
            range_fit_ =
                compute_range_fit(screen_points_, raw_points_, screen_width_, screen_height_);
        } else {
            range_fit_ = TouchRangeFit{};
            spdlog::debug("[TouchCalibrationPanel] No raw digitizer readings captured - "
                          "affine-only calibration (this is normal off evdev)");
        }

        state_ = State::VERIFY;
        start_countdown_timer();
        start_fast_revert_timer();
        // Notify the overlay the instant we enter VERIFY so it can re-enable
        // the original calibration for safe button dispatch. Fired here (not on
        // an input edge) so every commit path — release event, stall timer, and
        // legacy sample-on-press — re-enables affine identically (#943/#986).
        if (verify_entry_callback_) {
            verify_entry_callback_();
        }
        break;
    default:
        // No-op in IDLE, VERIFY, COMPLETE states
        break;
    }
}

bool TouchCalibrationPanel::is_bad_sample(const Point& sample) const {
    // ADC saturation (12-bit or 16-bit max)
    return sample.x == 4095 || sample.y == 4095 || sample.x == 65535 || sample.y == 65535;
}

void TouchCalibrationPanel::reset_samples() {
    sample_count_ = 0;
}

bool TouchCalibrationPanel::compute_median_point(Point& out, Point& out_raw, bool& out_has_raw) {
    std::vector<int> valid_x, valid_y;
    std::vector<int> raw_x, raw_y;
    out_has_raw = true;
    for (int i = 0; i < sample_count_; i++) {
        Point p{sample_buffer_[i].x, sample_buffer_[i].y};
        if (!is_bad_sample(p)) {
            valid_x.push_back(p.x);
            valid_y.push_back(p.y);
            if (sample_buffer_[i].has_device) {
                raw_x.push_back(sample_buffer_[i].device_x);
                raw_y.push_back(sample_buffer_[i].device_y);
            } else {
                out_has_raw = false;
            }
        }
    }

    if (static_cast<int>(valid_x.size()) < MIN_VALID_SAMPLES) {
        spdlog::warn("[TouchCalibrationPanel] Only {}/{} valid samples (need {})", valid_x.size(),
                     sample_count_, MIN_VALID_SAMPLES);
        return false;
    }

    std::sort(valid_x.begin(), valid_x.end());
    std::sort(valid_y.begin(), valid_y.end());

    // Reject if samples have excessive spread (noisy controller)
    int x_spread = valid_x.back() - valid_x.front();
    int y_spread = valid_y.back() - valid_y.front();
    if (x_spread > MAX_SAMPLE_SPREAD || y_spread > MAX_SAMPLE_SPREAD) {
        spdlog::warn("[TouchCalibrationPanel] Sample spread too large "
                     "(x_spread={}, y_spread={}, max={})",
                     x_spread, y_spread, MAX_SAMPLE_SPREAD);
        return false;
    }

    size_t mid_x = valid_x.size() / 2;
    size_t mid_y = valid_y.size() / 2;
    out.x = valid_x[mid_x];
    out.y = valid_y[mid_y];

    // Median the paired digitizer readings over the same accepted samples. No
    // separate spread/saturation gate - see the header for why the pixel-space
    // thresholds do not transfer to raw units.
    if (out_has_raw && !raw_x.empty()) {
        std::sort(raw_x.begin(), raw_x.end());
        std::sort(raw_y.begin(), raw_y.end());
        out_raw.x = raw_x[raw_x.size() / 2];
        out_raw.y = raw_y[raw_y.size() / 2];
    } else {
        out_has_raw = false;
    }

    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] median computation: {}/{} valid samples", valid_x.size(),
                     sample_count_);
        for (size_t i = 0; i < valid_x.size(); i++) {
            spdlog::warn("[TouchDebug]   valid[{}]: ({},{})", i, valid_x[i], valid_y[i]);
        }
        spdlog::warn("[TouchDebug]   spread: x={}, y={} (max={})", x_spread, y_spread,
                     MAX_SAMPLE_SPREAD);
        spdlog::warn("[TouchDebug]   median: ({},{})", out.x, out.y);
    }

    spdlog::debug("[TouchCalibrationPanel] Median from {}/{} valid samples: ({}, {})",
                  valid_x.size(), sample_count_, out.x, out.y);
    return true;
}

TouchCalibrationPanel::Progress TouchCalibrationPanel::get_progress() const {
    Progress p{};
    p.state = state_;
    p.current_sample = sample_count_;
    p.total_samples = SAMPLES_REQUIRED;

    switch (state_) {
    case State::POINT_1:
        p.point_num = 1;
        break;
    case State::POINT_2:
        p.point_num = 2;
        break;
    case State::POINT_3:
        p.point_num = 3;
        break;
    default:
        p.point_num = 0;
        break;
    }
    return p;
}

void TouchCalibrationPanel::add_sample(Point raw, const Point* device_raw) {
    // Auto-start on first tap if in IDLE state (don't count this tap as a sample —
    // the crosshair isn't visible yet, so the user's first tap ON the crosshair is touch 1)
    if (state_ == State::IDLE) {
        start();
        return;
    }

    if (state_ != State::POINT_1 && state_ != State::POINT_2 && state_ != State::POINT_3) {
        return;
    }

    if (sample_count_ < SAMPLES_REQUIRED) {
        sample_buffer_[sample_count_] = {raw.x, raw.y, device_raw ? device_raw->x : 0,
                                         device_raw ? device_raw->y : 0, device_raw != nullptr};
        sample_count_++;

        if (is_touch_debug_enabled()) {
            auto p = get_progress();
            spdlog::warn("[TouchDebug] sample {}/{} for point {}: raw=({},{}){}", sample_count_,
                         SAMPLES_REQUIRED, p.point_num, raw.x, raw.y,
                         is_bad_sample(raw) ? " REJECTED" : "");
        }

        // Only fire progress callback for intermediate samples, not the final
        // one that triggers state transition (avoids showing "touch 6 of 5")
        if (sample_count_ < SAMPLES_REQUIRED && sample_progress_callback_) {
            sample_progress_callback_();
        }
    }

    if (sample_count_ >= SAMPLES_REQUIRED) {
        Point median;
        Point raw_median;
        bool has_raw_median = false;
        // reads sample_buffer_
        const bool ok = compute_median_point(median, raw_median, has_raw_median);

        // Reset the per-point sample count BEFORE invoking the success/failure
        // callbacks. Those callbacks refresh the "touch N of 3" instruction
        // label; if the count were still at SAMPLES_REQUIRED when they run, the
        // label would render "touch 4 of 3" (current_sample + 1). Clearing first
        // means any UI refresh the callbacks trigger sees a fresh count of 0.
        reset_samples();

        if (ok) {
            capture_point(median, has_raw_median ? &raw_median : nullptr);
        } else if (failure_callback_) {
            failure_callback_("Too much noise — tap the target again slowly and precisely.");
        }
    }
}

void TouchCalibrationPanel::on_press(Point raw, const Point* device_raw) {
    // Opt-out: legacy sample-on-press behavior (byte-for-byte the pre-#943 path)
    // for A/B testing on real hardware.
    if (!debounce_enabled_) {
        add_sample(raw, device_raw);
        return;
    }

    // Capture the press; defer committing to on_release() or the stall timer.
    // A burst of PRESSED edges from one physical contact overwrites the same
    // pending press, so only the final position is committed once.
    pending_press_point_ = raw;
    pending_has_raw_ = device_raw != nullptr;
    if (device_raw) {
        pending_raw_point_ = *device_raw;
    }
    press_pending_ = true;
    press_time_ms_ = now_fn_();

    if (is_touch_debug_enabled()) {
        spdlog::debug("[TouchDebug] press captured ({},{}), pending commit", raw.x, raw.y);
    }

    // Arm the stall fallback for panels that never deliver a clean RELEASED.
    start_stall_timer();
}

void TouchCalibrationPanel::on_release() {
    // No-op when debounce is disabled — keeps the default path byte-for-byte
    // identical to pre-#943 behavior (release does nothing).
    if (!debounce_enabled_) {
        return;
    }
    stop_stall_timer();
    commit_pending(now_fn_());
}

void TouchCalibrationPanel::commit_pending(uint32_t now) {
    if (!press_pending_) {
        return;
    }

    // Release-immune refractory: a commit too soon after the last committed
    // sample is capacitive bounce, not a deliberate tap. Drop it. The very first
    // commit of a session has no prior sample to bounce off of, so it always
    // passes (has_committed_ arms the window).
    if (has_committed_ && now - last_sample_ms_ < REFRACTORY_MS) {
        press_pending_ = false;
        if (is_touch_debug_enabled()) {
            spdlog::debug("[TouchDebug] dropped bounce sample, dt={}ms", now - last_sample_ms_);
        }
        return;
    }

    add_sample(pending_press_point_, pending_has_raw_ ? &pending_raw_point_ : nullptr);
    last_sample_ms_ = now;
    has_committed_ = true;
    press_pending_ = false;
}

void TouchCalibrationPanel::commit_pending_if_stale(uint32_t now) {
    // Fallback for panels that never deliver a clean RELEASED: commit a press
    // that has been outstanding at least STALL_COMMIT_MS so calibration cannot
    // wedge forever.
    if (press_pending_ && (now - press_time_ms_ >= STALL_COMMIT_MS)) {
        commit_pending(now);
    }
}

void TouchCalibrationPanel::start_stall_timer() {
    stop_stall_timer();
    stall_timer_ = lv_timer_create(stall_timer_cb, STALL_COMMIT_MS, this);
    lv_timer_set_repeat_count(stall_timer_, 1);
}

void TouchCalibrationPanel::stop_stall_timer() {
    if (stall_timer_ != nullptr) {
        lv_timer_delete(stall_timer_);
        stall_timer_ = nullptr;
    }
}

void TouchCalibrationPanel::stall_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<TouchCalibrationPanel*>(lv_timer_get_user_data(timer));
    self->stall_timer_ = nullptr; // Timer auto-deletes (repeat_count=1)
    self->commit_pending_if_stale(self->now_fn_());
}

void TouchCalibrationPanel::accept() {
    stop_countdown_timer();
    stop_fast_revert_timer();

    if (state_ != State::VERIFY) {
        return;
    }

    state_ = State::COMPLETE;
    if (callback_) {
        callback_(&calibration_);
    }
}

void TouchCalibrationPanel::retry() {
    if (state_ != State::VERIFY) {
        return;
    }

    stop_countdown_timer();
    stop_fast_revert_timer();
    start(); // Resets state to POINT_1 and recalculates target positions
}

void TouchCalibrationPanel::cancel() {
    stop_countdown_timer();
    stop_fast_revert_timer();

    state_ = State::IDLE;
    calibration_.valid = false;
    if (callback_) {
        callback_(nullptr);
    }
}

void TouchCalibrationPanel::reset() {
    // Silent fresh-start for the singleton overlay's show() path (#943). This
    // mirrors a freshly constructed panel; it deliberately does NOT invoke the
    // completion callback the way cancel() does.
    stop_countdown_timer();
    stop_fast_revert_timer();
    stop_stall_timer();

    state_ = State::IDLE;
    calibration_.valid = false;
    range_fit_ = TouchRangeFit{};
    raw_points_valid_ = false;

    // Sample buffer + per-point capture progress.
    reset_samples();

    // Press-debounce state: a session that ended mid-press could leave a pending
    // press uncommitted, swallowing the next session's first tap.
    press_pending_ = false;
    pending_press_point_ = Point{};
    pending_raw_point_ = Point{};
    pending_has_raw_ = false;
    press_time_ms_ = 0;
    last_sample_ms_ = 0;
    has_committed_ = false;

    // Re-read the debounce setting so a value that changed since construction
    // (or was unset when the singleton was built early in startup) applies to
    // this session. RuntimeConfig caches the env read, so this is cheap.
    debounce_enabled_ = RuntimeConfig::touch_cal_debounce();

    // VERIFY-state broken-matrix detection counters.
    verify_raw_touch_count_ = 0;
    verify_onscreen_touch_count_ = 0;
}

TouchCalibrationPanel::State TouchCalibrationPanel::get_state() const {
    return state_;
}

Point TouchCalibrationPanel::get_target_position(int step) const {
    if (step < 0 || step > 2) {
        return Point{0, 0};
    }
    // Delegate to private helper that uses named constants
    return compute_target_position(step);
}

const TouchCalibration* TouchCalibrationPanel::get_calibration() const {
    if ((state_ == State::VERIFY || state_ == State::COMPLETE) && calibration_.valid) {
        return &calibration_;
    }
    return nullptr;
}

const TouchRangeFit& TouchCalibrationPanel::get_range_fit() const {
    return range_fit_;
}

void TouchCalibrationPanel::start_countdown_timer() {
    // Cancel first, for the same reason as start_fast_revert_timer(): a second
    // start would strand the first timer in LVGL's list pointing at this panel.
    stop_countdown_timer();
    countdown_remaining_ = verify_timeout_seconds_;
    countdown_timer_ = lv_timer_create(countdown_timer_cb, 1000, this);
    spdlog::debug("[TouchCalibrationPanel] Started countdown timer: {} seconds",
                  countdown_remaining_);

    // Immediately notify with initial value
    if (countdown_callback_) {
        countdown_callback_(countdown_remaining_);
    }
}

void TouchCalibrationPanel::stop_countdown_timer() {
    if (countdown_timer_ != nullptr) {
        lv_timer_delete(countdown_timer_);
        countdown_timer_ = nullptr;
        spdlog::debug("[TouchCalibrationPanel] Stopped countdown timer");
    }
}

void TouchCalibrationPanel::countdown_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<TouchCalibrationPanel*>(lv_timer_get_user_data(timer));
    self->countdown_remaining_--;
    spdlog::debug("[TouchCalibrationPanel] Countdown tick: {} seconds remaining",
                  self->countdown_remaining_);

    if (self->countdown_remaining_ > 0) {
        if (self->countdown_callback_) {
            self->countdown_callback_(self->countdown_remaining_);
        }
    } else {
        spdlog::debug("[TouchCalibrationPanel] Countdown expired, invoking timeout callback");
        if (self->timeout_callback_) {
            self->timeout_callback_();
        }
        self->stop_countdown_timer();
    }
}

void TouchCalibrationPanel::report_verify_touch(bool on_screen) {
    if (state_ != State::VERIFY)
        return;
    verify_raw_touch_count_++;
    if (on_screen) {
        verify_onscreen_touch_count_++;
    }
}

void TouchCalibrationPanel::start_fast_revert_timer() {
    // Cancel first, like start_stall_timer(). Overwriting the handle instead
    // orphans the previous lv_timer_t: it stays armed in LVGL's list holding
    // user_data == this, the destructor only ever deletes the newest handle,
    // and the orphan then fires on freed memory (ASAN: heap-buffer-overflow
    // WRITE at the `self->fast_revert_timer_ = nullptr` below, landing in an
    // unrelated test's process_lvgl). Re-entering VERIFY is enough to get here
    // twice.
    stop_fast_revert_timer();
    verify_raw_touch_count_ = 0;
    verify_onscreen_touch_count_ = 0;
    fast_revert_timer_ = lv_timer_create(fast_revert_timer_cb, FAST_REVERT_CHECK_MS, this);
    lv_timer_set_repeat_count(fast_revert_timer_, 1);
    spdlog::debug("[TouchCalibrationPanel] Started fast-revert timer ({}ms)", FAST_REVERT_CHECK_MS);
}

void TouchCalibrationPanel::stop_fast_revert_timer() {
    if (fast_revert_timer_) {
        lv_timer_delete(fast_revert_timer_);
        fast_revert_timer_ = nullptr;
    }
}

void TouchCalibrationPanel::fast_revert_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<TouchCalibrationPanel*>(lv_timer_get_user_data(timer));
    self->fast_revert_timer_ = nullptr; // Timer auto-deletes (repeat_count=1)

    if (self->state_ != State::VERIFY)
        return;

    if (self->verify_raw_touch_count_ > 0 && self->verify_onscreen_touch_count_ == 0) {
        spdlog::warn("[TouchCalibrationPanel] Fast-revert: {} raw touches, 0 on-screen — "
                     "matrix is broken, reverting",
                     self->verify_raw_touch_count_);
        if (self->fast_revert_callback_) {
            self->fast_revert_callback_();
        }
    } else {
        spdlog::debug("[TouchCalibrationPanel] Fast-revert check passed: {}/{} on-screen",
                      self->verify_onscreen_touch_count_, self->verify_raw_touch_count_);
    }
}

} // namespace helix
