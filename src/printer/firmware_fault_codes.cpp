// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_fault_codes.h"

#include "lvgl.h"
#include "printer_discovery.h"
#include "snapmaker_exceptions.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>

namespace helix::faultcodes {
namespace {

/// One firmware that reports faults as structured codes embedded in error
/// text. Everything vendor-specific lives in `classify_event`; this table
/// only carries the capability question and what to subscribe.
struct Provider {
    /// Status object whose presence marks the capability.
    const char* detect_object;
    /// Status objects carrying standing faults, for the subscription builder.
    std::vector<std::string> status_objects;
    /// Source tag for events this provider classifies.
    ErrorSource source;
    /// Read one error line (prefix already stripped) into an event, or
    /// nullopt when the line carries no code this firmware owns.
    std::optional<ErrorEvent> (*classify_event)(const Provider& p, const std::string& text);
    /// The console-equivalent coded lines for the faults currently standing,
    /// or nullopt when the frame says nothing about faults.
    std::optional<std::vector<std::string>> (*read_standing)(const nlohmann::json& status);
    /// The websocket method this firmware pushes each raise on (standing or
    /// oneshot), or null when it pushes none.
    const char* notification_method;
    /// One raise notification's params[0] into a coded line, or nullopt.
    std::optional<FaultNotification> (*read_notification)(const nlohmann::json& params0);
};

ErrorSeverity severity_of_event(const snapmaker::ExceptionSeverity& s) {
    switch (s) {
    case snapmaker::ExceptionSeverity::Informational:
        return ErrorSeverity::INFO;
    case snapmaker::ExceptionSeverity::Pause:
        return ErrorSeverity::WARNING;
    case snapmaker::ExceptionSeverity::Cancel:
        return ErrorSeverity::CRITICAL;
    }
    return ErrorSeverity::CRITICAL;
}

/// The vendor's translation of `level-id-index-code` text into an event.
std::optional<ErrorEvent> classify_snapmaker(const Provider& p, const std::string& text) {
    const auto code = snapmaker::decode_exception_code(text);
    if (!code) {
        return std::nullopt;
    }
    ErrorEvent e;
    e.source = p.source;
    e.severity = severity_of_event(snapmaker::severity_of(code->level));
    e.code = snapmaker::format_exception_code(*code);
    e.raw_detail = text;
    // A fault we have no wording for still classifies, carrying the
    // firmware's own sentence rather than a fabricated one.
    if (const auto known = snapmaker::exception_message(*code); !known.empty()) {
        // The table holds the English source; this is where the wording
        // becomes user-facing, so it translates here.
        e.detail = lv_tr(std::string(known).c_str());
    } else {
        e.detail = text;
    }
    return e;
}

/// The vendor's standing-fault lines. Each entry becomes a console-equivalent
/// `!!` line carrying the code and the firmware's own message, so the consumer
/// can feed it through the same classify path a console line takes.
std::optional<std::vector<std::string>> read_standing_snapmaker(const nlohmann::json& status) {
    if (!snapmaker::status_carries_exceptions(status)) {
        return std::nullopt;
    }
    std::vector<std::string> lines;
    for (const auto& active : snapmaker::read_active_exceptions(status)) {
        lines.push_back(snapmaker::coded_line(active));
    }
    return lines;
}

/// The vendor's raise notifications. params[0] is one exception entry in the
/// same field shape the standing list carries, so it reads through the same
/// entry reader and line formatter the status path uses.
std::optional<FaultNotification> read_notification_snapmaker(const nlohmann::json& params0) {
    const auto fault = snapmaker::read_exception_entry(params0);
    if (!fault) {
        return std::nullopt;
    }
    return FaultNotification{snapmaker::coded_line(*fault), fault->message};
}

/// The provider table. Adding a firmware with the same capability is one row
/// here and no call-site change.
const std::array<Provider, 1> kProviders{{
    {"exception_manager",
     {"exception_manager"},
     ErrorSource::SNAPMAKER,
     classify_snapmaker,
     read_standing_snapmaker,
     // Moonraker sends event "snapmaker:exception_notification" to websocket
     // clients as "notify_" + the part after the colon.
     "notify_exception_notification",
     read_notification_snapmaker},
}};

const Provider* provider_for(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    for (const auto& p : kProviders) {
        if (std::find(objects.begin(), objects.end(), p.detect_object) != objects.end()) {
            return &p;
        }
    }
    return nullptr;
}

const Provider* provider_for_method(const std::string& method) {
    for (const auto& p : kProviders) {
        if (p.notification_method && method == p.notification_method) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

bool firmware_reports_fault_codes(const PrinterDiscovery& hw) {
    return provider_for(hw) != nullptr;
}

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    const Provider* p = provider_for(hw);
    return p ? p->status_objects : std::vector<std::string>{};
}

std::optional<ErrorEvent> classify(const PrinterDiscovery& hw, const std::string& line) {
    const Provider* p = provider_for(hw);
    if (!p) {
        return std::nullopt;
    }
    const GcodeErrorLine parsed = parse_gcode_error_line(line);
    if (parsed.prefix == GcodeErrorPrefix::None) {
        return std::nullopt;
    }
    return p->classify_event(*p, parsed.text);
}

std::optional<std::vector<std::string>> read_standing_faults(const PrinterDiscovery& hw,
                                                             const nlohmann::json& status) {
    const Provider* p = provider_for(hw);
    return p ? p->read_standing(status) : std::nullopt;
}

std::vector<std::string> notification_methods() {
    std::vector<std::string> methods;
    for (const auto& p : kProviders) {
        if (p.notification_method) {
            methods.emplace_back(p.notification_method);
        }
    }
    return methods;
}

std::optional<FaultNotification> read_fault_notification(const std::string& method,
                                                         const nlohmann::json& params0) {
    const Provider* p = provider_for_method(method);
    return p ? p->read_notification(params0) : std::nullopt;
}

nlohmann::json fault_status_subset(const nlohmann::json& status) {
    nlohmann::json subset = nlohmann::json::object();
    if (!status.is_object()) {
        return subset;
    }
    for (const auto& p : kProviders) {
        for (const auto& obj : p.status_objects) {
            auto it = status.find(obj);
            if (it != status.end()) {
                subset[obj] = *it;
            }
        }
    }
    return subset;
}

} // namespace helix::faultcodes
