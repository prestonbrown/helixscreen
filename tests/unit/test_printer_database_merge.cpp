// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/scoped_env.h"
#include "printer_database_merge.h"
#include "printer_detector.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
namespace db = helix::printer_db;

TEST_CASE("An extension under one of a bundled entry's id_aliases is superseded",
          "[printer][db_shape][id_aliases]") {
    // A firmware that ships its own printer_database.d entry for a machine the
    // bundled database already knows, under its own spelling of the id (Reforge's
    // flashforge_creator5_pro beside our flashforge_creator_5_pro), would load as
    // a second entry of the same name and compete with ours on detection.
    json printers = json::parse(R"([
        { "id": "acme_x_1", "name": "Acme X1", "id_aliases": ["acme_x1", "acme_x_2"],
          "probe_type": "eddy" },
        { "id": "acme_x_2", "name": "Acme X2", "probe_type": "bltouch" }
    ])");
    const json extension = json::parse(R"([
        { "id": "acme_x1", "name": "Acme X1", "probe_type": "klicky" },
        { "id": "acme_x_2", "name": "Acme X2", "probe_type": "tap" },
        { "id": "acme_x_3", "name": "Acme X3" }
    ])");
    db::BundledIds ids = db::index_bundled(printers);
    // A bundled id is never an alias: acme_x_2 stays its own entry's.
    CHECK(ids.superseding == std::map<std::string, std::string>{{"acme_x1", "acme_x_1"}});

    db::MergeCounts counts;
    std::vector<std::string> errors;
    db::merge_extension_printers(printers, extension, ids, "vendor.json", counts, errors);

    CHECK(errors.empty());
    CHECK(counts.overrides == 1);
    CHECK(counts.additions == 1);
    REQUIRE(printers.size() == 3);
    CHECK(printers[0]["probe_type"] == "eddy");
    CHECK(printers[1]["probe_type"] == "tap");
    CHECK(printers[2]["id"] == "acme_x_3");
}

TEST_CASE("PrinterDetector: the bundled Creator 5 entries supersede Reforge's own ids",
          "[printer][db_shape][id_aliases][creator5]") {
    // Reforge (Klipper4FlashForge/firmware eda86c1) installs
    // printer_database.d/flashforge_creator5.json with these two ids.
    namespace fs = std::filesystem;
    auto temp_root = fs::temp_directory_path() /
                     ("test_printer_detector_reforge_ids_" + std::to_string(getpid()));
    struct RestoreDatabase {
        fs::path root;
        ~RestoreDatabase() {
            PrinterDetector::reload();
            fs::remove_all(root);
        }
    } restore{temp_root};
    {
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "printer_database.d");
        std::ofstream(temp_root / "printer_database.d" / "flashforge_creator5.json") << R"({
                "printers": [
                    { "id": "flashforge_creator5_pro", "name": "FlashForge Creator 5 Pro",
                      "image": "generic-corexy.png", "heuristics": [] },
                    { "id": "flashforge_creator5", "name": "FlashForge Creator 5",
                      "image": "generic-corexy.png", "heuristics": [] }
                ]
            })";
        setenv("HELIX_CONFIG_DIR", temp_root.c_str(), 1);
        PrinterDetector::reload();

        REQUIRE(PrinterDetector::get_load_status().loaded);
        CHECK(PrinterDetector::get_load_status().user_additions == 0);
        const auto& names = PrinterDetector::get_list_names();
        CHECK(std::count(names.begin(), names.end(), "FlashForge Creator 5 Pro") == 1);
        CHECK(std::count(names.begin(), names.end(), "FlashForge Creator 5") == 1);
        CHECK(PrinterDetector::get_image_for_printer("FlashForge Creator 5 Pro") ==
              "flashforge-creator-5-pro.png");
    }
}
