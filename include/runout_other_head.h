// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Continuing a print paused by a filament runout on another, already loaded
// toolhead (prestonbrown/helixscreen#2137).
//
// Two questions live here. Which heads may take over (assess(): generic, the
// same rules on every printer) and what the printer must be told to move the
// rest of the job onto the chosen one (build_plan(): one row per filament
// system in the provider table in runout_other_head.cpp). Generic code asks
// these functions and never names a firmware. Adding a filament system means
// adding one row there and a facts builder on its AmsBackend.
//
// The plan never resumes on another head by itself: the remaining tool changes
// of the job still name the empty head, so a plain resume sends the next one
// straight back to it. A row exists only for firmware that re-routes them.

#include "ams_type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix::runout_other_head {

/// A head's filament as the firmware records it: what the firmware compares
/// when it looks for a replacement head, and what a copy has to reproduce.
struct FilamentRecord {
    std::string vendor;
    std::string type;    ///< e.g. "PLA"
    std::string subtype; ///< e.g. "Basic", "Matte"
    /// The colours, each "RRGGBB" exactly as the firmware spells it. A
    /// single-colour spool has one entry.
    std::vector<std::string> colors;
    int alpha = 0xFF;
    int multi_mode = 0; ///< how a multi-colour spool's colours are laid out

    bool operator==(const FilamentRecord& o) const {
        return vendor == o.vendor && type == o.type && subtype == o.subtype && colors == o.colors &&
               alpha == o.alpha && multi_mode == o.multi_mode;
    }
};

/// What the printer reports about one toolhead.
struct HeadFacts {
    int head = -1;
    std::optional<double> nozzle_diameter;
    /// The nozzle's flow type ("standard", "high_flow"); empty when unknown.
    std::string nozzle_flow;
    /// Filament reached this head through its feeder and the head's runout
    /// detection is armed. A spool pushed in by hand does not count.
    bool loaded_by_feeder = false;
    /// nullopt when the head's record names no filament.
    std::optional<FilamentRecord> record;
};

/// The printer's state at a runout pause, as one filament backend sees it.
struct RunoutFacts {
    /// The head whose filament ran out.
    int runout_head = -1;
    /// The print is paused (not printing, not idle).
    bool paused = false;
    /// The head the toolhead carries now. The firmware continues only from the
    /// head that ran out.
    int active_head = -1;
    /// The filament the runout head was printing; nullopt when unknown.
    std::optional<FilamentRecord> original;
    /// The firmware's own runout switching (Endless Spool) is on; nullopt when
    /// not reported yet.
    std::optional<bool> auto_switch_enabled;
    /// Every head, the runout head included.
    std::vector<HeadFacts> heads;
};

/// Why a head cannot take over the print.
enum class HeadRefusal {
    None,             ///< it can
    IsRunoutHead,     ///< the head that ran out
    NozzleUnknown,    ///< a nozzle size or flow type is not reported
    NozzleSize,       ///< a different nozzle diameter
    NozzleFlow,       ///< a different flow type (standard vs high flow)
    NotFedByFeeder,   ///< empty, hand-fed, or its runout detection is off
    NoFilamentRecord, ///< loaded, but its filament is not identified
    MaterialMismatch, ///< a material the sliced temperatures do not suit
};

/// One head's answer.
struct HeadVerdict {
    int head = -1;
    HeadRefusal refusal = HeadRefusal::None;
    /// Eligible, but its colour differs from what ran out: warn before using it.
    bool color_differs = false;

    [[nodiscard]] bool eligible() const {
        return refusal == HeadRefusal::None;
    }
};

/// Every head, in head order, with the reason it can or
/// cannot take over. Empty when @p facts has no runout head or no original
/// filament to compare against.
[[nodiscard]] std::vector<HeadVerdict> assess(const RunoutFacts& facts);

/// Short user-facing reason for @p refusal, untranslated (TR_NOOP); empty for
/// None. Callers translate with lv_tr().
[[nodiscard]] const char* refusal_text(HeadRefusal refusal);

/// Why no plan could be built for a choice.
enum class PlanRefusal {
    None,
    Unsupported,         ///< this filament system has no way to do it
    NotPaused,           ///< the print is not paused
    RunoutHeadNotActive, ///< the toolhead no longer carries the head that ran out
    HeadNotEligible,     ///< assess() refuses the chosen head
    UnsafeRecord,        ///< the chosen head's record cannot be written as G-code
};

/// What to send to move the rest of the print onto the chosen head.
struct ContinuePlan {
    PlanRefusal refusal = PlanRefusal::None;
    /// G-code lines, sent in order as one script.
    std::vector<std::string> gcode;
    /// The head the firmware will actually continue on. It may be another
    /// head holding the identical filament, when the firmware finds that one
    /// first; -1 when there is no plan.
    int continues_on = -1;
    /// The firmware's own runout switching is off and the plan turns it on
    /// for this one switch, restoring it afterwards when it was known off.
    bool switches_auto_switch_on = false;

    [[nodiscard]] bool ok() const {
        return refusal == PlanRefusal::None;
    }
};

/// Whether filament system @p type can continue a runout on another head.
[[nodiscard]] bool supported(AmsType type);

/// The plan for continuing on @p chosen_head. Refused (never a partial plan)
/// when @p type has no row or any precondition fails.
[[nodiscard]] ContinuePlan build_plan(AmsType type, const RunoutFacts& facts, int chosen_head);

} // namespace helix::runout_other_head
