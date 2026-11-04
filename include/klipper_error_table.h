// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "error_event.h"
#include "json_fwd.h"

#include <string>
#include <vector>

namespace helix::printer {

/// A one-tap fix a table code offers. Strings are untranslated; the
/// classification below translates the label.
struct KlipperErrorAction {
    const char* label;
    const char* gcode; ///< "" when a service, not a gcode, performs it
    const char* log_tag;
};

/// Tag of the action whose work PrinterRecoveryService does rather than a
/// gcode (key298's MCU-bridge bounce), so the presenter can dispatch it.
inline constexpr const char* kKlipperErrorRecoveryServiceTag = "error_classify::key298_recover";

/// One decoded entry from the `!! {"code":"keyNNN","msg":...}` error channel.
struct KlipperErrorEntry {
    const char* message;
    const char* hint;
    AmsAlertLevel level;
    /// How the console-error classifier presents this code: CRITICAL blocks
    /// on a modal, WARNING is a toast.
    ErrorSeverity severity;
    /// Optional formatter that stringifies the `values` array into a
    /// human-readable locator (" in unit 1 slot B"). Caller appends the
    /// result to the friendly message. nullptr = no per-error format
    /// known yet (no regression — message displays unchanged).
    std::string (*format_values)(const nlohmann::json&) = nullptr;
    /// True when the firmware's own `msg` names the real cause and the table
    /// text cannot (one code, several causes: key843 fires for a busy box AND
    /// an unreadable tag). A caller with a firmware msg then prefers its
    /// wording as the message (#1387).
    bool prefer_fw_msg = false;
    /// The fix offered beside the error, nullptr for none.
    const KlipperErrorAction* recovery = nullptr;
};

/// Look up a `keyNNN` code emitted on Klipper's gcode-error channel.
/// Returns nullptr for an unknown code.
///
/// The table lives here rather than inside a backend because the codes arrive
/// on a channel every printer has, and the consumer is the application-layer
/// gcode-error router — not the filament system. Three of the codes (key111
/// pre-heat, key298 MCU bridge, key585 out of range) are Klipper-layer faults
/// a machine reports with no filament hardware attached at all, so a build
/// without CFS compiled in must still be able to translate them.
const KlipperErrorEntry* klipper_error_lookup(const std::string& key_code);

/// How the generic console-error classifier presents a code from this
/// channel: everything about the code that is policy rather than text.
struct KlipperErrorClassification {
    ErrorSource source;
    ErrorSeverity severity;
    /// Translated modal title, "" when the code carries none
    /// (klipper_error_modal_title).
    std::string title;
    /// Translated, ready for ErrorEvent::recovery_actions.
    std::vector<RecoveryAction> recovery_actions;
};

/// Classify a code on this channel. A code missing from the table takes its
/// family's rule, so a new box fault still blocks.
[[nodiscard]] KlipperErrorClassification klipper_error_classify(const std::string& key_code);

/// Translated modal title for a code on this channel: the box-fault family
/// names the filament system, every other code returns nullptr (the presenter
/// default). Also answers for a code recovered without a full classification
/// (the reconnect replay).
[[nodiscard]] const char* klipper_error_modal_title(const std::string& key_code);

} // namespace helix::printer
