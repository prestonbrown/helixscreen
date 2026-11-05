// SPDX-License-Identifier: GPL-3.0-or-later
#include "moonraker_error.h"

#include "hv/json.hpp"

// Free of LVGL, unlike moonraker_error.cpp: the ESP32 network component calls this
// and compiles this file itself, because helixapp links before it.
MoonrakerError MoonrakerError::from_json_rpc(const json& error_obj,
                                             const std::string& method_name) {
    MoonrakerError err;
    err.type = MoonrakerErrorType::JSON_RPC_ERROR;
    err.method = method_name;

    // Type-check before .get<T>(): a wrong-typed field (a string "code", a
    // null "message") throws nlohmann::type_error, and this runs on the
    // WebSocket thread inside the request tracker's dispatch path. A throw
    // there strands the caller's callback. Treat a wrong-typed field as
    // absent instead.
    const auto code_it = error_obj.find("code");
    if (code_it != error_obj.end() && code_it->is_number()) {
        err.code = code_it->get<int>();
    }

    const auto msg_it = error_obj.find("message");
    if (msg_it != error_obj.end() && msg_it->is_string()) {
        err.message = extract_friendly_message(msg_it->get<std::string>());
    }

    // Map specific error codes to types
    if (err.code == -32601) { // Method not found
        err.type = MoonrakerErrorType::VALIDATION_ERROR;
    } else if (err.message.find("not ready") != std::string::npos) {
        err.type = MoonrakerErrorType::NOT_READY;
    } else if (err.message.find("File not found") != std::string::npos) {
        err.type = MoonrakerErrorType::FILE_NOT_FOUND;
    }

    return err;
}
