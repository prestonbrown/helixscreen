// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/mock_config_storage.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/unique_temp_dir.h"
#include "config.h"
#include "config_storage.h"
#include "core/text_io.h"
#include "panel_widget_config.h"

#include <algorithm>
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

    std::string read_error;
    REQUIRE_FALSE(storage->load(read_error).has_value()); // missing = nullopt
    REQUIRE(read_error.empty());
    REQUIRE(storage->store("{\"config_version\": 19}\n"));
    auto doc = storage->load(read_error);
    REQUIRE(doc.has_value());
    REQUIRE(doc->find("config_version") != std::string::npos);
    REQUIRE(helix::test::staging_files_beside(path) == 0); // no temp litter after store

    storage->preserve_corrupt();
    REQUIRE_FALSE(storage->load(read_error).has_value());
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

    // Absent: nullopt and no read error.
    std::string read_error;
    REQUIRE_FALSE(storage->load(read_error).has_value());
    REQUIRE(read_error.empty());

    // Present but unreadable: a read error, not a bare nullopt. Otherwise
    // Config::init() can't tell this apart from "absent" and would silently
    // reset a locked-down existing config to defaults instead of preserving
    // it for diagnosis + attempting backup restore.
    {
        std::ofstream f(path);
        f << R"({"config_version": 19})";
    }
    REQUIRE(::chmod(path.c_str(), 0) == 0);
    REQUIRE_FALSE(storage->load(read_error).has_value());
    REQUIRE_FALSE(read_error.empty());

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

TEST_CASE("Config routes an unreadable load() into corrupt-preserve, not first-boot defaults",
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

// The K-Touch keeps settings.json on a 128 KB LittleFS partition of 4 KB
// blocks, and an atomic save needs room for the old and new copies at once.
// The fixture is a real 3-printer K-Touch settings.json (hosts and macro names
// redacted), written pretty-printed at 31.5 KB. With a fourth printer it must
// save under 16 KB, so a save never needs more than 8 of the 32 blocks.
TEST_CASE("small-footprint storage keeps a 4-printer K-Touch settings.json inside budget",
          "[config][storage]") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;
    ConfigDirGuard guard("small4");
    const std::string path = (guard.dir / "settings.json").string();
    fs::copy_file("tests/fixtures/config/ktouch_3_printers.json", path);

    helix::Config cfg;
    cfg.set_storage(helix::make_file_config_storage(path, helix::ConfigFootprint::Small));
    cfg.init(path);
    REQUIRE(cfg.get_printer_ids().size() == 3);
    cfg.add_printer("printer-4", cfg.get<nlohmann::json>("/printers/printer-3", {}));

    for (const auto& id : cfg.get_printer_ids()) {
        REQUIRE(cfg.set_active_printer(id));
        helix::PanelWidgetConfig home("home", cfg);
        home.load();
        home.save();
        REQUIRE(cfg.get<nlohmann::json>(cfg.df() + "panel_widgets/home", {}).is_object());
    }
    REQUIRE(cfg.save());

    const auto bytes = fs::file_size(path);
    INFO("settings.json with 4 printers: " << bytes << " bytes");
    REQUIRE(bytes < 16 * 1024);

    // Every printer's dashboard comes back as it was saved.
    helix::Config reread;
    reread.set_storage(helix::make_file_config_storage(path, helix::ConfigFootprint::Small));
    reread.init(path);
    for (const auto& id : cfg.get_printer_ids()) {
        REQUIRE(cfg.set_active_printer(id));
        REQUIRE(reread.set_active_printer(id));
        helix::PanelWidgetConfig before("home", cfg);
        helix::PanelWidgetConfig after("home", reread);
        before.load();
        after.load();
        CAPTURE(id);
        REQUIRE(before.entries().size() == after.entries().size());
        for (const auto& e : before.entries()) {
            CAPTURE(e.id);
            auto it = std::find_if(after.entries().begin(), after.entries().end(),
                                   [&](const auto& a) { return a.id == e.id; });
            REQUIRE(it != after.entries().end());
            REQUIRE(it->enabled == e.enabled);
            REQUIRE(it->col == e.col);
            REQUIRE(it->row == e.row);
            REQUIRE(it->colspan == e.colspan);
            REQUIRE(it->rowspan == e.rowspan);
        }
    }

    reread.clear_path();
    cfg.clear_path();
}

namespace {
/// Boots a config stamped @p version and reports whether a .pre-migration copy
/// is left beside it.
bool boot_leaves_pre_migration_copy(helix::ConfigFootprint footprint, int version,
                                    bool stale_copy_present) {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;
    ConfigDirGuard guard("premig");
    const std::string path = (guard.dir / "settings.json").string();
    {
        std::ofstream f(path);
        f << nlohmann::json{{"config_version", version}, {"wizard_completed", true}}.dump();
    }
    if (stale_copy_present) {
        std::ofstream(path + ".pre-migration") << R"({"config_version": 3})";
    }
    helix::Config cfg;
    cfg.set_storage(helix::make_file_config_storage(path, footprint));
    cfg.init(path);
    REQUIRE(cfg.get<int>("/config_version", 0) == helix::kCurrentConfigVersion);
    const bool exists = fs::exists(path + ".pre-migration");
    cfg.clear_path();
    return exists;
}
} // namespace

TEST_CASE("small-footprint storage keeps no .pre-migration copy", "[config][storage]") {
    constexpr int migrating = helix::kCurrentConfigVersion - 1;
    REQUIRE(boot_leaves_pre_migration_copy(helix::ConfigFootprint::Standard, migrating, false));
    REQUIRE_FALSE(boot_leaves_pre_migration_copy(helix::ConfigFootprint::Small, migrating, false));
    // One an earlier build left behind goes on the next boot, migrating or not.
    REQUIRE_FALSE(boot_leaves_pre_migration_copy(helix::ConfigFootprint::Small, migrating, true));
    REQUIRE_FALSE(boot_leaves_pre_migration_copy(helix::ConfigFootprint::Small,
                                                 helix::kCurrentConfigVersion, true));
}

// A below-floor document is replaced by defaults and its copy is the only one
// left, so small storage still writes it, compact. Serialized from memory here
// because the document did not come from the snapshot's sibling file.
TEST_CASE("small-footprint storage writes a below-floor copy compact", "[config][storage]") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;
    ConfigDirGuard guard("belowfloor");
    const std::string path = (guard.dir / "settings.json").string();
    auto mock = std::make_unique<helix::test::MockConfigStorage>(
        std::string(R"({"config_version": 3, "wizard_completed": true, "marker": "kept"})"));
    mock->small = true;

    helix::Config cfg;
    cfg.set_storage(std::move(mock));
    cfg.init(path);

    const auto copy = helix::text_io::read_file(path + ".pre-migration");
    REQUIRE(copy.has_value());
    CHECK(copy->find('\n') == std::string::npos);
    const auto doc = nlohmann::json::parse(*copy, nullptr, false);
    REQUIRE(doc.is_object());
    CHECK(doc.value("marker", "") == "kept");
    cfg.clear_path();
}
