// SPDX-License-Identifier: GPL-3.0-or-later
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

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* DB_PATH = "assets/config/printer_database.json";
constexpr const char* PRESET_DIR = "assets/config/presets";

// Runtime firmware-variant suffix. apply_preset_with_variants() probes the live
// printer objects and appends this to the database-resolved base preset, so
// `<base>_zmod.json` is reachable without a database entry of its own — but only
// if the base itself is referenced.
constexpr const char* VARIANT_SUFFIX = "_zmod";

json load_json(const std::string& path) {
    INFO("reading " << path << " (tests must run from the repo root)");
    REQUIRE(fs::exists(path));
    std::ifstream in(path);
    REQUIRE(in.good());
    return json::parse(in);
}

// Preset names named by some printer_database.json entry.
std::set<std::string> database_preset_refs() {
    json db = load_json(DB_PATH);
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
    INFO("scanning " << PRESET_DIR << " (tests must run from the repo root)");
    REQUIRE(fs::is_directory(PRESET_DIR));

    std::vector<std::string> stems;
    for (const auto& entry : fs::directory_iterator(PRESET_DIR)) {
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
        json preset = load_json(std::string(PRESET_DIR) + "/" + stem + ".json");

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
        if (ends_with(stem, VARIANT_SUFFIX)) {
            std::string base = stem.substr(0, stem.size() - std::string(VARIANT_SUFFIX).size());
            CAPTURE(base);
            INFO("'" << stem << "' is a firmware variant, so its base preset must be in the DB");
            CHECK(refs.count(base) > 0);
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
        std::string path = std::string(PRESET_DIR) + "/" + ref + ".json";
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
