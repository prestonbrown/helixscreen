// SPDX-License-Identifier: GPL-3.0-or-later

#include "pause_descriptor.h"

#include "firmware_error_envelope.h"
#include "json_utils.h"
#include "printer_discovery.h"

#include <array>

namespace helix::pause_descriptor {
namespace {

using json = nlohmann::json;

/// The print_stats key every provider below publishes its descriptor under.
constexpr const char* kCauseField = "exception";

int int_member(const json& obj, const char* key) {
    const json* v = json_util::find_member(obj, key);
    return v != nullptr && v->is_number_integer() ? v->get<int>() : -1;
}

/// The U1's filament-runout fault; its index is the head whose eN_filament
/// sensor ran out.
constexpr int kU1RunoutExceptionId = 523;

/// One firmware that publishes a structured pause cause.
struct Provider {
    bool (*present)(const PrinterDiscovery& hw);
    /// The lane a populated descriptor names as run out, or -1.
    int (*runout_lane)(const json& cause);
};

constexpr std::array<Provider, 1> kProviders = {{
    // Snapmaker U1: print_stats.exception {id, index, code, message, level}.
    // On these pauses print_stats.message is empty and the reason text lives
    // in the descriptor (#991). filament_detect marks the firmware.
    {[](const PrinterDiscovery& hw) { return hw.has_snapmaker(); },
     [](const json& cause) {
         return int_member(cause, "id") == kU1RunoutExceptionId ? int_member(cause, "index") : -1;
     }},
}};

} // namespace

json required_status_fields(const PrinterDiscovery& hw) {
    for (const Provider& p : kProviders) {
        if (p.present(hw)) {
            return json{{"print_stats", json::array({kCauseField})}};
        }
    }
    return json::object();
}

bool frame_carries_cause(const json& print_stats) {
    const json* exc = json_util::find_member(print_stats, kCauseField);
    return exc != nullptr && exc->is_object() && !exc->empty();
}

std::optional<PauseDescriptor> read(const json& print_stats) {
    const json* exc = json_util::find_member(print_stats, kCauseField);
    if (exc == nullptr || !exc->is_object()) {
        return std::nullopt;
    }
    PauseDescriptor cause;
    if (exc->empty()) {
        return cause;
    }
    cause.id = int_member(*exc, "id");
    cause.code = int_member(*exc, "code");
    const json* msg = json_util::find_member(*exc, "message");
    if (msg != nullptr && msg->is_string()) {
        cause.message = decode_firmware_error_text(msg->get_ref<const std::string&>());
    }
    for (const Provider& p : kProviders) {
        if (const int lane = p.runout_lane(*exc); lane >= 0) {
            cause.runout_lane = lane;
            break;
        }
    }
    return cause;
}

} // namespace helix::pause_descriptor
