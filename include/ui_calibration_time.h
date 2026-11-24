// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The one wording every calibration screen uses for its time, up front and as
// plain information: what the printer does on its own, and what needs the owner.

#include <string>

namespace helix::calibration {

/// What the printer does while the owner is away.
enum class AwayKind {
    Prints, ///< it prints a test piece
    Runs,   ///< it runs a routine: probing, heating, a sweep
};

/// "Prints on its own, about N min" / "Runs on its own, about N min" (translated).
[[nodiscard]] std::string time_away_line(AwayKind kind, int minutes);

/// "Needs you, about N min: <task>", or without the task when @p task is empty (translated).
[[nodiscard]] std::string time_you_line(int minutes, const std::string& task = {});

} // namespace helix::calibration
