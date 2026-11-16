// include/snapmaker_exceptions.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file snapmaker_exceptions.h
 * @brief The U1's structured fault codes, and what they mean to a user.
 *
 * The firmware raises faults with a module id, an index, a code and a level,
 * and embeds them in error text as `level-id-index-code`. Matching the English
 * sentence instead is brittle: the wording is the firmware's to change, and it
 * is not translated. The code is the stable identity.
 *
 * Levels mirror what the firmware will do about it: 1 nothing, 2 pause,
 * 3 cancel.
 *
 * The same faults also stand in a list - `exception_manager.exceptions` in the
 * status object - including persistent ones that survived a restart and were
 * never printed to the console this session. read_active_exceptions() reads
 * that list entry by entry.
 */
namespace helix::snapmaker {

struct ExceptionCode {
    int level = 0;
    int id = 0;
    int index = 0;
    int code = 0;
    bool operator==(const ExceptionCode& other) const {
        return level == other.level && id == other.id && index == other.index && code == other.code;
    }
};

enum class ExceptionSeverity { Informational, Pause, Cancel };

/// Find the FIRST four-part `level-id-index-code` anywhere in `text`.
/// nullopt when there is none - including for the firmware's three-part basic
/// form, which omits the level and would otherwise decode shifted by one field.
[[nodiscard]] std::optional<ExceptionCode> decode_exception_code(const std::string& text);

/// The four-part `LLLL-IIII-NNNN-CCCC` spelling every coded fault is reported
/// in: console lines, ErrorEvent codes, log tokens. One formatting so the
/// spellings a user compares by eye cannot drift apart.
[[nodiscard]] std::string format_exception_code(const ExceptionCode& c);

/// Our wording for a fault we recognise; empty for one we do not, so the caller
/// falls back to the firmware's own message rather than inventing one.
[[nodiscard]] std::string_view exception_message(const ExceptionCode& c);

/// What the firmware will do about a fault at this level. An unrecognised level
/// reads as Cancel: assuming the worst is the safe direction.
[[nodiscard]] ExceptionSeverity severity_of(int level);

/// One fault the firmware reports as currently standing.
struct ActiveException {
    ExceptionCode code;
    /// The firmware's own message string, verbatim and untranslated. Status
    /// frames arrive off the main thread, so wording for the screen belongs
    /// to the render path (faultcodes::classify), never here.
    std::string message;
    /// True when the fault survives a firmware restart.
    bool persistent = false;
};

/// True when this frame actually carries the exceptions array. Moonraker sends
/// delta frames, so a frame that omits `exception_manager` is silent about
/// faults, not reporting that none stand; a caller that cleared a fault banner
/// without asking this would clear it on every quiet frame.
[[nodiscard]] bool status_carries_exceptions(const nlohmann::json& status);

/// One fault, whichever channel carried it: an `exceptions` array entry from
/// the status object, or the entry a raise notification's params carry. Both
/// use the same field shape. nullopt when the entry is not an object.
[[nodiscard]] std::optional<ActiveException> read_exception_entry(const nlohmann::json& entry);

/// The console-equivalent line for one fault, `!! LLLL-IIII-XXXX-CCCC <msg>`
/// in the same format the classify path parses, so a fault read off any
/// channel feeds through the same pipe a console line takes. Pure.
[[nodiscard]] std::string coded_line(const ActiveException& fault);

/// The faults currently standing, one ActiveException per array entry. Each
/// entry's fields arrive as numbers and are read individually; a field that
/// arrives as any other type reads as unset rather than being parsed.
[[nodiscard]] std::vector<ActiveException> read_active_exceptions(const nlohmann::json& status);

} // namespace helix::snapmaker
