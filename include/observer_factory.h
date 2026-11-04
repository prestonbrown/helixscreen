// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file observer_factory.h
 * @brief Factory functions for creating type-safe LVGL observers with RAII cleanup
 *
 * Provides template-based observer creation that eliminates boilerplate callback code.
 * All observers return ObserverGuard for automatic cleanup.
 *
 * One observe<V>() takes a lambda handler, deferred through the UpdateQueue by default or
 * Dispatch::Immediate inside the subject notification. It forwards to one type-erased
 * detail::observe_core() per value type. Domain helpers wrap it for print state;
 * observe_language_change() lives in observe_language.h.
 */

#pragma once

#include "ui_observer_guard.h"

#include "connection_state.h"
#include "core/update_queue.h"
#include "helix_callback.h"
#include "lvgl/lvgl.h"
#include "print_lifecycle_state.h" // PrintState; declares PrintJobState

#include <cstdint>
#include <type_traits>

namespace helix::ui {

/// How an observer's handler runs relative to the subject notification.
enum class Dispatch : uint8_t {
    Deferred,  ///< From the UpdateQueue after the notification
    Immediate, ///< Inside lv_subject notify
};

namespace detail {

// The observer machinery is compiled once per value type in src/ui/observer_factory.cpp.
// Each call site only binds its owner into a helix::Callback; see observe_core() there for
// the context layout and the teardown invariant it depends on.

ObserverGuard observe_core(lv_subject_t* subject, const void* owner, helix::Callback<void(int)> fn,
                           Dispatch dispatch, const SubjectLifetime& lifetime);
ObserverGuard observe_core(lv_subject_t* subject, const void* owner,
                           helix::Callback<void(const char*)> fn, Dispatch dispatch,
                           const SubjectLifetime& lifetime);

/// One handler instance serves every notification, so a `mutable` handler keeps its state.
template <typename V, typename Panel, typename Handler>
helix::Callback<void(V)> bind_owner(Panel* panel, Handler&& handler) {
    return [panel, h = std::forward<Handler>(handler)](V value) mutable { h(panel, value); };
}

} // namespace detail

/**
 * @brief Observe an int or string subject with a lambda handler
 *
 * @code
 * guard_ = observe<int>(subject, this, [](Panel* self, int v) { ... }, lifetime);
 * guard_ = observe<const char*>(subject, this, [](Panel* self, const char* s) { ... },
 *                               lifetime, Dispatch::Immediate);
 * @endcode
 *
 * The default Dispatch::Deferred queues the handler through helix::ui::queue_update() to run
 * after the current subject notification completes, which prevents re-entrant observer
 * destruction crashes (#82); string values are copied first so they stay valid. Use
 * Dispatch::Immediate ONLY when the handler cannot modify observer lifecycle (no observer
 * reassignment, no widget destruction, no ObserverGuard mutation), or when it must see the
 * value before the notification returns.
 *
 * One handler instance serves every notification, so a `mutable` handler keeps its state.
 * A handler needing "value now, UI work later" is Immediate plus an explicit
 * lifetime_.defer() for the later half.
 *
 * @tparam V int or const char*; the subject's value type
 * @tparam Owner Deduced from @p owner; the handler receives it as its first argument
 * @param subject LVGL subject to observe
 * @param owner Object the handler acts on
 * @param handler Callable void(Owner*, V)
 * @param lifetime Death signal for @p subject. Required: pass the subject
 *        owner's get_subjects_lifetime(), or subject_never_freed() only when
 *        the subject genuinely cannot be freed before process exit.
 * @return ObserverGuard for RAII cleanup; empty (with a debug log) when @p subject
 *         or @p owner is null
 */
template <typename V, typename Owner, typename Handler>
ObserverGuard observe(lv_subject_t* subject, Owner* owner, Handler&& handler,
                      const SubjectLifetime& lifetime, Dispatch dispatch = Dispatch::Deferred) {
    static_assert(std::is_same_v<V, int> || std::is_same_v<V, const char*>,
                  "observe<V>: V is int or const char*");
    return detail::observe_core(subject, owner,
                                detail::bind_owner<V>(owner, std::forward<Handler>(handler)),
                                dispatch, lifetime);
}

// ============================================================================
// Domain-Specific Observer Helpers
// ============================================================================

/**
 * @brief Create print state observer with typed PrintJobState
 *
 * Common pattern used in 4+ files to react to print state changes.
 * Automatically casts the int subject value to PrintJobState enum.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable: void(Panel*, PrintJobState)
 * @param subject The RAW wire subject, `print_state_enum` — nothing else. This
 *        factory hard-casts to PrintJobState, and PrintState does not share its
 *        numbering past index 0, so handing it `print_lifecycle` compiles, runs,
 *        and answers a different question. Use observe_print_lifecycle() for
 *        that subject.
 * @param panel Panel instance
 * @param handler Lambda called with panel and typed PrintJobState
 * @param lifetime Death signal for @p subject. Required whenever the observing
 *        object can outlive the subject's owner — see observe<int>().
 *        print_state_enum belongs to PrinterState, so every caller that is not
 *        itself owned by PrinterState wants one.
 * @param dispatch Typing and dispatch are orthogonal. Pass Dispatch::Immediate when the caller
 *        reads the resulting state in the same turn (AbortManager's cancel detection does);
 *        otherwise leave the deferred default.
 * @return ObserverGuard for RAII cleanup
 */
template <typename Panel, typename Handler>
ObserverGuard observe_print_state(lv_subject_t* subject, Panel* panel, Handler&& handler,
                                  const SubjectLifetime& lifetime,
                                  Dispatch dispatch = Dispatch::Deferred) {
    return observe<int>(
        subject, panel,
        [handler = std::forward<Handler>(handler)](Panel* p, int state_int) {
            handler(p, static_cast<PrintJobState>(state_int));
        },
        lifetime, dispatch);
}

/**
 * @brief Create a print lifecycle observer with typed PrintState
 *
 * The derived-lifecycle sibling of observe_print_state(). Consumers asking a
 * capability question — "does a job own the machine right now?" — want this one,
 * because it is the only axis that can express a start the app has committed to
 * but the printer has not reported yet.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable: void(Panel*, PrintState)
 * @param subject The derived lifecycle subject, `print_lifecycle` — nothing else.
 *        See the warning on observe_print_state(): the two enums do not share
 *        numbering, and passing the wrong subject is silent.
 * @param panel Panel instance
 * @param handler Lambda called with panel and typed PrintState
 * @param lifetime Death signal for @p subject. Same rule as observe<int>():
 *        print_lifecycle belongs to PrinterState, so every caller not owned by
 *        PrinterState wants one.
 * @return ObserverGuard for RAII cleanup
 */
template <typename Panel, typename Handler>
ObserverGuard observe_print_lifecycle(lv_subject_t* subject, Panel* panel, Handler&& handler,
                                      const SubjectLifetime& lifetime) {
    return observe<int>(
        subject, panel,
        [handler = std::forward<Handler>(handler)](Panel* p, int state_int) {
            handler(p, static_cast<PrintState>(state_int));
        },
        lifetime);
}

} // namespace helix::ui
