// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_config_manager.h"

#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::MoonrakerConfigManager;

namespace {

/// Exact-match predicate: every section Moonraker reported is present.
///
/// classify_section_match() grades DEGREES of agreement, which is what production
/// needs; most assertions below only care about the clean case, and read better
/// spelled as one. A file may still carry sections Moonraker never reported (the
/// `[include ...]` line HelixScreen adds) — only absences count against it.
bool defines_all(const std::string& content, const std::vector<std::string>& required) {
    return MoonrakerConfigManager::classify_section_match(content, required).verdict ==
           helix::SectionMatch::Match;
}

} // namespace

// ============================================================================
// Task 1: has_section
// ============================================================================

TEST_CASE("has_section matches a whole, uncommented section header", "[config_manager]") {
    struct Row {
        const char* content;
        const char* section;
        bool expected;
    };
    const Row rows[] = {
        {"[spoolman]\nserver: http://localhost:7912\n", "spoolman", true},
        {"[server]\nhost: localhost\n", "spoolman", false},
        {"", "spoolman", false},
        {"# [spoolman]\n# server: http://localhost:7912\n", "spoolman", false},
        {"[update_manager timelapse]\ntype: git_repo\n", "update_manager timelapse", true},
        {"[spoolman_extra]\nkey: value\n", "spoolman", false},
        {"[spoolman]   \nserver: http://localhost:7912\n", "spoolman", true},
        {"[spoolman]\r\nserver: http://localhost:7912\r\n", "spoolman", true},
        {"  [spoolman]\nserver: http://localhost:7912\n", "spoolman", true},
    };
    for (const Row& r : rows) {
        INFO("content: " << r.content << " section: " << r.section);
        CHECK(MoonrakerConfigManager::has_section(r.content, r.section) == r.expected);
    }
}

// ============================================================================
// Task 2: add_section
// ============================================================================

TEST_CASE("add_section appends a section with its entries in order", "[config_manager]") {
    SECTION("after existing content, with a comment line") {
        auto result = MoonrakerConfigManager::add_section(
            "[server]\nhost: localhost\n", "spoolman",
            {{"server", "http://localhost:7912"}, {"sync_rate", "5"}}, "Added by HelixScreen");
        CHECK(MoonrakerConfigManager::has_section(result, "spoolman"));
        CHECK(MoonrakerConfigManager::has_section(result, "server"));
        CHECK(result.find("# Added by HelixScreen") != std::string::npos);
        CHECK(result.find("server: http://localhost:7912") != std::string::npos);
        CHECK(result.find("sync_rate: 5") != std::string::npos);
    }
    SECTION("into empty content, entries in order, no comment line when none given") {
        auto result = MoonrakerConfigManager::add_section(
            "", "spoolman",
            {{"server", "http://localhost:7912"}, {"sync_rate", "5"}, {"connection_timeout", "30"}},
            "");
        CHECK(MoonrakerConfigManager::has_section(result, "spoolman"));
        size_t server_pos = result.find("server: http://localhost:7912");
        size_t sync_pos = result.find("sync_rate: 5");
        size_t timeout_pos = result.find("connection_timeout: 30");
        REQUIRE(server_pos != std::string::npos);
        CHECK(server_pos < sync_pos);
        CHECK(sync_pos < timeout_pos);
        CHECK(timeout_pos != std::string::npos);
        CHECK(result.find('#') == std::string::npos);
    }
    SECTION("with no entries") {
        CHECK(MoonrakerConfigManager::has_section(
            MoonrakerConfigManager::add_section("", "spoolman", {}, ""), "spoolman"));
    }
}

// Timelapse install calls add_section() repeatedly and relies on it NOT
// rewriting an existing section; upsert_section() is the opt-in update path.
TEST_CASE("add_section leaves an existing section untouched", "[config_manager][upsert]") {
    std::string content = "[spoolman]\nserver: http://192.168.1.58:7912\n";
    auto result = MoonrakerConfigManager::add_section(content, "spoolman",
                                                      {{"server", "http://192.168.1.56:7912"}}, "");
    CHECK(result == content);
}

// ============================================================================
// Task 3: remove_section
// ============================================================================

TEST_CASE("remove_section deletes the section, its entries and its comment block",
          "[config_manager]") {
    struct Row {
        const char* name;
        std::string content;
        const char* section;
        std::vector<const char*> kept_sections;
        std::vector<const char*> gone_text;
    };
    const Row rows[] = {
        {"at the end of the file",
         "[server]\nhost: localhost\n\n[spoolman]\nserver: http://localhost:7912\n",
         "spoolman",
         {"server"},
         {"server: http://localhost:7912"}},
        {"with its preceding comment",
         "[server]\nhost: localhost\n\n# Added by HelixScreen\n[spoolman]\nserver: "
         "http://localhost:7912\n",
         "spoolman",
         {"server"},
         {"Added by HelixScreen"}},
        {"between other sections",
         "[server]\nhost: localhost\n\n[spoolman]\nserver: "
         "http://localhost:7912\n\n[authorization]\nenabled: true\n",
         "spoolman",
         {"server", "authorization"},
         {}},
        {"with spaces in its name",
         "[update_manager timelapse]\ntype: git_repo\npath: ~/timelapse\n\n[server]\nhost: "
         "localhost\n",
         "update_manager timelapse",
         {"server"},
         {}},
        {"just added by add_section",
         MoonrakerConfigManager::add_section("[server]\nhost: localhost\n", "spoolman",
                                             {{"server", "http://localhost:7912"}},
                                             "Added by HelixScreen"),
         "spoolman",
         {"server"},
         {}},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto result = MoonrakerConfigManager::remove_section(r.content, r.section);
            CHECK_FALSE(MoonrakerConfigManager::has_section(result, r.section));
            for (const char* kept : r.kept_sections) {
                CHECK(MoonrakerConfigManager::has_section(result, kept));
            }
            for (const char* gone : r.gone_text) {
                CHECK(result.find(gone) == std::string::npos);
            }
        }
    }

    SECTION("a missing section is a no-op") {
        std::string content = "[server]\nhost: localhost\n";
        CHECK(MoonrakerConfigManager::remove_section(content, "spoolman") == content);
    }
}

// ============================================================================
// Task 4: has_include_line, add_include_line, get_section_value
// ============================================================================

TEST_CASE("has_include_line finds only an uncommented include", "[config_manager]") {
    CHECK(MoonrakerConfigManager::has_include_line(
        "[include helixscreen.conf]\n[server]\nhost: localhost\n"));
    CHECK_FALSE(MoonrakerConfigManager::has_include_line("[server]\nhost: localhost\n"));
    CHECK_FALSE(MoonrakerConfigManager::has_include_line(
        "# [include helixscreen.conf]\n[server]\nhost: localhost\n"));
}

TEST_CASE("add_include_line inserts once, after leading comments, before the first section",
          "[config_manager]") {
    SECTION("before the first section") {
        auto result = MoonrakerConfigManager::add_include_line("[server]\nhost: localhost\n");
        REQUIRE(MoonrakerConfigManager::has_include_line(result));
        CHECK(result.find("[include helixscreen.conf]") < result.find("[server]"));
    }
    SECTION("after the leading comments") {
        auto result = MoonrakerConfigManager::add_include_line(
            "# Moonraker configuration\n# Generated by setup\n\n[server]\nhost: localhost\n");
        size_t comment_pos = result.find("# Moonraker configuration");
        size_t include_pos = result.find("[include helixscreen.conf]");
        CHECK(comment_pos < include_pos);
        CHECK(include_pos < result.find("[server]"));
    }
    SECTION("into empty content") {
        CHECK(
            MoonrakerConfigManager::has_include_line(MoonrakerConfigManager::add_include_line("")));
    }
    SECTION("never twice") {
        auto result = MoonrakerConfigManager::add_include_line(
            "[include helixscreen.conf]\n[server]\nhost: localhost\n");
        size_t first = result.find("[include helixscreen.conf]");
        CHECK(result.find("[include helixscreen.conf]", first + 1) == std::string::npos);
    }
}

TEST_CASE("get_section_value reads a key inside its own section only", "[config_manager]") {
    struct Row {
        const char* content;
        const char* section;
        const char* key;
        const char* expected; // "" = not found
    };
    const Row rows[] = {
        {"[spoolman]\nserver: http://localhost:7912\nsync_rate: 5\n", "spoolman", "server",
         "http://localhost:7912"},
        {"[spoolman]\nserver  :  http://localhost:7912\n", "spoolman", "server",
         "http://localhost:7912"},
        {"[spoolman]\nserver: http://localhost:7912\n", "spoolman", "missing_key", ""},
        {"[server]\nhost: localhost\n", "spoolman", "server", ""},
        // 'host' lives in [server], not [spoolman]
        {"[server]\nhost: localhost\n\n[spoolman]\nserver: http://localhost:7912\n", "spoolman",
         "host", ""},
    };
    for (const Row& r : rows) {
        INFO("content: " << r.content << " key: " << r.key);
        CHECK(MoonrakerConfigManager::get_section_value(r.content, r.section, r.key) == r.expected);
    }
}

// ============================================================================
// upsert_section: updating an existing section in place. add_section() returns
// early when the section exists, so changing a configured URL needs this path.
// ============================================================================

TEST_CASE("upsert_section writes keys into the target section and nothing else",
          "[config_manager][upsert]") {
    using Kv = std::vector<std::pair<std::string, std::string>>;
    struct Value {
        const char* section;
        const char* key;
        const char* expected; // "" = absent
    };
    struct Row {
        const char* name;
        std::string content;
        Kv entries;
        std::vector<Value> values;
        std::vector<const char*> kept_text;
        std::vector<const char*> gone_text;
    };
    const Kv new_server = {{"server", "http://new:7912"}};
    const Row rows[] = {
        // The stale value is gone entirely, not merely shadowed
        {"an existing key is updated",
         "[spoolman]\nserver: http://192.168.1.58:7912\n",
         {{"server", "http://192.168.1.56:7912"}},
         {{"spoolman", "server", "http://192.168.1.56:7912"}},
         {},
         {"192.168.1.58"}},
        {"unrelated sections and their keys survive",
         "[server]\nhost: localhost\nport: 7125\n\n[spoolman]\nserver: http://old:7912\n\n"
         "[authorization]\ntrusted_clients: 192.168.1.0/24\n",
         new_server,
         {{"server", "host", "localhost"},
          {"server", "port", "7125"},
          {"authorization", "trusted_clients", "192.168.1.0/24"},
          {"spoolman", "server", "http://new:7912"}},
         {},
         {}},
        {"unrelated keys inside the section survive",
         "[spoolman]\nserver: http://old:7912\nsync_rate: 5\n",
         new_server,
         {{"spoolman", "sync_rate", "5"}, {"spoolman", "server", "http://new:7912"}},
         {},
         {}},
        // A new key lands inside [spoolman], not in the section after it
        {"a missing key is appended to the section",
         "[spoolman]\nserver: http://old:7912\n\n[server]\nhost: localhost\n",
         {{"server", "http://new:7912"}, {"sync_rate", "10"}},
         {{"spoolman", "server", "http://new:7912"},
          {"spoolman", "sync_rate", "10"},
          {"server", "sync_rate", ""},
          {"server", "host", "localhost"}},
         {},
         {}},
        {"a same-named key in another section is left alone",
         "[some_other]\nserver: keep-me\n\n[spoolman]\nserver: http://old:7912\n",
         new_server,
         {{"some_other", "server", "keep-me"}, {"spoolman", "server", "http://new:7912"}},
         {},
         {}},
        {"comments in and above the section survive",
         "# Spoolman - added by HelixScreen\n[spoolman]\n# the server URL\nserver: "
         "http://old:7912\n",
         new_server,
         {{"spoolman", "server", "http://new:7912"}},
         {"# Spoolman - added by HelixScreen", "# the server URL"},
         {}},
        {"a header with no keys",
         "[spoolman]\n",
         new_server,
         {{"spoolman", "server", "http://new:7912"}},
         {},
         {}},
        {"empty content", "", new_server, {{"spoolman", "server", "http://new:7912"}}, {}, {}},
        // A bare line with no colon is neither a crash nor swallowed
        {"a malformed line is kept",
         "[spoolman]\nthis line has no colon\nserver: http://old:7912\n",
         new_server,
         {{"spoolman", "server", "http://new:7912"}},
         {"this line has no colon"},
         {}},
        {"an updated key keeps its indentation",
         "[spoolman]\n    server: http://old:7912\n",
         new_server,
         {},
         {"    server: http://new:7912"},
         {}},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto result =
                MoonrakerConfigManager::upsert_section(r.content, "spoolman", r.entries, "");
            // Exactly one header, whatever the input held
            size_t first = result.find("[spoolman]");
            REQUIRE(first != std::string::npos);
            CHECK(result.find("[spoolman]", first + 1) == std::string::npos);
            for (const Value& v : r.values) {
                INFO("[" << v.section << "] " << v.key);
                CHECK(MoonrakerConfigManager::get_section_value(result, v.section, v.key) ==
                      v.expected);
            }
            for (const char* text : r.kept_text) {
                CHECK(result.find(text) != std::string::npos);
            }
            for (const char* text : r.gone_text) {
                CHECK(result.find(text) == std::string::npos);
            }
        }
    }
}

TEST_CASE("upsert_section adds a missing section and is stable on a second pass",
          "[config_manager][upsert]") {
    SECTION("a missing section is added with its comment") {
        auto result = MoonrakerConfigManager::upsert_section(
            "[server]\nhost: localhost\n", "spoolman", {{"server", "http://localhost:7912"}},
            "Added by HelixScreen");
        CHECK(MoonrakerConfigManager::get_section_value(result, "spoolman", "server") ==
              "http://localhost:7912");
        CHECK(result.find("# Added by HelixScreen") != std::string::npos);
        CHECK(MoonrakerConfigManager::has_section(result, "server"));
    }
    SECTION("an unchanged value rewrites nothing") {
        std::string content = "[spoolman]\nserver: http://same:7912\n";
        auto once = MoonrakerConfigManager::upsert_section(content, "spoolman",
                                                           {{"server", "http://same:7912"}}, "");
        auto twice = MoonrakerConfigManager::upsert_section(once, "spoolman",
                                                            {{"server", "http://same:7912"}}, "");
        CHECK(once == twice);
        CHECK(MoonrakerConfigManager::get_section_value(twice, "spoolman", "server") ==
              "http://same:7912");
    }
    SECTION("content without a trailing newline gains one") {
        auto result = MoonrakerConfigManager::upsert_section(
            "[spoolman]\nserver: http://old:7912", "spoolman", {{"server", "http://new:7912"}}, "");
        CHECK(MoonrakerConfigManager::get_section_value(result, "spoolman", "server") ==
              "http://new:7912");
        CHECK(result.back() == '\n');
    }
}

// ============================================================================
// resolve_config_upload_location: can the file API write the config Moonraker
// actually loads? On stock Creality K2, config_file is
// /usr/share/moonraker/moonraker.conf while data_path is /mnt/UDISK/printer_data.
// ============================================================================

TEST_CASE("resolve_config_upload_location finds the config under data_path/config",
          "[config_manager][config_path]") {
    struct Row {
        const char* name;
        const char* config_file;
        const char* data_path;
        bool uploadable;
        const char* subdir;
        const char* filename;
    };
    const Row rows[] = {
        {"the standard Klipper layout", "/home/pi/printer_data/config/moonraker.conf",
         "/home/pi/printer_data", true, "", "moonraker.conf"},
        {"a nested subdirectory", "/home/pi/printer_data/config/subdir/moonraker.conf",
         "/home/pi/printer_data", true, "subdir", "moonraker.conf"},
        {"a trailing slash on data_path", "/home/pi/printer_data/config/moonraker.conf",
         "/home/pi/printer_data/", true, "", "moonraker.conf"},
        {"a non-default filename", "/home/pi/printer_data/config/moonraker-alt.conf",
         "/home/pi/printer_data", true, "", "moonraker-alt.conf"},
        // Stock Creality K2: Moonraker reads a file the file API cannot reach
        {"a config outside data_path", "/usr/share/moonraker/moonraker.conf",
         "/mnt/UDISK/printer_data", false, "", ""},
        // A naive string-prefix test would accept this
        {"a lookalike prefix", "/home/pi/printer_data_old/config/moonraker.conf",
         "/home/pi/printer_data", false, "", ""},
        // data_path/moonraker.conf is outside the file API's "config" root
        {"a config directly under data_path", "/home/pi/printer_data/moonraker.conf",
         "/home/pi/printer_data", false, "", ""},
        {"no config file", "", "/home/pi/printer_data", false, "", ""},
        {"no data_path", "/home/pi/printer_data/config/moonraker.conf", "", false, "", ""},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto info =
                MoonrakerConfigManager::resolve_config_upload_location(r.config_file, r.data_path);
            REQUIRE(info.uploadable == r.uploadable);
            if (!r.uploadable) {
                CHECK_FALSE(info.error.empty());
                continue;
            }
            CHECK(info.error.empty());
            CHECK(info.upload_subdir == r.subdir);
            CHECK(info.config_filename == r.filename);
            const std::string subdir = r.subdir;
            CHECK(info.path_for("helixscreen.conf") ==
                  (subdir.empty() ? "helixscreen.conf" : subdir + "/helixscreen.conf"));
        }
    }

    SECTION("the error names the unreachable path so the user can act on it") {
        auto info = MoonrakerConfigManager::resolve_config_upload_location(
            "/usr/share/moonraker/moonraker.conf", "/mnt/UDISK/printer_data");
        CHECK(info.error.find("/usr/share/moonraker/moonraker.conf") != std::string::npos);
    }
}

// ============================================================================
// server.config files[] handling — detecting an unreachable config WITHOUT
// needing an absolute path.
//
// Most Moonraker builds (verified on stock Creality K2, 2026-07) expose neither
// config.server.config_file nor config.server.data_path, and report files[] as
// bare relative names:
//   files: [ { "filename": "moonraker.conf",
//              "sections": ["server","file_manager","database","data_store",
//                           "machine","authorization","octoprint_compat","history"] } ]
// Reachability therefore has to be proven by content, not by path.
// ============================================================================

// The exact files[] entry captured from stock Creality K2 firmware.
static std::vector<helix::LoadedConfigFile> k2_server_config_files() {
    return {{"moonraker.conf",
             {"server", "file_manager", "database", "data_store", "machine", "authorization",
              "octoprint_compat", "history"}}};
}

TEST_CASE("select_primary_config_index picks the file defining [server]",
          "[config_manager][config_path]") {
    CHECK(MoonrakerConfigManager::select_primary_config_index(k2_server_config_files()) == 0);
    std::vector<helix::LoadedConfigFile> server_second = {
        {"helixscreen.conf", {"spoolman"}}, {"moonraker.conf", {"server", "file_manager"}}};
    CHECK(MoonrakerConfigManager::select_primary_config_index(server_second) == 1);
    // No [server] anywhere: the first entry
    std::vector<helix::LoadedConfigFile> no_server = {{"a.conf", {"history"}},
                                                      {"b.conf", {"spoolman"}}};
    CHECK(MoonrakerConfigManager::select_primary_config_index(no_server) == 0);
    // No usable entry
    CHECK(MoonrakerConfigManager::select_primary_config_index({}) == -1);
    std::vector<helix::LoadedConfigFile> blank = {{"", {"server"}}};
    CHECK(MoonrakerConfigManager::select_primary_config_index(blank) == -1);
}

// ============================================================================
// Root vs primary — the COSMOS split (#1242)
//
// select_primary_config_index answers "which file can prove reachability by
// content", which is why it looks for [server]: that file carries a rich
// section list to verify against. On every firmware where the root config also
// defines [server] the two questions have the same answer, so the distinction
// never surfaced.
//
// COSMOS 26.07.0 on the Elegoo Centauri Carbon splits them. The root config is
// user-editable and holds nothing but includes; [server] lives in a vendor
// directory the firmware replaces on upgrade. Writing there loses the setting.
//
// files[] captured live from each device on 2026-08-09. Moonraker reports the
// config chain root-first, then in include order — confirmed on all six.
// ============================================================================

// COSMOS 26.07.0, Elegoo Centauri Carbon. Root defines NO sections.
static std::vector<helix::LoadedConfigFile> cosmos_server_config_files() {
    return {{"moonraker.conf", {}},
            {"helixscreen.conf", {"spoolman"}},
            {"moonraker-readonly/moonraker.conf",
             {"server", "machine", "file_manager", "authorization", "octoprint_compat", "history",
              "announcements", "webcam webcam"}}};
}

TEST_CASE("select_root_config_index picks the user-editable root on COSMOS, not the vendor file",
          "[config_manager][config_path][1242]") {
    auto files = cosmos_server_config_files();

    // The vendor file is the one defining [server], and it is what the
    // reachability proof still has to download.
    CHECK(MoonrakerConfigManager::select_primary_config_index(files) == 2);

    // The write target must be the root, which COSMOS preserves across upgrades.
    int root = MoonrakerConfigManager::select_root_config_index(files);
    REQUIRE(root == 0);
    CHECK(files[static_cast<size_t>(root)].filename == "moonraker.conf");

    // The specific regression: never hand back a path inside the vendor tree.
    CHECK(files[static_cast<size_t>(root)].filename.find("-readonly/") == std::string::npos);
}

TEST_CASE("select_root_config_index leaves single-file firmwares alone",
          "[config_manager][config_path][1242]") {
    // K2: one file, defines [server]. Root and primary must agree, or the
    // original K2 unreachable-config fix regresses.
    auto k2 = k2_server_config_files();
    CHECK(MoonrakerConfigManager::select_root_config_index(k2) == 0);
    CHECK(MoonrakerConfigManager::select_primary_config_index(k2) == 0);
}

TEST_CASE("select_root_config_index picks the root ahead of its includes",
          "[config_manager][config_path][1242]") {
    // Snapmaker U1: root defines [server], five extended/ includes follow it.
    std::vector<helix::LoadedConfigFile> u1 = {
        {"moonraker.conf", {"server", "file_manager", "machine"}},
        {"extended/moonraker/00_keep.cfg", {}},
        {"extended/moonraker/01_timelapse_stub.cfg", {"timelapse"}},
        {"extended/moonraker/04_remote_screen.cfg", {"remote_screen"}}};
    CHECK(MoonrakerConfigManager::select_root_config_index(u1) == 0);

    // Flashforge AD5M reports the root as an absolute path, with a mod_data
    // include after it. Still index 0.
    std::vector<helix::LoadedConfigFile> ad5m = {
        {"/root/printer_data/config/moonraker.conf", {"server", "machine"}},
        {"/root/printer_data/config/mod_data/user.moonraker.conf", {"spoolman"}}};
    CHECK(MoonrakerConfigManager::select_root_config_index(ad5m) == 0);
}

TEST_CASE("select_root_config_index skips unusable entries",
          "[config_manager][config_path][1242]") {
    CHECK(MoonrakerConfigManager::select_root_config_index({}) == -1);

    std::vector<helix::LoadedConfigFile> blank_only = {{"", {}}};
    CHECK(MoonrakerConfigManager::select_root_config_index(blank_only) == -1);

    // A nameless leading entry must not shadow the real root behind it.
    std::vector<helix::LoadedConfigFile> blank_first = {{"", {}}, {"moonraker.conf", {"server"}}};
    CHECK(MoonrakerConfigManager::select_root_config_index(blank_first) == 1);
}

TEST_CASE("config_path_from_relative accepts a path inside the config root",
          "[config_manager][config_path]") {
    struct Row {
        const char* name;
        const char* filename;
        bool uploadable;
        const char* subdir;
    };
    const Row rows[] = {
        {"a bare filename as reported by K2", "moonraker.conf", true, ""},
        {"a subdirectory", "subdir/moonraker.conf", true, "subdir"},
        {"an absolute path", "/usr/share/moonraker/moonraker.conf", false, ""},
        {"a path escaping the config root", "../outside/moonraker.conf", false, ""},
        {"an empty filename", "", false, ""},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto info = MoonrakerConfigManager::config_path_from_relative(r.filename);
            REQUIRE(info.uploadable == r.uploadable);
            if (!r.uploadable) {
                CHECK_FALSE(info.error.empty());
                continue;
            }
            CHECK(info.upload_subdir == r.subdir);
            CHECK(info.config_filename == "moonraker.conf");
            const std::string subdir = r.subdir;
            CHECK(info.path_for("helixscreen.conf") ==
                  (subdir.empty() ? "helixscreen.conf" : subdir + "/helixscreen.conf"));
        }
    }
    SECTION("the error names a rejected absolute path") {
        auto info = MoonrakerConfigManager::config_path_from_relative(
            "/usr/share/moonraker/moonraker.conf");
        CHECK(info.error.find("/usr/share/moonraker/moonraker.conf") != std::string::npos);
    }
}

TEST_CASE("K2 case: a stray file under the config root fails the section match",
          "[config_manager][config_path]") {
    // The file HelixScreen previously wrote into data_path/config on the K2. It is
    // named moonraker.conf but is NOT what Moonraker loaded. This must be detected so
    // setup errors out instead of reporting a false success.
    auto files = k2_server_config_files();
    std::string stray = "[include helixscreen.conf]\n";
    CHECK_FALSE(defines_all(stray, files[0].sections));
}

TEST_CASE("K2 case: a partially-matching file still fails the section match",
          "[config_manager][config_path]") {
    auto files = k2_server_config_files();
    std::string partial = "[server]\nhost: 0.0.0.0\n";
    CHECK_FALSE(defines_all(partial, files[0].sections));
}

TEST_CASE("standard layout: the loaded config under the config root passes the section match",
          "[config_manager][config_path]") {
    auto files = k2_server_config_files();
    std::string real = "[server]\nhost: 0.0.0.0\n"
                       "[file_manager]\nenable_object_processing: True\n"
                       "[database]\n[data_store]\n"
                       "[machine]\nprovider: systemd_dbus\n"
                       "[authorization]\nforce_logins: False\n"
                       "[octoprint_compat]\n[history]\n";
    CHECK(defines_all(real, files[0].sections));
}

TEST_CASE("section match survives HelixScreen adding its include line",
          "[config_manager][config_path]") {
    // The check is a subset test, not equality: once setup adds
    // [include helixscreen.conf] the file has a section Moonraker never reported.
    // A later re-run must still recognise the file as reachable.
    auto files = k2_server_config_files();
    std::string real = "[server]\n[file_manager]\n[database]\n[data_store]\n[machine]\n"
                       "[authorization]\n[octoprint_compat]\n[history]\n";
    auto with_include = MoonrakerConfigManager::add_include_line(real);
    CHECK(defines_all(with_include, files[0].sections));
}

TEST_CASE("list_sections enumerates sections and ignores comments",
          "[config_manager][config_path]") {
    auto s =
        MoonrakerConfigManager::list_sections("[server]\nhost: x\n# [nope]\n[include a.conf]\n");
    REQUIRE(s.size() == 2);
    CHECK(s[0] == "server");
    CHECK(s[1] == "include a.conf");
    CHECK(MoonrakerConfigManager::list_sections("").empty());
}

TEST_CASE("the Match verdict is a subset test, not equality", "[config_manager][config_path]") {
    // Extra sections never disqualify a file — only missing ones do.
    CHECK(defines_all("[a]\n[b]\n", {}));
    CHECK(defines_all("[a]\n[b]\n", {"a"}));
    CHECK_FALSE(defines_all("[a]\n", {"a", "b"}));
}

// ============================================================================
// Standard Fluidd/Mainsail layout — verified against a real CB1/Voron
// (192.168.1.112, 2026-07). This is the "must keep working, must not emit a
// spurious error" side of the detection, and it is materially more complex than
// the K2 fixture: a two-entry config chain, section names containing spaces, and
// an [include] line present in the file text but absent from files[] sections.
// ============================================================================

// The exact files[] chain reported by the CB1.
static std::vector<helix::LoadedConfigFile> cb1_server_config_files() {
    return {{"moonraker.conf",
             {"server", "authorization", "octoprint_compat", "file_manager", "history", "spoolman",
              "update_manager", "update_manager mainsail", "update_manager mainsail-config",
              "update_manager Klipper-Adaptive-Meshing-Purging", "update_manager led_effect",
              "update_manager klipper_auto_speed", "update_manager klipper_tmc_autotune",
              "update_manager Klippain-ShakeTune", "update_manager update_klipper_and_mcus",
              "update_manager afc-software", "update_manager helixscreen"}},
            {"moonraker-obico-update.cfg", {"update_manager moonraker-obico"}}};
}

// The actual on-disk text of the CB1's ~/printer_data/config/moonraker.conf.
// Note [include moonraker-obico-update.cfg], which Moonraker does NOT report as a
// section of moonraker.conf — it reports the included file as its own files[] entry.
static std::string cb1_moonraker_conf_text() {
    return "[server]\nhost: 0.0.0.0\n"
           "[authorization]\nforce_logins: False\n"
           "[octoprint_compat]\n"
           "[file_manager]\n"
           "[history]\n"
           "[spoolman]\nserver: http://192.168.1.58:7912\n"
           "[update_manager]\nchannel: dev\n"
           "[update_manager mainsail]\n"
           "[update_manager mainsail-config]\n"
           "[update_manager Klipper-Adaptive-Meshing-Purging]\n"
           "[update_manager led_effect]\n"
           "[update_manager klipper_auto_speed]\n"
           "[update_manager klipper_tmc_autotune]\n"
           "[update_manager Klippain-ShakeTune]\n"
           "[update_manager update_klipper_and_mcus]\n"
           "[update_manager afc-software]\n"
           "[include moonraker-obico-update.cfg]\n"
           "[update_manager helixscreen]\n";
}

TEST_CASE("CB1: standard layout passes the section match with no spurious error",
          "[config_manager][config_path]") {
    // The regression that matters on the working-machine side: if this ever fails we
    // emit a "config not writable" error on a perfectly healthy Fluidd install.
    auto files = cb1_server_config_files();
    CHECK(defines_all(cb1_moonraker_conf_text(), files[0].sections));
}

TEST_CASE("CB1: list_sections captures spaced section names verbatim",
          "[config_manager][config_path]") {
    // "update_manager mainsail" is ONE section name, not a section plus a token.
    auto sections = MoonrakerConfigManager::list_sections(cb1_moonraker_conf_text());

    auto contains = [&](const std::string& want) {
        for (const auto& s : sections)
            if (s == want)
                return true;
        return false;
    };
    CHECK(contains("update_manager"));
    CHECK(contains("update_manager mainsail"));
    CHECK(contains("update_manager Klipper-Adaptive-Meshing-Purging"));
    CHECK(contains("update_manager Klippain-ShakeTune"));
    CHECK(contains("include moonraker-obico-update.cfg"));
}

TEST_CASE("CB1: spaced section names resolve individually", "[config_manager][config_path]") {
    auto text = cb1_moonraker_conf_text();
    CHECK(MoonrakerConfigManager::has_section(text, "update_manager mainsail"));
    CHECK(MoonrakerConfigManager::has_section(text, "update_manager afc-software"));
    CHECK(MoonrakerConfigManager::has_section(text, "update_manager"));
}

TEST_CASE("CB1: a bare section name is not satisfied by a spaced variant",
          "[config_manager][config_path]") {
    // Guards both directions: prefix confusion would make the subset check pass or
    // fail for the wrong reason on every update_manager-heavy config.
    CHECK_FALSE(
        MoonrakerConfigManager::has_section("[update_manager mainsail]\n", "update_manager"));
    CHECK_FALSE(
        MoonrakerConfigManager::has_section("[update_manager]\n", "update_manager mainsail"));
}

TEST_CASE("CB1: multi-file chain selects moonraker.conf, not the included .cfg",
          "[config_manager][config_path]") {
    auto files = cb1_server_config_files();
    int primary = MoonrakerConfigManager::select_primary_config_index(files);
    REQUIRE(primary == 0);
    CHECK(files[static_cast<size_t>(primary)].filename == "moonraker.conf");

    auto info = MoonrakerConfigManager::config_path_from_relative(
        files[static_cast<size_t>(primary)].filename);
    CHECK(info.uploadable);
    CHECK(info.path_for("helixscreen.conf") == "helixscreen.conf");
}

TEST_CASE("CB1: an [include] line in the text does not perturb the match",
          "[config_manager][config_path]") {
    auto files = cb1_server_config_files();

    // Moonraker reports the included file as its own files[] entry, never as a
    // section of the parent — so the parent's text legitimately carries a section
    // the reported list lacks. The subset direction (reported subset-of file) absorbs this.
    for (const auto& s : files[0].sections)
        CHECK(s.rfind("include", 0) != 0);

    CHECK(defines_all(cb1_moonraker_conf_text(), files[0].sections));

    // Adding HelixScreen's own include line on top must also not break it.
    auto with_ours = MoonrakerConfigManager::add_include_line(cb1_moonraker_conf_text());
    CHECK(defines_all(with_ours, files[0].sections));
}

TEST_CASE("CB1: the included .cfg matches its own reported sections",
          "[config_manager][config_path]") {
    auto files = cb1_server_config_files();
    std::string obico = "[update_manager moonraker-obico]\norigin: https://example/obico.git\n";
    CHECK(defines_all(obico, files[1].sections));
}

TEST_CASE("CB1: upsert rewrites the existing [spoolman] URL without disturbing the file",
          "[config_manager][upsert]") {
    // The CB1 already carries a [spoolman] section, making it the real-world target
    // for the upsert fix. Everything else in this dense config must survive intact.
    auto files = cb1_server_config_files();
    auto result = MoonrakerConfigManager::upsert_section(
        cb1_moonraker_conf_text(), "spoolman", {{"server", "http://192.168.1.56:7912"}}, "");

    CHECK(MoonrakerConfigManager::get_section_value(result, "spoolman", "server") ==
          "http://192.168.1.56:7912");
    CHECK(result.find("192.168.1.58") == std::string::npos);
    // Every section Moonraker reported still present, include line intact, and a
    // neighbouring key untouched.
    CHECK(defines_all(result, files[0].sections));
    CHECK(result.find("[include moonraker-obico-update.cfg]") != std::string::npos);
    CHECK(MoonrakerConfigManager::get_section_value(result, "update_manager", "channel") == "dev");
}

TEST_CASE("CB1: upsert targets a spaced section name precisely", "[config_manager][upsert]") {
    auto result = MoonrakerConfigManager::upsert_section(
        cb1_moonraker_conf_text(), "update_manager mainsail", {{"origin", "https://new"}}, "");
    CHECK(MoonrakerConfigManager::get_section_value(result, "update_manager mainsail", "origin") ==
          "https://new");
    // The bare [update_manager] section must not have been touched.
    CHECK(MoonrakerConfigManager::get_section_value(result, "update_manager", "channel") == "dev");
    CHECK(MoonrakerConfigManager::get_section_value(result, "update_manager", "origin").empty());
}

// ============================================================================
// Choosing WHERE [spoolman] is written.
//
// A config that already defines [spoolman] natively (verified on the CB1) must be
// updated in place. Writing our own helixscreen.conf and [include]-ing it would
// leave Moonraker with two [spoolman] sections — the same "I configured it and it
// wouldn't take" symptom, reached by a different route.
//
// Mirrors SpoolmanOverlay's decision: >1 defining file -> ambiguous (refuse),
// exactly 1 -> update that file in place, 0 -> helixscreen.conf + [include].
// ============================================================================

namespace {
enum class SpoolmanTarget { Ambiguous, InPlace, IncludeFile };

struct SpoolmanPlan {
    SpoolmanTarget mode;
    std::string target;
};

SpoolmanPlan decide_spoolman_target(const std::vector<helix::LoadedConfigFile>& files) {
    auto defining = MoonrakerConfigManager::find_files_defining_section(files, "spoolman");
    if (defining.size() > 1)
        return {SpoolmanTarget::Ambiguous, ""};
    if (defining.size() == 1)
        return {SpoolmanTarget::InPlace, files[defining[0]].filename};
    int primary = MoonrakerConfigManager::select_primary_config_index(files);
    return {SpoolmanTarget::IncludeFile,
            primary >= 0 ? files[static_cast<size_t>(primary)].filename : ""};
}
} // namespace

TEST_CASE("find_files_defining_section locates the defining file", "[config_manager][target]") {
    auto files = cb1_server_config_files();
    auto hits = MoonrakerConfigManager::find_files_defining_section(files, "spoolman");
    REQUIRE(hits.size() == 1);
    CHECK(files[hits[0]].filename == "moonraker.conf");

    CHECK(MoonrakerConfigManager::find_files_defining_section(files, "not_a_section").empty());
}

TEST_CASE("CB1 shape: native [spoolman] is updated in place", "[config_manager][target]") {
    auto plan = decide_spoolman_target(cb1_server_config_files());
    CHECK(plan.mode == SpoolmanTarget::InPlace);
    CHECK(plan.target == "moonraker.conf");
}

TEST_CASE("CB1 shape: in-place write changes the URL and adds no include",
          "[config_manager][target]") {
    auto files = cb1_server_config_files();
    auto result = MoonrakerConfigManager::upsert_section(cb1_moonraker_conf_text(), "spoolman",
                                                         {{"server", "http://192.168.1.56:7912"}},
                                                         "Spoolman - added by HelixScreen");

    CHECK(MoonrakerConfigManager::get_section_value(result, "spoolman", "server") ==
          "http://192.168.1.56:7912");
    CHECK(result.find("192.168.1.58") == std::string::npos);
    // No include is introduced, and helixscreen.conf plays no part.
    CHECK_FALSE(MoonrakerConfigManager::has_include_line(result));
    // Exactly one [spoolman], and the rest of the config survives.
    size_t first = result.find("[spoolman]");
    CHECK(result.find("[spoolman]", first + 1) == std::string::npos);
    CHECK(defines_all(result, files[0].sections));
}

TEST_CASE("fresh shape: no [spoolman] anywhere uses the include flow", "[config_manager][target]") {
    std::vector<helix::LoadedConfigFile> fresh = {
        {"moonraker.conf", {"server", "file_manager", "history"}}};
    auto plan = decide_spoolman_target(fresh);
    CHECK(plan.mode == SpoolmanTarget::IncludeFile);
    CHECK(plan.target == "moonraker.conf");
}

TEST_CASE("ambiguous shape: two loaded files defining [spoolman] is refused",
          "[config_manager][target]") {
    std::vector<helix::LoadedConfigFile> ambiguous = {{"moonraker.conf", {"server", "spoolman"}},
                                                      {"helixscreen.conf", {"spoolman"}}};
    auto plan = decide_spoolman_target(ambiguous);
    CHECK(plan.mode == SpoolmanTarget::Ambiguous);

    auto hits = MoonrakerConfigManager::find_files_defining_section(ambiguous, "spoolman");
    REQUIRE(hits.size() == 2);
    CHECK(ambiguous[hits[0]].filename == "moonraker.conf");
    CHECK(ambiguous[hits[1]].filename == "helixscreen.conf");
}

TEST_CASE("migration shape: native [spoolman] plus our own loaded one is ambiguous",
          "[config_manager][target]") {
    // Anyone who already ran the broken flow against a config with a native
    // [spoolman] ends up here. Because helixscreen.conf is loaded via the include
    // it appears in files[] and is counted, so we refuse rather than silently
    // reintroducing the duplicate.
    std::vector<helix::LoadedConfigFile> migration = {
        {"moonraker.conf",
         {"server", "authorization", "file_manager", "history", "spoolman", "update_manager"}},
        {"helixscreen.conf", {"spoolman"}}};
    auto plan = decide_spoolman_target(migration);
    CHECK(plan.mode == SpoolmanTarget::Ambiguous);
}

TEST_CASE("idempotence: fresh setup converges after a second run", "[config_manager][target]") {
    std::vector<helix::LoadedConfigFile> fresh = {
        {"moonraker.conf", {"server", "file_manager", "history"}}};

    // --- run 1: include flow ---
    REQUIRE(decide_spoolman_target(fresh).mode == SpoolmanTarget::IncludeFile);
    std::string moonraker = "[server]\nhost: 0.0.0.0\n[file_manager]\n[history]\n";
    std::string helix = MoonrakerConfigManager::upsert_section(
        "", "spoolman", {{"server", "http://a:7912"}}, "Spoolman - added by HelixScreen");
    moonraker = MoonrakerConfigManager::add_include_line(moonraker);
    REQUIRE(MoonrakerConfigManager::has_include_line(moonraker));

    // After the restart Moonraker loads helixscreen.conf, so it now appears in files[].
    std::vector<helix::LoadedConfigFile> after_run1 = {
        {"moonraker.conf", {"server", "file_manager", "history"}},
        {"helixscreen.conf", {"spoolman"}}};

    // --- run 2: in place on helixscreen.conf, moonraker.conf untouched ---
    auto plan2 = decide_spoolman_target(after_run1);
    CHECK(plan2.mode == SpoolmanTarget::InPlace);
    CHECK(plan2.target == "helixscreen.conf");

    const std::string moonraker_before = moonraker;
    helix = MoonrakerConfigManager::upsert_section(helix, "spoolman", {{"server", "http://a:7912"}},
                                                   "Spoolman - added by HelixScreen");
    CHECK(moonraker == moonraker_before);

    // No accumulation of includes or sections.
    size_t inc = moonraker.find("[include helixscreen.conf]");
    CHECK(moonraker.find("[include helixscreen.conf]", inc + 1) == std::string::npos);
    size_t sec = helix.find("[spoolman]");
    CHECK(helix.find("[spoolman]", sec + 1) == std::string::npos);

    // --- run 3 is byte-identical ---
    auto run3 = MoonrakerConfigManager::upsert_section(
        helix, "spoolman", {{"server", "http://a:7912"}}, "Spoolman - added by HelixScreen");
    CHECK(run3 == helix);
}

TEST_CASE("idempotence: CB1 in-place setup converges after a second run",
          "[config_manager][target]") {
    auto files = cb1_server_config_files();
    REQUIRE(decide_spoolman_target(files).mode == SpoolmanTarget::InPlace);

    auto run1 = MoonrakerConfigManager::upsert_section(cb1_moonraker_conf_text(), "spoolman",
                                                       {{"server", "http://192.168.1.56:7912"}},
                                                       "Spoolman - added by HelixScreen");
    auto run2 = MoonrakerConfigManager::upsert_section(run1, "spoolman",
                                                       {{"server", "http://192.168.1.56:7912"}},
                                                       "Spoolman - added by HelixScreen");

    CHECK(run1 == run2);
    CHECK_FALSE(MoonrakerConfigManager::has_include_line(run1));
    size_t sec = run1.find("[spoolman]");
    CHECK(run1.find("[spoolman]", sec + 1) == std::string::npos);
    CHECK(defines_all(run1, files[0].sections));
}

// ============================================================================
// Display and Remove act on the file that actually defines [spoolman].
//
// Both paths previously assumed helixscreen.conf. On a natively-configured
// Moonraker (CB1) that made the URL display blank and — far worse — made Remove
// no-op against helixscreen.conf while reporting success and leaving the real
// [spoolman] in place. That is a false success, the same class of bug as the
// original "reported connected, wasn't".
//
// TEST_MIRROR_OK: simulate_remove() below stands in for
// SpoolmanOverlay::remove_spoolman_config()'s dispatch on the shared resolution
// (Ambiguous / Unreachable -> refuse, Undefined -> nothing to remove, Defined ->
// delete from the resolved file). The overlay reaches that switch only through
// resolve_spoolman_target(), which is a chain of Moonraker HTTP round-trips on a
// live overlay, so the four statuses are constructed here instead. Every decision
// the simulation makes is delegated: find_files_defining_section() picks the
// target and remove_section() produces the text -- the stand-in supplies only the
// order the statuses are checked in. Residual gap: reordering the overlay switch
// would not fail these tests.
// ============================================================================

namespace {
enum class RemoveOutcome { Removed, NothingToRemove, RefusedAmbiguous, RefusedUnreachable };

struct RemoveResult {
    RemoveOutcome outcome;
    std::string target;      ///< file written (Removed only)
    std::string new_content; ///< resulting text (Removed only)
};

// `reachable` models the content-verification step: false = the K2 case, where the
// loaded config is not addressable through the file API's config root.
RemoveResult simulate_remove(const std::vector<helix::LoadedConfigFile>& files,
                             const std::map<std::string, std::string>& disk, bool reachable) {
    auto defining = MoonrakerConfigManager::find_files_defining_section(files, "spoolman");
    if (defining.size() > 1)
        return {RemoveOutcome::RefusedAmbiguous, "", ""};
    if (!reachable)
        return {RemoveOutcome::RefusedUnreachable, "", ""};
    if (defining.empty())
        return {RemoveOutcome::NothingToRemove, "", ""};

    const std::string& target = files[defining[0]].filename;
    auto it = disk.find(target);
    if (it == disk.end())
        return {RemoveOutcome::RefusedUnreachable, "", ""};
    return {RemoveOutcome::Removed, target,
            MoonrakerConfigManager::remove_section(it->second, "spoolman")};
}
} // namespace

TEST_CASE("CB1 display: URL comes from the native moonraker.conf, not blank",
          "[config_manager][target]") {
    auto files = cb1_server_config_files();
    auto defining = MoonrakerConfigManager::find_files_defining_section(files, "spoolman");
    REQUIRE(defining.size() == 1);
    CHECK(files[defining[0]].filename == "moonraker.conf");

    // The overlay reads the value out of the resolved file's content.
    auto url =
        MoonrakerConfigManager::get_section_value(cb1_moonraker_conf_text(), "spoolman", "server");
    CHECK(url == "http://192.168.1.58:7912");
    CHECK_FALSE(url.empty()); // the old helixscreen.conf assumption produced exactly this

    // ...and the old assumption really would have come up empty.
    CHECK(MoonrakerConfigManager::get_section_value("", "spoolman", "server").empty());
}

TEST_CASE("CB1 remove: deletes the native section, config no longer defines [spoolman]",
          "[config_manager][target]") {
    auto files = cb1_server_config_files();
    std::map<std::string, std::string> disk = {{"moonraker.conf", cb1_moonraker_conf_text()}};

    auto r = simulate_remove(files, disk, /*reachable=*/true);
    REQUIRE(r.outcome == RemoveOutcome::Removed);
    CHECK(r.target == "moonraker.conf");
    CHECK_FALSE(MoonrakerConfigManager::has_section(r.new_content, "spoolman"));
    CHECK(MoonrakerConfigManager::get_section_value(r.new_content, "spoolman", "server").empty());
    // Everything else in the config survives the removal.
    CHECK(MoonrakerConfigManager::has_section(r.new_content, "server"));
    CHECK(MoonrakerConfigManager::has_section(r.new_content, "update_manager mainsail"));
    CHECK(r.new_content.find("[include moonraker-obico-update.cfg]") != std::string::npos);
}

TEST_CASE("include-flow remove: still deletes from helixscreen.conf", "[config_manager][target]") {
    std::vector<helix::LoadedConfigFile> files = {{"moonraker.conf", {"server", "file_manager"}},
                                                  {"helixscreen.conf", {"spoolman"}}};
    std::map<std::string, std::string> disk = {
        {"moonraker.conf", "[server]\n[file_manager]\n[include helixscreen.conf]\n"},
        {"helixscreen.conf", "[spoolman]\nserver: http://a:7912\n"}};

    auto r = simulate_remove(files, disk, /*reachable=*/true);
    REQUIRE(r.outcome == RemoveOutcome::Removed);
    CHECK(r.target == "helixscreen.conf");
    CHECK_FALSE(MoonrakerConfigManager::has_section(r.new_content, "spoolman"));
}

TEST_CASE("ambiguous remove: refuses and modifies neither file", "[config_manager][target]") {
    std::vector<helix::LoadedConfigFile> files = {{"moonraker.conf", {"server", "spoolman"}},
                                                  {"helixscreen.conf", {"spoolman"}}};
    const std::string moonraker = "[server]\n[spoolman]\nserver: http://native:7912\n";
    const std::string helix = "[spoolman]\nserver: http://ours:7912\n";
    std::map<std::string, std::string> disk = {{"moonraker.conf", moonraker},
                                               {"helixscreen.conf", helix}};

    auto r = simulate_remove(files, disk, /*reachable=*/true);
    CHECK(r.outcome == RemoveOutcome::RefusedAmbiguous);
    CHECK(r.target.empty());
    // Neither file was touched — both still define [spoolman].
    CHECK(disk["moonraker.conf"] == moonraker);
    CHECK(disk["helixscreen.conf"] == helix);
    CHECK(MoonrakerConfigManager::has_section(disk["moonraker.conf"], "spoolman"));
    CHECK(MoonrakerConfigManager::has_section(disk["helixscreen.conf"], "spoolman"));
}

TEST_CASE("unreachable remove: reports failure, never a false success",
          "[config_manager][target]") {
    // K2 shape: Moonraker loads a config we cannot address through the file API.
    auto files = k2_server_config_files();
    std::map<std::string, std::string> disk; // nothing under the writable config root

    auto r = simulate_remove(files, disk, /*reachable=*/false);
    CHECK(r.outcome == RemoveOutcome::RefusedUnreachable);
    CHECK(r.outcome != RemoveOutcome::Removed);
    CHECK(r.outcome != RemoveOutcome::NothingToRemove);
    CHECK(r.target.empty());
}

TEST_CASE("remove with no [spoolman] anywhere: nothing to remove, not success",
          "[config_manager][target]") {
    std::vector<helix::LoadedConfigFile> files = {
        {"moonraker.conf", {"server", "file_manager", "history"}}};
    std::map<std::string, std::string> disk = {
        {"moonraker.conf", "[server]\n[file_manager]\n[history]\n"}};

    auto r = simulate_remove(files, disk, /*reachable=*/true);
    CHECK(r.outcome == RemoveOutcome::NothingToRemove);
    CHECK(r.outcome != RemoveOutcome::Removed);
    CHECK(r.target.empty());
}

// ============================================================================
// Absolute filenames that still land inside the writable config root.
//
// Moonraker names each loaded file relative to the ROOT config file's parent
// directory (server.py `_handle_config_request` does `path.relative_to(cfg_parent)`
// and falls back to the full absolute path on ValueError). When the root config
// lives outside the file-manager's config root — a vendor moonraker.conf under
// /usr/share carrying `[include /mnt/UDISK/printer_data/config/helixscreen.conf]` —
// the included file is reported absolute even though it sits squarely inside the
// writable root and is perfectly addressable through the file API.
//
// So an absolute name inside the caller-supplied config root is accepted; with
// no root supplied every absolute name is refused.
// ============================================================================

TEST_CASE("config_path_from_relative accepts an absolute path inside the supplied config root",
          "[config_manager][config_path][abs_root]") {
    const char* root = "/mnt/UDISK/printer_data/config";
    struct Row {
        const char* name;
        const char* filename;
        const char* root;
        bool uploadable;
        const char* subdir;
        const char* config_filename;
    };
    const Row rows[] = {
        {"a file directly under the root", "/mnt/UDISK/printer_data/config/helixscreen.conf", root,
         true, "", "helixscreen.conf"},
        {"a subdirectory of the root", "/mnt/UDISK/printer_data/config/extra/user.conf", root, true,
         "extra", "user.conf"},
        {"a trailing slash on the root", "/mnt/UDISK/printer_data/config/helixscreen.conf",
         "/mnt/UDISK/printer_data/config/", true, "", "helixscreen.conf"},
        // A relative name is already relative to the root, even one that repeats it
        {"a relative name with a root", "moonraker.conf", root, true, "", "moonraker.conf"},
        {"a relative name with a subdirectory", "extended/moonraker/04_remote_screen.cfg",
         "/oem/printer_data/config", true, "extended/moonraker", "04_remote_screen.cfg"},
        // Stock K2: the loaded root config lives outside the writable area
        {"outside the root", "/usr/share/moonraker/moonraker.conf", root, false, "", ""},
        // The prefix has to end on a path boundary
        {"a sibling sharing the root's prefix",
         "/mnt/UDISK/printer_data/config_backup/helixscreen.conf", root, false, "", ""},
        {"a sibling, slashed root", "/mnt/UDISK/printer_data/config_backup/x.conf",
         "/mnt/UDISK/printer_data/config/", false, "", ""},
        // A directory is not a config file
        {"the root itself", "/mnt/UDISK/printer_data/config", root, false, "", ""},
        {"the root itself, slashed", "/mnt/UDISK/printer_data/config/", root, false, "", ""},
        {".. after the prefix", "/mnt/UDISK/printer_data/config/../secrets/moonraker.conf", root,
         false, "", ""},
        {"a relative name escaping the root", "../outside/moonraker.conf", root, false, "", ""},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto info = MoonrakerConfigManager::config_path_from_relative(r.filename, r.root);
            REQUIRE(info.uploadable == r.uploadable);
            CHECK(info.config_filename == r.config_filename);
            if (r.uploadable) {
                CHECK(info.error.empty());
                CHECK(info.upload_subdir == r.subdir);
            } else {
                CHECK_FALSE(info.error.empty());
            }
        }
    }

    SECTION("with no root an absolute path is refused, and naming an empty root is the same") {
        auto omitted = MoonrakerConfigManager::config_path_from_relative(
            "/usr/share/moonraker/moonraker.conf");
        auto empty_root = MoonrakerConfigManager::config_path_from_relative(
            "/usr/share/moonraker/moonraker.conf", "");
        CHECK_FALSE(omitted.uploadable);
        CHECK_FALSE(empty_root.uploadable);
        CHECK(empty_root.error == omitted.error);
        CHECK(empty_root.error.find("/usr/share/moonraker/moonraker.conf") != std::string::npos);
    }
    SECTION("a refusal against a root names the path") {
        auto info = MoonrakerConfigManager::config_path_from_relative(
            "/usr/share/moonraker/moonraker.conf", root);
        CHECK(info.error.find("/usr/share/moonraker/moonraker.conf") != std::string::npos);
    }
}

// ============================================================================
// Section drift — an edited config is still the config Moonraker loaded.
//
// Moonraker serves the section list it PARSED AT STARTUP. Any edit since the last
// restart makes the reported list disagree with the file on disk, and the strict
// superset test read that as "this is not the file Moonraker loaded" and refused
// to continue.
//
// Captured live from a Snapmaker U1 (192.168.30.103) on 2026-08-14: HelixScreen
// had been uninstalled, which removed [update_manager helixscreen] from
// moonraker.conf, while Moonraker (up 16 days) still reported it. 12 of 13
// reported sections were present. Restarting Moonraker made the lists agree
// again, confirming the mechanism.
// ============================================================================

// The U1's /server/config files[] BEFORE the confirming restart.
static std::vector<helix::LoadedConfigFile> u1_server_config_files() {
    return {{"moonraker.conf",
             {"server", "machine", "authorization", "octoprint_compat", "history", "zeroconf",
              "snapmakercloud", "exception_manager", "client_manager", "repeater", "mqtt",
              "update_manager", "update_manager helixscreen"}},
            {"extended/moonraker/00_keep.cfg", {}},
            {"extended/moonraker/01_timelapse_stub.cfg", {"timelapse"}},
            {"extended/moonraker/02_internal_camera.cfg", {"webcam case"}},
            {"extended/moonraker/03_usb_camera.cfg", {}},
            {"extended/moonraker/04_remote_screen.cfg", {"webcam gui"}}};
}

// The U1's actual on-disk moonraker.conf, in file order. [update_manager helixscreen]
// is genuinely gone; [include ...] is in the text but Moonraker never reports it as
// a section of the parent.
static std::string u1_moonraker_conf_text() {
    return "[server]\nhost: 0.0.0.0\n"
           "[machine]\nprovider: systemd_cli\n"
           "[authorization]\nforce_logins: False\n"
           "[octoprint_compat]\n"
           "[history]\n"
           "[zeroconf]\n"
           "[snapmakercloud]\n"
           "[exception_manager]\n"
           "[client_manager]\n"
           "[repeater]\n"
           "[mqtt]\n"
           "[include extended/moonraker/*.cfg]\n"
           "[update_manager]\nchannel: dev\n";
}

TEST_CASE("classify_section_match: the U1 drift case is Drifted, not Mismatch",
          "[config_manager][config_path][drift]") {
    auto files = u1_server_config_files();
    auto m =
        MoonrakerConfigManager::classify_section_match(u1_moonraker_conf_text(), files[0].sections);

    CHECK(m.total == 13);
    CHECK(m.matched == 12);
    CHECK(m.verdict == helix::SectionMatch::Drifted);
    REQUIRE(m.missing.size() == 1);
    CHECK(m.missing[0] == "update_manager helixscreen");

    // The old strict test is exactly what failed here.
    CHECK_FALSE(defines_all(u1_moonraker_conf_text(), files[0].sections));
}

TEST_CASE("classify_section_match: an unedited config is a clean Match",
          "[config_manager][config_path][drift]") {
    auto files = cb1_server_config_files();
    auto m = MoonrakerConfigManager::classify_section_match(cb1_moonraker_conf_text(),
                                                            files[0].sections);
    CHECK(m.verdict == helix::SectionMatch::Match);
    CHECK(m.matched == m.total);
    CHECK(m.total == files[0].sections.size());
    CHECK(m.missing.empty());
}

TEST_CASE("classify_section_match: the K2 stray file is a Mismatch",
          "[config_manager][config_path][drift]") {
    // The file HelixScreen previously wrote into data_path/config on the K2: named
    // moonraker.conf, but not what Moonraker loaded. Drift tolerance must not
    // resurrect the false success this originally caught.
    auto files = k2_server_config_files();
    auto m = MoonrakerConfigManager::classify_section_match("[include helixscreen.conf]\n",
                                                            files[0].sections);
    CHECK(m.verdict == helix::SectionMatch::Mismatch);
    CHECK(m.matched == 0);
    CHECK(m.total == 8);
    CHECK(m.missing.size() == 8);
}

TEST_CASE("classify_section_match: the K2 decoy with only [server] is a Mismatch",
          "[config_manager][config_path][drift]") {
    auto files = k2_server_config_files();
    auto m = MoonrakerConfigManager::classify_section_match("[server]\nhost: 0.0.0.0\n",
                                                            files[0].sections);
    CHECK(m.verdict == helix::SectionMatch::Mismatch);
    CHECK(m.matched == 1);
    CHECK(m.total == 8);
}

TEST_CASE("classify_section_match: drift is graded by how many went missing, not what fraction "
          "survived",
          "[config_manager][config_path][drift]") {
    // Two unrelated moonraker.conf files agree on the whole stock section set, so a
    // majority is not evidence of anything. The threshold is an ABSOLUTE count:
    // one missing always, a quarter of the list once that is more.
    const std::vector<std::string> required = {"a", "b", "c", "d", "e", "f", "g", "h"};

    // 8 sections tolerate 2 missing.
    CHECK(MoonrakerConfigManager::drift_tolerance(8) == 2);

    auto six =
        MoonrakerConfigManager::classify_section_match("[a]\n[b]\n[c]\n[d]\n[e]\n[f]\n", required);
    CHECK(six.matched == 6);
    CHECK(six.verdict == helix::SectionMatch::Drifted);

    // A strict majority — which the old rule accepted — is now a different file.
    auto majority =
        MoonrakerConfigManager::classify_section_match("[a]\n[b]\n[c]\n[d]\n[e]\n", required);
    CHECK(majority.matched == 5);
    CHECK(majority.missing.size() == 3);
    CHECK(majority.verdict == helix::SectionMatch::Mismatch);

    auto half = MoonrakerConfigManager::classify_section_match("[a]\n[b]\n[c]\n[d]\n", required);
    CHECK(half.matched == 4);
    CHECK(half.verdict == helix::SectionMatch::Mismatch);

    // Short lists still get one free section, or a single uninstall would read as a
    // wrong file on a firmware that loads almost nothing.
    const std::vector<std::string> three = {"a", "b", "c"};
    CHECK(MoonrakerConfigManager::drift_tolerance(3) == 1);
    CHECK(MoonrakerConfigManager::classify_section_match("[a]\n[b]\n", three).verdict ==
          helix::SectionMatch::Drifted);
    CHECK(MoonrakerConfigManager::classify_section_match("[a]\n", three).verdict ==
          helix::SectionMatch::Mismatch);
}

TEST_CASE("classify_section_match: a decoy sharing the stock section set is still a Mismatch",
          "[config_manager][config_path][drift]") {
    // The regression the majority rule opened: an unrelated moonraker.conf left under
    // the writable config root by an earlier HelixScreen release shares every stock
    // section with the vendor config Moonraker actually loaded, and differs only in
    // the extras. Under a fraction rule it scored well above the bar and got written
    // to; the file Moonraker reads never changed and setup reported success.
    const std::vector<std::string> vendor = {"server",
                                             "file_manager",
                                             "database",
                                             "data_store",
                                             "machine",
                                             "authorization",
                                             "history",
                                             "update_manager mainsail",
                                             "update_manager fluidd",
                                             "webcam",
                                             "job_queue",
                                             "announcements"};
    const std::string decoy = "[server]\n[file_manager]\n[database]\n[data_store]\n[machine]\n"
                              "[authorization]\n[history]\n[webcam]\n";

    auto m = MoonrakerConfigManager::classify_section_match(decoy, vendor);
    CHECK(m.matched == 8);
    CHECK(m.total == 12);
    CHECK(m.verdict == helix::SectionMatch::Mismatch);
}

TEST_CASE("classify_section_match: nothing present is a Mismatch",
          "[config_manager][config_path][drift]") {
    auto m =
        MoonrakerConfigManager::classify_section_match("[completely]\n[other]\n", {"a", "b", "c"});
    CHECK(m.verdict == helix::SectionMatch::Mismatch);
    CHECK(m.matched == 0);
    CHECK(m.missing.size() == 3);
}

TEST_CASE("classify_section_match: an empty required list matches vacuously",
          "[config_manager][config_path][drift]") {
    // The caller guards the no-section-list case separately; here it is a Match by
    // definition rather than a division-by-zero or a Mismatch.
    auto m = MoonrakerConfigManager::classify_section_match("[server]\n", {});
    CHECK(m.verdict == helix::SectionMatch::Match);
    CHECK(m.total == 0);
    CHECK(m.matched == 0);
    CHECK(m.missing.empty());
}

TEST_CASE("classify_section_match: missing names come back in the reported order, verbatim",
          "[config_manager][config_path][drift]") {
    // Spaced names are one section each — the message the user reads must name them
    // exactly as Moonraker did.
    const std::vector<std::string> required = {"server",         "update_manager mainsail",
                                               "update_manager", "history",
                                               "machine",        "database",
                                               "file_manager",   "update_manager helixscreen"};
    auto m = MoonrakerConfigManager::classify_section_match(
        "[server]\n[update_manager]\n[history]\n[machine]\n[database]\n[file_manager]\n", required);
    CHECK(m.verdict == helix::SectionMatch::Drifted);
    REQUIRE(m.missing.size() == 2);
    CHECK(m.missing[0] == "update_manager mainsail");
    CHECK(m.missing[1] == "update_manager helixscreen");
}

TEST_CASE("Match means every reported section is genuinely present",
          "[config_manager][config_path][drift]") {
    // Checked against has_section() directly rather than against a second aggregate,
    // so the verdict is pinned to the primitive and not to a restatement of itself.
    auto files = k2_server_config_files();
    std::string real = "[server]\n[file_manager]\n[database]\n[data_store]\n[machine]\n"
                       "[authorization]\n[octoprint_compat]\n[history]\n";

    auto m = MoonrakerConfigManager::classify_section_match(real, files[0].sections);
    REQUIRE(m.verdict == helix::SectionMatch::Match);
    CHECK(m.missing.empty());
    CHECK(m.matched == files[0].sections.size());
    for (const auto& s : files[0].sections) {
        INFO("section=" << s);
        CHECK(MoonrakerConfigManager::has_section(real, s));
    }

    // An added include line gives the file a section Moonraker never reported. That
    // must stay a Match, or a second setup run would refuse the file it just wrote.
    auto with_include = MoonrakerConfigManager::add_include_line(real);
    CHECK(MoonrakerConfigManager::classify_section_match(with_include, files[0].sections).verdict ==
          helix::SectionMatch::Match);
}

// SpoolmanOverlay::verify_config_reachable() accepts a candidate exactly when
// grade_config_candidate() does — everything else it owns is the retry recursion
// and the logging. Calling the grader is what keeps the accept/reject rule in one
// place: the local copy this replaced knew only the Match/Drifted/Mismatch verdict
// and never had the speculative or in-place rejections, so the two tests below
// could not have existed against it.
namespace {
enum class VerifyOutcome { Proceed, Unreachable };

VerifyOutcome simulate_verify(const std::string& content, const std::vector<std::string>& required,
                              bool in_place = false, bool speculative = false) {
    return MoonrakerConfigManager::grade_config_candidate(content, required, in_place, speculative)
                   .accepted
               ? VerifyOutcome::Proceed
               : VerifyOutcome::Unreachable;
}
} // namespace

TEST_CASE("verify dispatch: U1 drift proceeds, K2 stray and decoy still fail",
          "[config_manager][config_path][drift]") {
    auto u1 = u1_server_config_files();
    CHECK(simulate_verify(u1_moonraker_conf_text(), u1[0].sections) == VerifyOutcome::Proceed);

    auto k2 = k2_server_config_files();
    CHECK(simulate_verify("[include helixscreen.conf]\n", k2[0].sections) ==
          VerifyOutcome::Unreachable);
    CHECK(simulate_verify("[server]\nhost: 0.0.0.0\n", k2[0].sections) ==
          VerifyOutcome::Unreachable);

    // The healthy CB1 install keeps proceeding.
    auto cb1 = cb1_server_config_files();
    CHECK(simulate_verify(cb1_moonraker_conf_text(), cb1[0].sections) == VerifyOutcome::Proceed);
}

TEST_CASE("verify dispatch: an inferred path may not lean on drift tolerance",
          "[config_manager][config_path][drift]") {
    // Four reported sections with one absent is inside drift tolerance, so the
    // verdict alone would accept this file.
    const std::vector<std::string> required = {"server", "file_manager", "authorization",
                                               "octoprint_compat"};
    const std::string drifted = "[server]\n[file_manager]\n[authorization]\n";
    REQUIRE(MoonrakerConfigManager::classify_section_match(drifted, required).verdict ==
            helix::SectionMatch::Drifted);

    // A path derived from what Moonraker reported spends that tolerance...
    CHECK(simulate_verify(drifted, required) == VerifyOutcome::Proceed);
    // ...a guessed one may not: drift is indistinguishable from a wrong guess when
    // nothing tied the path to the reported name in the first place.
    CHECK(simulate_verify(drifted, required, /*in_place=*/false, /*speculative=*/true) ==
          VerifyOutcome::Unreachable);

    // A guessed path that matches exactly is still accepted.
    const std::string exact = "[server]\n[file_manager]\n[authorization]\n[octoprint_compat]\n";
    CHECK(simulate_verify(exact, required, /*in_place=*/false, /*speculative=*/true) ==
          VerifyOutcome::Proceed);
}

TEST_CASE("verify dispatch: an in-place rewrite target must define [spoolman]",
          "[config_manager][config_path][drift]") {
    const std::vector<std::string> required = {"server", "file_manager"};
    const std::string without = "[server]\n[file_manager]\n";
    const std::string with = "[server]\n[file_manager]\n[spoolman]\nserver: http://a:7912\n";

    // Not an in-place rewrite: the reported section list alone decides, and both
    // files satisfy it.
    CHECK(simulate_verify(without, required) == VerifyOutcome::Proceed);
    CHECK(simulate_verify(with, required) == VerifyOutcome::Proceed);

    // In place, the candidate was selected *because* some file defines [spoolman];
    // one that does not is a different file however well the rest lines up.
    CHECK(simulate_verify(without, required, /*in_place=*/true) == VerifyOutcome::Unreachable);
    CHECK(simulate_verify(with, required, /*in_place=*/true) == VerifyOutcome::Proceed);
}

// ============================================================================
// Include line with an explicit target
//
// The K2 needs `[include /mnt/UDISK/printer_data/config/helixscreen.conf]` in a
// vendor moonraker.conf that lives in /usr/share/moonraker: a bare
// "helixscreen.conf" there resolves against the vendor directory, where no such
// file exists — and Moonraker refuses to start on an include it cannot match.
// ============================================================================

TEST_CASE("has_include_line matches the include target exactly", "[config_manager][include_line]") {
    const std::string absolute = "/mnt/UDISK/printer_data/config/helixscreen.conf";
    CHECK(MoonrakerConfigManager::has_include_line(
        "[server]\nhost: 0.0.0.0\n[include " + absolute + "]\n", absolute));
    // The K2 trap: a relative include exists and points somewhere else entirely
    CHECK_FALSE(MoonrakerConfigManager::has_include_line("[include helixscreen.conf]\n[server]\n",
                                                         absolute));
    // ...and an absolute include is not the relative default
    CHECK_FALSE(MoonrakerConfigManager::has_include_line("[include " + absolute + "]\n"));
}

TEST_CASE("add_include_line inserts an absolute target once, before the first section",
          "[config_manager][include_line]") {
    const std::string target = "/mnt/UDISK/printer_data/config/helixscreen.conf";
    const std::string out = MoonrakerConfigManager::add_include_line(
        "# vendor config\n[server]\nhost: 0.0.0.0\n", target);

    CHECK(out.find("[include " + target + "]") != std::string::npos);
    CHECK(MoonrakerConfigManager::has_include_line(out, target));
    CHECK(out.find("# vendor config") != std::string::npos);
    CHECK(out.find("host: 0.0.0.0") != std::string::npos);
    CHECK(out.find("[include ") < out.find("[server]"));
    CHECK(MoonrakerConfigManager::add_include_line(out, target) == out);
}

// ============================================================================
// set_existing_value: the update_manager channel sync
// ============================================================================

namespace {

std::string set_channel(const std::string& content, const std::string& want) {
    return MoonrakerConfigManager::set_existing_value(content, "update_manager helixscreen",
                                                      "channel", want);
}

const char* const kStanza = "[server]\n"
                            "host: 0.0.0.0\n"
                            "\n"
                            "# HelixScreen Update Manager\n"
                            "[update_manager helixscreen]\n"
                            "type: web\n"
                            "channel: stable\n"
                            "repo: prestonbrown/helixscreen\n"
                            "path: /opt/helixscreen\n"
                            "\n"
                            "[update_manager timelapse]\n"
                            "type: git_repo\n"
                            "channel: dev\n";

} // namespace

TEST_CASE("set_existing_value rewrites only the stanza's channel line",
          "[config_manager][update_channel]") {
    const std::string out = set_channel(kStanza, "beta");

    CHECK(MoonrakerConfigManager::get_section_value(out, "update_manager helixscreen", "channel") ==
          "beta");
    // Another section's channel line is not ours to touch.
    CHECK(MoonrakerConfigManager::get_section_value(out, "update_manager timelapse", "channel") ==
          "dev");
    CHECK(out.find("# HelixScreen Update Manager\n") != std::string::npos);
    CHECK(out.find("repo: prestonbrown/helixscreen\npath: /opt/helixscreen\n") !=
          std::string::npos);
    CHECK(out.find("channel: stable") == std::string::npos);
}

TEST_CASE("set_existing_value leaves a matching stanza byte-identical",
          "[config_manager][update_channel]") {
    CHECK(set_channel(kStanza, "stable") == kStanza);
    // Whitespace after the colon is formatting, not a different value.
    const std::string spaced = "[update_manager helixscreen]\nchannel:   beta  \n";
    CHECK(set_channel(spaced, "beta") == spaced);
}

TEST_CASE("set_existing_value ignores a channel line outside the stanza",
          "[config_manager][update_channel]") {
    // The helixscreen stanza has no channel line; the one after it belongs to timelapse.
    const std::string content = "[update_manager helixscreen]\n"
                                "type: web\n"
                                "[update_manager timelapse]\n"
                                "channel: stable\n";
    CHECK(set_channel(content, "beta") == content);
}

TEST_CASE("set_existing_value never adds a missing stanza or channel line",
          "[config_manager][update_channel]") {
    const std::string no_stanza = "[server]\nhost: 0.0.0.0\n[update_manager mainsail]\n"
                                  "channel: stable\n";
    CHECK(set_channel(no_stanza, "beta") == no_stanza);
    CHECK(set_channel("", "beta").empty());

    // Hand-written stanza with no channel: Moonraker's default, the operator's call.
    const std::string hand_written = "[update_manager helixscreen]\ntype: web\n";
    CHECK(set_channel(hand_written, "beta") == hand_written);

    // A commented-out channel is not a channel line.
    const std::string commented = "[update_manager helixscreen]\n#channel: stable\ntype: web\n";
    CHECK(set_channel(commented, "beta") == commented);
}

TEST_CASE("set_existing_value handles a stanza at end of file",
          "[config_manager][update_channel]") {
    const std::string content = "[server]\nhost: 0.0.0.0\n\n[update_manager helixscreen]\n"
                                "type: web\nchannel: beta";
    const std::string out = set_channel(content, "stable");
    CHECK(MoonrakerConfigManager::get_section_value(out, "update_manager helixscreen", "channel") ==
          "stable");
    CHECK(MoonrakerConfigManager::get_section_value(out, "server", "host") == "0.0.0.0");
}

TEST_CASE("set_existing_value reads and rewrites a CRLF file", "[config_manager][update_channel]") {
    const std::string crlf = "[server]\r\nhost: 0.0.0.0\r\n[update_manager helixscreen]\r\n"
                             "type: web\r\nchannel: stable\r\n";
    // A CR is line ending, not part of the value: a match is no change.
    CHECK(set_channel(crlf, "stable") == crlf);

    const std::string out = set_channel(crlf, "beta");
    CHECK(MoonrakerConfigManager::get_section_value(out, "update_manager helixscreen", "channel") ==
          "beta");
    CHECK(out == "[server]\r\nhost: 0.0.0.0\r\n[update_manager helixscreen]\r\n"
                 "type: web\r\nchannel: beta\r\n");
}

TEST_CASE("set_existing_value adds no newline the file did not end with",
          "[config_manager][update_channel]") {
    CHECK(set_channel("[update_manager helixscreen]\nchannel: beta", "stable") ==
          "[update_manager helixscreen]\nchannel: stable");
}

TEST_CASE("set_existing_value compares past an inline comment and keeps it",
          "[config_manager][update_channel]") {
    const std::string hash = "[update_manager helixscreen]\nchannel: beta  # pinned\n";
    CHECK(set_channel(hash, "beta") == hash);
    CHECK(set_channel(hash, "stable") ==
          "[update_manager helixscreen]\nchannel: stable  # pinned\n");

    const std::string semi = "[update_manager helixscreen]\nchannel: stable ; note\n";
    CHECK(set_channel(semi, "stable") == semi);
    CHECK(set_channel(semi, "beta") == "[update_manager helixscreen]\nchannel: beta ; note\n");
}

TEST_CASE("set_existing_value leaves duplicate stanzas alone", "[config_manager][update_channel]") {
    const std::string dup = "[update_manager helixscreen]\nchannel: stable\n"
                            "[server]\nhost: 0.0.0.0\n"
                            "[update_manager helixscreen]\ntype: web\n";
    CHECK(set_channel(dup, "beta") == dup);
}

// ============================================================================
// find_section_file: where Moonraker reads a section from
// ============================================================================

namespace {

constexpr const char* kHelixSection = "update_manager helixscreen";
const std::string kStanzaText = "[update_manager helixscreen]\ntype: web\nchannel: stable\n";
const std::string kServer = "[server]\nhost: 0.0.0.0\n";

std::string find_stanza(const std::map<std::string, std::string>& files) {
    return MoonrakerConfigManager::find_section_file(files, "moonraker.conf", kHelixSection);
}

} // namespace

TEST_CASE("find_section_file follows includes to the defining file",
          "[config_manager][update_channel_sync]") {
    using Files = std::map<std::string, std::string>;
    struct Row {
        const char* name;
        Files files;
        const char* expected;
    };
    const Row rows[] = {
        {"moonraker.conf itself", {{"moonraker.conf", kServer + kStanzaText}}, "moonraker.conf"},
        {"a direct include",
         {{"moonraker.conf", kServer + "[include mainsail.conf]\n"},
          {"mainsail.conf", kStanzaText}},
         "mainsail.conf"},
        {"a nested include",
         {{"moonraker.conf", "[include mainsail.conf]\n"},
          {"mainsail.conf", "[include helix/updates.conf]\n"},
          {"helix/updates.conf", kStanzaText}},
         "helix/updates.conf"},
        // conf.d/a.conf names b.conf, which is conf.d/b.conf, never the root's b.conf
        {"an include resolves against the including file's directory",
         {{"moonraker.conf", "[include conf.d/a.conf]\n"},
          {"conf.d/a.conf", "[include b.conf]\n"},
          {"conf.d/b.conf", kStanzaText},
          {"b.conf", "[server]\n"}},
         "conf.d/b.conf"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            CHECK(find_stanza(r.files) == r.expected);
        }
    }
}

TEST_CASE("find_section_file expands a glob include", "[config_manager][update_channel_sync]") {
    const std::map<std::string, std::string> files = {
        {"moonraker.conf", kServer + "[include conf.d/*.conf]\n"},
        {"conf.d/fluidd.conf", "[update_manager fluidd]\ntype: web\n"},
        {"conf.d/helix.conf", kStanzaText},
        {"conf.d/nested/other.conf", "[update_manager other]\n"},
    };
    CHECK(find_stanza(files) == "conf.d/helix.conf");

    SECTION("a single star never descends into a subdirectory") {
        auto nested = files;
        nested.erase("conf.d/helix.conf");
        nested["conf.d/nested/other.conf"] = kStanzaText;
        CHECK(find_stanza(nested).empty());
    }
}

TEST_CASE("find_section_file stops at an include cycle", "[config_manager][update_channel_sync]") {
    CHECK(find_stanza({{"moonraker.conf", "[include a.conf]\n"},
                       {"a.conf", "[include b.conf]\n"},
                       {"b.conf", "[include a.conf]\n" + kStanzaText}}) == "b.conf");
    CHECK(find_stanza({{"moonraker.conf", "[include moonraker.conf]\n" + kServer}}).empty());
}

TEST_CASE("find_section_file is empty when no file reached defines the stanza",
          "[config_manager][update_channel_sync]") {
    SECTION("no file defines it") {
        CHECK(find_stanza({{"moonraker.conf", kServer + "[include mainsail.conf]\n"},
                           {"mainsail.conf", "[update_manager mainsail]\n"}})
                  .empty());
    }
    SECTION("only a file nothing includes defines it") {
        CHECK(find_stanza({{"moonraker.conf", kServer}, {"mainsail.conf", kStanzaText}}).empty());
    }
    SECTION("a commented-out include reads nothing") {
        CHECK(find_stanza({{"moonraker.conf", kServer + "#[include mainsail.conf]\n"},
                           {"mainsail.conf", kStanzaText}})
                  .empty());
    }
    SECTION("no root file") {
        CHECK(find_stanza({{"mainsail.conf", kStanzaText}}).empty());
    }
}

TEST_CASE("find_section_file names the file Moonraker reads the stanza from last",
          "[config_manager][update_channel_sync]") {
    SECTION("an include after moonraker.conf's own stanza overrides it") {
        CHECK(find_stanza({{"moonraker.conf", kStanzaText + "[include mainsail.conf]\n"},
                           {"mainsail.conf", kStanzaText}}) == "mainsail.conf");
    }
    SECTION("moonraker.conf's stanza after the include overrides the included one") {
        CHECK(find_stanza({{"moonraker.conf", "[include mainsail.conf]\n" + kStanzaText},
                           {"mainsail.conf", kStanzaText}}) == "moonraker.conf");
    }
}
