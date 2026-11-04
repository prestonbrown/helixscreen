// SPDX-License-Identifier: GPL-3.0-or-later

// HELIX_MOCK_SKIP_WRAPPERS=1: the mock printer behaves as if helix_skips.cfg
// is loaded, so the print-detail skip toggles can be driven under --test.

#include "core/text_io.h"
#include "env_knobs.h"
#include "klipper_config_parser.h"
#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

using namespace helix;
namespace sw = helix::skip_wrappers;

namespace {

constexpr sw::Op kAllOps[] = {sw::Op::BedMesh, sw::Op::Qgl, sw::Op::ZTilt};

/// The leveling sections a printer with these objects has in its config.
json leveling_sections(bool bed_mesh, bool qgl, bool z_tilt) {
    json s = json::object();
    if (bed_mesh) {
        s["bed_mesh"] = json::object();
    }
    if (qgl) {
        s["quad_gantry_level"] = json::object();
    }
    if (z_tilt) {
        s["z_tilt"] = json::object();
    }
    return s;
}

} // namespace

bool MoonrakerClientMock::skip_wrappers_loaded() {
    return env_flag("HELIX_MOCK_SKIP_WRAPPERS");
}

json MoonrakerClientMock::skip_wrapper_sections() const {
    if (!skip_wrappers_loaded()) {
        return json::object();
    }
    const auto& hw = hardware();
    json sections = leveling_sections(hw.has_bed_mesh(), hw.has_qgl(), hw.has_z_tilt());

    KlipperConfigParser parser;
    parser.parse(sw::generate(sw::wrappable(sections)));
    for (const auto& name : parser.get_sections()) {
        json& out = sections[text_io::to_lower(name)];
        for (const auto& key : parser.get_keys(name)) {
            out[key] = parser.get(name, key);
        }
    }
    return sections;
}

json MoonrakerClientMock::skip_wrapper_status() const {
    json status = json::object();
    if (!skip_wrappers_loaded()) {
        return status;
    }
    const auto& hw = hardware();
    json flags = json::object();
    for (sw::Op op :
         sw::wrappable(leveling_sections(hw.has_bed_mesh(), hw.has_qgl(), hw.has_z_tilt()))) {
        flags[sw::flag_for(op)] = skip_flags_[static_cast<size_t>(op)].load();
    }
    status[std::string("gcode_macro ") + sw::kPrepMacro] = flags;
    // A mock that has just leveled, so the QGL and Z-tilt toggles show at once.
    if (hw.has_qgl()) {
        status["quad_gantry_level"] = {{"applied", true}};
    }
    if (hw.has_z_tilt()) {
        status["z_tilt"] = {{"applied", true}};
    }
    return status;
}

bool MoonrakerClientMock::consume_skip(sw::Op op) {
    if (!skip_wrappers_loaded()) {
        return false;
    }
    int skipped = 0;
    if (!skip_flags_[static_cast<size_t>(op)].compare_exchange_strong(skipped, 1)) {
        return false;
    }
    dispatch_status_update(
        {{std::string("gcode_macro ") + sw::kPrepMacro, {{sw::flag_for(op), 1}}}});
    return true;
}

void MoonrakerClientMock::append_skip_wrapper_objects(json& objects) {
    if (!skip_wrappers_loaded()) {
        return;
    }
    auto has = [&objects](const std::string& name) {
        return std::find(objects.begin(), objects.end(), name) != objects.end();
    };
    std::vector<std::string> names = {sw::kPrepMacro};
    for (const auto& command : sw::wrapped_commands(sw::wrappable(
             leveling_sections(has("bed_mesh"), has("quad_gantry_level"), has("z_tilt"))))) {
        names.push_back(command);
    }
    for (const auto& name : names) {
        const std::string object = "gcode_macro " + name;
        if (!has(object)) {
            objects.push_back(object);
        }
    }
}

// `_HELIX_PREP` resets every flag; SET_GCODE_VARIABLE MACRO=_HELIX_PREP sets
// one. A pre-start block carries several lines in one script, so each line is
// read on its own. Never ends the script.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_skip_wrappers(const std::string& g) {
    if (!skip_wrappers_loaded() || g.find(sw::kPrepMacro) == std::string::npos) {
        return std::nullopt;
    }
    json flags = json::object();
    std::istringstream lines(g);
    std::string raw;
    while (std::getline(lines, raw)) {
        const std::string line(text_io::trim(raw));
        if (line == sw::kPrepMacro) {
            for (sw::Op op : kAllOps) {
                skip_flags_[static_cast<size_t>(op)] = 1;
                flags[sw::flag_for(op)] = 1;
            }
            continue;
        }
        if (line.rfind("SET_GCODE_VARIABLE", 0) != 0 ||
            line.find(std::string("MACRO=") + sw::kPrepMacro + " ") == std::string::npos) {
            continue;
        }
        const size_t value_at = line.find("VALUE=");
        if (value_at == std::string::npos) {
            continue;
        }
        const int value = line.compare(value_at + 6, 1, "0") == 0 ? 0 : 1;
        for (sw::Op op : kAllOps) {
            if (line.find(std::string("VARIABLE=") + sw::flag_for(op) + " ") != std::string::npos) {
                skip_flags_[static_cast<size_t>(op)] = value;
                flags[sw::flag_for(op)] = value;
            }
        }
    }
    if (!flags.empty()) {
        spdlog::debug("[MoonrakerClientMock] skip flags now {}", flags.dump());
        dispatch_status_update({{std::string("gcode_macro ") + sw::kPrepMacro, flags}});
    }
    return std::nullopt;
}
