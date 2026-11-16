// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gcode_narration_router.h"
#include "json_fwd.h"

#include <string>

// Friend of GcodeNarrationRouter: drives a raw narration line straight through
// process_line() without standing up a MoonrakerClient + WebSocket.
struct GcodeNarrationRouterTestAccess {
    static void feed(helix::GcodeNarrationRouter& r, const std::string& line) {
        r.process_line(line);
    }
    static void notify(helix::GcodeNarrationRouter& r, const nlohmann::json& msg) {
        r.on_notify_gcode_response(msg);
    }
};
