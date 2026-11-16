// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace helix {

/// Stable insertion sort for the short lists UI code orders: sensors, fans,
/// gates, vendors, menu rows. Every std::sort and std::stable_sort call
/// instantiates introsort, heap and merge code for its element type and
/// comparator, which the firmware image carries once per call site; this is
/// one short loop. Equal elements keep their input order, so it replaces
/// std::stable_sort exactly. Quadratic: keep it to lists of tens, not of
/// files or history jobs.
template <typename It, typename Less> void sort_small(It first, It last, Less less) {
    if (first == last) {
        return;
    }
    for (It i = std::next(first); i != last; ++i) {
        auto item = std::move(*i);
        It j = i;
        for (; j != first; --j) {
            It prev = std::prev(j);
            if (!less(item, *prev)) {
                break;
            }
            *j = std::move(*prev);
        }
        *j = std::move(item);
    }
}

template <typename It> void sort_small(It first, It last) {
    sort_small(first, last, std::less<>());
}

template <typename T, typename Less> void sort_small(std::vector<T>& items, Less less) {
    sort_small(items.begin(), items.end(), less);
}

template <typename T> void sort_small(std::vector<T>& items) {
    sort_small(items.begin(), items.end(), std::less<>());
}

} // namespace helix
