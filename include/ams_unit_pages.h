// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ams_unit_pages.h
 * @brief The page model behind the multi-unit AMS screen: one unit per page.
 *
 * Units that feed the same hub are consecutive pages, so paging inside a hub
 * keeps the hub, buffer and toolhead fixed. Pure data, no LVGL.
 */

#include "ams_types.h"

#include <functional>
#include <vector>

namespace helix::ui {

struct UnitPage {
    int unit_index = -1;        ///< index into AmsSystemInfo::units
    int group = 0;              ///< hub group, 0-based, in page order
    int same_hub_before = 0;    ///< units of this page's hub on earlier pages
    int same_hub_after = 0;     ///< units of this page's hub on later pages
    bool drying_before = false; ///< one of those earlier units is drying
    bool drying_after = false;  ///< one of those later units is drying
};

/// Pages in display order. Units naming the same non-empty AmsUnit::hub_id form one group
/// (consecutive pages); an empty hub_id is a group of its own. Groups are ordered by the
/// first unit of each in info.units; inside a group, info.units order. Absent units
/// (AmsUnit::absent) get no page. @p is_drying(unit_index) answers the dryer question.
std::vector<UnitPage> build_unit_pages(const AmsSystemInfo& info,
                                       const std::function<bool(int)>& is_drying);

/// Index of the page showing @p unit_index, or -1.
int page_of_unit(const std::vector<UnitPage>& pages, int unit_index);

/// The page to open on: the page of the unit holding info.path_active_slot(), else 0
/// (-1 when there are no pages).
int initial_unit_page(const std::vector<UnitPage>& pages, const AmsSystemInfo& info);

} // namespace helix::ui
