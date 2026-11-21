// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// GcodeErrorRouter::clean_error_text lives in its own TU so the ESP32 firmware
// slice can link it for the e-stop recovery dialog without pulling in the full
// error-routing web (toasts, modals, RPC correlation) that gcode_error_router
// .cpp drags along — the same seam-driven split as moonraker_api_power.cpp.

#include "firmware_error_envelope.h"
#include "gcode_error_router.h"
#include "klipper_error_table.h"
#include "lvgl.h"

#include "hv/json.hpp"

namespace helix {

void GcodeErrorRouter::clean_error_text(std::string& text, std::string& out_code,
                                        FirmwareCodeScheme* out_scheme) {
    out_code.clear();
    if (out_scheme) {
        *out_scheme = FirmwareCodeScheme::Opaque;
    }

    // A firmware JSON envelope, whole or spliced into Klipper prose: the code
    // moves out to the caller and the message takes the object's place, so the
    // prose around it (Klipper's restart footer) survives.
    if (const auto env = find_firmware_error_envelope(text)) {
        out_code = env->code;
        if (out_scheme) {
            *out_scheme = env->scheme;
        }
        // Not behind HELIX_HAS_CFS: key111/key298/key585 are Klipper-layer
        // faults a machine reports with no filament hardware, and a build
        // without CFS still has to translate them rather than show the raw
        // firmware string.
        const printer::KlipperErrorEntry* entry = env->scheme == FirmwareCodeScheme::ErrorTable
                                                      ? printer::klipper_error_lookup(env->code)
                                                      : nullptr;
        if (entry) {
#if defined(__cpp_exceptions)
            try {
#else
            {
#endif
                // Table entries that prefer it (key843: one code, several
                // causes) decode with the firmware's real wording.
                std::string message = (entry->prefer_fw_msg && !env->msg.empty())
                                          ? env->msg
                                          : std::string(lv_tr(entry->message));
                if (entry->format_values) {
                    auto values = nlohmann::json::array();
                    if (!env->values_json.empty()) {
                        values = nlohmann::json::parse(env->values_json, nullptr, false);
                    }
                    message += entry->format_values(values);
                }
                // The curated wording replaces the whole text: spliced into
                // the firmware's prose it would mix languages, and a toast's
                // truncation could cut the hint.
                text = message + ". " + lv_tr(entry->hint);
                return;
#if defined(__cpp_exceptions)
            } catch (...) {
                // A throw from the error-table formatters (fmt::runtime over a
                // translated format string) leaves the firmware's own msg.
            }
#else
            }
#endif
        }
        // The firmware's own msg in the object's place, keeping the prose
        // around it. With no msg there is nothing to put there.
        if (!env->msg.empty()) {
            text = env->text();
        }
    }
}

} // namespace helix
