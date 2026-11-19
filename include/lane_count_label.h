// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "display_numbering.h"

#include <string>

namespace helix::ui {

/**
 * @brief How many positions a unit has: "4 slots", "8 lanes".
 *
 * One fixed translated form per noun, never a count spliced into a plural
 * word. Russian numerals take three forms (1 слот / 2-4 слота / 5+ слотов) and
 * a card header is not the place to decide between them, so each locale writes
 * the one form it wants to read there.
 *
 * @param count number of positions, shown verbatim
 */
std::string lane_count_label(LaneNoun noun, int count);

} // namespace helix::ui
