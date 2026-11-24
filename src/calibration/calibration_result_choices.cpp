// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_result_choices.h"

#include "save_config_live.h"

#include "hv/json.hpp"

namespace helix::calibration {

ResultChoices result_choices(const ResultFacts& facts) {
    ResultChoices c;
    c.offer_save = facts.can_save && facts.staged;
    // Keep for Now is also the way out when there is nothing to save or drop.
    c.offer_keep = true;
    c.keep = facts.live ? KeepMeaning::InUse : KeepMeaning::NotYetInUse;
    c.discard = facts.staged ? DiscardMeaning::RestartDropsIt : DiscardMeaning::NothingToUndo;
    c.save_restarts = facts.save_restarts;
    return c;
}

ResultFacts staged_result_facts(const PrinterDiscovery& hw, const nlohmann::json& pending_items,
                                bool can_save) {
    ResultFacts f;
    f.staged = !pending_items.is_object() || !pending_items.empty();
    f.save_restarts = save_config::restart_required(hw, pending_items);
    f.live = f.staged && !f.save_restarts;
    f.can_save = can_save;
    return f;
}

} // namespace helix::calibration
