// SPDX-License-Identifier: GPL-3.0-or-later

// Creality CFS mock hardware (HELIX_MOCK_AMS=cfs): the stock K1 `box` frame and
// the box gcode that drives it, so the production AmsBackendCfs runs under --test.

#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

using namespace helix;
using mock_internal::effective_mock_ams_env;

bool MoonrakerClientMock::is_mock_cfs() const {
    // "cfs"/"cfs-k1": the K1 stock dialect. try_create_mock() declines these
    // values so the production AmsBackendCfs runs (pair with
    // HELIX_MOCK_PRINTER=k1 to latch the dialect).
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "cfs" || ams_type == "cfs-k1";
}

nlohmann::json MoonrakerClientMock::cfs_box_status_json() const {
    // Stock K1 `box` frame: T1 = unit 1, one array entry per bay. Bay A
    // carries a spool; the others report the "none"/-1 sentinels. Same shape
    // the unit-test fixtures feed parse_stock_box_status().
    auto box = nlohmann::json::parse(R"({
        "state": "connect", "filament": 0, "auto_refill": 0, "enable": 1,
        "same_material": 0,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {"state": "connect", "filament": "None",
               "vender": ["Creality", "none", "none", "none"],
               "remain_len": ["212", "-1", "-1", "-1"],
               "color_value": ["0E8E4F", "-1", "-1", "-1"],
               "material_type": ["000003", "-1", "-1", "-1"]}
    })");
    // HELIX_MOCK_CFS_BOXES lists the box addresses on the bus ("1,2,4"); box 1
    // is always present, every other listed box is empty. "none" is a K2 with
    // no CFS: the firmware still publishes `box`, every address reading "None"
    // and the box itself "disconnect".
    const char* boxes = std::getenv("HELIX_MOCK_CFS_BOXES");
    if (boxes && std::string_view(boxes) == "none") {
        box["state"] = "disconnect";
        box["map"] = nlohmann::json::object();
        for (const char* unit : {"T1", "T2", "T3", "T4"}) {
            box[unit] = {{"state", "None"},
                         {"filament", "None"},
                         {"version", "-1"},
                         {"vender", {"-1", "-1", "-1", "-1"}},
                         {"remain_len", {"-1", "-1", "-1", "-1"}},
                         {"color_value", {"-1", "-1", "-1", "-1"}},
                         {"material_type", {"-1", "-1", "-1", "-1"}}};
        }
        return box;
    }
    for (const char* c = boxes; c && *c; ++c) {
        if (*c < '2' || *c > '4') {
            continue;
        }
        const std::string unit = std::string("T") + *c;
        box[unit] = {{"state", "connect"},
                     {"filament", "None"},
                     {"vender", {"none", "none", "none", "none"}},
                     {"remain_len", {"-1", "-1", "-1", "-1"}},
                     {"color_value", {"-1", "-1", "-1", "-1"}},
                     {"material_type", {"-1", "-1", "-1", "-1"}}};
        for (char bay : {'A', 'B', 'C', 'D'}) {
            box["map"][unit + bay] = unit + bay;
        }
    }
    // The loaded bay: its unit's `filament` carries the bay letter, "None" elsewhere.
    if (const int loaded = cfs_loaded_slot_.load(); loaded >= 0) {
        // The box frame's wire key for a unit, not a display label.
        const std::string unit =
            "T" + std::to_string(loaded / 4 + 1); // DISPLAY_NUMBERING_OK: box frame wire key
        if (box.contains(unit)) {
            box[unit]["filament"] = std::string(1, static_cast<char>('A' + loaded % 4));
        }
    }
    return box;
}

void MoonrakerClientMock::simulate_cfs_find_cut_pos() {
    // The real sweep streams a position line every ~0.4s for ~60s and the
    // gcode/script RPC returns when the macro finishes, so the terminal lines
    // always precede the RPC ack. The mock reproduces that order by
    // dispatching the lines synchronously inside gcode_script (the same shape
    // the G0 out-of-range handler uses for its `!!` broadcast): a listener
    // registered before the send sees them, and the RPC's completion fires
    // after. The cut position scales off the persona envelope (a K1 Max
    // envelope reproduces the verified 304.0 reading).
    const double cut_y = helix::mock::descriptor(printer_type_).axis_max.y - 3.5;
    char buf[96];
    snprintf(buf, sizeof(buf), "Found cut position y: %.1f", cut_y);
    dispatch_gcode_response(buf);
    snprintf(buf, sizeof(buf), "MODIFY_BOX_CFG: success, cut_pos_y=%.1f,", cut_y);
    dispatch_gcode_response(buf);
    snprintf(buf, sizeof(buf), "SAVE_BOX_CFG ok: cut_pos_y=%.1f", cut_y);
    dispatch_gcode_response(buf);
}

// Gated only by is_mock_cfs() at the call site: CR_BOX_* is the K2 dialect's
// vocabulary, and a K1-dialect script never contains it, so no persona check is needed.
bool MoonrakerClientMock::apply_cfs_cr_box_script(const std::string& gcode) {
    bool touched = false;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("CR_BOX_EXTRUDE", 0) == 0) {
            // CR_BOX_EXTRUDE TNN=T<unit><bay>
            const size_t t = line.find("TNN=T");
            if (t == std::string::npos || t + 6 >= line.size()) {
                continue;
            }
            const int unit = line[t + 5] - '0';
            const int bay = line[t + 6] - 'A';
            if (unit < 1 || unit > 4 || bay < 0 || bay > 3) {
                continue;
            }
            cfs_loaded_slot_.store((unit - 1) * 4 + bay);
            touched = true;
        } else if (line.rfind("CR_BOX_RETRUDE", 0) == 0) {
            cfs_loaded_slot_.store(-1);
            touched = true;
        }
    }
    if (touched) {
        // The toolhead switch sees filament exactly while a bay is loaded.
        dispatch_status_update(
            {{"box", cfs_box_status_json()},
             {"filament_switch_sensor filament_sensor",
              {{"filament_detected", cfs_toolhead_filament_detected()}, {"enabled", true}}}});
    }
    return touched;
}

bool MoonrakerClientMock::apply_cfs_box_custom_command(const std::string& gcode) {
    const size_t c = gcode.find("CMD=");
    if (c == std::string::npos) {
        return false;
    }
    size_t start = c + 4;
    size_t end = gcode.find_first_of(" \t", start);
    const std::string cmd =
        gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);

    // Geometry scales off the persona envelope the way the verified K1 Max
    // values scale off its 307.5: safe_pos_y = y_max - 16 (291.5 on a Max).
    // The Max parks at the captured box.cfg extrude_pos_x (184.5); smaller
    // K1s were never captured, so they scale the X envelope.
    const auto max = helix::mock::descriptor(printer_type_).axis_max;
    const double safe_y = max.y - 16.0;
    const double extrude_x = printer_type_ == PrinterType::CREALITY_K1_MAX ? 184.5 : max.x * 0.8;

    // Move to a parked position: homed, motors on, snapshot-dispatched as one
    // frame (same shape the G0 handler emits).
    auto park = [&](double x, double y) {
        {
            std::lock_guard<std::mutex> pos_lock(pos_mutex_);
            pos_x_.store(x);
            pos_y_.store(y);
        }
        motors_enabled_.store(true);
        {
            std::lock_guard<std::mutex> lock(homed_axes_mutex_);
            homed_axes_ = "xyz";
        }
        reset_idle_timeout();
        dispatch_status_update(
            {{"toolhead", {{"homed_axes", "xyz"}, {"position", {x, y, pos_z_.load(), 0.0}}}},
             {"motion_report",
              {{"live_position", {x, y, pos_z_.load() + gcode_offset_z_.load(), 0.0}}}}});
    };

    if (cmd == "XYZ_ZERO") {
        // Full home (incl. PRTouch Z) then park; the real firmware echoes the
        // park coordinates on the response stream.
        park(extrude_x, safe_y);
        char buf[64];
        snprintf(buf, sizeof(buf), "x_park = %.2f", extrude_x);
        dispatch_gcode_response(buf);
        snprintf(buf, sizeof(buf), "y_park = %.2f", safe_y);
        dispatch_gcode_response(buf);
        return true;
    }
    if (cmd == "COORDINATES_ADJUST_PREPARE") {
        park(extrude_x, safe_y);
        return true;
    }
    if (cmd == "COORDINATES_ADJUST_SAVE_POS") {
        // The firmware reads the LIVE toolhead position; mock reports the
        // same pair the save lines carry.
        double x, y, z;
        read_position_snapshot(x, y, z);
        char buf[112];
        snprintf(buf, sizeof(buf), "cmd_save_extrude_pos x=%.2f y=%.2f", x, y);
        dispatch_gcode_response(buf);
        snprintf(buf, sizeof(buf), "MODIFY_BOX_CFG: success, extrude_pos_x=%.1f,", x);
        dispatch_gcode_response(buf);
        snprintf(buf, sizeof(buf), "SAVE_BOX_CFG ok: extrude_pos_x=%.1f,extrude_pos_y=%.1f", x, y);
        dispatch_gcode_response(buf);
        return true;
    }
    if (cmd == "Y_SAFE") {
        park(pos_x_.load(), safe_y);
        return true;
    }
    spdlog::debug("[MoonrakerClientMock] BOX_CUSTOM_COMMAND {} not simulated", cmd);
    return false;
}
