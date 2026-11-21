// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {
class PrinterDiscovery;
} // namespace helix

/**
 * @file error_wording.h
 * @brief What Klipper's own error messages mean to a user.
 *
 * Klipper and Kalico raise errors as English prose: `MCU 'mcu' shutdown:
 * Timer too close`, `Move out of range: 251.000 100.000 0.300 [12.400]`. Each
 * row here keys one message on the anchored, case-sensitive prefix of its
 * reason and turns it into a summary, a cause and a fix, with the names and
 * numbers the message carries filled in. A firmware that adds its own prose
 * errors contributes a row set gated by a detection predicate.
 *
 * One classification, many presenters: the console-error classifier, the
 * Klipper recovery dialog, the print stop reason and the event journal all
 * ask classify() and render its answer; none of them matches text itself.
 * A message no row knows keeps the firmware's own words.
 *
 * classify() is pure and untranslated, so any thread may call it; word()
 * translates and is main-thread only.
 */
namespace helix::error_wording {

/// One row of a wording table; opaque outside error_wording.cpp.
struct Row;

/// What gets the printer going again once the cause is dealt with.
enum class Recovery {
    None,            ///< nothing to run: fix the cause, then carry on
    FirmwareRestart, ///< a shutdown: FIRMWARE_RESTART
    RestartAfterFix, ///< a config error: fix the file, then RESTART
    Retry,           ///< the command can simply be sent again once fixed
    Home,            ///< home the axes, then retry
};

/// What part of the printer the message is about.
enum class Category {
    Heater,
    Mcu,
    McuConnect, ///< a board Klipper could not reach at startup
    Driver,
    Homing,
    Probe,
    Move,
    Accelerometer,
    Config,
    Command,
    EmergencyStop,
    Filament,
};

/// How the console presents the message. FollowPrint is the generic rule: a
/// rejected command blocks while a print runs or is paused, and is a toast
/// otherwise.
enum class Severity { FollowPrint, Warning };

/// One recognized message.
struct Classification {
    /// Stable row id, the error map's (`MCU-1`, `HT-2`): for logs and tests.
    std::string id;
    Category category = Category::Mcu;
    Severity severity = Severity::FollowPrint;
    Recovery recovery = Recovery::None;
    /// Capture 0 is the board named by a `MCU '<name>' shutdown: ` prefix
    /// ("" when there is none); the row's own captures follow from 1.
    std::vector<std::string> captures;
    /// The reason line, prefixes and Klipper's canned trailers removed.
    std::string reason;
    /// True when the row carries wording. A row without any only classifies
    /// (severity, category), and the firmware's text is what the user reads.
    bool worded = false;
    /// The row that matched, for word().
    const Row* row = nullptr;
};

/// Classify one error text: a `!!` line with or without its prefix, a
/// `webhooks.state_message`, a `print_stats.message`. Rows a firmware adds
/// apply only when @p hw detects that firmware; nullptr asks the generic rows
/// only. nullopt for a message no row knows.
[[nodiscard]] std::optional<Classification> classify(std::string_view text,
                                                     const PrinterDiscovery* hw = nullptr);

/// A classification in the current language, captures filled in. Main thread
/// only.
struct Worded {
    std::string summary; ///< a few words, for a title or a stop reason
    std::string cause;   ///< what went wrong, one or two sentences
    std::string fix;     ///< what the user does about it
    /// The cause, a blank line, then the fix.
    [[nodiscard]] std::string detail() const;
};

/// nullopt when @p c is a row without wording.
[[nodiscard]] std::optional<Worded> word(const Classification& c);

/// @p text as a stop reason: the summary of the row that words it, else
/// @p text unchanged. Main thread only.
[[nodiscard]] std::string reason_summary(const std::string& text,
                                         const PrinterDiscovery* hw = nullptr);

/// The journal's class for a category: "heater", "mcu", "homing", "move",
/// "command", ... Pure.
[[nodiscard]] const char* journal_class(Category category);

} // namespace helix::error_wording
