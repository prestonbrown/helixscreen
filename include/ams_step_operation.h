// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

/**
 * @file ams_step_operation.h
 * @brief Step progress operation type detection for AMS load/unload/swap operations
 *
 * Pure logic for determining which step progress to display, extracted
 * from AmsPanel for testability. No LVGL or UI dependencies.
 */

namespace helix {

/// Operation types for dynamic step progress.
///
/// Backend-specific step models (e.g. the Snapmaker U1's firmware Home / Select
/// / Heat / Move phases) are not separate operation types: each backend
/// supplies its own OperationStepModel and driving index subject for these base
/// operations through IStepOperation (ams_step_model.h), so the UI renders generically.
enum class StepOperationType {
    LOAD_FRESH, ///< Loading into empty toolhead
    LOAD_SWAP,  ///< Swap: unload current + load new
    UNLOAD,     ///< Explicit unload operation
};

/**
 * @brief Result of step operation detection
 */
struct StepOperationResult {
    bool should_recreate = false; ///< True if step progress should be (re)created
    StepOperationType op_type = StepOperationType::LOAD_FRESH;
    int jump_to_step = -1; ///< If >= 0, jump stepper to this step after creation
};

/**
 * @brief Detect which step operation type to show based on action transitions
 *
 * Handles both the initial detection (when an external operation starts) and
 * mid-operation upgrades (e.g., UNLOAD → LOAD_SWAP when loading starts
 * after an unload).
 *
 * @param action          Current AMS action
 * @param prev_action     Previous AMS action
 * @param current_op      Current operation type being displayed
 * @param is_external     True if this is an externally-initiated operation (not from our UI)
 * @param filament_loaded True if filament is currently loaded in the toolhead
 * @param is_active_action IStepOperation::action_tracks_step_operation(action) for the
 *        active backend, the same answer the sidebar's visibility check reads.
 * @param unloads_stand_alone AmsBackend::unloads_stand_alone() for the active
 *        backend: an unload is never the first half of a swap, so UNLOADING
 *        means UNLOAD and a LOADING after it is a fresh load of its own.
 * @return StepOperationResult with detection result
 */
inline StepOperationResult detect_step_operation(AmsAction action, AmsAction prev_action,
                                                 StepOperationType current_op, bool is_external,
                                                 bool filament_loaded, bool is_active_action,
                                                 bool unloads_stand_alone) {
    StepOperationResult result;

    // External operation just started (transitioned from IDLE to any active action)
    if (is_external && is_active_action && prev_action == AmsAction::IDLE) {
        result.should_recreate = true;

        if (action == AmsAction::LOADING) {
            // Started directly with loading — fresh load
            result.op_type = StepOperationType::LOAD_FRESH;
        } else if (unloads_stand_alone && action == AmsAction::UNLOADING) {
            result.op_type = StepOperationType::UNLOAD;
        } else if (filament_loaded && !unloads_stand_alone) {
            // Filament loaded + unload-like first action → swap
            result.op_type = StepOperationType::LOAD_SWAP;
        } else {
            // Nothing loaded — default to fresh load; will upgrade if needed
            result.op_type = StepOperationType::LOAD_FRESH;
        }
        return result;
    }

    // Explicit unload detection (not part of a swap where UNLOADING follows cutting/tip-forming).
    // An unload already on screen keeps its bar when it returns to UNLOADING after a heat step.
    if (is_external && action == AmsAction::UNLOADING && prev_action != AmsAction::CUTTING &&
        prev_action != AmsAction::FORMING_TIP && current_op != StepOperationType::LOAD_SWAP &&
        current_op != StepOperationType::UNLOAD) {
        result.should_recreate = true;
        result.op_type = StepOperationType::UNLOAD;
        return result;
    }

    // Mid-operation upgrade: what looked like a standalone unload is actually a swap.
    // Loading started after unloading — upgrade to LOAD_SWAP so remaining steps display.
    // Where unloads stand alone, that load is a new operation with its own steps.
    if (is_external && action == AmsAction::LOADING && current_op == StepOperationType::UNLOAD) {
        result.should_recreate = true;
        if (unloads_stand_alone) {
            result.op_type = StepOperationType::LOAD_FRESH;
            return result;
        }
        result.op_type = StepOperationType::LOAD_SWAP;
        result.jump_to_step = 2; // Skip heat + cut/tip (already done)
        return result;
    }

    return result; // No change needed
}

} // namespace helix
