// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_print_area_intrusion.h
 * @brief Capability of a filament system whose mounted hardware takes part of the bed
 *
 * Reached with AmsBackend::as<IPrintAreaIntrusion>(). CFS implements it for the
 * K1 upgrade's rear-mounted cassette. Without it the system takes no bed area.
 */

#pragma once

namespace helix {

class IPrintAreaIntrusion {
  public:
    /// Bed Y the mounted hardware makes unprintable, in mm. A rear-mounted
    /// cassette can sit over the far edge of the bed; the print-start fit check
    /// subtracts this from the machine's Y limit.
    [[nodiscard]] virtual double printable_y_shrink_mm() const = 0;

  protected:
    ~IPrintAreaIntrusion() = default;
};

} // namespace helix
