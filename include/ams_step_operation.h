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

/// Operation types for dynamic step progress.
///
/// Backend-specific step models (e.g. the Snapmaker U1's firmware Home / Select
/// / Heat / Move phases) are no longer separate operation types — each backend
/// supplies its own OperationStepModel and driving index subject for these base
/// operations via AmsBackend::get_operation_step_model /
/// get_operation_step_index_subject, so the UI renders generically.
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
 * @param is_active_action AmsBackend::action_tracks_step_operation(action) for the
 *        active backend. Passed in rather than recomputed here: this used to
 *        carry its own copy of the action list, the sidebar's visibility check
 *        carried another, and the two disagreed about SELECTING.
 * @return StepOperationResult with detection result
 */
inline StepOperationResult detect_step_operation(AmsAction action, AmsAction prev_action,
                                                 StepOperationType current_op, bool is_external,
                                                 bool filament_loaded, bool is_active_action) {
    StepOperationResult result;

    // External operation just started (transitioned from IDLE to any active action)
    if (is_external && is_active_action && prev_action == AmsAction::IDLE) {
        result.should_recreate = true;

        if (action == AmsAction::LOADING) {
            // Started directly with loading — fresh load
            result.op_type = StepOperationType::LOAD_FRESH;
        } else if (filament_loaded) {
            // Filament loaded + unload-like first action → swap
            result.op_type = StepOperationType::LOAD_SWAP;
        } else {
            // Nothing loaded — default to fresh load; will upgrade if needed
            result.op_type = StepOperationType::LOAD_FRESH;
        }
        return result;
    }

    // Explicit unload detection (not part of a swap where UNLOADING follows cutting/tip-forming)
    if (is_external && action == AmsAction::UNLOADING && prev_action != AmsAction::CUTTING &&
        prev_action != AmsAction::FORMING_TIP && current_op != StepOperationType::LOAD_SWAP) {
        result.should_recreate = true;
        result.op_type = StepOperationType::UNLOAD;
        return result;
    }

    // Mid-operation upgrade: what looked like a standalone unload is actually a swap.
    // Loading started after unloading — upgrade to LOAD_SWAP so remaining steps display.
    if (is_external && action == AmsAction::LOADING && current_op == StepOperationType::UNLOAD) {
        result.should_recreate = true;
        result.op_type = StepOperationType::LOAD_SWAP;
        result.jump_to_step = 2; // Skip heat + cut/tip (already done)
        return result;
    }

    return result; // No change needed
}
