// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file advanced_panel_types.h
 * @brief Umbrella header for advanced panel type definitions
 *
 * This header provides backward compatibility by including all the
 * domain-specific type headers. New code should include the specific
 * headers directly for better compile times and clearer dependencies.
 *
 * @see spoolman_types.h - Filament tracking types (SpoolInfo, FilamentUsageRecord)
 * @see calibration_types.h - Calibration types (ScrewTiltResult, InputShaperResult, MachineLimits)
 * @see macro_types.h - Macro types (MacroInfo, MacroCategory)
 */

// Domain-specific type headers
#include "calibration_types.h"
#include "macro_types.h"
#include "spoolman_types.h"

#include <functional>
#include <string>

// ============================================================================
// Generic Callback Type Aliases
// ============================================================================
// These callbacks are used across multiple domains and don't belong
// to any single feature area.

namespace helix {
/// Success callback (no data)
using AdvancedSuccessCallback = std::function<void()>;

/// Error callback with message
using AdvancedErrorCallback = std::function<void(const std::string& error)>;

/// Progress callback (0-100 percent)
using AdvancedProgressCallback = std::function<void(int percent)>;

/// Progress callback for resonance testing: percent plus the phase it belongs to.
/// Separate from AdvancedProgressCallback because SHAPER_CALIBRATE is the one
/// long operation whose UI text depends on a phase the percentage cannot encode.
using ShaperProgressCallback = std::function<void(int percent, ShaperCalibrationPhase phase)>;
} // namespace helix
