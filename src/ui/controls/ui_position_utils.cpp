// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_position_utils.h"

#include "unit_conversions.h"

#include <cstdio>

namespace helix {
namespace ui {
namespace position {

char* format_position(int centimm, char* buffer, size_t buffer_size) {
    double mm = helix::units::from_centimm(centimm);
    snprintf(buffer, buffer_size, "%7.2f mm", mm);
    return buffer;
}

} // namespace position
} // namespace ui
} // namespace helix
