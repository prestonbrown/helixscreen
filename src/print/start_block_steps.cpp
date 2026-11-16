// SPDX-License-Identifier: GPL-3.0-or-later
#include "start_block_steps.h"

#include "core/text_io.h"
#include "gcode_ops_detector.h"
#include "operation_patterns.h"
#include "print_start_phase_label.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace helix {

namespace {

using gcode::GCodeOpsDetector;

/// G28 naming Z and no other axis touches the probe at one point; any other
/// G28, a bare one included, homes the axes.
StartBlockStepKind homing_kind(std::string_view line) {
    std::string_view params = line.substr(0, line.find(';'));
    bool z = false;
    bool xy = false;
    const auto tokens = text_io::split_ws(params);
    for (size_t i = 1; i < tokens.size(); ++i) {
        const char axis = static_cast<char>(std::toupper(static_cast<unsigned char>(tokens[i][0])));
        z |= axis == 'Z';
        xy |= axis == 'X' || axis == 'Y';
    }
    return z && !xy ? StartBlockStepKind::ProbeZ : StartBlockStepKind::HomeAxes;
}

/// Whether @p word, a line's upper-cased command word, is one of @p category's
/// keywords. The detector matches a macro keyword anywhere in the line, a
/// comment included; the step must be the command the line runs.
bool command_is(const std::string& word, OperationCategory category) {
    for (size_t i = 0; i < kOperationKeywordsCount; ++i) {
        const OperationKeyword& kw = kOperationKeywords[i];
        if (kw.category != category) {
            continue;
        }
        if (kw.exact_match ? word == kw.keyword : word.find(kw.keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A bed-mesh line that probes. Klipper's other BED_MESH_* commands load, clear
/// or offset a stored mesh: that runs no work on its line, so it is not a step.
/// The category's phase is shared; this filter is the step scan's own.
bool calibrates_mesh(const std::string& word) {
    return word == "BED_MESH_CALIBRATE" || word.rfind("BED_MESH_", 0) != 0;
}

std::optional<StartBlockStepKind> step_kind(const gcode::DetectedOperation& op) {
    const std::optional<PrintStartPhase> phase = phase_of_category(op.type);
    if (!phase) {
        return std::nullopt;
    }
    const std::string word = text_io::to_upper(GCodeOpsDetector::command_word(op.raw_line));
    if (!command_is(word, op.type)) {
        return std::nullopt;
    }
    switch (*phase) {
    case PrintStartPhase::HOMING:
        return word == "G28" ? homing_kind(op.raw_line) : StartBlockStepKind::HomeAxes;
    case PrintStartPhase::QGL:
        return StartBlockStepKind::LevelGantry;
    case PrintStartPhase::Z_TILT:
        return StartBlockStepKind::TiltZ;
    case PrintStartPhase::BED_MESH:
        return calibrates_mesh(word) ? std::optional(StartBlockStepKind::BedMesh) : std::nullopt;
    case PrintStartPhase::CLEANING:
        return StartBlockStepKind::CleanNozzle;
    case PrintStartPhase::PURGING:
        return StartBlockStepKind::Purge;
    case PrintStartPhase::SOAKING:
        // A chamber-soak category line can be SET_HEATER_TEMPERATURE, which
        // returns at once, so the position cannot follow its work; and the
        // collector's temperature detection owns SOAKING. Not a step: with a
        // start block, the soak row appears once that detection enters it.
    case PrintStartPhase::MOTOR_SYNC:
        // SYNC_MOTORS RESTORE_SYNC=1 shares the command word and returns at
        // once without syncing, so the line cannot place the work. The
        // console's "Motors synchronization started" enters the phase.
    default:
        return std::nullopt;
    }
}

/// Homing, leveling and probing return once the toolhead has finished; a
/// clean or purge is moves, which Klipper queues and returns from at once.
bool runs_on_its_line(StartBlockStepKind kind) {
    return kind != StartBlockStepKind::CleanNozzle && kind != StartBlockStepKind::Purge;
}

} // namespace

PrintStartPhase start_block_step_phase(StartBlockStepKind kind) {
    switch (kind) {
    case StartBlockStepKind::HomeAxes:
    case StartBlockStepKind::ProbeZ:
        return PrintStartPhase::HOMING;
    case StartBlockStepKind::LevelGantry:
        return PrintStartPhase::QGL;
    case StartBlockStepKind::TiltZ:
        return PrintStartPhase::Z_TILT;
    case StartBlockStepKind::BedMesh:
        return PrintStartPhase::BED_MESH;
    case StartBlockStepKind::CleanNozzle:
        return PrintStartPhase::CLEANING;
    case StartBlockStepKind::Purge:
        return PrintStartPhase::PURGING;
    }
    return PrintStartPhase::INITIALIZING;
}

std::vector<StartBlockStep> scan_start_block(std::string_view head, bool truncated) {
    if (truncated) {
        const size_t last_newline = head.rfind('\n');
        head = last_newline == std::string_view::npos ? std::string_view{}
                                                      : head.substr(0, last_newline + 1);
    }
    gcode::DetectionConfig config;
    config.max_scan_bytes = head.size() + 1;
    config.max_scan_lines = static_cast<int>(head.size()) + 1;
    config.stop_at_first_extrusion = false;
    config.stop_at_layer_marker = true;
    config.every_occurrence = true;
    const gcode::ScanResult scan = GCodeOpsDetector(config).scan_content(head);

    std::vector<StartBlockStep> steps;
    for (const auto& op : scan.operations) {
        if (const auto kind = step_kind(op)) {
            steps.push_back({op.byte_offset, *kind, runs_on_its_line(*kind),
                             text_io::to_upper(GCodeOpsDetector::command_word(op.raw_line))});
        }
    }
    if (scan.first_extrusion_offset) {
        steps.push_back({*scan.first_extrusion_offset, StartBlockStepKind::Purge, false, "G1"});
    }
    // One step per line: the first a line maps to.
    std::stable_sort(
        steps.begin(), steps.end(),
        [](const StartBlockStep& a, const StartBlockStep& b) { return a.offset < b.offset; });
    steps.erase(std::unique(steps.begin(), steps.end(),
                            [](const StartBlockStep& a, const StartBlockStep& b) {
                                return a.offset == b.offset;
                            }),
                steps.end());
    return steps;
}

void StartBlockFollower::set_steps(std::vector<StartBlockStep> steps) {
    steps_ = std::move(steps);
    next_ = 0;
}

std::optional<StartBlockStep> StartBlockFollower::advance(uint64_t position) {
    std::optional<StartBlockStep> reached;
    while (next_ < steps_.size() && steps_[next_].offset <= position) {
        const StartBlockStep& step = steps_[next_];
        if (!step.runs_on_its_line || step.offset == position) {
            reached = step;
        }
        ++next_;
    }
    return reached;
}

} // namespace helix
