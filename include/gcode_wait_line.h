// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace helix {

/// The blocking temperature wait a gcode line runs. Each ends differently:
/// M109/M190/M191 return on a target of 0 or a print cancel, while
/// TEMPERATURE_WAIT ends only at its bound or at a shutdown.
enum class WaitKind { Other, M109, M190, M191, TemperatureWait };

struct WaitLine {
    WaitKind kind = WaitKind::Other;
    /// M109's T, or the extruder a TEMPERATURE_WAIT names (extruder = 0,
    /// extruderN = N). nullopt for M109 with no T: the active tool.
    std::optional<int> tool;
    /// M109/M190/M191's S, the only parameter Klipper waits on; nullopt = no
    /// S, which Klipper reads as 0 and returns at once.
    std::optional<double> target;
    std::string sensor;            ///< TEMPERATURE_WAIT's SENSOR, lower-cased, unquoted; "" = none
    std::optional<double> minimum; ///< TEMPERATURE_WAIT's MINIMUM
    std::optional<double> maximum; ///< TEMPERATURE_WAIT's MAXIMUM
};

/// Classify one gcode line. A comment (`;` onward), leading whitespace and
/// lower-case commands are allowed; anything that is not one of the waits,
/// such as a macro whose body holds the wait, is Other.
WaitLine classify_wait_line(std::string_view line);

/// A heater target a gcode line sets, waiting or not.
struct HeaterTargetLine {
    /// The heater, lower-cased: "extruder" (M104/M109, extruderN with T),
    /// "heater_bed" (M140/M190), or SET_HEATER_TEMPERATURE's HEATER; "" for a
    /// line that sets no heater.
    std::string heater;
    std::optional<double> target; ///< S or TARGET; nullopt when the line names none
};

/// The heater target @p line sets: M104, M109, M140, M190 and
/// SET_HEATER_TEMPERATURE, read the way classify_wait_line() reads its waits.
HeaterTargetLine heater_target_line(std::string_view line);

/// How far under its target a heat wait returns. A heater under PID can settle a
/// fraction of a degree under its target and never read the target itself, so
/// a wait for exactly the target can hold the gcode queue for good.
inline constexpr double kHeatWaitMarginC = 2.0;

/// The TEMPERATURE_WAIT that returns once @p sensor reads within
/// kHeatWaitMarginC of @p target_c: "TEMPERATURE_WAIT SENSOR=extruder MINIMUM=148".
std::string heat_wait_line(std::string_view sensor, double target_c);

/// Bytes read from virtual_sdcard.file_position to find the line there.
inline constexpr size_t kWaitLineWindow = 256;

/// The line that starts at @p bytes[0], where @p bytes is a read of
/// @p requested bytes at virtual_sdcard.file_position. Klipper advances
/// file_position past a line only once its command returns, so during a
/// blocking wait the position is the START of the waiting line. Fewer bytes
/// than requested means the file ended, and its last line needs no newline.
/// nullopt when there is no line, or when the window filled before the
/// line's newline and before any comment: a command longer than the window.
std::optional<std::string_view> first_line_at_position(std::string_view bytes, size_t requested);

} // namespace helix
