// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_print_routing.h
 * @brief Capability of a filament system that sends the firmware gcode ahead of a job
 *
 * Reached with AmsBackend::as<IPrintRouting>(). The Snapmaker U1, MultiACE, CFS, the AD5X IFS and
 * the mock implement it. A backend without it has nothing to send and no routing to recall: no
 * pre-print step, no standing options, no purge preamble, and a reprint with no recorded routing.
 * Every default below gives that same answer, so a backend overrides only what its firmware has.
 *
 * Whether a print start runs the step at all is the constant traits
 * AmsBackend::requires_preprint_send() and print_routing_resets_to_identity(). The firmware's
 * default tool map, AmsBackend::firmware_default_routing(), is a question every backend answers.
 */

#pragma once

#include "ams_error.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace helix {

class IPrintRouting {
  public:
    /**
     * @brief Routing the printer last ran a CONFIGURED print task with.
     *
     * AmsBackend::get_tool_mapping() answers "what routing is live right now" and goes
     * empty the moment the task ends, which is exactly when a reprint needs the
     * answer. This is the snapshot taken while the task was still configured:
     * logical tool -> physical head for the most recent print, retained after the
     * firmware clears its used-heads flags and (on some firmware) resets the
     * table itself.
     *
     * EMPTY means NOT KNOWN: no configured task has been observed. A caller must
     * not read that as identity; run it through FilamentMapper::reprint_remap(),
     * which turns "not known" into "do not send" rather than into a default map.
     *
     * Not keyed to a file: it describes the most recent print, which is the only
     * job a reprint can target.
     */
    [[nodiscard]] virtual std::vector<int> last_print_tool_mapping() const {
        return {};
    }

    /**
     * @brief Does the firmware route each file tool to its own slot by default?
     *
     * When last_print_tool_mapping() is not known, a reprint has no routing to
     * replay. A backend answering true runs every tool the pre-print send leaves
     * out at the slot of the same number, so the identity map for the file's
     * tools states that default rather than guessing one; a reprint sends it to
     * overwrite any map the printer still holds for its next print. A backend
     * whose jobs carry their own crossover (Snapmaker U1) answers false: there
     * an identity map written for a reprint is a confident wrong answer, so an
     * unknown routing writes no map. What such a reprint may still send is
     * AmsBackend::print_routing_resets_to_identity()'s question.
     */
    [[nodiscard]] virtual bool default_print_routing_is_identity() const {
        return false;
    }

    /**
     * @brief Firmware-native pre-print command sequence (e.g. print_task_config).
     *
     * Backends that answer AmsBackend::requires_preprint_send() == true emit
     * firmware-native gcode (SET_PRINT_USED_EXTRUDERS / SET_PRINT_EXTRUDER_MAP) BEFORE
     * PRINT_START. The caller gates on that trait and sends the returned gcode.
     *
     * @param tools_used Logical tools the gcode body uses
     * @param remap      Logical tool -> physical head, only for changed tools
     * @return Newline-joined gcode (no trailing newline), or "" when none
     */
    [[nodiscard]] virtual std::string build_preprint_gcode(const std::set<int>& tools_used,
                                                           const std::map<int, int>& remap) const {
        (void)tools_used;
        (void)remap;
        return "";
    }

    /**
     * @brief The pre-print step for a reprint whose routing is unknown.
     *
     * Asked only of a backend answering AmsBackend::print_routing_resets_to_identity():
     * the slots @p tools_used run at under the firmware's default routing, and no
     * map. A backend that cannot vouch for that default right now answers the
     * routing-free part instead. Default: the routing-free part.
     */
    [[nodiscard]] virtual std::string
    build_unrouted_preprint_gcode(const std::set<int>& tools_used) const {
        (void)tools_used;
        return build_preprint_gcode({}, {});
    }

    /**
     * @brief Whether the pre-print send can honor this remap right now.
     *
     * The pre-print counterpart to AmsBackend::can_set_tool_mapping(): a remap naming a
     * slot that cannot feed is refused here, and PrintStartController stops
     * the print with the error's user_msg instead of sending a map the
     * firmware would act on with a slot it cannot reach. A backend that
     * refuses here also returns "" from build_preprint_gcode() for the same
     * arguments.
     *
     * @param tools_used Logical tools the gcode body uses
     * @param remap      Logical tool -> physical slot, only for changed tools
     */
    [[nodiscard]] virtual AmsError check_preprint_remap(const std::set<int>& tools_used,
                                                        const std::map<int, int>& remap) const {
        (void)tools_used;
        (void)remap;
        return AmsErrorHelper::success();
    }

    /**
     * @brief The user's standing per-print choices, for a print already running.
     *
     * A print some other client started (Mainsail, a slicer, Moonraker's job
     * queue) skips this screen's pre-print step. This is the part of that step
     * that still applies once the print is running, rendered by the same
     * functions build_preprint_gcode() uses: no routing (it is refused or too
     * late mid-print), only choices that act later in the print. It is sent
     * right after such a start, by the HelixPrint plugin or by this screen.
     *
     * @return Newline-joined gcode (no trailing newline), or "" when none
     */
    [[nodiscard]] virtual std::string standing_print_gcode() const {
        return "";
    }

    /**
     * @brief This screen is about to start a print on a pre-print-send backend.
     *
     * Called by every start that runs the pre-print step, as the start is
     * dispatched, so a backend holding per-print state can begin it for the
     * print that follows. Main thread.
     */
    virtual void note_print_start_from_here() {}

    /**
     * @brief Gcode to run ahead of a user-requested purge, or "" for none.
     *
     * A machine whose firmware ships a "go to the purge bin" routine answers it
     * here, so every Purge surface sends it before the purge macro or the raw
     * extrude fallback and the filament lands in the bin instead of wherever the
     * nozzle stopped. Callers dispatch the purge only once this has completed.
     *
     * @param job_holds_machine A print owns the toolhead (printing or paused):
     *        the answer must not home then.
     */
    [[nodiscard]] virtual std::string purge_preamble_gcode(bool job_holds_machine) const {
        (void)job_holds_machine;
        return {};
    }

  protected:
    ~IPrintRouting() = default;
};

} // namespace helix
