// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_result_choices.h"

#include "save_config_live.h"

#include "hv/json.hpp"

namespace helix::calibration {

ResultChoices result_choices(const ResultFacts& facts) {
    ResultChoices c;
    c.offer_save = facts.staged;
    // Keep for Now is also the way out when there is nothing to save or drop.
    c.offer_keep = true;
    c.keep = facts.live ? KeepMeaning::InUse : KeepMeaning::NotYetInUse;
    c.discard = facts.staged ? DiscardMeaning::RestartDropsIt : DiscardMeaning::NothingToUndo;
    c.save_restarts = facts.save_restarts;
    c.also_staged = facts.also_staged;
    if (facts.control_switched && facts.staged) {
        c.must_save = true;
        c.offer_keep = false;
        c.offer_discard = false;
    }
    return c;
}

ResultFacts staged_result_facts(const PrinterDiscovery& hw, const nlohmann::json& pending_items,
                                const std::function<bool(const std::string&)>& own) {
    ResultFacts f;
    f.staged = !pending_items.is_object() || !pending_items.empty();
    nlohmann::json mine = nlohmann::json::object();
    if (pending_items.is_object()) {
        for (const auto& [section, options] : pending_items.items()) {
            if (own && own(section)) {
                mine[section] = options;
            } else {
                f.also_staged.push_back(section);
            }
        }
    }
    // The save writes everything pending; whether this result is in use already
    // depends on its own sections only.
    f.save_restarts = save_config::restart_required(hw, pending_items);
    f.live = f.staged && !mine.empty() && !save_config::restart_required(hw, mine);
    return f;
}

} // namespace helix::calibration
