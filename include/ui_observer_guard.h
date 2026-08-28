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
 * The bool value signals subject liveness: true = alive, false = dead.
 *
 * Before destroying a subject, owners MUST set *lifetime = false, then
 * reset(). Multiple services may hold shared_ptr copies of the same
 * lifetime — setting the bool ensures ALL ObserverGuards detect subject
 * death, even when the shared_ptr refcount hasn't reached zero. (#816)
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
        : observer_(lv_subject_add_observer(subject, cb, user_data)),
          created_epoch_(s_invalidation_epoch.load(std::memory_order_acquire)) {}

    /// Construct with a cleanup callback for the user_data context.
    /// The cleanup runs in reset() to free the context and expire weak tokens.
    ObserverGuard(lv_subject_t* subject, lv_observer_cb_t cb, void* user_data,
                  std::function<void()> cleanup)
        : observer_(lv_subject_add_observer(subject, cb, user_data)),
          created_epoch_(s_invalidation_epoch.load(std::memory_order_acquire)),
          cleanup_(std::move(cleanup)) {}

    ~ObserverGuard() {
        reset();
    }

    ObserverGuard(ObserverGuard&& other) noexcept
        : observer_(std::exchange(other.observer_, nullptr)),
          alive_token_(std::move(other.alive_token_)),
          has_alive_token_(std::exchange(other.has_alive_token_, false)),
          created_epoch_(other.created_epoch_), cleanup_(std::move(other.cleanup_)) {}

    ObserverGuard& operator=(ObserverGuard&& other) noexcept {
        if (this != &other) {
            reset();
            observer_ = std::exchange(other.observer_, nullptr);
            alive_token_ = std::move(other.alive_token_);
            has_alive_token_ = std::exchange(other.has_alive_token_, false);
            created_epoch_ = other.created_epoch_;
            cleanup_ = std::move(other.cleanup_);
        }
        return *this;
    }

    ObserverGuard(const ObserverGuard&) = delete;
    ObserverGuard& operator=(const ObserverGuard&) = delete;

    /**
     * @brief Signal that all subjects have been torn down (soft restart).
     *
     * Bumps a monotonic invalidation epoch. Any ObserverGuard created BEFORE
     * this call had its subject freed by StaticSubjectRegistry::deinit_all()
     * (LVGL already removed+freed the observer), so its reset() will skip
     * lv_observer_remove() to avoid touching freed memory. Observers created
     * AFTER this call — e.g. widgets built during init_printer_state()'s
     * finalize_setup() before revalidate_all() — are attached to live
     * subjects and are removed normally on reset().
     *
     * Call invalidate_all() AFTER StaticSubjectRegistry::deinit_all() and
     * BEFORE any re-initialization.
     *
     * The per-creation epoch is why the old global boolean was insufficient:
     * a boolean suppressed removal for window-created observers too, orphaning
     * live observers on live subjects → use-after-free when later notified
     * (debug bundles 449TVQ82 / X3RA4252, LedWidget on the static LED subject).
     */
    static void invalidate_all() {
        s_invalidation_epoch.fetch_add(1, std::memory_order_release);
    }
    /**
     * @brief Marks the end of a reinit window. No-op in the epoch model.
     *
     * Removal coherence is decided per-observer by created_epoch_ vs the
     * current invalidation epoch, so no global "valid again" flip is needed.
     * Retained for call-site compatibility (Application::init_printer_state(),
     * including its early-return error paths).
     */
    static void revalidate_all() {}

    void reset() {
        if (observer_) {
            // If we have a lifetime token and either (a) it expired (all shared_ptrs
            // gone), or (b) the pointed-to bool was set to false (source signaled
            // subject death), the subject's observers were already freed by
            // lv_subject_deinit(). Calling lv_observer_remove() would crash.
            //
            // Check (b) is critical: multiple services may hold shared_ptr copies of
            // the same SubjectLifetime. When the source resets its copy and deinits
            // the subject, other holders keep the shared_ptr alive (refcount > 0),
            // so expired() returns false. But the bool value was set to false by the
            // source before destruction, allowing us to detect the dead subject.
            // (#816, #673)
            bool subject_dead = false;
            if (has_alive_token_) {
                auto locked = alive_token_.lock();
                subject_dead = !locked || !*locked;
            }
            // An observer created before the most recent invalidate_all() had
            // its subject freed by StaticSubjectRegistry::deinit_all(); LVGL
            // already removed+freed the observer, so lv_observer_remove() would
            // touch freed memory. An observer created during/after that window
            // (e.g. a widget built mid-reinit) is on a LIVE subject and must be
            // removed normally — skipping it orphans a live observer node whose
            // context we are about to free → UAF on the next notify (#449TVQ82,
            // #X3RA4252). created_epoch_ distinguishes the two cases; the old
            // global boolean could not.
            bool freed_by_deinit =
                created_epoch_ < s_invalidation_epoch.load(std::memory_order_acquire);
            if (!subject_dead && !freed_by_deinit && lv_is_initialized()) {
                lv_observer_remove(observer_);
            }
            // If LVGL is already torn down or the subject was freed, just release
            observer_ = nullptr;
            alive_token_.reset();
            has_alive_token_ = false;
        }
        // Free the observer context (expires weak_alive tokens in deferred lambdas).
        //
        // OUTSIDE the `if (observer_)` on purpose. The factories allocate the
        // context BEFORE constructing the guard, so an attach that comes back
        // null still leaves us owning it -- and lv_subject_add_observer_obj()
        // returns null for a subject whose type is still LV_SUBJECT_TYPE_INVALID,
        // i.e. any observer installed before its subject's init_subjects() ran.
        // Skipping the cleanup there stranded the context for the process
        // lifetime and showed up as observe_int_*<Owner> origins in the ASan
        // leak ratchet (#1279).
        //
        // Safe for every other path: a normal reset() has just removed the
        // observer above, a second reset() finds cleanup_ already null, and
        // release() deliberately nulls cleanup_ to keep its documented
        // leave-the-context-alive contract.
        if (cleanup_) {
            cleanup_();
            cleanup_ = nullptr;
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
    /// Monotonic counter bumped by invalidate_all() each teardown. Observers
    /// compare their created_epoch_ against it to decide whether their subject
    /// was already freed by deinit_all() (skip removal) or is still live
    /// (remove normally). Replaces the old global s_subjects_valid boolean,
    /// which could not tell window-created observers from pre-teardown ones.
    static inline std::atomic<uint64_t> s_invalidation_epoch{0};

    lv_observer_t* observer_ = nullptr;
    std::weak_ptr<bool> alive_token_; ///< Tracks dynamic subject lifetime
    /// Distinguishes "token never set" from "token expired". Required because a
    /// default-constructed weak_ptr reports expired() == true, which would cause
    /// static-subject guards to falsely skip lv_observer_remove() and leak observers.
    bool has_alive_token_ = false;
    /// Invalidation epoch captured when this guard's observer was registered.
    /// If < s_invalidation_epoch at reset() time, the subject was deinited by a
    /// later teardown and the observer is already freed — skip removal.
    uint64_t created_epoch_ = 0;
    /// Cleanup callback to free the observer context (LambdaObserverContext).
    /// When called, destroys the shared_ptr<bool> alive token, expiring weak_ptr
    /// copies held by deferred lambdas so they skip execution on destroyed widgets.
    std::function<void()> cleanup_;
};
