// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>

class IMoonrakerAPI;

namespace helix::ui {

/// Run the active AMS backend's prepare_for_resume, then dispatch the
/// Resume StandardMacro. On either error path (prep failure or macro send
/// failure) emit a contextual `NOTIFY_ERROR` toast and invoke `on_failure`,
/// which call sites typically use to clear optimistic-UI state.
///
/// All work is async; this returns immediately. `on_failure` and the
/// internal toast emission fire on the **main thread** even when the
/// underlying macro error callback originates from a background WebSocket
/// thread — the helper bounces those through the UpdateQueue. Callers
/// don't need to do their own marshalling for the on_failure body, but
/// they must still ensure any state it touches is alive at the time of
/// the bounce (singletons or main-thread-owned state are the usual
/// choices).
///
/// @param api          Moonraker API to use for macro execution. Must
///                     outlive the dispatch — typically a panel member or
///                     a singleton-owned pointer. Must not be nullptr;
///                     callers handle the api == nullptr case (e.g. mock
///                     mode fallback) before calling.
/// @param log_prefix   Spdlog tag for log lines, e.g. `"[Print Status]"`.
///                     Passed by value so the helper owns its own copy
///                     and the async lambdas don't depend on the caller's
///                     storage outliving the dispatch.
/// @param on_failure   Optional. Invoked on the main thread on either
///                     error path AFTER the toast has been emitted.
///                     Default: no-op.
void dispatch_prepared_resume(IMoonrakerAPI* api, std::string log_prefix,
                              std::function<void()> on_failure = {});

/// Show the "Print Was Terminated — Restart from the beginning?" modal.
/// Exposed so post-resume backstops (e.g. AmsBackendSnapmaker) can surface it
/// after a silent RESUME no-op, not just the up-front prepare_for_resume gate.
/// on_failure may be null.
void show_restart_required_modal(IMoonrakerAPI* api, const std::string& filename,
                                 std::string log_prefix, std::function<void()> on_failure);

} // namespace helix::ui
