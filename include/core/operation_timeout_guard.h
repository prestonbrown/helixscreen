// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/subject.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>

class SubjectManager;

/**
 * @brief Reusable timeout guard for async operations with optional LVGL subject binding
 *
 * Manages a one-shot LVGL timer that fires if an async operation (e.g., Moonraker gcode)
 * never calls back. Optionally drives an integer subject for XML button disabling.
 *
 * Usage:
 * @code
 * // In panel init_subjects():
 * operation_guard_.init_subject("my_operation_in_progress", subjects_);
 *
 * // Before API call:
 * operation_guard_.begin(30000, [this] { NOTIFY_WARNING("Operation timed out"); });
 *
 * // In success/error callbacks (via ui_queue_update()):
 * operation_guard_.end();
 * @endcode
 */
class OperationTimeoutGuard {
  public:
    OperationTimeoutGuard() = default;
    ~OperationTimeoutGuard();

    // Non-copyable, non-movable (timer + subject ownership)
    OperationTimeoutGuard(const OperationTimeoutGuard&) = delete;
    OperationTimeoutGuard& operator=(const OperationTimeoutGuard&) = delete;
    OperationTimeoutGuard(OperationTimeoutGuard&&) = delete;
    OperationTimeoutGuard& operator=(OperationTimeoutGuard&&) = delete;

    /**
     * @brief Register an LVGL subject for XML button disabling
     *
     * Call once during panel init_subjects(). The subject is set to 1 on begin()
     * and 0 on end()/timeout. Skip if panel doesn't need XML bindings.
     *
     * @param subject_name Name for XML binding (e.g., "controls_operation_in_progress")
     * @param subjects SubjectManager for RAII cleanup
     */
    void init_subject(const char* subject_name, SubjectManager& subjects);

    /**
     * @brief Start operation with timeout
     *
     * Sets active=true, subject=1 (if registered), creates one-shot LVGL timer.
     * If already active, cancels the existing timer first.
     *
     * @param timeout_ms Timeout duration in milliseconds
     * @param on_timeout Callback to invoke if timeout fires (responsible for NOTIFY + cleanup)
     */
    void begin(uint32_t timeout_ms, std::function<void()> on_timeout);

    /**
     * @brief Operation completed — cancel timeout, reset state
     *
     * Sets active=false, subject=0 (if registered), deletes timer.
     * Safe to call multiple times (idempotent).
     */
    void end();

    /**
     * @brief Check if an operation is in progress
     */
    bool is_active() const {
        return active_;
    }

    /**
     * @brief Get the subject pointer (nullptr if init_subject() not called)
     */
    lv_subject_t* subject() {
        return has_subject_ ? &subject_ : nullptr;
    }

    /**
     * @brief The armed timeout timer, or nullptr when no operation is pending
     *
     * Test-observability only: lets a test fire the timeout immediately
     * (lv_timer_set_period + lv_timer_ready) instead of waiting out the real
     * budget, while still running the handler the production callsite installed.
     */
    [[nodiscard]] lv_timer_t* pending_timer() const noexcept {
        return timer_;
    }

  private:
    bool active_ = false;
    bool has_subject_ = false;
    helix::Subject subject_{};
    lv_timer_t* timer_ = nullptr;
    std::function<void()> on_timeout_;

    void cancel_timer();
    static void timer_callback(lv_timer_t* timer);
};
