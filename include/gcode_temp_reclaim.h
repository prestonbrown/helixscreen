// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file gcode_temp_reclaim.h
 * @brief The one rule for "is this G-code file ours to delete?"
 *
 * PrintSelectDetailView reclaims its downloaded G-code from seven places: the
 * destructor, view teardown, a size-mismatch re-download, a transfer error, an
 * oversize reject, and the adopt-the-new-copy swap. Each one used to call
 * std::remove() on whatever path it happened to hold, which was safe only
 * because every path in the view came from the same download and therefore lived
 * in our own cache directory.
 *
 * Reading a G-code in place from Moonraker's own `gcodes` root breaks that
 * premise: the same variable can now carry the user's print file. Guarding the
 * seven callers would be the same rule written seven times, so the rule lives
 * here instead, at the sink — a path that is not ours simply cannot be deleted,
 * whichever caller offers it.
 */

#include <string_view>

namespace helix::ui {

/**
 * @brief True when @p path is a file we downloaded into @p temp_dir.
 *
 * Positional, not a flag: what makes a file reclaimable is living inside our own
 * cache directory. A flag would have to be set correctly at every site that
 * assigns a path, which is the failure mode this exists to remove.
 *
 * Refuses when either argument is empty — an unset cache dir is not licence to
 * delete — and refuses any path containing a `..` component, which could start
 * inside @p temp_dir and still resolve to Moonraker's gcodes root.
 */
[[nodiscard]] inline bool is_reclaimable_download(std::string_view path,
                                                  std::string_view temp_dir) {
    if (path.empty() || temp_dir.empty()) {
        return false;
    }
    // Require a real child, not a sibling that merely shares a prefix
    // ("/cache/gcode_temp" must not claim "/cache/gcode_temp_old/x.gcode").
    std::string_view dir = temp_dir;
    while (dir.size() > 1 && dir.back() == '/') {
        dir.remove_suffix(1);
    }
    if (path.size() <= dir.size() + 1)
        return false;
    if (path.compare(0, dir.size(), dir) != 0)
        return false;
    if (path[dir.size()] != '/')
        return false;

    // Reject traversal anywhere in the remainder. Checked on segment
    // boundaries so a legitimate name like "..thumb.gcode" still passes.
    std::string_view rest = path.substr(dir.size());
    size_t i = 0;
    while (i < rest.size()) {
        if (rest[i] == '/') {
            const size_t start = i + 1;
            size_t end = rest.find('/', start);
            if (end == std::string_view::npos)
                end = rest.size();
            if (rest.substr(start, end - start) == "..")
                return false;
            i = end;
            continue;
        }
        ++i;
    }
    return true;
}

} // namespace helix::ui
