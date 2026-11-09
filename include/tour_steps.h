// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace helix::tour {

enum class TooltipAnchor {
    Center, ///< No target — tooltip screen-centered
    PreferBelow,
    PreferAbove,
    PreferRight, ///< Typical for navbar buttons (tooltip to the right of left-side navbar)
    PreferLeft,
    /// Left of a wide target, with the card narrowed to fit the space there.
    LeftNarrowed,
};

/// Which screen a step's target lives on. The tour opens it before showing the step.
enum class TourPanel {
    Home,
    Filament,
    AmsOverlay, ///< The filament system's screen, opened over the Filament panel
};

struct TourStep {
    /// Target widget name for `lv_obj_find_by_name()`. Empty = centered, no highlight.
    std::string target_name;

    /// Translation keys for title and body (passed through `lv_tr()` at render time).
    std::string title_key;
    std::string body_key;

    TooltipAnchor anchor_hint = TooltipAnchor::PreferBelow;

    /// Additional widget names to sequentially spotlight inside this step (step 2 only).
    /// Each is highlighted for ~1.5s before the next. Empty = single target only.
    std::vector<std::string> sub_spotlights;

    TourPanel panel = TourPanel::Home;

    /// Tour version that introduced this step. An upgrader sees only newer steps.
    int since_version = 1;

    /// Shown only to someone who has already seen an earlier tour.
    bool upgrade_only = false;

    /// LVGL image path shown inside the card; empty for none.
    std::string preview_image;

    /// Tried when target_name is absent (a screen with two layouts names them differently).
    std::string fallback_target_name;
};

/// Build the tour step list. A `seen_version` of 0 is the full tour; anything else
/// is the upgrade tour holding only the steps newer than that version.
std::vector<TourStep> build_tour_steps(bool has_ams, int seen_version = 0);

/// The `seen_version` for an automatic start, from the persisted tour state. Someone
/// who has never finished a tour, or whose state predates versioning, gets the full tour.
int auto_start_seen_version(bool completed, int last_seen_version);

/// Convenience: queries `AmsState::instance().backend_count() > 0`.
bool hardware_has_ams();

} // namespace helix::tour
