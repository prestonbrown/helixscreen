// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_narration_matcher.h
 * @brief Capability of a filament system whose console narration names operation phases
 *
 * Reached with AmsBackend::as<INarrationMatcher>(). AFC implements it. A backend
 * without it never advances a phase from the console and logs no drift hints.
 */

#pragma once

#include <optional>
#include <string>

namespace helix {

class INarrationMatcher {
  public:
    /// Map one `//` narration body (prefix already stripped) to a phase id.
    /// nullopt => not a recognized phase line.
    ///
    /// A `//` body came from a macro's own respond_info, so implementations may
    /// use loose substring needles here and tolerate upstream rewording. Do NOT
    /// route bare console lines through this; see match_bare_narration_phase().
    [[nodiscard]] virtual std::optional<std::string>
    match_narration_phase(const std::string& narration) const = 0;

    /// Map one console line that arrived WITHOUT the `//` prefix to a phase id.
    /// nullopt => not a recognized phase line.
    ///
    /// Separate from match_narration_phase() because the input is a different
    /// kind of text: unprefixed responses are the printer's open console, mixing
    /// M105 reports, `echo:` output and USER-CONTROLLED gcode filenames in with
    /// any narration. Implementations must match anchored line shapes, never
    /// loose substrings: a needle like `cut` would fire on `haircut.gcode`.
    [[nodiscard]] virtual std::optional<std::string>
    match_bare_narration_phase(const std::string& line) const = 0;

    /// True when an UNMATCHED console line looks like this backend's narration,
    /// i.e. is worth a drift hint in the log. Deliberately looser than the two
    /// matchers, since its whole job is catching reworded narration, but it must
    /// exclude the lines the backend emits that have no phase by design, or
    /// every operation reports itself as drift.
    [[nodiscard]] virtual bool is_narration_drift_candidate(const std::string& line) const = 0;

  protected:
    ~INarrationMatcher() = default;
};

} // namespace helix
