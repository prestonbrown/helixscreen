// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_step_model.h
 * @brief Capability of a filament system that owns its operation step bar
 *
 * Reached with AmsBackend::as<IStepOperation>(). The Snapmaker U1, the AD5X IFS and the tool
 * changer implement it: each has a firmware phase source and publishes the phase index itself.
 * A backend without it gets its bar from its narration phases, or the sidebar's coarse fallback;
 * operation_step_model() and operation_step_index_subject() make that choice.
 */

#pragma once

#include "ams_step_operation.h"
#include "ams_types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Forward-declared so a backend can name the subject that drives its step index without
// ams_state.h, which would be a circular include.
typedef struct _lv_subject_t lv_subject_t;

namespace helix {

class AmsBackend;

/// One ordered step in a backend's operation step bar.
struct OperationStep {
    std::string label;      ///< display label (translatable)
    int phase_id = -1;      ///< backend phase index this step represents (-1 = positional)
    bool optional = false;  ///< stays greyed/Pending when never reached this op
    bool live_temp = false; ///< render a live "<label> cur/target°C" while current
    /// What this step looks like to consumers that read the coarse AmsAction:
    /// the kind of work (HEATING, CUTTING, PURGING), or for a step that only
    /// positions the machine, the operation's direction. The Snapmaker, AD5X
    /// IFS and tool changer backends publish it as the action while the step
    /// is current (AmsBackend::project_action()); the others still assign theirs.
    std::optional<AmsAction> coarse;
};

/// Ordered step labels for an operation. Empty => backend has no specialized
/// step model and the sidebar falls back to the coarse AmsAction model.
struct OperationStepModel {
    std::vector<OperationStep> steps;
    /// No steps AND nothing worth falling back to. Empty `steps` on its own
    /// means "I have no model, use the coarse bar"; `suppressed` means "I have
    /// no phases, and that coarse bar would describe a different kind of
    /// machine than this one." The sidebar renders no step bar at all for a
    /// suppressed model - a tool changer with no phase source has nothing to
    /// say, and Heat/Feed/Purge is not a truer answer than silence.
    bool suppressed = false;

    /// The coarse action the step at @p index projects to, the same positional
    /// index the step bar highlights. nullopt when no step sits there or it
    /// declares no projection.
    [[nodiscard]] std::optional<AmsAction> action_at(int index) const {
        if (index < 0 || index >= static_cast<int>(steps.size())) {
            return std::nullopt;
        }
        return steps[static_cast<size_t>(index)].coarse;
    }
};

class IStepOperation {
  public:
    /**
     * @brief Ordered step model for an operation type.
     *
     * @param op  Operation being performed
     * @return ordered steps, or empty for the coarse fallback
     */
    [[nodiscard]] virtual OperationStepModel
    get_operation_step_model(StepOperationType op) const = 0;

    /**
     * @brief Subject the sidebar observes to learn the CURRENT step index.
     *
     * The value is the step index (-1 = none/idle). nullptr => the sidebar derives the
     * index from the coarse AmsAction map.
     *
     * The returned subject MUST be a static/singleton subject (the sidebar
     * observes it with a member ObserverGuard and no SubjectLifetime token).
     *
     * @param op  Operation being performed
     * @return subject pointer, or nullptr for the coarse fallback
     */
    [[nodiscard]] virtual lv_subject_t* get_operation_step_index_subject(StepOperationType op) = 0;

    /**
     * @brief The operation the backend is running, when it knows which.
     *
     * A sidebar that did not start the operation (the AMS panel opened mid-run)
     * otherwise guesses it from the action frames, and a load that is still
     * heating reads the same as a swap. nullopt (default) => not known; the
     * sidebar guesses. Main thread.
     */
    [[nodiscard]] virtual std::optional<StepOperationType> running_step_operation() const {
        return std::nullopt;
    }

    /**
     * @brief The heater a live-temp step of the current step model heats.
     *
     * A machine with a heater per tool heats a different one at each tool's
     * step, and Klipper's active extruder need not be it. Empty (default) =>
     * the active extruder. Main thread.
     *
     * @param step_index  Index into the model get_operation_step_model() built
     */
    [[nodiscard]] virtual std::string step_heater(int /*step_index*/) const {
        return {};
    }

    /**
     * @brief Does `action` mean an operation the STEP BAR should be following?
     *
     * Drives both halves of the step bar from one definition: whether the bar is
     * visible, and whether an IDLE -> operation transition counts as an
     * operation STARTING.
     *
     * Default is ams_action_is_filament_operation() - heat, feed, purge, cut,
     * form tip, retract. A backend whose operations are reported with a
     * different action set overrides this, rather than the sidebar growing a
     * branch per backend family.
     *
     * Broader than "is anything happening": that is ams_action_is_busy(), which
     * the slot pulse uses and which no backend needs to override.
     */
    [[nodiscard]] virtual bool action_tracks_step_operation(AmsAction action) const {
        return ams_action_is_filament_operation(action);
    }

  protected:
    ~IStepOperation() = default;
};

/**
 * @brief The step model the bar shows for @p op on @p backend
 *
 * IStepOperation's answer where the backend has it, else the steps of its narration template
 * (AmsBackend::toolchange_phase_template()): empty when it has none, which is the coarse fallback.
 */
[[nodiscard]] OperationStepModel operation_step_model(const AmsBackend& backend,
                                                      StepOperationType op);

/**
 * @brief The subject that drives the bar's current step for @p op on @p backend
 *
 * IStepOperation's answer where the backend has it. Else AmsState's toolchange step subject, which
 * the GcodeNarrationRouter writes, when the backend has a narration template for @p op, and nullptr
 * without one.
 */
[[nodiscard]] lv_subject_t* operation_step_index_subject(AmsBackend& backend, StepOperationType op);

} // namespace helix
