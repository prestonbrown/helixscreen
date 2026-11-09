// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/telemetry_test_fixture.h"
#include "install_provenance.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

TEST_CASE("Install provenance: a marker reports only an allowlisted word", "[telemetry][session]") {
    namespace ip = helix::install_provenance;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("helix_provenance_" + std::to_string(::getpid()) + "_marker");
    std::filesystem::create_directories(dir);
    const auto write = [&](const std::string& body) {
        std::ofstream(dir / ip::kMarkerFile, std::ios::trunc) << body;
    };

    CHECK(std::string(ip::read_install_source(dir.string())) == "unknown");
    write("kiauh\n");
    CHECK(std::string(ip::read_install_source(dir.string())) == "kiauh");
    write("helper-script");
    CHECK(std::string(ip::read_install_source(dir.string())) == "helper-script");
    write("/home/alice/secret\n");
    CHECK(std::string(ip::read_install_source(dir.string())) == "unknown");
    write("KIAUH\n");
    CHECK(std::string(ip::read_install_source(dir.string())) == "unknown");
    write("");
    CHECK(std::string(ip::read_install_source(dir.string())) == "unknown");

    std::filesystem::remove_all(dir);
}

TEST_CASE("Install provenance: the host mod comes from its filesystem marker",
          "[telemetry][session]") {
    namespace ip = helix::install_provenance;
    const auto root = std::filesystem::temp_directory_path() /
                      ("helix_provenance_" + std::to_string(::getpid()) + "_root");
    const auto with = [&](const std::string& path) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream(root / path) << "";
        return std::string(ip::detect_host_mod(root.string()));
    };

    CHECK(with("nothing/here") == "none");
    CHECK(with("ZMOD") == "zmod");
    CHECK(with("opt/config/mod/.shell/common.sh") == "forge_x");
    CHECK(with("usr/data/config/mod/.shell/platform.sh") == "forge_x");
    CHECK(with("root/printer_software/klipper") == "klipper_mod");
    CHECK(with("usr/bin/update-cosmos") == "cosmos");
    CHECK(with("etc/hooks/lmd.d/30-helixscreen.sh") == "paxx");
    CHECK(with("usr/data/pellcorp/k1") == "simple_af");
    CHECK(with("useremain/rinkhals/.version") == "rinkhals");

    std::filesystem::remove_all(root);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: carries the stored install source",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    std::ofstream(temp_dir() / helix::install_provenance::kMarkerFile) << "kiauh\n";
    tm.record_session();
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 2);

    CHECK(snapshot[0]["app"]["install_source"] == "unknown");
    CHECK(snapshot[1]["app"]["install_source"] == "kiauh");
    CHECK(snapshot[1]["app"]["host_mod"] ==
          std::string(helix::install_provenance::detect_host_mod()));
}
