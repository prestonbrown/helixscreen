// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_unit_pages.h"

#include <map>
#include <string>

namespace helix::ui {

std::vector<UnitPage> build_unit_pages(const AmsSystemInfo& info,
                                       const std::function<bool(int)>& is_drying) {
    // Members of each group, in info.units order; groups in first-member order.
    std::vector<std::vector<int>> groups;
    std::map<std::string, size_t> group_of_hub;
    for (size_t i = 0; i < info.units.size(); ++i) {
        const AmsUnit& unit = info.units[i];
        if (unit.absent)
            continue;
        if (unit.hub_id.empty()) {
            groups.push_back({static_cast<int>(i)});
            continue;
        }
        auto [it, inserted] = group_of_hub.try_emplace(unit.hub_id, groups.size());
        if (inserted)
            groups.emplace_back();
        groups[it->second].push_back(static_cast<int>(i));
    }

    std::vector<UnitPage> pages;
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& members = groups[g];
        std::vector<bool> drying(members.size(), false);
        for (size_t m = 0; m < members.size(); ++m)
            drying[m] = is_drying && is_drying(members[m]);

        for (size_t m = 0; m < members.size(); ++m) {
            UnitPage page;
            page.unit_index = members[m];
            page.group = static_cast<int>(g);
            page.same_hub_before = static_cast<int>(m);
            page.same_hub_after = static_cast<int>(members.size() - m - 1);
            for (size_t k = 0; k < m; ++k)
                page.drying_before = page.drying_before || drying[k];
            for (size_t k = m + 1; k < members.size(); ++k)
                page.drying_after = page.drying_after || drying[k];
            pages.push_back(page);
        }
    }
    return pages;
}

int page_of_unit(const std::vector<UnitPage>& pages, int unit_index) {
    for (size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].unit_index == unit_index)
            return static_cast<int>(i);
    }
    return -1;
}

int initial_unit_page(const std::vector<UnitPage>& pages, const AmsSystemInfo& info) {
    if (pages.empty())
        return -1;
    const int slot = info.path_active_slot();
    if (slot >= 0) {
        for (size_t i = 0; i < info.units.size(); ++i) {
            const AmsUnit& unit = info.units[i];
            if (!unit.absent && slot >= unit.first_slot_global_index &&
                slot < unit.first_slot_global_index + unit.slot_count) {
                const int page = page_of_unit(pages, static_cast<int>(i));
                if (page >= 0)
                    return page;
            }
        }
    }
    return 0;
}

} // namespace helix::ui
