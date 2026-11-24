// SPDX-License-Identifier: GPL-3.0-or-later
/// @file calibration_result_choices.h
/// @brief What a finished calibration offers: Save to Config, Keep for Now, Discard.
///
/// One decision for every calibration result page (components/calibration_result.xml),
/// so the screens agree on what each choice does and says. No LVGL.
#pragma once

#include "json_fwd.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {
class PrinterDiscovery;
}

namespace helix::calibration {

/// The facts about one result that decide its choices.
struct ResultFacts {
    /// Klipper holds the result for the next SAVE_CONFIG
    /// (configfile.save_config_pending_items is not empty).
    bool staged = true;
    /// The running firmware already uses the new value.
    bool live = false;
    /// A SAVE_CONFIG now restarts Klipper.
    bool save_restarts = true;
    /// The run first rewrote the heater's control in printer.cfg (PID to MPC or back):
    /// without this result saved the heater has no tuning for its new control and
    /// will not heat, so Keep for Now and Discard would both leave it unusable.
    bool control_switched = false;
    /// Other sections Klipper holds for the same SAVE_CONFIG: Save writes them and
    /// Discard drops them along with this result.
    std::vector<std::string> also_staged;
};

/// What Keep for Now leaves behind.
enum class KeepMeaning {
    InUse,       ///< the new value works now and is lost at the next restart unless saved
    NotYetInUse, ///< staged only: it takes effect when saved, which restarts Klipper
};

/// What Discard has to do.
enum class DiscardMeaning {
    RestartDropsIt, ///< Klipper holds it for the next save: a restart drops it (and its live
                    ///< effect)
    NothingToUndo,  ///< nothing was staged: leaving drops it
};

struct ResultChoices {
    bool offer_save = true;
    bool offer_keep = true;
    bool offer_discard = true;
    /// Keep and Discard are withheld: only saving leaves the printer usable.
    bool must_save = false;
    KeepMeaning keep = KeepMeaning::NotYetInUse;
    DiscardMeaning discard = DiscardMeaning::RestartDropsIt;
    bool save_restarts = true;
    std::vector<std::string> also_staged;
};

[[nodiscard]] ResultChoices result_choices(const ResultFacts& facts);

/// The facts for a result the calibration staged in Klipper, from
/// configfile.save_config_pending_items (null or unreadable counts as staged).
/// live and save_restarts come from save_config::restart_required(): a result is
/// in use now exactly when a save may skip the restart, which only a firmware
/// with SAVE_CONFIG RESTART=0 can say; elsewhere it is assumed not live.
/// Save is Klipper's own SAVE_CONFIG, which firmware that supervises HelixScreen still allows
/// (#2154 refuses only HelixScreen's own file writes), so it is offered there too.
/// @p own names the sections this result staged; every other pending section is
/// also_staged.
[[nodiscard]] ResultFacts staged_result_facts(const PrinterDiscovery& hw,
                                              const nlohmann::json& pending_items,
                                              const std::function<bool(const std::string&)>& own);

} // namespace helix::calibration
