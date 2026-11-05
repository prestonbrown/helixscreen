// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_includes.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::system;

// ---------------------------------------------------------------------------
// extract_includes
// ---------------------------------------------------------------------------

TEST_CASE("extract_includes - parses include directives", "[config][includes]") {
    SECTION("Single include") {
        std::string content = "[include macros.cfg]\n[printer]\nkinematics: corexy\n";
        auto result = extract_includes(content);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "macros.cfg");
    }

    SECTION("Multiple includes") {
        std::string content = "[include macros.cfg]\n"
                              "[include conf.d/motors.cfg]\n"
                              "[printer]\n"
                              "kinematics: corexy\n"
                              "[include extras.cfg]\n";
        auto result = extract_includes(content);
        REQUIRE(result.size() == 3);
        REQUIRE(result[0] == "macros.cfg");
        REQUIRE(result[1] == "conf.d/motors.cfg");
        REQUIRE(result[2] == "extras.cfg");
    }

    SECTION("Glob include") {
        std::string content = "[include conf.d/*.cfg]\n";
        auto result = extract_includes(content);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "conf.d/*.cfg");
    }

    SECTION("No includes") {
        std::string content = "[printer]\n"
                              "kinematics: corexy\n"
                              "[stepper_x]\n"
                              "step_pin: PA0\n";
        auto result = extract_includes(content);
        REQUIRE(result.empty());
    }

    SECTION("Mixed content - includes among regular sections") {
        std::string content = "[printer]\n"
                              "kinematics: corexy\n"
                              "[include macros.cfg]\n"
                              "[stepper_x]\n"
                              "step_pin: PA0\n";
        auto result = extract_includes(content);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "macros.cfg");
    }

    SECTION("Whitespace in include directive") {
        std::string content = "[include  macros.cfg ]\n";
        auto result = extract_includes(content);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "macros.cfg");
    }
}

// ---------------------------------------------------------------------------
// config_get_directory
// ---------------------------------------------------------------------------

TEST_CASE("config_get_directory - extracts parent directory", "[config][includes]") {
    SECTION("Root file returns empty string") {
        REQUIRE(config_get_directory("printer.cfg") == "");
    }

    SECTION("Subdirectory file") {
        REQUIRE(config_get_directory("conf.d/macros.cfg") == "conf.d");
    }

    SECTION("Nested path") {
        REQUIRE(config_get_directory("a/b/c.cfg") == "a/b");
    }
}

// ---------------------------------------------------------------------------
// config_resolve_path
// ---------------------------------------------------------------------------

TEST_CASE("config_resolve_path - resolves include relative to current file", "[config][includes]") {
    SECTION("Root-level include from root file") {
        REQUIRE(config_resolve_path("printer.cfg", "macros.cfg") == "macros.cfg");
    }

    SECTION("Subdirectory include from root file") {
        REQUIRE(config_resolve_path("printer.cfg", "conf.d/macros.cfg") == "conf.d/macros.cfg");
    }

    SECTION("Nested include from subdirectory file") {
        REQUIRE(config_resolve_path("conf.d/base.cfg", "extras/more.cfg") ==
                "conf.d/extras/more.cfg");
    }
}

// ---------------------------------------------------------------------------
// config_glob_match
// ---------------------------------------------------------------------------

TEST_CASE("config_glob_match - glob pattern matching", "[config][includes]") {
    SECTION("Exact match") {
        REQUIRE(config_glob_match("macros.cfg", "macros.cfg") == true);
    }

    SECTION("Wildcard matches") {
        REQUIRE(config_glob_match("conf.d/*.cfg", "conf.d/macros.cfg") == true);
    }

    SECTION("Wildcard does not match different directory") {
        REQUIRE(config_glob_match("conf.d/*.cfg", "other/macros.cfg") == false);
    }

    SECTION("Question mark matches single character") {
        REQUIRE(config_glob_match("macro?.cfg", "macros.cfg") == true);
    }

    SECTION("Question mark does not match multiple characters") {
        REQUIRE(config_glob_match("macro?.cfg", "macross.cfg") == false);
    }

    SECTION("Star matches empty string") {
        REQUIRE(config_glob_match("*.cfg", ".cfg") == true);
    }
}

// ---------------------------------------------------------------------------
// config_match_glob
// ---------------------------------------------------------------------------

TEST_CASE("config_match_glob - matches files against glob pattern", "[config][includes]") {
    SECTION("Basic glob matches files in subdirectory") {
        std::map<std::string, std::string> files = {
            {"printer.cfg", ""},
            {"macros/start.cfg", ""},
            {"macros/end.cfg", ""},
            {"other.cfg", ""},
        };

        auto result = config_match_glob(files, "printer.cfg", "macros/*.cfg");
        REQUIRE(result.size() == 2);
        // Result should be sorted
        REQUIRE(result[0] == "macros/end.cfg");
        REQUIRE(result[1] == "macros/start.cfg");
    }
}

// ---------------------------------------------------------------------------
// resolve_active_files - core integration tests
// ---------------------------------------------------------------------------

TEST_CASE("resolve_active_files - determines active config files", "[config][includes]") {
    SECTION("Simple chain - one include") {
        std::map<std::string, std::string> files = {
            {"printer.cfg", "[include macros.cfg]\n[printer]\nkinematics: corexy\n"},
            {"macros.cfg", "[gcode_macro START]\ngcode: G28\n"},
        };

        auto active = resolve_active_files(files, "printer.cfg");
        REQUIRE(active.count("printer.cfg") == 1);
        REQUIRE(active.count("macros.cfg") == 1);
    }

    SECTION("Glob includes match multiple files") {
        std::map<std::string, std::string> files = {
            {"printer.cfg", "[include conf.d/*.cfg]\n"},
            {"conf.d/a.cfg", "[stepper_x]\nstep_pin: PA0\n"},
            {"conf.d/b.cfg", "[stepper_y]\nstep_pin: PA1\n"},
            {"backup.cfg", "[printer]\nkinematics: cartesian\n"},
        };

        auto active = resolve_active_files(files, "printer.cfg");
        REQUIRE(active.size() == 3);
        REQUIRE(active.count("printer.cfg") == 1);
        REQUIRE(active.count("conf.d/a.cfg") == 1);
        REQUIRE(active.count("conf.d/b.cfg") == 1);
        REQUIRE(active.count("backup.cfg") == 0);
    }

    SECTION("Nested includes - 3 levels deep") {
        std::map<std::string, std::string> files = {
            {"printer.cfg", "[include macros.cfg]\n"},
            {"macros.cfg", "[include helpers.cfg]\n"},
            {"helpers.cfg", "[gcode_macro HELPER]\ngcode: M117 hi\n"},
        };

        auto active = resolve_active_files(files, "printer.cfg");
        REQUIRE(active.size() == 3);
        REQUIRE(active.count("printer.cfg") == 1);
        REQUIRE(active.count("macros.cfg") == 1);
        REQUIRE(active.count("helpers.cfg") == 1);
    }

    SECTION("Circular includes do not cause infinite loop") {
        std::map<std::string, std::string> files = {
            {"a.cfg", "[include b.cfg]\n"},
            {"b.cfg", "[include a.cfg]\n"},
        };

        auto active = resolve_active_files(files, "a.cfg");
        REQUIRE(active.size() == 2);
        REQUIRE(active.count("a.cfg") == 1);
        REQUIRE(active.count("b.cfg") == 1);
    }

    SECTION("Max depth enforcement stops deep chains") {
        std::map<std::string, std::string> files = {
            {"f0.cfg", "[include f1.cfg]\n"}, {"f1.cfg", "[include f2.cfg]\n"},
            {"f2.cfg", "[include f3.cfg]\n"}, {"f3.cfg", "[include f4.cfg]\n"},
            {"f4.cfg", "[include f5.cfg]\n"}, {"f5.cfg", "[include f6.cfg]\n"},
            {"f6.cfg", "# leaf\n"},
        };

        // max_depth=5 means depths 0-5 are processed (6 files), depth 6 is not
        auto active = resolve_active_files(files, "f0.cfg", 5);
        REQUIRE(active.size() == 6);
        REQUIRE(active.count("f0.cfg") == 1);
        REQUIRE(active.count("f1.cfg") == 1);
        REQUIRE(active.count("f2.cfg") == 1);
        REQUIRE(active.count("f3.cfg") == 1);
        REQUIRE(active.count("f4.cfg") == 1);
        REQUIRE(active.count("f5.cfg") == 1);
        REQUIRE(active.count("f6.cfg") == 0);
    }

    SECTION("Missing included file does not crash") {
        std::map<std::string, std::string> files = {
            {"printer.cfg", "[include nonexistent.cfg]\n"},
        };

        auto active = resolve_active_files(files, "printer.cfg");
        REQUIRE(active.size() == 1);
        REQUIRE(active.count("printer.cfg") == 1);
    }

    SECTION("Backup files excluded when not included") {
        std::map<std::string, std::string> files = {
            {"printer.cfg", "[include macros.cfg]\n[printer]\nkinematics: corexy\n"},
            {"macros.cfg", "[gcode_macro START]\ngcode: G28\n"},
            {"printer-backup.cfg", "[printer]\nkinematics: cartesian\n"},
            {"macros-old.cfg", "[gcode_macro OLD]\ngcode: M0\n"},
        };

        auto active = resolve_active_files(files, "printer.cfg");
        REQUIRE(active.size() == 2);
        REQUIRE(active.count("printer.cfg") == 1);
        REQUIRE(active.count("macros.cfg") == 1);
        REQUIRE(active.count("printer-backup.cfg") == 0);
        REQUIRE(active.count("macros-old.cfg") == 0);
    }

    SECTION("Realistic multi-file config with globs and explicit includes") {
        std::map<std::string, std::string> files = {
            {"printer.cfg", "[include macros.cfg]\n"
                            "[include conf.d/*.cfg]\n"
                            "[printer]\n"
                            "kinematics: corexy\n"},
            {"macros.cfg", "[gcode_macro START]\ngcode: G28\n"},
            {"conf.d/motor.cfg", "[stepper_x]\nstep_pin: PA0\n"},
            {"conf.d/fans.cfg", "[fan]\npin: PA2\n"},
            {"old-printer.cfg", "[printer]\nkinematics: cartesian\n"},
            {"test.cfg", "[gcode_macro TEST]\ngcode: M0\n"},
        };

        auto active = resolve_active_files(files, "printer.cfg");
        REQUIRE(active.size() == 4);
        REQUIRE(active.count("printer.cfg") == 1);
        REQUIRE(active.count("macros.cfg") == 1);
        REQUIRE(active.count("conf.d/motor.cfg") == 1);
        REQUIRE(active.count("conf.d/fans.cfg") == 1);
        REQUIRE(active.count("old-printer.cfg") == 0);
        REQUIRE(active.count("test.cfg") == 0);
    }
}

// ============================================================================
// walk_config_includes
// ============================================================================

namespace {

struct WalkFakes {
    std::map<std::string, std::string> server;
    std::vector<std::string> dirs_listed;
    int whole_trees = 0;
    std::vector<std::string> fetched;
    std::set<std::string> active;
    int completions = 0;
    std::string error;
    bool fail_list = false;
    bool fatal_fetch = false;

    void run() {
        walk_config_includes(
            "printer.cfg",
            [this](const std::string& dir, bool whole, std::function<void(std::vector<std::string>)> ok,
                   std::function<void(std::string)> fail) {
                if (fail_list) {
                    fail("reset");
                    return;
                }
                whole ? (void)++whole_trees : dirs_listed.push_back(dir);
                std::vector<std::string> out;
                for (const auto& [path, _] : server) {
                    if (whole || config_get_directory(path) == dir)
                        out.push_back(path);
                }
                ok(std::move(out));
            },
            [this](const std::string& path, std::function<void(std::string)> ok,
                   std::function<void(std::string, bool)> fail) {
                if (fatal_fetch) {
                    fail("the printer disconnected", true);
                    return;
                }
                fetched.push_back(path);
                ok(server.at(path));
            },
            [this](const std::set<std::string>& a, const std::map<std::string, std::string>&) {
                ++completions;
                active = a;
            },
            [this](const std::string& e) { error = e; });
    }
};

} // namespace

TEST_CASE("walk_config_includes lists only the directories includes name", "[config][includes]") {
    WalkFakes w;
    w.server = {{"printer.cfg", "[include macros.cfg]\n[include conf.d/*.cfg]\n"},
                {"macros.cfg", "[m]\n"},
                {"conf.d/a.cfg", "[a]\n"},
                {"mod/huge/never.cfg", "[never]\n"}};
    w.run();

    CHECK(w.whole_trees == 0);
    CHECK(std::set<std::string>(w.dirs_listed.begin(), w.dirs_listed.end()) ==
          std::set<std::string>{"", "conf.d"});
    CHECK(w.dirs_listed.size() == 2);
    CHECK(w.fetched.size() == 3); // only the include chain is downloaded
    REQUIRE(w.completions == 1);
    CHECK(w.active == std::set<std::string>{"printer.cfg", "macros.cfg", "conf.d/a.cfg"});
}

TEST_CASE("walk_config_includes needs the whole tree only for a wildcard directory",
          "[config][includes]") {
    WalkFakes w;
    w.server = {{"printer.cfg", "[include */x.cfg]\n[include gone/y.cfg]\n[include ../z.cfg]\n"},
                {"a/x.cfg", "[x]\n"}};
    w.run();

    CHECK(w.whole_trees == 1);
    REQUIRE(w.completions == 1);
    CHECK(w.active == std::set<std::string>{"printer.cfg", "a/x.cfg"});
}

TEST_CASE("walk_config_includes reports a missing root and a failed listing", "[config][includes]") {
    WalkFakes absent;
    absent.server = {{"other.cfg", "[o]\n"}};
    absent.run();
    CHECK(absent.completions == 1);
    CHECK(absent.active.empty());
    CHECK(absent.fetched.empty());

    WalkFakes failed;
    failed.fail_list = true;
    failed.run();
    CHECK(failed.completions == 0);
    CHECK(failed.error == "Failed to list config files: reset");
}

TEST_CASE("walk_config_includes stops on a fatal download failure", "[config][includes]") {
    WalkFakes w;
    w.server = {{"printer.cfg", "[p]\n"}};
    w.fatal_fetch = true;
    w.run();

    CHECK(w.completions == 0);
    CHECK(w.error == "Failed to download printer.cfg: the printer disconnected");
}
