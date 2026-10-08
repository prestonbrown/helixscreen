// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// @file hardware_fingerprint.h
/// @brief Hardware-shape fingerprint for reconnect change detection.

#include "printer_discovery.h"

#include <cstddef>

namespace helix {

/// Compute a deterministic hash of the *hardware shape* of a PrinterDiscovery.
///
/// Captures the names + counts of hardware components (heaters, fans, sensors,
/// LEDs, steppers, AMS components, filament sensors, width sensors), the
/// capability flags (has_qgl, has_probe, mmu_type, etc.), the macro set, and
/// identity strings (hostname, MCU, kinematics).
///
/// Deliberately excludes volatile data: build_volume, mcu_versions, raw
/// printer_objects, software_version (which can bump on a firmware restart
/// without hardware change). Two discoveries that differ only in volatile
/// fields yield the same fingerprint.
///
/// Used by Application::on_discovery_complete to detect "reconnect with same
/// hardware" so user-facing side-effects (LED chip population, hardware
/// validation toasts, targeted reconfig wizard, telemetry snapshots) can
/// skip — see application.cpp for the gating rationale (issue #1117).
///
/// @return size_t hash. Two fingerprints are equal iff the relevant hardware
///         shape is identical. Ordering is hash-orderable, not meaningful.
size_t compute_hardware_fingerprint(const PrinterDiscovery& hw);

/// One printer session's discoveries, by fingerprint: the first counts as a change, and
/// after that only a different hardware shape does. reset() when the printer scope ends,
/// so the next printer's first discovery runs the full pipeline whatever its shape.
class HardwareChangeTracker {
  public:
    /// Records @p fingerprint; true when this discovery is a hardware change.
    bool note(size_t fingerprint);
    void reset();

  private:
    bool first_ = true;
    size_t last_ = 0;
};

} // namespace helix
