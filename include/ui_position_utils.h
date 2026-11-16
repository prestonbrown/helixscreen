// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

/**
 * @file ui_position_utils.h
 * @brief Position formatting utilities for UI display
 *
 * PrinterMotionState stores positions as centimillimeters (mm × 100) to
 * preserve 2 decimal places of precision using integer subjects.
 *
 * For unit conversions, use `unit_conversions.h`:
 * - `to_centimm()` - mm to centimm for storage
 * - `from_centimm()` - centimm to mm for calculations
 */

namespace helix {
namespace ui {
namespace position {

/**
 * @brief Format a position value with 2 decimal places and mm suffix
 *
 * Formats as "150.00 mm" with right-aligned width for consistent display.
 * Uses `from_centimm()` from unit_conversions.h internally.
 *
 * @param centimm Position in centimillimeters (e.g., 12750 for 127.50mm)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 *
 * @code{.cpp}
 * char buf[16];
 * lv_label_set_text(label, format_position(12750, buf, sizeof(buf)));
 * // Result: " 127.50 mm" (the number is padded to 7 characters)
 * @endcode
 */
char* format_position(int centimm, char* buffer, size_t buffer_size);

} // namespace position
} // namespace ui
} // namespace helix
