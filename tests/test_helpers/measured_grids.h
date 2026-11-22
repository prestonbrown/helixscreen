// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

namespace helix::test {

/// One measured geometry: a resolution, the home grid container's content box
/// at that resolution, the gutter (home_grid_gap at that breakpoint), and the grid
/// the sizing model produces, in tracks.
///
/// content_w/content_h/gutter were read off a live instance per resolution:
/// `HELIX_SCREEN_SIZE=<WxH> helix-screen --test -vv`, then the
/// `[PanelWidgetManager] Grid layout:` and `Track geometry:` lines. The content
/// box is a property of the panel chrome and does not change with the track
/// count, so it is stable data to pin against.
struct MeasuredGrid {
    const char* name;
    int panel_w, panel_h;
    int content_w, content_h;
    int gutter;
    int cols, rows;
};

// clang-format off
inline const std::vector<MeasuredGrid> kMeasuredGrids = {
    //  name                        panel        content     gut   grid
    {"micro 480x272",            480,  272,   430,  264,   2,   12,  8},
    {"micro portrait 272x480",   272,  480,   264,  394,   2,    8, 12},
    {"tiny 480x320",             480,  320,   418,  312,   2,   10,  8},
    {"tiny portrait 320x480",    320,  480,   312,  394,   2,    8, 10},
    {"small 480x400",            480,  400,   414,  388,   4,   10, 10},
    {"small portrait 400x480",   400,  480,   388,  390,   4,   10, 10},
    {"medium 800x480",           800,  480,   710,  466,   5,   12,  8},
    {"portrait 480x800",         480,  800,   466,  664,   5,    8, 12},
    {"large 1024x600",          1024,  600,   904,  584,  10,   16, 10},
    {"large portrait 600x1024",  600, 1024,   584,  868,  10,   10, 14},
    {"xlarge 1280x720",         1280,  720,  1128,  700,  10,   16, 10},
    {"xlarge portrait 720x1280", 720, 1280,   700, 1120,  10,   10, 16},
    {"ultrawide 1920x440",      1920,  440,  1832,  428,   4,   46, 10},
    {"ultratall 440x1920",       440, 1920,   428, 1768,   4,   10, 44},
    {"ultratall 320x1480",       320, 1480,   312, 1332,   2,    8, 34},
    {"ultrawide 1480x320",      1480,  320,  1418,  312,   2,   36,  8},
    {"xxlarge 1920x1080",        1920, 1080,  1720, 1056,  12,   24, 14},
    {"micro 320x240",            320,  240,   270,  232,   2,    8,  6},
    {"xxlarge portrait 1080x1920", 1080, 1920, 1056, 1756, 12,   14, 24},
    {"xxlarge portrait 1080x2400", 1080, 2400, 1056, 2236, 12,   14, 32},
    {"ultrawide 1920x480",      1920,  480,  1830,  466,   5,   30,  8},
    {"ultrawide 2560x720",      2560,  720,  2464,  700,  10,   34, 10},
};
// clang-format on

} // namespace helix::test
