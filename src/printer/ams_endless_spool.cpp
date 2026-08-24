// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_endless_spool.cpp
 * @brief Backend-agnostic endless-spool model: restriction text, group
 *        construction, and the one group-to-edge projection.
 *
 * Nothing here touches a backend, a mutex or LVGL widgets, so it is directly
 * unit-testable. The projection in particular must exist exactly once - four
 * backends previously each invented their own, and Happy Hare's
 * `// Use first match` loop is what made a 4-gate group render as four
 * arbitrary arrows.
 */

#include "ams_types.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <algorithm>
#include <map>

namespace helix::printer {

std::string endless_spool_restriction_text(EndlessSpoolRestriction restriction) {
    switch (restriction) {
    case EndlessSpoolRestriction::None:
        return {};
    case EndlessSpoolRestriction::MultiUnit:
        return lv_tr("Cannot be changed from here on a multi-unit MMU");
    case EndlessSpoolRestriction::FirmwareManaged:
        return lv_tr("The printer's firmware chooses the backup spool itself");
    case EndlessSpoolRestriction::NotReady:
        return lv_tr("Waiting for the filament system to report its state");
    case EndlessSpoolRestriction::PluginMissing:
        return lv_tr("No automatic backup-spool package is installed");
    case EndlessSpoolRestriction::PluginReadOnly:
        return lv_tr("Configured in the backup-spool package, not from here");
    }
    return {};
}

EndlessSpoolStatus endless_spool_status(const EndlessSpoolCapabilities& caps) {
    EndlessSpoolStatus out;

    // Unsupported says nothing. "Off" would be a lie of a different shape: a
    // printer without the mechanism is not a printer with it switched off.
    if (caps.availability == EndlessSpoolAvailability::Unsupported) {
        return out;
    }

    if (caps.availability == EndlessSpoolAvailability::RequiresPlugin) {
        out.kind = EndlessSpoolStatusKind::NeedsPlugin;
        if (!caps.provider.empty()) {
            // The backend knows the package by name, so name it - that is the
            // one thing the user can act on.
            out.text = lv_tr("Needs the {} package to switch spools");
            const auto pos = out.text.find("{}");
            if (pos != std::string::npos) {
                out.text.replace(pos, 2, caps.provider);
            }
        } else {
            // No backend populates provider in this state today (AD5X cannot
            // know whether you would install lessWaste or bambufy), so the
            // restriction text is what actually renders.
            out.text = endless_spool_restriction_text(caps.restriction);
            if (out.text.empty()) {
                out.text = lv_tr("No automatic backup-spool package is installed");
            }
        }
        return out;
    }

    switch (caps.enabled) {
    case EndlessSpoolEnabled::On:
        out.kind = EndlessSpoolStatusKind::On;
        out.text = lv_tr("Switches to a backup spool on runout");
        break;
    case EndlessSpoolEnabled::Off:
        out.kind = EndlessSpoolStatusKind::Off;
        out.text = lv_tr("Will not switch spools on runout");
        break;
    case EndlessSpoolEnabled::Unknown:
        out.kind = EndlessSpoolStatusKind::Unknown;
        out.text = lv_tr("Backup spool switching state unknown");
        break;
    }

    // A proper noun needs no translation, so attributing the behaviour costs no
    // string. Empty for every backend that implements this itself.
    if (!caps.provider.empty()) {
        out.text += " (" + caps.provider + ")";
    }

    // Second line: why it is the way it is, or why you cannot change it here.
    // Both halves matter - "nothing will switch" without "and the firmware owns
    // that decision" is the greyed-control-with-no-explanation complaint again.
    const std::string reason = endless_spool_restriction_text(caps.restriction);
    if (!reason.empty()) {
        out.text += "\n" + reason;
    }
    return out;
}

EndlessSpoolConfig endless_spool_config_from_edges(const std::vector<int>& edges) {
    EndlessSpoolConfig cfg;
    for (int slot = 0; slot < static_cast<int>(edges.size()); ++slot) {
        const int backup = edges[static_cast<size_t>(slot)];
        if (backup < 0 || backup == slot) {
            continue;
        }
        EndlessSpoolGroup group;
        group.members = {slot, backup};
        group.ordered = true;
        cfg.groups.push_back(std::move(group));
    }
    return cfg;
}

EndlessSpoolConfig endless_spool_config_from_groups(const std::vector<int>& group_ids) {
    // std::map, not unordered_map: the group order in the result is observable
    // (it drives the projection's iteration and any future group rendering), and
    // a stable ascending-by-id order is the one a reader can predict.
    std::map<int, std::vector<int>> by_id;
    for (int slot = 0; slot < static_cast<int>(group_ids.size()); ++slot) {
        const int id = group_ids[static_cast<size_t>(slot)];
        if (id < 0) {
            continue;
        }
        by_id[id].push_back(slot);
    }

    EndlessSpoolConfig cfg;
    for (auto& [id, members] : by_id) {
        // A group of one backs nothing up. Emitting it would make "is grouped"
        // and "has a backup" disagree, and Happy Hare hands us exactly this
        // shape: every ungrouped gate gets its own standalone id.
        if (members.size() < 2) {
            continue;
        }
        EndlessSpoolGroup group;
        group.id = id;
        group.members = std::move(members);
        group.ordered = false;
        cfg.groups.push_back(std::move(group));
    }
    return cfg;
}

std::vector<int> endless_spool_backup_edges(const EndlessSpoolConfig& cfg, int slot_count) {
    std::vector<int> edges(slot_count < 0 ? 0 : static_cast<size_t>(slot_count), -1);
    const auto in_range = [&edges](int slot) {
        return slot >= 0 && slot < static_cast<int>(edges.size());
    };

    // First group to give a slot a successor wins, so this agrees with
    // endless_spool_backup_for()'s single-slot walk. Neither builder can produce
    // a slot with two successors, but a hand-built config can, and the two
    // entry points must not disagree about it.
    const auto assign = [&](int from, int to) {
        if (in_range(from) && to >= 0 && from != to && edges[static_cast<size_t>(from)] < 0) {
            edges[static_cast<size_t>(from)] = to;
        }
    };

    for (const auto& group : cfg.groups) {
        if (group.ordered) {
            for (size_t i = 0; i + 1 < group.members.size(); ++i) {
                assign(group.members[i], group.members[i + 1]);
            }
            continue;
        }
        // Undirected: project the clique onto a ring, members[i] -> members[i+1]
        // and the last member back to the first.
        //
        // The projection this replaced pointed every member at the FIRST other
        // member, which for a 4-gate Happy Hare group drew 0->1, 1->0, 2->0,
        // 3->0 - a picture that says "slot 1 is everyone's backup", which is not
        // what a clique means. A ring says the true thing the widget is able to
        // say: every member hands off to another member, and following the
        // arrows visits the whole group exactly once. Neither shape is the whole
        // relation - see endless_spool_status()' sibling note and the
        // ui_endless_spool_arrows widget, which has one target per source and no
        // way to draw a pool - but only one of them privileges an arbitrary
        // member.
        for (size_t i = 0; i < group.members.size(); ++i) {
            assign(group.members[i], group.members[(i + 1) % group.members.size()]);
        }
    }
    return edges;
}

int endless_spool_backup_for(const EndlessSpoolConfig& cfg, int slot) {
    if (slot < 0) {
        return -1;
    }
    // Same rules as endless_spool_backup_edges(), evaluated for one slot so the
    // caller does not have to know the system's slot count. The first group that
    // yields a successor wins - the same rule endless_spool_backup_edges() uses.
    for (const auto& group : cfg.groups) {
        const auto it = std::find(group.members.begin(), group.members.end(), slot);
        if (it == group.members.end()) {
            continue;
        }
        if (group.ordered) {
            const auto next = std::next(it);
            if (next != group.members.end()) {
                return *next;
            }
            continue; // Tail of an ordered chain has no successor.
        }
        // Same ring as endless_spool_backup_edges(): next member, wrapping.
        const auto pos = static_cast<size_t>(std::distance(group.members.begin(), it));
        const int next = group.members[(pos + 1) % group.members.size()];
        if (next != slot) {
            return next;
        }
    }
    return -1;
}

} // namespace helix::printer
