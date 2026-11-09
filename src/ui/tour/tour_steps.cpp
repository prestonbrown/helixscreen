// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_steps.h"

#include "ams_state.h"

#include <algorithm>
#include <utility>

namespace helix::tour {

bool hardware_has_ams() {
    return AmsState::instance().is_available();
}

// i18n: the literals below are translation keys (the English UI text),
// resolved indirectly via lv_tr(step.title_key.c_str()) in tour_overlay.cpp.
// They are live keys even though no lv_tr("...") call wraps them here —
// `translation_sync obsolete` finds them through its reference scan, not the
// extractor patterns. Keys must be their own English text — English registers
// no translation pack (translation_loader.cpp), so a semantic key like
// "Welcome to HelixScreen" renders RAW in the English UI. Renaming one here
// means renaming it in translations/*.yml too.
std::vector<TourStep> build_tour_steps(bool has_ams, int seen_version) {
    std::vector<TourStep> steps;
    steps.reserve(11);

    // Welcome (centered card, no target).
    steps.push_back({"",
                     "Welcome to HelixScreen",
                     "Let's take a quick tour so you can find things fast.",
                     TooltipAnchor::Center,
                     {}});

    TourStep whats_new{"",
                       "New in 1.1",
                       "Here's a quick look at what changed since your last version.",
                       TooltipAnchor::Center,
                       {}};
    whats_new.since_version = 2;
    whats_new.upgrade_only = true;
    steps.push_back(std::move(whats_new));

    // Home widget example: widget root objects are named by their factory widget_id
    // (see panel_widget_manager.cpp). AMS is the most visually distinctive tile.
    steps.push_back({has_ams ? "ams" : "nozzle_temps",
                     "Your printer at a glance",
                     "Tap any tile to open its full controls or toggle its state.",
                     TooltipAnchor::PreferBelow,
                     {}});

    TourStep customize{"fan_stack",
                       "Customize your home screen",
                       "Long-press any tile to enter edit mode. The rebuilt grid lets you "
                       "rearrange, resize, remove, or add widgets, and drag them between pages.",
                       TooltipAnchor::PreferBelow,
                       {}};
    customize.since_version = 2;
    steps.push_back(std::move(customize));

    // Print Status only has content during a print, so the card carries a picture of it.
    TourStep print_status{"",
                          "A new Print Status screen",
                          "During a print, see a chip for each heater, fan, and light, a checklist "
                          "of the start sequence, and progress with controls in one strip.",
                          TooltipAnchor::Center,
                          {}};
    print_status.since_version = 2;
    print_status.preview_image = "A:assets/images/tour/print_status_preview.png";
    steps.push_back(std::move(print_status));

    steps.push_back({"nav_btn_controls",
                     "Controls",
                     "Move the toolhead, home axes, level the bed, and fine-tune temperatures, "
                     "fans, and other motion settings.",
                     TooltipAnchor::PreferRight,
                     {}});
    steps.push_back({"nav_btn_filament",
                     "Filament",
                     "Load, unload, and swap spools, set temperatures, and monitor runout sensors.",
                     TooltipAnchor::PreferRight,
                     {}});

    TourStep filament_panel{"filament_operations_group",
                            "Load and unload",
                            "Load, unload, and purge from here. Choose a material to set its "
                            "temperatures first.",
                            TooltipAnchor::LeftNarrowed,
                            {}};
    filament_panel.panel = TourPanel::Filament;
    filament_panel.since_version = 2;
    steps.push_back(std::move(filament_panel));

    if (has_ams) {
        TourStep ams{"ams_unit_card",
                     "Your filament system",
                     "Tap a slot to load or unload it, pick a spool, or edit its details. Your "
                     "edits stay in step with the printer and Spoolman.",
                     TooltipAnchor::PreferBelow,
                     {}};
        ams.panel = TourPanel::AmsOverlay;
        ams.since_version = 2;
        ams.fallback_target_name = "unit_cards_row";
        steps.push_back(std::move(ams));
    }

    steps.push_back({"nav_btn_advanced",
                     "Advanced",
                     "Macros, G-code console, calibration tools (PID, input shaper, z-offset), "
                     "firmware updates, and developer utilities.",
                     TooltipAnchor::PreferRight,
                     {}});
    steps.push_back({"nav_btn_settings",
                     "Settings",
                     "Network, display, sound, printer setup, and more. You can replay this tour "
                     "anytime from Help & About.",
                     TooltipAnchor::PreferRight,
                     {}});

    const bool upgrading = seen_version > 0;
    steps.erase(std::remove_if(steps.begin(), steps.end(),
                               [&](const TourStep& s) {
                                   return upgrading ? s.since_version <= seen_version
                                                    : s.upgrade_only;
                               }),
                steps.end());
    return steps;
}

int auto_start_seen_version(bool completed, int last_seen_version) {
    return completed ? last_seen_version : 0;
}

} // namespace helix::tour
