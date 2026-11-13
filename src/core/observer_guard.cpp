// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/observer_guard.h"

// Out of line: every class holding a guard would otherwise inline reset() into its own
// destructor and move operations, one copy per translation unit.

ObserverGuard::~ObserverGuard() {
    reset();
}

ObserverGuard::ObserverGuard(ObserverGuard&& other) noexcept
    : observer_(std::exchange(other.observer_, nullptr)),
      alive_token_(std::move(other.alive_token_)),
      has_alive_token_(std::exchange(other.has_alive_token_, false)),
      created_epoch_(other.created_epoch_), subject_(std::exchange(other.subject_, nullptr)),
      // std::exchange, not std::move: a moved-from std::function is left in
      // a valid but UNSPECIFIED state, and libc++ keeps the target when the
      // callable fits its small-object buffer - which `[ctx]{ delete ctx; }`
      // does. A source left holding the cleanup deletes the observer's
      // context from its own destructor while this guard keeps the observer
      // attached, so the next notify reads freed memory.
      cleanup_(std::exchange(other.cleanup_, nullptr)) {}

ObserverGuard& ObserverGuard::operator=(ObserverGuard&& other) noexcept {
    if (this != &other) {
        reset();
        observer_ = std::exchange(other.observer_, nullptr);
        alive_token_ = std::move(other.alive_token_);
        has_alive_token_ = std::exchange(other.has_alive_token_, false);
        created_epoch_ = other.created_epoch_;
        subject_ = std::exchange(other.subject_, nullptr);
        cleanup_ = std::exchange(other.cleanup_, nullptr);
    }
    return *this;
}

void ObserverGuard::reset() {
    if (observer_) {
        // If we have a lifetime token and either (a) it expired (all shared_ptrs
        // gone), or (b) the pointed-to bool was set to false (source signaled
        // subject death), the subject's observers were already freed by
        // lv_subject_deinit(). Calling lv_observer_delete() would crash.
        //
        // Check (b) is critical: multiple services may hold shared_ptr copies of
        // the same SubjectLifetime. When the source resets its copy and deinits
        // the subject, other holders keep the shared_ptr alive (refcount > 0),
        // so expired() returns false. But the bool value was set to false by the
        // source before destruction, allowing us to detect the dead subject.
        // (#816, #673)
        bool subject_dead = false;
        // A token from the subject's owner is authoritative: the owner flips it before
        // freeing the subject, so the epoch below has nothing to add. The shared
        // never-freed claim names no owner and falls through to the epoch.
        bool owner_token = false;
        if (has_alive_token_) {
            auto locked = alive_token_.lock();
            subject_dead = !locked || !*locked;
            owner_token = locked && locked != subject_never_freed();
        }
        // An observer created before the most recent invalidate_all() MAY
        // have had its subject freed by StaticSubjectRegistry::deinit_all()
        // — then LVGL already removed+freed the observer and
        // lv_observer_delete() would touch freed memory. The epoch is
        // process-global though: it says a registry teardown happened, not
        // that THIS subject was one of the freed ones. A subject declared
        // teardown-exempt by its owner (the reseeded theme globals) keeps
        // its observers allocated across the bump, and skipping removal
        // there orphans a live observer node whose context we are about to
        // free → UAF on the next notify (#449TVQ82, #X3RA4252). An
        // observer created during/after the window (e.g. a widget built
        // mid-reinit) is on a LIVE subject and is removed normally —
        // created_epoch_ distinguishes the two cases.
        bool freed_by_deinit =
            (!owner_token || s_process_exit.load(std::memory_order_acquire)) &&
            created_epoch_ < s_invalidation_epoch.load(std::memory_order_acquire) &&
            !subject_is_teardown_exempt(subject_);
        if (!subject_dead && !freed_by_deinit && lv_is_initialized()) {
            lv_observer_delete(observer_);
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
    // Skipping the cleanup there would leak the context for the process
    // lifetime (#1279).
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
