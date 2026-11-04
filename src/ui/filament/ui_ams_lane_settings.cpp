// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_settings.h"

#include "ui_error_reporting.h"
#include "ui_print_start_controller.h"
#include "ui_toast_manager.h"

#include "ams_backend.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "filament_database.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::ui::lane_settings {

bool show_tool_row(const AmsBackend* backend) {
    return backend && helix::printer::can_write_mapping_table(*backend);
}

bool show_backup_row(const helix::printer::EndlessSpoolCapabilities& caps,
                     const helix::printer::EndlessSpoolConfig& cfg, int slot) {
    if (!caps.available()) {
        return false;
    }
    // Editable implies there is something to write even before anything is set.
    return caps.editable() || helix::printer::endless_spool_backup_for(cfg, slot) >= 0;
}

std::vector<int> backup_candidates(const std::vector<int>& bays, int slot) {
    std::vector<int> candidates;
    for (int bay : bays) {
        if (bay != slot) {
            candidates.push_back(bay);
        }
    }
    return candidates;
}

std::string backup_options(LaneNoun noun, const std::vector<int>& candidates, int slot,
                           const BackupEligibleFn& eligible) {
    std::string options = lv_tr("None");

    // One option per candidate, in the backend's own word ("Lane 1", "Gate 1").
    for (int i : candidates) {
        options += "\n" + helix::ui::lane_label(noun, i);
        if (slot >= 0 && eligible) {
            switch (eligible(slot, i)) {
            case helix::printer::BackupEligibility::Incompatible:
                options += std::string(" ") + lv_tr("(incompatible)");
                break;
            case helix::printer::BackupEligibility::GradeDiffers:
                // Selectable, and labelled so the choice is an informed one:
                // same polymer, but a filled grade that prints slower and wears
                // a brass nozzle.
                options += std::string(" ") + lv_tr("(different grade)");
                break;
            case helix::printer::BackupEligibility::Eligible:
                break;
            }
        }
    }
    return options;
}

bool backup_refused(int slot, int backup, const BackupEligibleFn& eligible) {
    if (backup < 0 || slot < 0) {
        return false; // "None" clears a backup; nothing to be compatible with.
    }
    // Only the hard verdict refuses. A grade difference is tagged in the option
    // list and then allowed: the print surviving its runout is worth more than
    // the grade being exact, and the user has been told which lane it is.
    return eligible && eligible(slot, backup) == helix::printer::BackupEligibility::Incompatible;
}

BackupEligibleFn eligibility_of(const AmsBackend* backend) {
    if (backend == nullptr) {
        return [](int, int) { return helix::printer::BackupEligibility::Eligible; };
    }
    return [backend](int slot, int candidate) {
        return backend->endless_spool_backup_eligibility(slot, candidate);
    };
}

std::string tool_options(int tool_count) {
    std::string options = lv_tr("None");
    for (int i = 0; i < tool_count; ++i) {
        options += "\n" + helix::ui::tool_label(i);
    }
    return options;
}

int assigned_tool(const AmsBackend& backend, int slot) {
    const auto mapping = backend.get_tool_mapping();
    // A multi-tool lane routes several tools; the one it is running is the
    // slot's own mapped_tool, when the table agrees it runs this slot.
    const int active = backend.get_slot_info(slot).mapped_tool;
    if (active >= 0 && static_cast<size_t>(active) < mapping.size() &&
        mapping[static_cast<size_t>(active)] == slot) {
        return active;
    }
    for (size_t tool = 0; tool < mapping.size(); ++tool) {
        if (mapping[tool] == slot) {
            return static_cast<int>(tool);
        }
    }
    return -1;
}

int backup_of(const AmsBackend& backend, int slot) {
    // One shared projection for the group relation - see
    // helix::printer::endless_spool_backup_for(). show_backup_row() makes the
    // same call, so a read-only row is only ever shown where this is >= 0.
    return helix::printer::endless_spool_backup_for(backend.get_endless_spool_config(), slot);
}

bool commit_assigned_tool(AmsBackend& backend, int slot, int tool) {
    if (tool < 0) {
        return true;
    }
    spdlog::info("[LaneSettings] Assigned tool for slot {}: tool {}", slot, tool);

    const auto mapping = backend.get_tool_mapping();
    for (size_t i = 0; i < mapping.size(); ++i) {
        if (static_cast<int>(i) != tool && mapping[i] == slot) {
            spdlog::warn("[LaneSettings] Tool {} will share slot {} with tool {}", tool, slot, i);
            // The position is a bare prefix, not part of the sentence: every
            // locale that inflects would otherwise have the verb or a
            // preposition agree with a noun that changes per backend.
            std::string msg =
                helix::ui::lane_label(backend.lane_noun(), slot) + ": " +
                fmt::format(lv_tr("{} and {} are both mapped here"), helix::ui::tool_label(tool),
                            helix::ui::tool_label(static_cast<int>(i)));
            ToastManager::instance().show(ToastSeverity::WARNING, msg.c_str());
            break;
        }
    }

    auto result = backend.set_tool_mapping(tool, slot);
    if (!result.success()) {
        // notify_ams_error() logs the technical message itself.
        helix::ui::notify_ams_error(result, lv_tr("Couldn't assign the tool"));
        return false;
    }
    helix::ui::adopt_hand_tool_mapping(backend.backend_index(), tool, slot);
    return true;
}

bool commit_backup(AmsBackend& backend, int slot, int backup) {
    spdlog::info("[LaneSettings] Backup for slot {}: {}", slot, backup);
    auto result = backend.set_endless_spool_backup(slot, backup);
    if (!result.success()) {
        helix::ui::notify_ams_error(result, lv_tr("Couldn't set the backup"));
        return false;
    }
    // The panel's endless-spool arrows redraw from the slots version.
    AmsState::instance().bump_slots_version();
    return true;
}

void notify_backup_refused(const AmsBackend& backend, int slot, int backup) {
    const std::string current_material = backend.get_slot_info(slot).material;
    const std::string backup_material = backend.get_slot_info(backup).material;
    spdlog::warn("[LaneSettings] Backend rejected backup slot {} for slot {} ({} / {})", backup,
                 slot, current_material, backup_material);

    // Name the materials only when they are in fact the problem. A backend with
    // a stricter rule (AD5X IFS matches colour and port presence too) can refuse
    // two slots holding the same material, and "PLA cannot use PLA as backup"
    // would be nonsense.
    std::string msg;
    if (!current_material.empty() && !backup_material.empty() &&
        !filament::materials_compatible(current_material, backup_material)) {
        msg = fmt::format(lv_tr("Incompatible materials: {} cannot use {} as backup"),
                          current_material, backup_material);
    } else {
        msg = lv_tr("That slot cannot stand in for this one");
    }
    ToastManager::instance().show(ToastSeverity::ERROR, msg.c_str());
}

} // namespace helix::ui::lane_settings
