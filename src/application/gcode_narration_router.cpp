// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_narration_router.h"

#include "ui_panel_filament.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "i_moonraker_client.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

namespace {

constexpr const char* NOTIFY_HANDLER_NAME = "gcode_narration_router";

} // namespace

GcodeNarrationRouter::GcodeNarrationRouter(IMoonrakerAPI* api, IMoonrakerClient* client)
    : api_(api), client_(client) {
    if (!client_) {
        spdlog::warn("[GcodeNarrationRouter] Null client — handler not registered");
        return;
    }

    // [L072] register_method_callback runs on the WS thread; MoonrakerClient
    // copies the callback list under lock and invokes outside it, so
    // unregister_method_callback in our dtor does NOT block in-flight
    // invocations. lifetime_.bg_cb wraps delivery: the WS thread queues the
    // body to the main thread with a generation snapshot; on main-thread
    // dispatch the gen is re-checked, so a callback that fires after the dtor
    // invalidates lifetime_ is silently dropped. Because the body is deferred
    // to main, on_notify_gcode_response (and process_line) run on the MAIN
    // thread — the direct AmsState::set_narration_phase write is therefore
    // thread-safe with no second defer.
    client_->register_method_callback(
        "notify_gcode_response", NOTIFY_HANDLER_NAME,
        lifetime_.bg_cb("GcodeNarrationRouter::on_notify",
                        [this](const nlohmann::json& msg) { on_notify_gcode_response(msg); }));
}

GcodeNarrationRouter::~GcodeNarrationRouter() {
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", NOTIFY_HANDLER_NAME);
    }
}

std::optional<std::string> parse_unknown_command(const std::string& body) {
    // Klipper reports a macro that hit an undefined command as
    // `// Unknown command:"STATUS_PURGING"` — respond_info, NOT `!!` — and
    // Moonraker still answers `ok` for the enclosing script. Anchored at the
    // start of the body so an ordinary narration line that happens to mention
    // the phrase cannot claim to be one.
    static constexpr std::string_view PREFIX = "unknown command";
    if (body.size() <= PREFIX.size())
        return std::nullopt;
    for (size_t i = 0; i < PREFIX.size(); ++i) {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(body[i]))) != PREFIX[i])
            return std::nullopt;
    }

    // Only a colon and whitespace may separate the phrase from the quoted name.
    size_t i = PREFIX.size();
    while (i < body.size() && (body[i] == ':' || body[i] == ' ' || body[i] == '\t'))
        ++i;
    if (i >= body.size() || body[i] != '"')
        return std::nullopt;

    size_t close = body.find('"', i + 1);
    if (close == std::string::npos || close == i + 1)
        return std::nullopt;
    return body.substr(i + 1, close - i - 1);
}

void GcodeNarrationRouter::process_line(const std::string& line) {
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos)
        return;

    // `!!` is GcodeErrorRouter's contract and is never narration.
    if (line.compare(start, 2, "!!") == 0)
        return;

    // Two channels, two matchers. A `//` body is a macro's own respond_info, so
    // the loose needles in match_narration_phase() are appropriate there. An
    // unprefixed line is the open console — M105 reports, `echo:` chatter and the
    // user's gcode filename — and goes to the shape-anchored bare matcher, which
    // is where AFC happens to put its load/unload narration.
    const bool prefixed = line.compare(start, 2, "//") == 0;
    std::string body;
    if (prefixed) {
        size_t body_start = line.find_first_not_of(" \t", start + 2);
        if (body_start == std::string::npos)
            return;
        body = line.substr(body_start);
    } else {
        body = line.substr(start);
    }

    // An aborted macro reports its missing command here rather than through
    // `!!`, so this router is the only place that sees it. Claim the line before
    // any matcher does — the command name carries whatever verb the macro was
    // named after, and `Unknown command:"STATUS_PURGING"` otherwise reads as a
    // real purge phase and drives the step bar forwards on an error.
    if (auto missing = parse_unknown_command(body)) {
        filament_panel_report_unknown_command(*missing);
        return;
    }

    // Runs on the main thread (the ctor's lifetime_.bg_cb wrapper defers the
    // notify body to main), so these synchronous AmsState accesses are safe.
    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    auto id =
        prefixed ? backend->match_narration_phase(body) : backend->match_bare_narration_phase(body);
    if (!id) {
        // Drift hint: a line the backend recognizes as its own but that matches
        // no phase is the fingerprint of an upstream rewording. The backend
        // decides what "its own" means — asking it is what makes the hint reach
        // the unprefixed lines, none of which contain the string "afc". Debug
        // level, and deduped so the interesting lines are greppable.
        if (backend->is_narration_drift_candidate(body) && unmatched_logged_.insert(body).second) {
            spdlog::debug("[GcodeNarration] no phase matched for '{}' — either the narration "
                          "wording changed upstream or this line has no phase in the template; "
                          "check match_narration_phase()/match_bare_narration_phase()",
                          body);
        }
        return;
    }

    const auto op = AmsState::instance().get_active_step_operation();
    const auto tmpl = backend->toolchange_phase_template(op);
    for (size_t k = 0; k < tmpl.size(); ++k) {
        if (tmpl[k].id == *id) {
            spdlog::debug("[GcodeNarration] phase '{}' -> step {} ({})", *id, k, tmpl[k].label);
            AmsState::instance().set_narration_phase(static_cast<int>(k), tmpl[k].label);
            return;
        }
    }
    // Matched a phase id the active operation's template doesn't contain — no
    // index to advance to; leave the step subject untouched.
}

void GcodeNarrationRouter::on_notify_gcode_response(const nlohmann::json& msg) {
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    const auto& params = msg["params"];
    if (params[0].is_array()) {
        for (const auto& line : params[0]) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    } else if (params[0].is_string()) {
        for (const auto& line : params) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    }
}

} // namespace helix
