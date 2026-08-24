// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// GcodeErrorRouter::clean_error_text lives in its own TU so the ESP32 firmware
// slice can link it for the e-stop recovery dialog without pulling in the full
// error-routing web (toasts, modals, RPC correlation) that gcode_error_router
// .cpp drags along — the same seam-driven split as moonraker_api_power.cpp.

#include "gcode_error_router.h"

#if HELIX_HAS_CFS
#include "ams_backend_cfs.h"
#endif

#include "lvgl.h"

#include "hv/json.hpp"

namespace helix {

void GcodeErrorRouter::clean_error_text(std::string& text, std::string& out_code) {
    out_code.clear();

    // K2's Klipper builds emit errors in two shapes:
    //   1. Pure JSON:     `{"code":"key849","msg":"...","values":[...]}`
    //   2. Embedded JSON: `Internal error during connect: !{"code":"key298",...}`
    //      (observed K2 Plus 2026-05-24 when klipper_mcu shutdown)
    // Scan for the first `{"code":` anywhere in the line. If found, parse
    // from there; otherwise fall through to the heuristic rewrites.
    auto json_start = text.find("{\"code\"");
    if (json_start != std::string::npos) {
        // Brace-balance forward from json_start to find the matching close
        // brace, ignoring `{`/`}` inside string literals. nlohmann::parse
        // requires whole-input -- it won't ignore trailing garbage -- so we
        // extract just [json_start, obj_end) before parsing.
        size_t i = json_start;
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        size_t obj_end = std::string::npos;
        for (; i < text.size(); ++i) {
            char c = text[i];
            if (in_string) {
                if (escape) {
                    escape = false;
                } else if (c == '\\') {
                    escape = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (--depth == 0) {
                    obj_end = i + 1;
                    break;
                }
            }
        }

        if (obj_end != std::string::npos) {
            std::string json_str = text.substr(json_start, obj_end - json_start);
            try {
                auto j = nlohmann::json::parse(json_str);
                if (j.contains("code") && j["code"].is_string()) {
                    out_code = j["code"].get<std::string>();
                    nlohmann::json values = nlohmann::json::array();
                    if (j.contains("values")) {
                        values = j["values"];
                    }
#if HELIX_HAS_CFS
                    if (auto friendly = printer::CfsErrorDecoder::lookup_message_with_values(
                            out_code, values)) {
                        text = friendly->first + ". " + friendly->second;
                        return;
                    }
#else
                    (void)values;
#endif
                }
                if (j.contains("msg") && j["msg"].is_string()) {
                    text = j["msg"].get<std::string>();
                }
            } catch (...) {
                // Malformed JSON despite the {"code" prefix -- leave text
                // untouched and fall through to heuristic patterns.
            }
        }
    }

    // Heuristic friendlier-text rewrites for common non-coded patterns.
    if (text.find("Must home axis") != std::string::npos ||
        text.find("must home") != std::string::npos) {
        text = lv_tr("Must home axes first");
        return;
    }
    if (text.find("spi_transfer_response") != std::string::npos) {
        text = lv_tr("Accelerometer communication failed. Try again.");
        return;
    }
}

} // namespace helix
