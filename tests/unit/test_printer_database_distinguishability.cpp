// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gate over assets/config/printer_database.json: the shipped detector has to be
// able to tell every show_in_list entry apart from every other one.
//
// Each entry is replayed on the fingerprint it authors for itself - a hardware
// snapshot carrying every signal the entry's own heuristics look for, and
// nothing else - and PrinterDetector::detect() has to answer with that entry's
// machine, leading every candidate picturing a different machine by
// DETECT_MIN_MARGIN. An entry whose own best case ends in a tie is never
// auto-saved (the tie is resolved by heuristic count and database order, which
// meets_autosave_threshold refuses to persist); one whose own best case is won
// by another entry is worse, because that owner's printer is saved as the
// wrong model. Both mean the two entries are not distinguishable.
//
// Collisions the shipped database already carries are listed in
// known_collisions() and ratcheted: a new one fails the gate, and a listed one
// that stops reproducing fails it too, so the list never outlives the defect.

#include "printer_detector.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* DB_PATH = "assets/config/printer_database.json";

json load_database() {
    INFO("reading " << DB_PATH << " (tests must run from the repo root)");
    REQUIRE(fs::exists(DB_PATH));
    std::ifstream in(DB_PATH);
    REQUIRE(in.good());
    return json::parse(in);
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    return lowercase(haystack).find(lowercase(needle)) != std::string::npos;
}

/// The name a pattern describes: "^box$" is the object "box".
std::string unanchored(std::string pattern) {
    if (!pattern.empty() && pattern.front() == '^')
        pattern.erase(0, 1);
    if (!pattern.empty() && pattern.back() == '$')
        pattern.pop_back();
    return pattern;
}

/// Entry ids the shipped database cannot tell apart today, with the shape of
/// each collision. Removing an entry from this list is the way to claim the
/// database fix that resolves it; the gate fails if the claim is not true.
const std::map<std::string, std::string>& known_collisions() {
    static const std::map<std::string, std::string> known = {
        {"zerog_mercury_one",
         "authors only corexy kinematics and a bed size, which any corexy machine of that size "
         "out-claims"},
        {"zerog_mercury_one_plus", "same authoring gap as zerog_mercury_one, at 370mm"},
        {"zerog_mercury_one_hydra",
         "triple-Z corexy with z_tilt is the Voron Trident fingerprint, which the Trident entry "
         "scores higher"},
        {"zerog_mercury_one_plus_hydra", "same Trident overlap as zerog_mercury_one_hydra"},
        {"zerog_nebula_255", "same Trident overlap as zerog_mercury_one_hydra"},
        {"zerog_nebula_370", "same Trident overlap as zerog_mercury_one_hydra"},
        {"flsun_delta",
         "authors only class evidence - delta kinematics, delta_calibrate, stepper_a and "
         "class-string hostnames; no hardware fingerprint separates it from the other delta "
         "vendors, so a bare delta reports the family and stays ambiguous "
         "(prestonbrown/helixscreen#1607)"},
        {"venture_delta",
         "same class-only authoring as flsun_delta; its bed window is corroborating and "
         "separates nothing (prestonbrown/helixscreen#1607)"},
    };
    return known;
}

/// The hardware snapshot an entry authors for itself: every pattern its own
/// heuristics look for, shaped the way PrinterDiscovery reports it, and nothing
/// the entry does not mention. Fields the entry never scores stay empty rather
/// than being guessed at, so the replay claims nothing the entry does not. The
/// lists count as reported, the way a completed discovery leaves them, so an
/// absence the entry authors for itself is a real absence.
PrinterHardwareData fingerprint_of(const json& entry) {
    PrinterHardwareData hw;
    hw.objects_reported = true;
    const json heuristics = entry.value("heuristics", json::array());

    // Hostname: one string satisfying every hostname_match pattern at once, most
    // specific first, without tripping the entry's own hostname_exclude.
    std::vector<json> hostname_patterns;
    std::vector<std::string> hostname_excludes;
    std::vector<std::string> kinematics_excludes;
    for (const auto& h : heuristics) {
        const std::string type = h.value("type", "");
        if (type == "hostname_match")
            hostname_patterns.push_back(h);
        else if (type == "hostname_exclude")
            hostname_excludes.push_back(h.value("pattern", ""));
        else if (type == "kinematics_exclude")
            kinematics_excludes.push_back(h.value("pattern", ""));
    }
    std::stable_sort(hostname_patterns.begin(), hostname_patterns.end(),
                     [](const json& a, const json& b) {
                         return a.value("confidence", 0) > b.value("confidence", 0);
                     });
    for (const auto& h : hostname_patterns) {
        const std::string pattern = h.value("pattern", "");
        if (pattern.empty() || contains_ci(hw.hostname, pattern))
            continue;
        const std::string candidate = hw.hostname.empty() ? pattern : hw.hostname + "-" + pattern;
        const bool excluded =
            std::any_of(hostname_excludes.begin(), hostname_excludes.end(),
                        [&](const std::string& ex) { return contains_ci(candidate, ex); });
        if (!excluded)
            hw.hostname = candidate;
    }

    bool volume_set = false;
    for (const auto& h : heuristics) {
        const std::string type = h.value("type", "");
        const std::string pattern = unanchored(h.value("pattern", ""));

        if (type == "sensor_match" || type == "sensor_required") {
            hw.sensors.push_back(pattern);
        } else if (type == "fan_match" || type == "fan_required") {
            hw.fans.push_back(pattern);
        } else if (type == "fan_combo") {
            for (const auto& p : h.value("patterns", json::array()))
                if (p.is_string())
                    hw.fans.push_back(p.get<std::string>());
        } else if (type == "led_match" || type == "led_required") {
            hw.leds.push_back(pattern);
        } else if (type == "object_exists" || type == "object_required") {
            hw.printer_objects.push_back(pattern);
        } else if (type == "macro_match") {
            hw.printer_objects.push_back("gcode_macro " + pattern);
        } else if (type == "board_match") {
            hw.printer_objects.push_back("temperature_sensor " + pattern);
        } else if (type == "kinematics_match") {
            const bool excluded =
                std::any_of(kinematics_excludes.begin(), kinematics_excludes.end(),
                            [&](const std::string& ex) { return contains_ci(pattern, ex); });
            if (hw.kinematics.empty() && !excluded)
                hw.kinematics = pattern;
        } else if (type == "mcu_match") {
            if (hw.mcu.empty())
                hw.mcu = pattern;
            hw.mcu_list.push_back(pattern);
        } else if (type == "cpu_arch_match") {
            hw.cpu_arch = pattern;
        } else if (type == "stepper_count") {
            if (pattern == "stepper_a") {
                hw.steppers = {"stepper_a", "stepper_b", "stepper_c"};
            } else if (pattern.rfind("z_count_", 0) == 0) {
                const int z = std::stoi(pattern.substr(8));
                hw.steppers = {"stepper_x", "stepper_y", "stepper_z"};
                for (int i = 1; i < z; ++i)
                    hw.steppers.push_back("stepper_z" + std::to_string(i));
            }
        } else if (type == "tool_count") {
            if (pattern.rfind("tool_count_", 0) == 0) {
                const int tools = std::stoi(pattern.substr(11));
                hw.heaters = {"extruder"};
                for (int i = 1; i < tools; ++i)
                    hw.heaters.push_back("extruder" + std::to_string(i));
                hw.heaters.push_back("heater_bed");
            }
        } else if (type == "build_volume_range" && !volume_set) {
            // The centre of the entry's own window: the one size the entry
            // vouches for.
            auto middle = [&](const char* lo, const char* hi) {
                if (h.contains(lo) && h.contains(hi))
                    return (h.value(lo, 0.0f) + h.value(hi, 0.0f)) / 2.0f;
                return h.contains(lo) ? h.value(lo, 0.0f) : h.value(hi, 0.0f);
            };
            hw.build_volume.x_max = middle("min_x", "max_x");
            hw.build_volume.y_max = middle("min_y", "max_y");
            volume_set = true;
        }
    }

    // The objects list is the superset discovery reports: every named sensor,
    // fan, LED and heater appears there too.
    for (const auto* names : {&hw.sensors, &hw.fans, &hw.leds, &hw.heaters})
        hw.printer_objects.insert(hw.printer_objects.end(), names->begin(), names->end());

    return hw;
}

std::string describe(const PrinterHardwareData& hw) {
    auto join = [](const std::vector<std::string>& v) {
        std::string out;
        for (const auto& s : v)
            out += (out.empty() ? "" : ", ") + s;
        return out;
    };
    std::ostringstream os;
    os << "hostname='" << hw.hostname << "' kinematics='" << hw.kinematics << "' mcu='" << hw.mcu
       << "' cpu_arch='" << hw.cpu_arch << "' volume=" << hw.build_volume.x_max << "x"
       << hw.build_volume.y_max << " sensors=[" << join(hw.sensors) << "] fans=[" << join(hw.fans)
       << "] leds=[" << join(hw.leds) << "] heaters=[" << join(hw.heaters) << "] steppers=["
       << join(hw.steppers) << "] objects=[" << join(hw.printer_objects) << "]";
    return os.str();
}

} // namespace

TEST_CASE("every show_in_list printer database entry is distinguishable on its own fingerprint",
          "[printer][database][distinguishability]") {
    const json db = load_database();
    REQUIRE(db.contains("printers"));

    std::map<std::string, std::string> image_of_name;
    for (const auto& entry : db["printers"])
        image_of_name[entry.value("name", "")] = entry.value("image", "");

    int replayed = 0;
    for (const auto& entry : db["printers"]) {
        if (!entry.value("show_in_list", true))
            continue;
        ++replayed;

        const std::string id = entry.value("id", "");
        const std::string name = entry.value("name", "");
        const std::string image = entry.value("image", "");
        const PrinterHardwareData hw = fingerprint_of(entry);
        const PrinterDetectionResult result = PrinterDetector::detect(hw);

        std::string collision;
        if (!result.detected()) {
            collision = "its own fingerprint detects nothing";
        } else if (image_of_name[result.type_name] != image) {
            collision = "its own fingerprint detects as '" + result.type_name + "' (" +
                        std::to_string(result.confidence) + "%)";
        } else if (result.ambiguous()) {
            collision = "ties '" + result.runner_up_type_name + "' (" +
                        std::to_string(result.runner_up_confidence) + "%, " +
                        std::to_string(result.tied_count) + " tied) on its own fingerprint";
        }

        const auto known = known_collisions().find(id);
        INFO(id << " (" << name << "): " << describe(hw));
        INFO("detected '" << result.type_name << "' " << result.confidence << "% via "
                          << result.reason << "; runner-up '" << result.runner_up_type_name << "' "
                          << result.runner_up_confidence << "%, margin " << result.margin());
        if (collision.empty()) {
            INFO(id << " is listed in known_collisions() but is now distinguishable: "
                       "remove it from the list");
            CHECK(known == known_collisions().end());
        } else {
            INFO(id << " " << collision);
            CHECK(known != known_collisions().end());
        }
    }

    // The replay ran against the shipped database, not an empty or stub one.
    REQUIRE(replayed > 50);
}

TEST_CASE("a printer database entry's own fingerprint scores for that entry",
          "[printer][database][distinguishability]") {
    // The replay is only a gate if the fingerprint it builds is evidence the
    // entry actually collects: an entry scored here at less than its own
    // strongest heuristic would mean the synthesis dropped a signal, and every
    // verdict above would be about a weaker machine than the entry describes.
    const json db = load_database();
    for (const auto& entry : db["printers"]) {
        if (!entry.value("show_in_list", true))
            continue;
        int strongest = 0;
        for (const auto& h : entry.value("heuristics", json::array())) {
            if (!h.value("corroborating", false) && h.value("type", "") != "build_volume_range")
                strongest = std::max(strongest, h.value("confidence", 0));
        }
        const PrinterDetectionResult result = PrinterDetector::detect(fingerprint_of(entry));
        INFO(entry.value("id", "") << ": " << describe(fingerprint_of(entry)));
        INFO("detected '" << result.type_name << "' " << result.confidence << "%");
        CHECK(result.confidence >= strongest);
    }
}
