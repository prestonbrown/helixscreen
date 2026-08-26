// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#pragma once

#include "touch_calibration.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>

namespace helix {

class TouchCalibrationPanelTestAccess;

/**
 * @brief Touch calibration panel state machine
 *
 * Manages a 3-point touch calibration workflow:
 *
 * States:
 *   IDLE -> POINT_1 -> POINT_2 -> POINT_3 -> VERIFY -> COMPLETE
 *            |          |          |          |
 *            v          v          v          v
 *        (capture)  (capture)  (capture)  (accept/retry)
 *
 * Usage:
 * 1. Create panel and set screen size
 * 2. Set completion callback
 * 3. Call start() to begin calibration
 * 4. Display target at get_target_position(step)
 * 5. Call capture_point() when user touches screen
 * 6. In VERIFY state, display calibration and allow accept()/retry()
 * 7. Callback invoked with calibration data on accept() or nullptr on cancel()
 */
class TouchCalibrationPanel {
  public:
    /**
     * @brief Calibration state machine states
     */
    enum class State {
        IDLE,    ///< Not calibrating
        POINT_1, ///< Waiting for first calibration point
        POINT_2, ///< Waiting for second calibration point
        POINT_3, ///< Waiting for third calibration point
        VERIFY,  ///< Calibration computed, waiting for accept/retry
        COMPLETE ///< Calibration accepted
    };

    /**
     * @brief Callback invoked when calibration completes or is cancelled
     *
     * @param cal Pointer to calibration data if accepted, nullptr if cancelled
     */
    using CompletionCallback = std::function<void(const TouchCalibration* cal)>;

    /**
     * @brief Callback invoked when calibration fails (e.g., degenerate points)
     *
     * @param reason Human-readable failure reason
     */
    using FailureCallback = std::function<void(const char* reason)>;

    /**
     * @brief Callback invoked each second during verify countdown
     * @param seconds_remaining Seconds until timeout (10, 9, 8, ...)
     */
    using CountdownCallback = std::function<void(int seconds_remaining)>;

    /**
     * @brief Callback invoked when verify timeout expires without accept
     */
    using TimeoutCallback = std::function<void()>;

    /**
     * @brief Callback invoked when fast-revert triggers (broken matrix detected)
     */
    using FastRevertCallback = std::function<void()>;

    /**
     * @brief Callback invoked after each sample is added (for UI updates)
     */
    using SampleProgressCallback = std::function<void()>;

    /**
     * @brief Callback invoked exactly once when the panel enters VERIFY
     *
     * Fired the instant the third point commits and the matrix validates,
     * regardless of which path triggered the commit (release event, stall
     * timer, or legacy sample-on-press). The overlay uses this to re-enable
     * the original calibration so Accept/Retry dispatch on their true screen
     * locations — tying that to a specific input edge is fragile (the
     * commit-on-release debounce moved the transition off the press edge and
     * left VERIFY running on raw, Y-inverted coordinates, #943/#986).
     */
    using VerifyEntryCallback = std::function<void()>;

    /**
     * @brief Snapshot of calibration progress for UI display
     */
    struct Progress {
        State state;
        int point_num;      ///< 1-3 for POINT states, 0 otherwise
        int current_sample; ///< Samples collected for current point (0 to total_samples)
        int total_samples;  ///< Samples required per point (SAMPLES_REQUIRED)
    };

    TouchCalibrationPanel();
    ~TouchCalibrationPanel();

    // Non-copyable
    TouchCalibrationPanel(const TouchCalibrationPanel&) = delete;
    TouchCalibrationPanel& operator=(const TouchCalibrationPanel&) = delete;

    // Non-movable (LVGL timer user-data holds raw 'this' pointer)
    TouchCalibrationPanel(TouchCalibrationPanel&&) = delete;
    TouchCalibrationPanel& operator=(TouchCalibrationPanel&&) = delete;

    /**
     * @brief Set the completion callback
     * @param cb Callback to invoke when calibration completes or is cancelled
     */
    void set_completion_callback(CompletionCallback cb);

    /**
     * @brief Set the failure callback
     * @param cb Callback to invoke when calibration fails (degenerate points, etc.)
     */
    void set_failure_callback(FailureCallback cb);

    /**
     * @brief Set callback for countdown ticks during verify state
     */
    void set_countdown_callback(CountdownCallback cb);

    /**
     * @brief Set callback for timeout expiration
     */
    void set_timeout_callback(TimeoutCallback cb);

    /**
     * @brief Set callback for fast-revert (broken matrix during verify)
     */
    void set_fast_revert_callback(FastRevertCallback cb);

    /**
     * @brief Set callback for sample progress during point capture
     */
    void set_sample_progress_callback(SampleProgressCallback cb);

    /**
     * @brief Set callback fired when the panel transitions into VERIFY
     */
    void set_verify_entry_callback(VerifyEntryCallback cb);

    /**
     * @brief Set verify timeout duration (default: 10 seconds)
     */
    void set_verify_timeout_seconds(int seconds);

    /**
     * @brief Report a touch event during verify state for broken-matrix detection
     * @param on_screen Whether the transformed point is within screen bounds
     */
    void report_verify_touch(bool on_screen);

    /**
     * @brief Set the screen dimensions for target position calculations
     * @param width Screen width in pixels
     * @param height Screen height in pixels
     */
    void set_screen_size(int width, int height);

    /**
     * @brief Start or restart calibration
     *
     * Transitions to POINT_1 state.
     */
    void start();

    /**
     * @brief Capture a raw touch point for the current calibration step
     * @param raw Raw touch coordinates from touch controller
     *
     * Only valid in POINT_1, POINT_2, or POINT_3 states.
     * Advances to next state after capture.
     */
    void capture_point(Point raw, const Point* device_raw = nullptr);

    /**
     * @brief Add a raw touch sample to the current capture buffer
     *
     * Collects multiple samples per calibration point. When SAMPLES_REQUIRED
     * samples have been collected, filters out ADC-saturated values, computes
     * the median, and advances the state machine via capture_point().
     *
     * @param raw Raw touch coordinates from touch controller
     * @param device_raw Optional PRE-SWAP, PRE-SCALE digitizer reading behind
     *        `raw` (lv_evdev_get_last_raw via helix::get_last_raw_touch). Null on
     *        any backend that cannot supply one, which drops the run back to the
     *        affine-only result with no other change in behaviour.
     */
    void add_sample(Point raw, const Point* device_raw = nullptr);

    /**
     * @brief Handle a press edge (LV_EVENT_PRESSED) for the current step
     * @param raw Raw touch coordinates from the touch controller
     *
     * Capture-on-press, commit-on-release model (issue #943). Capacitive panels
     * (Goodix/Q2) emit multiple press/release cycles per single physical contact;
     * sampling on every press inflated one tap into several samples and cascaded
     * the state machine. Instead of sampling here, we capture the press point and
     * defer committing it to on_release() (or the stall timer). A release-IMMUNE
     * time refractory (REFRACTORY_MS since the last committed sample) drops bounce.
     *
     * When debounce is disabled (HELIX_TOUCH_CAL_DEBOUNCE=0) this samples
     * immediately, preserving the exact legacy sample-on-press behavior for A/B
     * testing. Wired to LV_EVENT_PRESSED by the Settings overlay and first-boot
     * wizard.
     */
    void on_press(Point raw, const Point* device_raw = nullptr);

    /**
     * @brief Handle a release edge (LV_EVENT_RELEASED)
     *
     * Commits the pending press captured by on_press(), subject to the refractory
     * window. No-op when debounce is disabled (legacy: release does nothing).
     * Wired to LV_EVENT_RELEASED by the Settings overlay and first-boot wizard.
     */
    void on_release();

    /**
     * @brief Accept the computed calibration
     *
     * Only valid in VERIFY state.
     * Transitions to COMPLETE and invokes callback with calibration data.
     */
    void accept();

    /**
     * @brief Retry calibration from the beginning
     *
     * Only valid in VERIFY state.
     * Transitions back to POINT_1 and clears captured points.
     */
    void retry();

    /**
     * @brief Cancel calibration
     *
     * Returns to IDLE state and invokes callback with nullptr.
     */
    void cancel();

    /**
     * @brief Reset ALL per-session state to a fresh-constructed baseline (#943)
     *
     * Unlike cancel(), reset() is a SILENT fresh-start: it does NOT invoke the
     * completion callback. The Settings-launched overlay reuses a persistent
     * singleton panel, so it must reset on every show to behave like the
     * first-run wizard (which builds a brand-new panel each time). Without this,
     * stale state — an armed press-debounce gate, a half-filled sample buffer, a
     * non-IDLE state-machine position, verify touch counters — carried into the
     * next show and made the second calibration misbehave.
     *
     * Returns the panel to: IDLE state, no computed calibration, empty sample
     * buffer, cleared debounce gate, zeroed verify counters, stopped timers.
     * Re-reads the debounce setting from RuntimeConfig so a value that changed
     * (or was unset at construction) takes effect on the next session.
     */
    void reset();

    /**
     * @brief Get current state
     * @return Current state machine state
     */
    State get_state() const;

    /**
     * @brief Get target position for a calibration step
     * @param step Step number (0, 1, or 2)
     * @return Screen coordinates where target should be displayed
     *
     * Returns (0, 0) for out-of-range step values.
     *
     * Default target positions (for 800x480 screen):
     *   Step 0: (120, 96)  - 15% from left, 20% from top
     *   Step 1: (400, 374) - center X, 78% from top
     *   Step 2: (680, 96)  - 85% from left, 20% from top
     */
    Point get_target_position(int step) const;

    /**
     * @brief Get current calibration progress for UI display
     * @return Progress snapshot with state, point number, and sample counts
     */
    Progress get_progress() const;

    /**
     * @brief Get computed calibration data
     * @return Pointer to calibration if in VERIFY/COMPLETE state, nullptr otherwise
     */
    const TouchCalibration* get_calibration() const;

    /**
     * @brief Get the evdev range decomposition of the computed calibration
     *
     * Solved alongside get_calibration() when every captured point carried a raw
     * digitizer reading. `.valid == false` means the affine returned by
     * get_calibration() is the whole answer, exactly as before #1259.
     *
     * The two are alternatives, not layers: get_calibration() is the affine over
     * the CURRENT evdev range (what VERIFY installs, since VERIFY runs before the
     * range is touched), while this is the range plus the residual affine that
     * together reproduce the same mapping once the range has been re-programmed.
     * Persisting or applying both full forms would double-count.
     *
     * @return The range fit; `.valid == false` when none could be solved
     */
    const TouchRangeFit& get_range_fit() const;

  private:
    State state_ = State::IDLE;
    int screen_width_ = 800;
    int screen_height_ = 480;
    CompletionCallback callback_;
    FailureCallback failure_callback_;
    CountdownCallback countdown_callback_;
    TimeoutCallback timeout_callback_;
    FastRevertCallback fast_revert_callback_;
    SampleProgressCallback sample_progress_callback_;
    VerifyEntryCallback verify_entry_callback_;
    int verify_timeout_seconds_ = 10;
    int countdown_remaining_ = 0;
    lv_timer_t* countdown_timer_ = nullptr;

    Point screen_points_[3]; ///< Target screen positions
    Point touch_points_[3];  ///< Captured pre-affine (post-evdev) touch positions
    /// Captured PRE-SWAP, PRE-SCALE digitizer readings, paired index-for-index
    /// with touch_points_. Only meaningful while raw_points_valid_.
    Point raw_points_[3];
    /// Every point captured this session carried a raw reading. Goes false the
    /// moment one does not, because a range fit needs all three.
    bool raw_points_valid_ = false;
    TouchCalibration calibration_;
    TouchRangeFit range_fit_;

    // Multi-sample filtering
    static constexpr int SAMPLES_REQUIRED = 3;
    static constexpr int MIN_VALID_SAMPLES = 2;
    static constexpr int MAX_SAMPLE_SPREAD = 60; ///< Max allowed range in either axis (pixels)

    struct RawSample {
        int x = 0;
        int y = 0;
        int device_x = 0; ///< pre-swap, pre-scale digitizer reading
        int device_y = 0;
        bool has_device = false;
    };
    RawSample sample_buffer_[SAMPLES_REQUIRED]{};
    int sample_count_ = 0;

    // Press-debounce (issue #943): capture-on-press, commit-on-release model with
    // a release-IMMUNE time refractory. Capacitive panels emit several
    // press/release cycles per physical contact; committing on every press
    // inflated one tap into multiple samples. ON by default; opt out with
    // HELIX_TOUCH_CAL_DEBOUNCE=0.
    //
    // `debounce_enabled_` is seeded once from RuntimeConfig::touch_cal_debounce()
    // in the constructor (and re-read in reset()). When false, on_press() samples
    // immediately and on_release() is a no-op — byte-for-byte the pre-#943 path.
    bool debounce_enabled_ = true;
    bool press_pending_ = false;  ///< a press is captured, awaiting commit
    Point pending_press_point_{}; ///< point captured at press time
    Point pending_raw_point_{};   ///< digitizer reading behind that press
    bool pending_has_raw_ = false;
    uint32_t press_time_ms_ = 0;  ///< now() when the pending press started
    uint32_t last_sample_ms_ = 0; ///< now() of last COMMITTED sample (refractory anchor)
    bool has_committed_ = false;  ///< a sample committed this session (arms the refractory)

    /// Release-immune refractory: a commit within this many ms of the previous
    /// committed sample is treated as bounce and dropped. Capacitive bounce
    /// resolves in <50ms; deliberate taps are >200ms apart.
    static constexpr uint32_t REFRACTORY_MS = 150;

    /// Commit the pending press if no release has arrived within this many ms.
    /// Panels that never deliver a clean release would otherwise be
    /// uncalibratable; the stall timer fires this fallback.
    static constexpr uint32_t STALL_COMMIT_MS = 600;

    /// Monotonic-ms clock used for the refractory + stall logic. Defaults to
    /// lv_tick_get(); tests inject a deterministic source via the test-access class.
    std::function<uint32_t()> now_fn_ = []() { return static_cast<uint32_t>(lv_tick_get()); };

    /// One-shot LVGL timer: commits a pending press for panels that never send a
    /// clean RELEASED. Armed in on_press(), cancelled on commit/release/reset.
    lv_timer_t* stall_timer_ = nullptr;

    /// Commit the pending press (if any), dropping it as bounce when the
    /// refractory window has not elapsed since the last committed sample.
    void commit_pending(uint32_t now);

    /// Commit the pending press only if it has been outstanding >= STALL_COMMIT_MS
    /// (the stall-timer fallback, also driven directly by tests).
    void commit_pending_if_stale(uint32_t now);

    void start_stall_timer();
    void stop_stall_timer();
    static void stall_timer_cb(lv_timer_t* timer);

    friend class TouchCalibrationPanelTestAccess;

    /// Check if a sample has ADC-saturated or screen-edge phantom values
    bool is_bad_sample(const Point& sample) const;

    /// Compute median from valid samples in the buffer.
    ///
    /// `out_raw` receives the median of the digitizer readings belonging to the
    /// SAME samples that passed the screen-space filter, and `out_has_raw` says
    /// whether every one of them carried a reading. The raw values deliberately
    /// get no spread or saturation gate of their own: MAX_SAMPLE_SPREAD is a pixel
    /// budget, and on a 4096-unit digitizer feeding an 800px panel the equivalent
    /// raw spread is five times larger, so reusing the constant would reject good
    /// taps. The screen-space gate already vetted the tap; this only needs the
    /// paired reading.
    bool compute_median_point(Point& out, Point& out_raw, bool& out_has_raw);

    /// Reset sample buffer for new point capture
    void reset_samples();

    /// Calculate target position for a given step using screen dimensions
    Point compute_target_position(int step) const;

    /// Start countdown timer when entering VERIFY state
    void start_countdown_timer();

    /// Stop countdown timer
    void stop_countdown_timer();

    /// Timer callback - static member
    static void countdown_timer_cb(lv_timer_t* timer);

    // Fast-revert: detect broken matrices during verify by tracking touch events
    int verify_raw_touch_count_ = 0;
    int verify_onscreen_touch_count_ = 0;
    lv_timer_t* fast_revert_timer_ = nullptr;
    static constexpr int FAST_REVERT_CHECK_MS = 3000;

    void start_fast_revert_timer();
    void stop_fast_revert_timer();
    static void fast_revert_timer_cb(lv_timer_t* timer);

    // Calibration target positions as screen ratios
    // These form a well-distributed triangle for accurate affine transform
    // Y ratios at 20%-78% to keep crosshairs clear of header/footer chrome
    static constexpr float TARGET_0_X_RATIO = 0.15f; ///< 15% from left edge
    static constexpr float TARGET_0_Y_RATIO = 0.20f; ///< 20% from top
    static constexpr float TARGET_1_X_RATIO = 0.50f; ///< Center X
    static constexpr float TARGET_1_Y_RATIO = 0.78f; ///< 78% from top (clear of buttons)
    static constexpr float TARGET_2_X_RATIO = 0.85f; ///< 85% from left
    static constexpr float TARGET_2_Y_RATIO = 0.20f; ///< 20% from top
};

} // namespace helix
