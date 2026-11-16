// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_motion_api.h
 * @brief Motion control operations via Moonraker
 *
 * A MoonrakerAPI sub-API that encapsulates all motion control functionality
 * (homing, relative moves, absolute positioning) in a dedicated class.
 * Uses MoonrakerClient for JSON-RPC transport.
 */

#pragma once

#include "helix_shared_callback.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <cstdint>
#include <functional>
#include <string>

// Forward declarations
namespace helix {
class IMoonrakerClient;
class PrinterState;
} // namespace helix

/**
 * @brief Motion Control API operations via Moonraker
 *
 * Provides high-level operations for homing axes, relative movement, and
 * absolute positioning through G-code commands sent via Moonraker's
 * printer.gcode.script endpoint.
 *
 * All methods include safety validation (axis validity, distance/position
 * bounds, feedrate limits) before generating G-code.
 *
 * Usage:
 *   MoonrakerMotionAPI motion(client, safety_limits);
 *   motion.home_axes("XY",
 *       []() { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerMotionAPI : public helix::IMotionAPI {
  public:
    using SuccessCallback = helix::SharedCallback<void()>;
    using ErrorCallback = helix::SharedCallback<void(const MoonrakerError&)>;

    /// Homing timeout: 5 minutes for G28 on large printers
    static constexpr uint32_t kHomingTimeoutMs = 300000;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param state PrinterState instance (must remain valid during API lifetime);
     *              consulted by the homing guard to refuse G28 while printing
     * @param safety_limits Reference to safety limits (must remain valid during API lifetime)
     */
    MoonrakerMotionAPI(helix::IMoonrakerClient& client, helix::PrinterState& state,
                       const SafetyLimits& safety_limits);
    virtual ~MoonrakerMotionAPI() = default;

    // ========================================================================
    // Motion Control Operations
    // ========================================================================

    /**
     * @brief Home one or more axes
     *
     * @param axes Axes to home (e.g., "XY", "Z", "XYZ", empty for all)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void home_axes(const std::string& axes, SuccessCallback on_success,
                   ErrorCallback on_error) override;

    /**
     * @brief Move an axis by a relative amount
     *
     * @param axis Axis name ('X', 'Y', 'Z', 'E')
     * @param distance Distance to move in mm
     * @param feedrate Movement speed in mm/min (0 for default)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void move_axis(char axis, double distance, double feedrate, SuccessCallback on_success,
                   ErrorCallback on_error) override;

    /**
     * @brief Relative multi-axis move as ONE gcode script
     *
     * XY combined on a single G0 (true diagonal) at xy_feedrate, Z on its own
     * G0 at z_feedrate. Used by the jog coalescer flush path. Zero deltas ->
     * on_success immediately, no RPC.
     *
     * @param dx X delta in mm (0 to skip)
     * @param dy Y delta in mm (0 to skip)
     * @param dz Z delta in mm (0 to skip)
     * @param xy_feedrate Movement speed for the XY move in mm/min (0 for default)
     * @param z_feedrate Movement speed for the Z move in mm/min (0 for default)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void move_relative(double dx, double dy, double dz, double xy_feedrate, double z_feedrate,
                       SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Generate G-code for a relative multi-axis move (public for direct
     * unit testing, mirrors generate_move_gcode)
     */
    static std::string generate_relative_move_gcode(double dx, double dy, double dz,
                                                    double xy_feedrate, double z_feedrate);

    /**
     * @brief Generate G-code for an absolute multi-axis move: Z first, then XY
     * combined, unless the target descends below a known current_z (public for
     * direct unit testing)
     */
    static std::string generate_absolute_move_gcode(const helix::AxisTarget& target,
                                                    double xy_feedrate, double z_feedrate,
                                                    std::optional<double> current_z = std::nullopt);

    /**
     * @brief Generate G-code for a single-axis absolute move (public for
     * direct unit testing; E is valid here and not in the multi-axis form)
     */
    static std::string generate_absolute_move_gcode(char axis, double position, double feedrate);

    /**
     * @brief Set absolute position for an axis
     *
     * @param axis Axis name ('X', 'Y', 'Z', 'E')
     * @param position Absolute position in mm
     * @param feedrate Movement speed in mm/min (0 for default)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void move_to_position(char axis, double position, double feedrate, SuccessCallback on_success,
                          ErrorCallback on_error) override;

    /**
     * @brief Absolute multi-axis move as ONE gcode script
     *
     * X and Y combine on a single G0 at xy_feedrate; Z gets its own G0 at
     * z_feedrate. Z moves FIRST (a lift clears the bed before travel) unless
     * the target descends below a known current_z, in which case XY travels
     * first and Z descends last. Unset axes are not commanded. An empty
     * target -> on_success immediately, no RPC.
     *
     * @param target Axes to move and their absolute positions in mm
     * @param xy_feedrate Movement speed for the XY move in mm/min (0 for default)
     * @param z_feedrate Movement speed for the Z move in mm/min (0 for default)
     * @param on_success Success callback
     * @param on_error Error callback
     * @param current_z Z the toolhead sits at when this script starts; nullopt
     * when unknown (Z then always moves first)
     */
    void move_to(const helix::AxisTarget& target, double xy_feedrate, double z_feedrate,
                 SuccessCallback on_success, ErrorCallback on_error,
                 std::optional<double> current_z = std::nullopt) override;

  protected:
    helix::IMoonrakerClient& client_;
    helix::PrinterState& state_;
    const SafetyLimits& safety_limits_;

    /**
     * @brief Generate G-code for homing axes
     */
    std::string generate_home_gcode(const std::string& axes);

    /**
     * @brief Generate G-code for relative movement
     */
    std::string generate_move_gcode(char axis, double distance, double feedrate);

    /**
     * @brief Execute G-code via printer.gcode.script JSON-RPC
     *
     * Annotates G-code with source comment and sends via client.
     *
     * @param caller_surfaces_errors Whether @p on_error actually SHOWS the user
     *        something. Pass false when it only logs: a spdlog line is not a
     *        user-visible report, and letting it claim ownership suppresses
     *        Klipper's `!!` broadcast for the same failure. See
     *        include/rpc_error_policy.h.
     */
    void execute_gcode(const std::string& gcode, SuccessCallback on_success, ErrorCallback on_error,
                       uint32_t timeout_ms = 0, bool caller_surfaces_errors = true);
};
