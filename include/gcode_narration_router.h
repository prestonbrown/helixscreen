// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"

#include <optional>
#include <set>
#include <string>

#include "hv/json.hpp"

class IMoonrakerAPI;

// Test-only accessor (declared in the test TU), forward-declared here so the
// friend grant below can name it explicitly.
struct GcodeNarrationRouterTestAccess;

namespace helix {
class IMoonrakerClient;

/// Pure: extract the command name from Klipper's `Unknown command:"X"` response
/// (prefix already stripped), nullopt for anything else.
///
/// This one shape is unambiguous and, unlike a `!!` error, is how Klipper reports
/// a macro that aborted part-way through its own body — Moonraker still answers
/// `ok` for the script, so nothing else in the stack can tell the difference
/// between "the macro ran" and "the macro died on line 4". Deliberately narrow:
/// it is not a general terminating-response classifier.
[[nodiscard]] std::optional<std::string> parse_unknown_command(const std::string& body);

/// Consumes narration lines from notify_gcode_response and routes them to the
/// active AmsBackend's step model, updating the AmsState toolchange_step
/// subject. Sibling of GcodeErrorRouter; owns a SEPARATE subscription key.
/// Does NOT surface `!!` errors — those are GcodeErrorRouter's contract.
///
/// Both response channels are routed, to DIFFERENT matchers: `//` bodies to
/// AmsBackend::match_narration_phase(), unprefixed lines to
/// match_bare_narration_phase(). AFC emits its load/unload narration without the
/// prefix, so a `//`-only filter drops the semantically important half of the
/// step model; a shared loose matcher would instead let arbitrary console text
/// (gcode filenames!) drive the step bar. See
/// docs/devel/FILAMENT_MANAGEMENT.md § "AFC console response contract".
///
/// It also claims `Unknown command:"X"` responses, which report an aborted macro
/// that Moonraker nonetheless acknowledged with `ok`, and hands them to the
/// filament panel so a dead operation cannot finish with a checkmark.
///
/// Lifetime: owned by `Application`. Registers a callback in the ctor and
/// unregisters it in the dtor; the MoonrakerClient pointer is not owned and
/// must outlive this router.
class GcodeNarrationRouter {
  public:
    GcodeNarrationRouter(IMoonrakerAPI* api, IMoonrakerClient* client);
    ~GcodeNarrationRouter();

    GcodeNarrationRouter(const GcodeNarrationRouter&) = delete;
    GcodeNarrationRouter& operator=(const GcodeNarrationRouter&) = delete;

  private:
    /// Test-only access to the private narration glue (`process_line`). The
    /// accessor lives in the global namespace (test TU), hence the leading `::`.
    friend struct ::GcodeNarrationRouterTestAccess;

    /// Live `notify_gcode_response` handler. Wrapped by `lifetime_.bg_cb` at
    /// the subscription layer, so the WHOLE body runs on the MAIN thread.
    void on_notify_gcode_response(const nlohmann::json& msg);

    /// Routes a single response line: only `//` narration is acted on. Runs on
    /// the main thread (bg_cb defers the notify body), so the synchronous
    /// AmsState::set_narration_phase write is thread-safe.
    void process_line(const std::string& line);

    IMoonrakerAPI* api_;
    IMoonrakerClient* client_;

    /// Narration bodies that mentioned the active backend but matched no phase,
    /// deduped so the drift hint logs once per distinct line. The step bar fails
    /// silently when upstream rewords a narration string, so this is the only
    /// signal that a rename happened.
    std::set<std::string> unmatched_logged_;

    /// [L072] Generation guard for the callback captured by MoonrakerClient.
    /// Declared last so it destructs FIRST — outstanding tokens are invalidated
    /// before anything else the body might touch.
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
