// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_fault_codes.h"

#include "lvgl.h"
#include "printer_discovery.h"
#include "qidi_fault_codes.h"
#include "snapmaker_exceptions.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>

#include "hv/json.hpp"

namespace helix::faultcodes {
namespace {

/// One firmware that reports faults as structured codes embedded in error
/// text. Everything vendor-specific lives in `classify_event`; this table
/// only carries the capability question and what to subscribe.
struct Provider {
    /// True when this firmware runs on the printer.
    bool (*detect)(const PrinterDiscovery& hw);
    /// Status objects carrying standing faults, for the subscription builder.
    std::vector<std::string> status_objects;
    /// Source tag for events this provider classifies.
    ErrorSource source;
    /// Read one error line (prefix already stripped) into an event, or
    /// nullopt when the line carries no code this firmware owns.
    std::optional<ErrorEvent> (*classify_event)(const Provider& p, const std::string& text,
                                                const ClassifyContext& ctx);
    /// The console-equivalent coded lines for the faults currently standing,
    /// or nullopt when the frame says nothing about faults.
    std::optional<std::vector<std::string>> (*read_standing)(const nlohmann::json& status);
    /// The websocket method this firmware pushes each raise on (standing or
    /// oneshot), or null when it pushes none.
    const char* notification_method;
    /// One raise notification's params[0] into a coded line, or nullopt.
    std::optional<FaultNotification> (*read_notification)(const nlohmann::json& params0);
    /// The firmware raises some faults on `//` info lines or with no prefix
    /// at all. Such a line reaches classify_event only through this flag, and
    /// classify_event must then claim nothing but lines carrying its code.
    bool reads_unprefixed_lines;
    /// The firmware's console also carries a code-stripped prose copy of each
    /// fault it pushes coded, so an uncoded line waits for its coded twin.
    bool sends_prose_twins;
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
std::optional<ErrorEvent> classify_snapmaker(const Provider& p, const std::string& text,
                                             const ClassifyContext& /*ctx*/) {
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

bool detect_snapmaker(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), "exception_manager") != objects.end();
}

/// QIDI's extras raise QD codes with or without a Box; the Box (its own
/// detection, or QIDI's box_extras object) is what marks the firmware.
bool detect_qidi(const PrinterDiscovery& hw) {
    if (hw.qidi_box_slot_count() > 0) {
        return true;
    }
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), "box_extras") != objects.end();
}

std::optional<ErrorEvent> classify_qidi(const Provider& /*p*/, const std::string& text,
                                        const ClassifyContext& ctx) {
    return qidi::classify_fault_line(text, ctx);
}

std::optional<std::vector<std::string>> read_no_standing(const nlohmann::json& /*status*/) {
    return std::nullopt;
}

/// The provider table. Adding a firmware with the same capability is one row
/// here and no call-site change.
const std::array<Provider, 2> kProviders{{
    {detect_snapmaker,
     {"exception_manager"},
     ErrorSource::SNAPMAKER,
     classify_snapmaker,
     read_standing_snapmaker,
     // Moonraker sends event "snapmaker:exception_notification" to websocket
     // clients as "notify_" + the part after the colon.
     "notify_exception_notification",
     read_notification_snapmaker,
     /*reads_unprefixed_lines=*/false,
     /*sends_prose_twins=*/true},
    // QIDI's standing fault (save_variables retry_step) belongs to the Box
    // backend, which can tell whether live state still agrees with it.
    {detect_qidi,
     {},
     ErrorSource::QIDI,
     classify_qidi,
     read_no_standing,
     nullptr,
     nullptr,
     /*reads_unprefixed_lines=*/true,
     /*sends_prose_twins=*/false},
}};

const Provider* provider_for(const PrinterDiscovery& hw) {
    for (const auto& p : kProviders) {
        if (p.detect(hw)) {
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

bool firmware_sends_prose_twins(const PrinterDiscovery& hw) {
    const Provider* p = provider_for(hw);
    return p != nullptr && p->sends_prose_twins;
}

std::optional<ErrorEvent> classify(const PrinterDiscovery& hw, const std::string& line,
                                   const ClassifyContext& ctx) {
    const Provider* p = provider_for(hw);
    if (!p) {
        return std::nullopt;
    }
    const GcodeErrorLine parsed = parse_gcode_error_line(line);
    if (parsed.prefix != GcodeErrorPrefix::None) {
        return p->classify_event(*p, parsed.text, ctx);
    }
    if (!p->reads_unprefixed_lines) {
        return std::nullopt;
    }
    std::string text = line;
    if (text.rfind("//", 0) == 0) {
        text = text.substr(text.size() > 2 && text[2] == ' ' ? 3 : 2);
    }
    return p->classify_event(*p, text, ctx);
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
