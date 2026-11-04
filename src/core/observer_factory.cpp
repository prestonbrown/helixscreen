// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/observer_factory.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace helix::ui::detail {

namespace {

/**
 * @brief Context owned by an observer's ObserverGuard (freed by its cleanup)
 *
 * The `alive` token lets deferred lambdas (queued via queue_update) detect
 * when the observer has been destroyed between queueing and execution.
 * Without this, the owner pointer bound into the handler becomes dangling if
 * the owner is destroyed during a widget rebuild.
 * See issue #174: SEGV in LedWidget::update_light_icon() via stale pointer.
 */
template <typename V> struct LambdaObserverContext {
    std::shared_ptr<const helix::Callback<void(V)>> fn;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

using IntContext = LambdaObserverContext<int>;
using StringContext = LambdaObserverContext<const char*>;

template <typename Context> Context* context_of(lv_observer_t* obs) {
    return static_cast<Context*>(lv_observer_get_user_data(obs));
}

// LOAD-BEARING INVARIANT: the synchronous bodies below have NO defense
// against a freed context. Reading c->fn / c->alive on freed-but-not-yet-reused
// heap copies a plausible shared_ptr (a refcount bump) on freed memory →
// SIGSEGV in __aarch64_ldadd4 (the crash in bundles 449TVQ82/X3RA4252). The
// weak alive and lifetime guards protect only the DEFERRED lambda, not these
// copies. Safety therefore depends entirely on ObserverGuard::reset()
// removing the observer from the subject BEFORE freeing the context (its
// `delete ctx` cleanup). reset() guarantees that ordering for any observer on
// a live subject; the per-creation invalidation epoch (ui_observer_guard.h)
// ensures an observer created during a printer-state reinit window is still
// removed rather than orphaned.
//
// Every queued lambda copies what it needs (the handler shared_ptr, the value)
// so it is self-contained even if the context is freed before it runs (#82),
// and checks the weak alive token so it skips once the observer is gone (#174).

void int_deferred_cb(lv_observer_t* obs, lv_subject_t* subj) {
    auto* c = context_of<IntContext>(obs);
    int value = lv_subject_get_int(subj);
    helix::ui::queue_update("observe<int>::apply",
                            [fn = c->fn, value, weak_alive = std::weak_ptr<bool>(c->alive)]() {
                                if (weak_alive.expired())
                                    return;
                                (*fn)(value);
                            });
}

void int_immediate_cb(lv_observer_t* obs, lv_subject_t* subj) {
    // Copied before the call: a handler that resets its own guard frees the context.
    auto fn = context_of<IntContext>(obs)->fn;
    (*fn)(lv_subject_get_int(subj));
}

void string_deferred_cb(lv_observer_t* obs, lv_subject_t* subj) {
    auto* c = context_of<StringContext>(obs);
    const char* str = lv_subject_get_string(subj);
    helix::ui::queue_update("observe<const char*>::apply",
                            [fn = c->fn, str_copy = std::string(str ? str : ""),
                             weak_alive = std::weak_ptr<bool>(c->alive)]() {
                                if (weak_alive.expired())
                                    return;
                                (*fn)(str_copy.c_str());
                            });
}

void string_immediate_cb(lv_observer_t* obs, lv_subject_t* subj) {
    auto fn = context_of<StringContext>(obs)->fn;
    const char* str = lv_subject_get_string(subj);
    (*fn)(str ? str : "");
}

bool attachable(lv_subject_t* subject, const void* owner) {
    if (subject && owner) {
        return true;
    }
    spdlog::debug("[observe] null {} - observer not attached", subject ? "owner" : "subject");
    return false;
}

template <typename Context>
ObserverGuard attach(lv_subject_t* subject, lv_observer_cb_t cb, Context* ctx,
                     const SubjectLifetime& lifetime) {
    ObserverGuard guard(subject, cb, ctx, [ctx]() { delete ctx; });
    if (lifetime) {
        guard.set_alive_token(lifetime);
    }
    return guard;
}

} // namespace

ObserverGuard observe_core(lv_subject_t* subject, const void* owner, helix::Callback<void(int)> fn,
                           Dispatch dispatch, const SubjectLifetime& lifetime) {
    if (!attachable(subject, owner)) {
        return ObserverGuard();
    }
    auto* ctx = new IntContext{std::make_shared<const helix::Callback<void(int)>>(std::move(fn))};
    return attach(subject, dispatch == Dispatch::Deferred ? int_deferred_cb : int_immediate_cb, ctx,
                  lifetime);
}

ObserverGuard observe_core(lv_subject_t* subject, const void* owner,
                           helix::Callback<void(const char*)> fn, Dispatch dispatch,
                           const SubjectLifetime& lifetime) {
    if (!attachable(subject, owner)) {
        return ObserverGuard();
    }
    auto* ctx = new StringContext{
        std::make_shared<const helix::Callback<void(const char*)>>(std::move(fn))};
    return attach(subject,
                  dispatch == Dispatch::Deferred ? string_deferred_cb : string_immediate_cb, ctx,
                  lifetime);
}

} // namespace helix::ui::detail
