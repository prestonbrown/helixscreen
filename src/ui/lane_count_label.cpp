// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lane_count_label.h"

#include "i18n_plural.h"

#include <cstdint>
#include <cstdio>

namespace helix::ui {

std::string lane_count_label(LaneNoun noun, int count) {
    // Wide enough for the longest form: ru "Печатающие головки" is 35 bytes.
    // Each tr_plural() sits inside its snprintf so the format-specifier gate
    // sees the pair - a format held in a variable between the two is invisible
    // to it. No default: a new enumerator must fail to compile here, not pass a
    // null format to snprintf.
    char buf[64];
    const auto n = static_cast<uint64_t>(count);
    switch (noun) {
    case LaneNoun::Lane:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d lanes", n), count);
        return buf;
    case LaneNoun::Gate:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d gates", n), count);
        return buf;
    case LaneNoun::Tool:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d tools", n), count);
        return buf;
    case LaneNoun::Feeder:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d feeders", n), count);
        return buf;
    case LaneNoun::Toolhead:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d toolheads", n), count);
        return buf;
    case LaneNoun::Slot:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d slots", n), count);
        return buf;
    }
    return {};
}

} // namespace helix::ui
