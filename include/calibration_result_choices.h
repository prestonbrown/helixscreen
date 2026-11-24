// SPDX-License-Identifier: GPL-3.0-or-later
/// @file calibration_result_choices.h
/// @brief What a finished calibration offers: Save to Config, Keep for Now, Discard.
///
/// One decision for every calibration result page (components/calibration_result.xml),
/// so the screens agree on what each choice does and says. No LVGL.
#pragma once

#include "json_fwd.h"

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
    /// HelixScreen may write printer.cfg (the firmware does not supervise it).
    bool can_save = true;
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
    KeepMeaning keep = KeepMeaning::NotYetInUse;
    DiscardMeaning discard = DiscardMeaning::RestartDropsIt;
    bool save_restarts = true;
};

[[nodiscard]] ResultChoices result_choices(const ResultFacts& facts);

/// The facts for a result the calibration staged in Klipper, from
/// configfile.save_config_pending_items (null or unreadable counts as staged).
/// live and save_restarts come from save_config::restart_required(): a result is
/// in use now exactly when a save may skip the restart, which only a firmware
/// with SAVE_CONFIG RESTART=0 can say; elsewhere it is assumed not live.
[[nodiscard]] ResultFacts staged_result_facts(const PrinterDiscovery& hw,
                                              const nlohmann::json& pending_items, bool can_save);

} // namespace helix::calibration
