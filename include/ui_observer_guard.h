// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_observer_guard.h
 * @brief RAII wrapper for LVGL observer cleanup with subject lifetime tracking
 *
 * @pattern Guard that removes observer on destruction; release() for pre-destroyed subjects.
 *          For dynamic subjects (per-fan, per-sensor, per-extruder), use SubjectLifetime
 *          tokens to prevent use-after-free when subjects are deinited before observers.
 * @threading Main thread only (invalidate_all/revalidate_all use atomic for safety)
 * @gotchas Checks lv_is_initialized() - safe during LVGL shutdown.
 *          Dynamic subjects MUST provide a SubjectLifetime token — see printer_fan_state.h,
 *          temperature_sensor_manager.h, printer_temperature_state.h.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

/**
 * @brief Shared token that tracks whether a dynamic subject is still alive.
 *
 * Dynamic subject owners (PrinterFanState, TemperatureSensorManager,
 * PrinterTemperatureState) create a SubjectLifetime per dynamic subject.
 * When the subject is deinited, the shared_ptr is destroyed, expiring
 * all weak_ptr copies held by ObserverGuards. This prevents the guard
 * from calling lv_observer_remove() on an already-freed observer.
 *
 * Static subjects (singleton lifetime) don't need this — pass empty token.
 */
using SubjectLifetime = std::shared_ptr<bool>;

/**
 * @brief RAII wrapper for LVGL observers - auto-removes on destruction
 *
 * For observers on dynamic subjects, set an alive token via set_alive_token()
 * or the factory functions. When the token expires (subject deinited), reset()
 * skips lv_observer_remove() because the observer was already freed by
 * lv_subject_deinit().
 */
class ObserverGuard {
  public:
    ObserverGuard() = default;

    ObserverGuard(lv_subject_t* subject, lv_observer_cb_t cb, void* user_data)
        : observer_(lv_subject_add_observer(subject, cb, user_data)) {}

    /// Construct with a cleanup callback for the user_data context.
    /// The cleanup runs in reset() to free the context and expire weak tokens.
    ObserverGuard(lv_subject_t* subject, lv_observer_cb_t cb, void* user_data,
                  std::function<void()> cleanup)
        : observer_(lv_subject_add_observer(subject, cb, user_data)), cleanup_(std::move(cleanup)) {
    }

    ~ObserverGuard() {
        reset();
    }

    ObserverGuard(ObserverGuard&& other) noexcept
        : observer_(std::exchange(other.observer_, nullptr)),
          alive_token_(std::move(other.alive_token_)),
          has_alive_token_(std::exchange(other.has_alive_token_, false)),
          cleanup_(std::move(other.cleanup_)) {}

    ObserverGuard& operator=(ObserverGuard&& other) noexcept {
        if (this != &other) {
            reset();
            observer_ = std::exchange(other.observer_, nullptr);
            alive_token_ = std::move(other.alive_token_);
            has_alive_token_ = std::exchange(other.has_alive_token_, false);
            cleanup_ = std::move(other.cleanup_);
        }
        return *this;
    }

    ObserverGuard(const ObserverGuard&) = delete;
    ObserverGuard& operator=(const ObserverGuard&) = delete;

    /**
     * @brief Signal that all subjects have been torn down (soft restart).
     *
     * After this call, all ObserverGuard::reset() calls will release instead
     * of calling lv_observer_remove(), because the observers have already been
     * freed by lv_subject_deinit(). Call invalidate_all() AFTER
     * StaticSubjectRegistry::deinit_all() and BEFORE any re-initialization.
     * Call revalidate_all() at the END of init_printer_state() when all new
     * observers are attached to live subjects.
     */
    static void invalidate_all() {
        s_subjects_valid.store(false, std::memory_order_release);
    }
    static void revalidate_all() {
        s_subjects_valid.store(true, std::memory_order_release);
    }

    void reset() {
        if (observer_) {
            // If we have a lifetime token and it expired, the subject (and our
            // observer) was already destroyed by lv_subject_deinit(). Calling
            // lv_observer_remove() here would be a use-after-free crash.
            bool subject_dead = has_alive_token_ && alive_token_.expired();
            if (!subject_dead && s_subjects_valid.load(std::memory_order_acquire) &&
                lv_is_initialized()) {
                lv_observer_remove(observer_);
            }
            // If LVGL is already torn down or subjects invalidated, just release
            observer_ = nullptr;
            alive_token_.reset();
            has_alive_token_ = false;
            // Free the observer context (expires weak_alive tokens in deferred lambdas)
            if (cleanup_) {
                cleanup_();
                cleanup_ = nullptr;
            }
        }
    }

    /**
     * @brief Attach a subject lifetime token for dynamic subject safety.
     *
     * Call this after construction when observing a dynamic subject.
     * The observer factory functions handle this automatically.
     */
    void set_alive_token(const SubjectLifetime& token) {
        alive_token_ = token;
        has_alive_token_ = true;
    }

    /**
     * @brief Release ownership without calling lv_observer_remove()
     *
     * Use during shutdown when subjects may already be destroyed.
     * The observer will not be removed from the subject (it may already be gone).
     */
    void release() {
        observer_ = nullptr;
        alive_token_.reset();
        has_alive_token_ = false;
        // NOTE: Do NOT call cleanup_ here. release() leaves the observer
        // registered in LVGL, so the ctx (user_data) must remain valid.
        // The ctx becomes a permanent leak, but that's acceptable for the
        // shutdown/pre-deinit paths where release() is used.
        cleanup_ = nullptr;
    }

    explicit operator bool() const {
        return observer_ != nullptr;
    }
    lv_observer_t* get() const {
        return observer_;
    }

  private:
    static inline std::atomic<bool> s_subjects_valid{true};

    lv_observer_t* observer_ = nullptr;
    std::weak_ptr<bool> alive_token_; ///< Tracks dynamic subject lifetime
    /// Distinguishes "token never set" from "token expired". Required because a
    /// default-constructed weak_ptr reports expired() == true, which would cause
    /// static-subject guards to falsely skip lv_observer_remove() and leak observers.
    bool has_alive_token_ = false;
    /// Cleanup callback to free the observer context (LambdaObserverContext).
    /// When called, destroys the shared_ptr<bool> alive token, expiring weak_ptr
    /// copies held by deferred lambdas so they skip execution on destroyed widgets.
    std::function<void()> cleanup_;
};
