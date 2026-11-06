// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: data-integrity gate over shipped assets/config/*.json, not over a C++ symbol
//
// Data-integrity gate over the two shipped asset files that have to agree with
// each other: assets/config/presets/*.json and assets/config/printer_database.json.
//
// PrinterDetector reads the preset name off the matched database entry
// (printer_detector.cpp, `best_match.preset = printer["preset"]`). A preset file
// that no database entry points at is therefore unreachable by detection: it is
// only ever applied by the factory-tarball path in mk/cross.mk, so every other
// install route (manual install, network-detected printer, wizard pick) silently
// misses every setting it carries. That is how the Elegoo Centauri Carbon shipped
// with an orphaned cc1.json (#1260).

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kDbPath = "assets/config/printer_database.json";
constexpr const char* kPresetDir = "assets/config/presets";

// Runtime firmware-variant suffixes. apply_preset_with_variants() probes the
// live printer objects and appends one to the database-resolved base preset, so
// `<base>_zmod.json` is reachable without a database entry of its own — but only
// if the base itself is referenced.
constexpr const char* kVariantSuffixes[] = {"_zmod", "_opencreator"};

json load_json(const std::string& path) {
    INFO("reading " << path << " (tests must run from the repo root)");
    REQUIRE(fs::exists(path));
    std::ifstream in(path);
    REQUIRE(in.good());
    return json::parse(in);
}

// Preset names named by some printer_database.json entry.
std::set<std::string> database_preset_refs() {
    json db = load_json(kDbPath);
    REQUIRE(db.contains("printers"));
    REQUIRE(db["printers"].is_array());

    std::set<std::string> refs;
    for (const auto& printer : db["printers"]) {
        std::string preset = printer.value("preset", "");
        if (!preset.empty()) {
            refs.insert(preset);
        }
    }
    // Sanity floor: if this collapses to nothing the database was misparsed and
    // every assertion below would pass vacuously. Kept far below the real count
    // so that unlinking a single preset trips the orphan check below, not this.
    REQUIRE(refs.size() >= 5);
    return refs;
}

// Basenames (without ".json") of every shipped preset file.
std::vector<std::string> preset_file_stems() {
    INFO("scanning " << kPresetDir << " (tests must run from the repo root)");
    REQUIRE(fs::is_directory(kPresetDir));

    std::vector<std::string> stems;
    for (const auto& entry : fs::directory_iterator(kPresetDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            stems.push_back(entry.path().stem().string());
        }
    }
    REQUIRE(stems.size() >= 5);
    return stems;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() > suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

TEST_CASE("every shipped preset is reachable from printer_database.json",
          "[printer_detector][presets][assets]") {
    const std::set<std::string> refs = database_preset_refs();

    for (const std::string& stem : preset_file_stems()) {
        CAPTURE(stem);
        json preset = load_json(std::string(kPresetDir) + "/" + stem + ".json");

        // A file with no top-level "preset" key is a reference config, not a
        // platform preset — it cannot trigger preset mode even if it were
        // applied. assets/config/presets/README.md documents voron-v2-afc.json
        // as exactly that ("Reference config, not auto-baked"). Nothing to link.
        if (!preset.contains("preset") || !preset["preset"].is_string()) {
            continue;
        }

        // Self-identification: the file's own marker is what lands in
        // settings.json on the baked-tarball path, and what
        // get_name_for_preset() matches against. It must equal the filename.
        CHECK(preset["preset"].get<std::string>() == stem);

        if (refs.count(stem) > 0) {
            continue;
        }

        // Not named directly — the only other reachable shape is a runtime
        // firmware variant whose base preset IS named.
        bool is_variant = false;
        for (const std::string suffix : kVariantSuffixes) {
            if (ends_with(stem, suffix)) {
                std::string base = stem.substr(0, stem.size() - suffix.size());
                CAPTURE(base);
                INFO("'" << stem
                         << "' is a firmware variant, so its base preset must be in the DB");
                CHECK(refs.count(base) > 0);
                is_variant = true;
                break;
            }
        }
        if (is_variant) {
            continue;
        }

        FAIL_CHECK("orphaned preset '"
                   << stem << "': no printer_database.json entry has \"preset\": \"" << stem
                   << "\", so only the mk/cross.mk factory tarball can ever apply it");
    }
}

TEST_CASE("every printer_database.json preset reference resolves to a file",
          "[printer_detector][presets][assets]") {
    for (const std::string& ref : database_preset_refs()) {
        CAPTURE(ref);
        std::string path = std::string(kPresetDir) + "/" + ref + ".json";
        INFO("database names preset '" << ref << "' but " << path << " does not exist");
        REQUIRE(fs::exists(path));

        // The referenced file must also identify itself as that preset, or the
        // baked-tarball copy of it lands in settings.json under the wrong name
        // and preset mode resolves to a different printer (or to nothing).
        json preset = load_json(path);
        REQUIRE(preset.contains("preset"));
        CHECK(preset["preset"].get<std::string>() == ref);
    }
}

// Entries that share one preset are one family, and a preset install that
// identifies no machine of it persists the family's default. That default is
// named in data with "preset_default", so it must be unambiguous.
TEST_CASE("each preset names at most one printer_database.json default",
          "[printer_detector][presets][assets]") {
    json db = load_json(kDbPath);
    std::map<std::string, std::vector<std::string>> defaults;
    size_t marked = 0;
    for (const auto& printer : db["printers"]) {
        if (!printer.value("preset_default", false)) {
            continue;
        }
        ++marked;
        const std::string preset = printer.value("preset", "");
        INFO("'" << printer.value("name", "") << "' is a preset_default with no preset");
        CHECK_FALSE(preset.empty());
        defaults[preset].push_back(printer.value("name", ""));
    }
    REQUIRE(marked >= 1);
    for (const auto& [preset, names] : defaults) {
        CAPTURE(preset, names);
        CHECK(names.size() == 1);
    }
}

// A preset shared by only one database entry has nothing to disambiguate: the
// no-evidence answer can only ever be that entry. Once a second entry joins the
// family, get_name_for_preset()'s no-evidence answer floats with array order
// unless one entry is marked. Named ids here (rather than "size() == 1" as
// above) so removing a marker fails this test even when the array happens to
// already list the base model first.
TEST_CASE("every preset family with more than one database entry names a default",
          "[printer_detector][presets][assets]") {
    json db = load_json(kDbPath);
    std::map<std::string, std::vector<std::string>> ids_by_preset;
    std::map<std::string, int> defaults_by_preset;
    for (const auto& printer : db["printers"]) {
        std::string preset = printer.value("preset", "");
        if (preset.empty()) {
            continue;
        }
        ids_by_preset[preset].push_back(printer.value("id", ""));
        if (printer.value("preset_default", false)) {
            ++defaults_by_preset[preset];
        }
    }
    for (const auto& [preset, ids] : ids_by_preset) {
        if (ids.size() < 2) {
            continue;
        }
        CAPTURE(preset, ids);
        INFO("preset '" << preset << "' has " << ids.size()
                        << " database entries and no preset_default marker (or more than "
                           "one); its no-evidence default floats with array order");
        CHECK(defaults_by_preset[preset] == 1);
    }
}

// Stock Creator 5 Pro firmware declares this fan as a fan_generic (SET_FAN_SPEED
// drives it); any other prefix leaves it "newly discovered" in Hardware Health.
TEST_CASE("the stock Creator 5 Pro preset names chamber_heat_fan as a fan_generic",
          "[printer_detector][presets][assets]") {
    const json pro = load_json(std::string(kPresetDir) + "/creator5_pro.json");
    const std::string expected = pro["printer"]["hardware"]["expected"].dump();
    CHECK(expected.find("\"fan_generic chamber_heat_fan\"") != std::string::npos);
}

// Z-Mod declares [led chamber_light] on every AD5X whether or not a light is
// wired, and a stock AD5X has none, so the preset turns the led capability off.
TEST_CASE("the AD5X preset turns the light capability off and names its led object",
          "[printer_detector][presets][assets]") {
    const json ad5x = load_json(std::string(kPresetDir) + "/ad5x.json");
    const json& printer = ad5x["printer"];
    REQUIRE(printer.contains("capability_overrides"));
    CHECK(printer["capability_overrides"].value("led", "") == "disable");
    const json& expected = printer["hardware"]["expected"];
    REQUIRE(expected.is_array());
    CHECK(std::find(expected.begin(), expected.end(), "led chamber_light") != expected.end());
}

// OpenCreator's runout switches are AFC's tool_start sensors (one per
// [AFC_extruder extruder<N>], opencreator-fs printer.afc-standalone.cfg), its
// fm_ex<N> wheel sensors are clog-only (printer.filament.cfg) and its chamber
// heater fan is a Kalico heater_fan_throttled (printer.pro.cfg) and its dock
// macro DOCK_EXISTING_TOOL (printer.macro.cfg). The stock firmware's fd_ex<N>
// switches, heater_fan chamber_heat_fan and TOOLCHANGE_PARK do not exist
// there, and naming them raises missing-hardware warnings and leaves runout
// pointing at nothing.
TEST_CASE("the OpenCreator Creator 5 presets name the hardware OpenCreator has",
          "[printer_detector][presets][assets]") {
    for (const char* stem : {"creator5_opencreator", "creator5_pro_opencreator"}) {
        CAPTURE(stem);
        const json preset = load_json(std::string(kPresetDir) + "/" + stem + ".json");
        const std::string text = preset.dump();
        CHECK(text.find("fd_ex") == std::string::npos);
        CHECK(text.find("\"heater_fan chamber_heat_fan\"") == std::string::npos);
        CHECK(text.find("TOOLCHANGE_PARK") == std::string::npos);

        const json& sensors = preset["printer"]["filament_sensors"]["sensors"];
        REQUIRE(sensors.is_array());
        REQUIRE(sensors.size() == 4);
        for (int lane = 0; lane < 4; ++lane) {
            CAPTURE(lane);
            const json& s = sensors[static_cast<size_t>(lane)];
            CHECK(s["klipper_name"] ==
                  "filament_switch_sensor extruder" + std::to_string(lane) + "_tool_start");
            CHECK(s["lane"] == lane);
            CHECK(s["role"] == "runout");
        }
    }
    const json pro = load_json(std::string(kPresetDir) + "/creator5_pro_opencreator.json");
    const json& expected = pro["printer"]["hardware"]["expected"];
    CHECK(std::find(expected.begin(), expected.end(), "heater_fan_throttled chamber_heat_fan") !=
          expected.end());
}
