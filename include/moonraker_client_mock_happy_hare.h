// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file moonraker_client_mock_happy_hare.h
 * @brief Happy Hare 4 on one ERCF-style unit, as the objects its Klipper plugin publishes.
 *
 * The state `printer.mmu` and `mmu_machine` report, and the MMU_* / T<n> commands that
 * change it, so --test runs the production AmsBackendHappyHare. Pure: no locks, no
 * client, no timers. MoonrakerClientMock owns one behind a mutex, plays an operation's
 * steps on a timer and dispatches status() after each.
 */
namespace helix::mock {

/// One gate's spool, as MMU_GATE_MAP holds it.
struct HappyHareGate {
    int status = 1; ///< -1 unknown, 0 empty, 1 available, 2 from buffer
    uint32_t rgb = 0;
    std::string material;
    std::string vendor;
    std::string name; ///< gate_name: the filament's name
    int spool_id = -1;
    int temperature = 0;
};

/// One frame of an operation in progress: what printer.mmu reads while it runs.
struct HappyHareStep {
    std::string action;
    int filament_pos = 0;
    int gate = -1;
    int tool = -1;
    int emptied_gate = -1; ///< MMU_EJECT: the gate that reads empty from this step on
};

/// What a command did.
struct HappyHareReply {
    bool handled = false;             ///< false: not a Happy Hare command, the line goes on
    std::string error;                ///< non-empty: refused, with Happy Hare's message
    std::vector<HappyHareStep> steps; ///< a move to play, one frame per step
};

class HappyHareSim {
  public:
    /// filament_pos values from Happy Hare's mmu.py.
    static constexpr int kPosUnloaded = 0;
    static constexpr int kPosLoaded = 10;
    static constexpr int kGateBypass = -2;

    /// @p gates gates (clamped to 1..16) filled from the mock Spoolman's spools 1-8 in
    /// turn, gate 0 loaded in the toolhead and gate 3 empty.
    /// @p environment_sensor the unit's environment_sensor object, "" for none.
    HappyHareSim(int gates, std::string environment_sensor);

    /// The whole printer.mmu object.
    [[nodiscard]] nlohmann::json status() const;
    /// The live mmu_machine object (happy_hare_version and unit_0).
    [[nodiscard]] nlohmann::json machine_status() const;
    /// The configfile.settings sections a v4 install declares.
    [[nodiscard]] nlohmann::json config_sections() const;

    /// Handle one G-code line. A move comes back as steps to play; anything else has
    /// already changed the state.
    HappyHareReply command(const std::string& gcode);
    /// Apply one step of a move. The last step of a move clears busy().
    void apply(const HappyHareStep& step, bool last);
    /// A move is playing: Happy Hare refuses another until it ends.
    [[nodiscard]] bool busy() const {
        return busy_;
    }
    /// The print's starting tool, selected and loaded in one go, the way a print's
    /// first T<n> leaves the MMU before the status reaches the screen.
    void load_tool_now(int tool);

    [[nodiscard]] int gate() const {
        return gate_;
    }
    [[nodiscard]] int tool() const {
        return tool_;
    }
    [[nodiscard]] int filament_pos() const {
        return filament_pos_;
    }
    [[nodiscard]] const std::vector<HappyHareGate>& gates() const {
        return gates_;
    }

  private:
    [[nodiscard]] int gate_for_tool(int tool) const;
    [[nodiscard]] int tool_for_gate(int gate) const;
    [[nodiscard]] bool loaded() const {
        return filament_pos_ != kPosUnloaded;
    }
    void append_unload(std::vector<HappyHareStep>& steps) const;
    void append_load(std::vector<HappyHareStep>& steps, int gate, int tool) const;
    HappyHareReply move(std::vector<HappyHareStep> steps);
    HappyHareReply gate_map(const std::string& gcode);
    HappyHareReply endless_spool(const std::string& gcode);

    std::vector<HappyHareGate> gates_;
    std::vector<int> ttg_map_;
    std::vector<int> endless_spool_groups_;
    std::string environment_sensor_;
    int gate_ = 0;
    int tool_ = 0;
    int filament_pos_ = kPosLoaded;
    std::string action_ = "Idle";
    std::string print_state_ = "idle";
    std::string reason_for_pause_;
    bool endless_spool_enabled_ = true;
    bool sync_feedback_enabled_ = true;
    bool fan_enabled_ = true;
    int fan_mode_ = 2; ///< MMU_FAN FAN_FORCED: 0 off, 1 on, 2 auto
    bool busy_ = false;
};

} // namespace helix::mock
