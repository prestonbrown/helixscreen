// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <optional>
#include <string>

namespace helix {
class PrinterDiscovery;
} // namespace helix

/**
 * @file pause_descriptor.h
 * @brief Firmwares that report WHY a print paused as a structured descriptor.
 *
 * This module reads the descriptor; pause_cause.h classifies a pause from it.
 *
 * Mainline Klipper only says a print paused; print_stats.message may carry
 * prose. Some firmware also publishes a structured cause (a fault id, a code
 * and the reason text) beside print_stats, which tells, for example, a
 * defect-detection pause from a filament runout without matching a sentence.
 *
 * This is the one module that knows which firmwares publish one and under
 * which key. PrinterPrintState asks it to read a frame and never names a
 * firmware. Adding another is a row in the provider table.
 */
namespace helix::pause_descriptor {

/// One pause descriptor. -1 / empty mean "none".
struct PauseDescriptor {
    int id = -1;
    int code = -1;
    /// Firmware error envelope already decoded to its text.
    std::string message;
};

/// Status fields the discovered firmware's pause descriptor needs, as a
/// printer.objects.subscribe objects map; an empty object when it has none.
[[nodiscard]] nlohmann::json required_status_fields(const PrinterDiscovery& hw);

/// True when this print_stats frame carries a non-empty pause cause.
[[nodiscard]] bool frame_carries_cause(const nlohmann::json& print_stats);

/// The cause this print_stats frame reports. nullopt when the frame is silent
/// about it (key absent, or Moonraker's null for a field this firmware never
/// populates): leave whatever is latched. An all -1 PauseDescriptor when the
/// firmware reports no cause (an empty descriptor: a user pause, or the cause
/// cleared).
[[nodiscard]] std::optional<PauseDescriptor> read(const nlohmann::json& print_stats);

} // namespace helix::pause_descriptor
