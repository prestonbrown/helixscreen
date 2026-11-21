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
    /// The head's feeder can supply it right now: feeder present, automatic
    /// loading on, filament at its port, runout detection armed. On the head
    /// that ran out this means filament is still there (a jam or a tangle),
    /// and a firmware that refeeds in place resumes on it.
    bool feeder_can_supply = false;
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
    /// The filament the runout head was printing, as recorded when the pause
    /// began (a spool swapped in since does not change it); nullopt when unknown.
    std::optional<FilamentRecord> original;
    /// The firmware's own runout switching (Endless Spool) is on; nullopt when
    /// not reported yet.
    std::optional<bool> auto_switch_enabled;
    /// Every head, the runout head included.
    std::vector<HeadFacts> heads;
    /// The firmware's logical tool -> head routing as last reported; empty when
    /// the filament system reports none. A switch rewrites it.
    std::vector<int> routing;
    /// The firmware is running an action of its own inside the pause (resuming,
    /// or switching heads by itself) and refuses another until it is done.
    bool printer_busy = false;
};

/// Why a head cannot take over the print.
enum class HeadRefusal {
    None,             ///< it can
    IsRunoutHead,     ///< the head that ran out
    NozzleUnknown,    ///< a nozzle size or flow type is not reported
    NozzleSize,       ///< a different nozzle diameter
    NozzleFlow,       ///< a different flow type (standard vs high flow)
    NotFedByFeeder,   ///< empty, hand-fed, or its runout detection is off
    NoFilamentRecord, ///< loaded, but its filament is not fully identified
    MaterialMismatch, ///< a material the sliced temperatures do not suit
    /// The head that ran out can still be fed, so the firmware would resume on
    /// it instead (every other head reads this).
    RunoutHeadStillFeeds,
    /// A material the filament database does not know, on either side: nothing
    /// says the sliced temperatures suit it.
    MaterialUnknown,
    /// The printer is resuming or switching heads by itself (every other head).
    PrinterBusy,
    /// The routing already moved the rest of the print off the runout head
    /// (every other head).
    AlreadyMoved,
    /// Another head is active now; the firmware continues only from the head
    /// that ran out (every other head).
    RunoutHeadNotActive,
    /// The filament system cannot copy this head's record onto the empty head.
    RecordNotWritable,
};

/// One head's answer.
struct HeadVerdict {
    int head = -1;
    HeadRefusal refusal = HeadRefusal::None;
    /// Eligible, but its colour differs from what ran out: warn before using it.
    bool color_differs = false;
    /// Eligible, but its material type or variant differs from what ran out
    /// (PLA Silk for PLA Matte): the rest prints at the original's temperature.
    bool variant_differs = false;

    [[nodiscard]] bool eligible() const {
        return refusal == HeadRefusal::None;
    }
};

/// Every head, in head order, with the reason it can or
/// cannot take over. Empty when @p facts has no runout head or no original
/// filament to compare against.
[[nodiscard]] std::vector<HeadVerdict> assess(const RunoutFacts& facts);

/// assess() plus what filament system @p type itself cannot do for a head (a
/// record it cannot write). What the chooser shows and build_plan() checks.
[[nodiscard]] std::vector<HeadVerdict> assess(AmsType type, const RunoutFacts& facts);

/// Short user-facing reason for @p refusal, untranslated (TR_NOOP); empty for
/// None. Callers translate with lv_tr().
[[nodiscard]] const char* refusal_text(HeadRefusal refusal);

/// Why no plan could be built for a choice.
enum class PlanRefusal {
    None,
    Unsupported,         ///< this filament system has no way to do it
    NotPaused,           ///< the print is not paused
    RunoutHeadNotActive, ///< the toolhead no longer carries the head that ran out
    PrinterBusy,         ///< the firmware is resuming or switching by itself
    HeadNotEligible,     ///< assess() refuses the chosen head
    UnsafeRecord,        ///< the chosen head's record cannot be written as G-code
    AutoSwitchUnknown,   ///< the firmware has not said whether its runout switching is on
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
    /// for this one switch; restore_gcode turns it back off.
    bool switches_auto_switch_on = false;
    /// What puts the firmware's settings back as they were. The plan's last
    /// line already does it, but a script stops at its first error, so a
    /// caller whose send failed or did not switch sends this on its own.
    /// Empty when nothing needs restoring.
    std::string restore_gcode;
    /// What resets the empty head's record to "no filament" once the print has
    /// moved off it, so the borrowed label matches no later runout and no next
    /// print. Sent only while the empty head still carries the label.
    std::string clear_label_gcode;
    /// What puts the empty head's record back as it was before the label, for
    /// a switch that did not happen. Sent only while it still carries the label.
    std::string unlabel_gcode;
    /// What resumes the print on continues_on once the routing has moved there,
    /// for a switch whose own resume failed.
    std::string resume_gcode;

    [[nodiscard]] bool ok() const {
        return refusal == PlanRefusal::None;
    }
};

/// Whether filament system @p type can continue a runout on another head.
[[nodiscard]] bool supported(AmsType type);

/// The plan for continuing on @p chosen_head. Refused (never a partial plan)
/// when @p type has no row or any precondition fails.
[[nodiscard]] ContinuePlan build_plan(AmsType type, const RunoutFacts& facts, int chosen_head);

/// Whether the routing moved every logical tool of the runout head onto
/// @p target since @p before. False when no routing was reported before
/// sending: the first routing frame is no evidence.
[[nodiscard]] bool moved_to(const RunoutFacts& before, const RunoutFacts& now, int target);

/// The head the routing moved the runout head's tools to since @p before, or
/// -1 when it did not move them (or moved them apart).
[[nodiscard]] int moved_onto(const RunoutFacts& before, const RunoutFacts& now);

/// What came back for the sent plan.
enum class SendReply {
    None,          ///< nothing yet
    Ok,            ///< the script ran; the firmware may still have declined
    TransportLost, ///< a timeout or a dropped link: the printer may still be at it
    Refused,       ///< the printer answered with an error
};

/// What to do next about a sent switch.
enum class SwitchStep {
    Wait,     ///< keep watching
    Done,     ///< moved to the target head and printing again
    Diverted, ///< moved, but to another head than the target
    Stranded, ///< moved to the target head, but the print is still paused
    Fail,     ///< not moved: report it and put back what the plan changed
};

/// Frames reporting the switch may trail the reply this long.
inline constexpr uint32_t kSwitchSettleMs = 5000;
/// Past this since sending, a switch that has not shown is not coming. Longer
/// than the send's own timeout, so a slow heat and purge is not called failed.
inline constexpr uint32_t kSwitchGiveUpMs = 180000;

/// The next step for a sent switch, judged by its target: the routing moved
/// the runout head's tools onto @p target and the print left the pause. A
/// clean reply does not mean success (the firmware declines by doing nothing),
/// and the pause ending alone does not either (the firmware resumes on the
/// runout head when it refeeds in place). The firmware rewrites the routing
/// before its resume, and the print stays paused through the heat and purge,
/// so a moved routing waits for the reply before calling the print stranded.
/// Without a routing reported before sending, only the pause ending counts.
[[nodiscard]] SwitchStep judge_switch(const RunoutFacts& before, const RunoutFacts& now, int target,
                                      SendReply reply, uint32_t ms_since_reply,
                                      uint32_t ms_since_send);

/// Whether the empty head now carries @p label: the record write went through,
/// whatever became of the switch after it.
[[nodiscard]] bool label_written(const RunoutFacts& now, const FilamentRecord& label);

/// Whether to offer the switch at all: the runout head is known, its feeder
/// cannot supply it (otherwise the firmware resumes on it, and Resume is the
/// answer), the printer is not resuming or switching by itself, and the
/// routing has not already moved the print off the runout head.
[[nodiscard]] bool worth_offering(const RunoutFacts& facts);

/// Whether the runout head is still the one the pause is about: the printer is
/// not resuming or switching by itself, the toolhead carries the runout head,
/// and the routing has not moved the print off it. Guidance about the runout
/// head (a filament reading, Resume) holds only while this does.
[[nodiscard]] bool runout_head_in_charge(const RunoutFacts& facts);

/// What to do about a restore a switch left pending (a setting it turned on
/// for itself), whenever the facts may have moved.
enum class PendingRestoreStep {
    Keep,  ///< nothing to do yet: no pending restore, a switch in flight, or the
           ///< setting not reported
    Send,  ///< the setting still reads on: send the restore
    Clear, ///< the setting reads off: the restore is done, forget it
};

/// @param pending a restore is pending (persisted across restarts)
/// @param switch_in_flight a sent switch is still being watched; its script
///        turns the setting off itself
/// @param auto_switch_enabled the firmware's runout switching as reported
[[nodiscard]] PendingRestoreStep pending_restore_step(bool pending, bool switch_in_flight,
                                                      std::optional<bool> auto_switch_enabled);

/// The restore a sent switch left pending, as the active printer's settings
/// hold it, so a restart or a lost link cannot leave the setting changed.
/// Empty when none. Main thread.
[[nodiscard]] std::string read_pending_restore();
/// Store @p gcode as pending (empty clears it). Main thread.
void write_pending_restore(const std::string& gcode);

} // namespace helix::runout_other_head
