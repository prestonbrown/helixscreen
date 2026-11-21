// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "error_event.h"

#include <optional>
#include <string>
#include <string_view>

/**
 * @file qidi_fault_codes.h
 * @brief QIDI's `QD<k>_<module>_<index>` fault codes, and what they mean to a user.
 *
 * QIDI's Klipper extras raise faults as `Code:QDE_004_006; Message:<English>`
 * (spacing and labels vary between releases, and one form is the bare code).
 * The code is the identity; the sentence is QIDI's to change. Modules seen:
 * 002 fans, 004 the QIDI Box and its filament path, 005 heater checks.
 *
 * Most Box codes arrive on `//` info lines, which no ordinary comment
 * classifier reads; the provider in firmware_fault_codes.cpp accepts a
 * comment line only when it carries one of these codes.
 *
 * docs/devel/printers/QIDI_SUPPORT.md ("Fault codes") lists each code, where
 * QIDI raises it and what the printer does.
 */
namespace helix::qidi {

struct FaultCode {
    char kind = 'E'; ///< the letter after QD: E (Klipper extras), C (closed components)
    int module = 0;
    int index = 0;
    bool operator==(const FaultCode& o) const {
        return kind == o.kind && module == o.module && index == o.index;
    }
};

/// The first `QD<k>_<mmm>_<iii>` anywhere in @p text, or nullopt.
[[nodiscard]] std::optional<FaultCode> decode_fault_code(std::string_view text);

/// `QDE_004_006`: the spelling a user searches for.
[[nodiscard]] std::string format_fault_code(const FaultCode& c);

/// The fault a `retry_step` save variable names (`QDE_004_006_3`,
/// `QDE_004_016_00`), with its slot number in @p slot (0 when the code names
/// none). nullopt for an empty or `None` value.
[[nodiscard]] std::optional<FaultCode> decode_retry_step(std::string_view retry_step, int* slot);

/// True when QIDI's own TRY_MOVE_AGAIN re-runs the step that raised @p c: the
/// Box's load and unload faults, which record it in `retry_step`.
[[nodiscard]] bool try_again_retries(const FaultCode& c);

/// One coded line (prefix and labels in any of QIDI's forms) as an event: our
/// wording when the table has a row, the firmware's message otherwise.
/// nullopt when @p text carries no QD code. Translates: main thread only.
[[nodiscard]] std::optional<ErrorEvent> classify_fault_line(const std::string& text,
                                                            const ClassifyContext& ctx);

} // namespace helix::qidi
