// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_steps.h"

#include "ams_state.h"

namespace helix::tour {

bool hardware_has_ams() {
    return AmsState::instance().backend_count() > 0;
}

// i18n: the literals below are translation keys (the English UI text),
// resolved indirectly via lv_tr(step.title_key.c_str()) in tour_overlay.cpp.
// They are live keys even though no lv_tr("...") call wraps them here —
// `translation_sync obsolete` finds them through its reference scan, not the
// extractor patterns. Keys must be their own English text — English registers
// no translation pack (translation_loader.cpp), so a semantic key like
// "Welcome to HelixScreen" renders RAW in the English UI. Renaming one here
// means renaming it in translations/*.yml too.
std::vector<TourStep> build_tour_steps(bool has_ams) {
    std::vector<TourStep> steps;
    steps.reserve(8);

    // 1. Welcome (centered card, no target)
    steps.push_back({"",
                     "Welcome to HelixScreen",
                     "Let's take a quick tour so you can find things fast.",
                     TooltipAnchor::Center,
                     {}});

    // 2. Home widget example — highlight a concrete tile. Widget root objects
    //    are named by their factory widget_id (see panel_widget_manager.cpp).
    //    Prefer AMS if the printer has one (most visually distinctive).
    steps.push_back({has_ams ? "ams" : "nozzle_temps",
                     "Your printer at a glance",
                     "Tap any tile to open its full controls or toggle its state.",
                     TooltipAnchor::PreferBelow,
                     {}});

    // 3. Long-press to customize — highlight a different tile so the user sees
    //    the edit-mode message paired with a concrete example they can try.
    steps.push_back({"fan_stack",
                     "Customize your home screen",
                     "Long-press any tile to enter edit mode — rearrange, resize, remove, or add "
                     "widgets, and add extra pages for your preferred layout.",
                     TooltipAnchor::PreferBelow,
                     {}});

    // 4. Print status widget on home (navbar Print button lacks a highlightable
    //    name, and the print-status tile on home is more informative anyway).
    steps.push_back({"print_status",
                     "Print status",
                     "Monitor prints in progress — time remaining, current layer, and progress. "
                     "Tap to pause, resume, or cancel the active job.",
                     TooltipAnchor::PreferBelow,
                     {}});

    // 5-8. Navbar tour
    steps.push_back({"nav_btn_controls",
                     "Controls",
                     "Move the toolhead, home axes, level the bed, and fine-tune temperatures, "
                     "fans, and other motion settings.",
                     TooltipAnchor::PreferRight,
                     {}});
    steps.push_back({"nav_btn_filament",
                     "Filament",
                     "Load, unload, and swap spools. View your multi-filament system at a glance, "
                     "map colors to slots, and monitor runout sensors.",
                     TooltipAnchor::PreferRight,
                     {}});
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

    return steps;
}

} // namespace helix::tour
