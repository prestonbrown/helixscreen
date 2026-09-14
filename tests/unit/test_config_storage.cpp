// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/mock_config_storage.h"
#include "config.h"
#include "config_storage.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

using helix::ConfigDirGuard;

TEST_CASE("file storage round-trips a document atomically", "[config][storage]") {
    fs::path dir = fs::temp_directory_path() / "helix-storage-test";
    fs::create_directories(dir);
    std::string path = (dir / "settings.json").string();
    auto storage = helix::make_file_config_storage(path);

    REQUIRE_FALSE(storage->load().has_value()); // missing = nullopt
    REQUIRE(storage->store("{\"config_version\": 19}\n"));
    auto doc = storage->load();
    REQUIRE(doc.has_value());
    REQUIRE(doc->find("config_version") != std::string::npos);
    REQUIRE_FALSE(fs::exists(path + ".tmp")); // no temp litter after store

    storage->preserve_corrupt();
    REQUIRE_FALSE(storage->load().has_value());
    REQUIRE(fs::exists(path + ".corrupt"));

    fs::remove_all(dir);
}

TEST_CASE("file storage load() distinguishes absent from present-but-unreadable",
          "[config][storage]") {
    if (::geteuid() == 0) {
        SKIP("Test requires non-root euid — root bypasses permission bits, chmod 000 still opens");
    }

    fs::path dir = fs::temp_directory_path() / "helix-storage-unreadable-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string path = (dir / "settings.json").string();
    auto storage = helix::make_file_config_storage(path);

    // Absent: no throw, nullopt.
    REQUIRE_FALSE(storage->load().has_value());

    // Present but unreadable: must throw, NOT return nullopt — otherwise
    // Config::init() can't tell this apart from "absent" and would silently
    // reset a locked-down existing config to defaults instead of preserving
    // it for diagnosis + attempting backup restore.
    {
        std::ofstream f(path);
        f << R"({"config_version": 19})";
    }
    REQUIRE(::chmod(path.c_str(), 0) == 0);
    REQUIRE_THROWS_AS(storage->load(), std::exception);

    ::chmod(path.c_str(), 0644);
    fs::remove_all(dir);
}

TEST_CASE("Config routes load and save through an injected backend", "[config][storage]") {
    auto mock = std::make_unique<helix::test::MockConfigStorage>(
        std::string(R"({"config_version": 19, "wizard_completed": true})"));
    auto* mock_raw = mock.get();

    helix::Config cfg;
    cfg.set_storage(std::move(mock));
    cfg.init("config/settings-test.json");

    REQUIRE(cfg.get<bool>("/wizard_completed", false) == true);

    cfg.set<int>("/test_marker", 42);
    REQUIRE(cfg.save());
    REQUIRE(mock_raw->store_calls >= 1);
    REQUIRE(mock_raw->doc.has_value());
    REQUIRE(mock_raw->doc->find("test_marker") != std::string::npos);
}

TEST_CASE("Config routes a load() throw into corrupt-preserve, not first-boot defaults",
          "[config][storage]") {
    auto mock = std::make_unique<helix::test::MockConfigStorage>(
        std::string(R"({"config_version": 19, "wizard_completed": true})"));
    mock->unreadable = true;
    auto* mock_raw = mock.get();

    helix::Config cfg;
    cfg.set_storage(std::move(mock));
    cfg.init("config/settings-test.json");

    // preserve_corrupt() only runs on the corrupt/unreadable recovery path —
    // never on "absent" (first-boot). Its firing proves Config took the
    // recovery branch rather than silently defaulting.
    REQUIRE_FALSE(mock_raw->corrupt_stash.empty());
    REQUIRE(mock_raw->corrupt_stash.find("wizard_completed") != std::string::npos);

    cfg.clear_path();
}

TEST_CASE("Config::init() end-to-end: chmod-000 config routes into corrupt-preserve",
          "[config][storage]") {
    if (::geteuid() == 0) {
        SKIP("Test requires non-root euid — root bypasses permission bits, chmod 000 still opens");
    }

    ConfigDirGuard guard("chmod000");
    std::string config_path = (guard.dir / "settings-test.json").string();
    {
        std::ofstream f(config_path);
        f << R"({"config_version": 19, "wizard_completed": true})";
    }
    REQUIRE(::chmod(config_path.c_str(), 0) == 0);

    helix::Config cfg;
    // Filename only — ConfigDirGuard's HELIX_CONFIG_DIR supplies the directory.
    cfg.init("settings-test.json");

    // Recovery path taken: the unreadable document was preserved for
    // diagnosis exactly as a corrupt/unparseable one would be. If this were
    // (incorrectly) treated as "absent", no .corrupt file would ever appear.
    REQUIRE(fs::exists(config_path + ".corrupt"));

    cfg.clear_path();
    ::chmod(config_path.c_str(), 0644);
}
