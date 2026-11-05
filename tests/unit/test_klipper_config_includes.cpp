// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/update_queue.h"
#include "klipper_config_includes.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
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

    SECTION("Only a column-0 header counts") {
        std::string content = "  [include indented.cfg]\n"
                              "\t[include tabbed.cfg]\n"
                              "#[include commented.cfg]\n"
                              "[include real.cfg]\r\n";
        auto result = extract_includes(content);
        REQUIRE(result == std::vector<std::string>{"real.cfg"});
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

TEST_CASE("resolve_active_files - a file included from two places is read twice",
          "[config][includes]") {
    std::map<std::string, std::string> files = {
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n"},
        {"a.cfg", "[include common.cfg]\n"},
        {"b.cfg", "[include common.cfg]\n[include b.cfg]\n"},
        {"common.cfg", "[gcode_macro X]\ngcode: G28\n"},
    };

    std::vector<ConfigSegment> read_order;
    auto active = resolve_active_files(files, "printer.cfg", 5, &read_order);

    REQUIRE(active.size() == 4);
    std::vector<std::string> common_reads;
    for (const auto& segment : read_order) {
        if (segment.file == "common.cfg") {
            common_reads.push_back(segment.file);
        }
    }
    REQUIRE(common_reads.size() == 2);
    // b.cfg including itself is recursion, which Klipper refuses: read once.
    REQUIRE(std::count_if(read_order.begin(), read_order.end(), [](const ConfigSegment& s) {
                return s.file == "b.cfg" && s.begin == 0;
            }) == 1);
}

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
// Glob directory-separator semantics
//
// Klipper resolves includes with glob.glob(pattern, recursive=True)
// (klippy/configfile.py:451). In Python that makes `**` cross directory
// separators while a single `*` still never matches a '/'. Matching `/` with a
// single star silently pulls sections from nested files into the parent
// directory's include, so an edit lands in a file Klipper never read.
// ============================================================================

TEST_CASE("config_glob_match - single star stops at directory separator",
          "[config][includes][glob]") {
    SECTION("Single star does not cross a separator") {
        CHECK(config_glob_match("conf.d/*.cfg", "conf.d/options.cfg"));
        CHECK_FALSE(config_glob_match("conf.d/*.cfg", "conf.d/nested/deep.cfg"));
    }

    SECTION("Single star still matches within one path segment") {
        CHECK(config_glob_match("*.cfg", "printer.cfg"));
        CHECK_FALSE(config_glob_match("*.cfg", "conf.d/options.cfg"));
    }

    SECTION("Double star crosses separators") {
        CHECK(config_glob_match("conf.d/**/*.cfg", "conf.d/nested/deep.cfg"));
        CHECK_FALSE(config_glob_match("conf.d/**.cfg", "conf.d/nested/deep.cfg"));
        CHECK(config_glob_match("conf.d/**.cfg", "conf.d/deep.cfg"));
    }

    SECTION("Question mark does not match a separator") {
        CHECK(config_glob_match("conf.d/a?c.cfg", "conf.d/abc.cfg"));
        CHECK_FALSE(config_glob_match("a?c.cfg", "a/c.cfg"));
    }

    SECTION("Literal paths are unaffected") {
        CHECK(config_glob_match("conf.d/options.cfg", "conf.d/options.cfg"));
        CHECK_FALSE(config_glob_match("conf.d/options.cfg", "conf.d/other.cfg"));
    }
}

// ---------------------------------------------------------------------------
// download_include_graph
// ---------------------------------------------------------------------------

namespace {

/// Holds every download until the test answers it, so a case controls the order
/// and can read how many were outstanding at once.
struct DeferredDownloads {
    struct Pending {
        std::string path;
        helix::SharedCallback<void(std::string)> ok;
        helix::SharedCallback<void(std::string, bool)> fail;
    };

    std::map<std::string, std::string> server;
    std::vector<Pending> pending;
    std::vector<std::string> requested;
    size_t max_outstanding = 0;

    ConfigDownloadFn fn() {
        return [this](const std::string& path, helix::SharedCallback<void(std::string)> ok,
                      helix::SharedCallback<void(std::string, bool)> fail) {
            requested.push_back(path);
            pending.push_back({path, std::move(ok), std::move(fail)});
            max_outstanding = std::max(max_outstanding, pending.size());
        };
    }

    /// Answer the oldest outstanding download from `server`.
    bool answer_next() {
        if (pending.empty())
            return false;
        Pending p = std::move(pending.front());
        pending.erase(pending.begin());
        p.ok(server.at(p.path));
        return true;
    }

    void answer_all() {
        while (answer_next()) {
        }
    }

    bool fail_path(const std::string& path) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->path == path) {
                Pending p = std::move(*it);
                pending.erase(it);
                p.fail("connection reset", false);
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> listing() const {
        std::vector<std::string> out;
        for (const auto& [path, _] : server)
            out.push_back(path);
        return out;
    }
};

struct GraphResult {
    int completions = 0;
    int errors = 0;
    std::set<std::string> active;
    std::map<std::string, std::string> contents;
    std::string error;
};

void run_graph(DeferredDownloads& dl, GraphResult& r, size_t max_in_flight = 4) {
    download_include_graph(
        dl.listing(), "printer.cfg", dl.fn(),
        [&r](const std::set<std::string>& active, const std::map<std::string, std::string>& c) {
            ++r.completions;
            r.active = active;
            r.contents = c;
        },
        [&r](const std::string& err) {
            ++r.errors;
            r.error = err;
        },
        max_in_flight);
}

} // namespace

TEST_CASE("download_include_graph - fetches only files the include chain reaches",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include macros.cfg]\n[include conf.d/*.cfg]\n[include sub/x.cfg]\n"},
        {"macros.cfg", "[gcode_macro PRINT_START]\ngcode:\n  G28\n"},
        {"conf.d/a.cfg", "[fan]\n"},
        {"conf.d/b.cfg", "[heater_bed]\n"},
        {"conf.d/nested/c.cfg", "[never]\n"},
        {"config_backups/printer-20250205_154251.cfg", "[include macros.cfg]\n"},
        {"sub/x.cfg", "[include y.cfg]\n"},
        {"sub/y.cfg", "[stepper_x]\n"},
        {"unrelated.cfg", "[nope]\n"},
    };
    GraphResult r;
    run_graph(dl, r);
    dl.answer_all();

    const std::set<std::string> expected = {"printer.cfg",  "macros.cfg", "conf.d/a.cfg",
                                            "conf.d/b.cfg", "sub/x.cfg",  "sub/y.cfg"};
    CHECK(std::set<std::string>(dl.requested.begin(), dl.requested.end()) == expected);
    CHECK(dl.requested.size() == expected.size()); // nothing fetched twice
    REQUIRE(r.completions == 1);
    CHECK(r.errors == 0);
    CHECK(r.active == expected);
    CHECK(r.contents.at("macros.cfg") == dl.server.at("macros.cfg"));
    CHECK(r.contents.count("config_backups/printer-20250205_154251.cfg") == 0);
}

TEST_CASE("download_include_graph - never has more than the limit outstanding",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    for (int i = 0; i < 12; ++i) {
        const std::string name = "conf.d/f" + std::to_string(i) + ".cfg";
        dl.server[name] = "[x" + std::to_string(i) + "]\n";
    }
    dl.server["printer.cfg"] = "[include conf.d/*.cfg]\n";
    GraphResult r;
    run_graph(dl, r, 4);
    dl.answer_all();

    REQUIRE(r.completions == 1);
    CHECK(dl.requested.size() == 13);
    CHECK(dl.max_outstanding == 4);
    CHECK(r.active.size() == 13);
}

TEST_CASE("download_include_graph - a failed download reports an error, not a partial result",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n[include c.cfg]\n"},
        {"a.cfg", "[a]\n"},
        {"b.cfg", "[gcode_macro PRINT_START]\ngcode:\n  G28\n"},
        {"c.cfg", "[include d.cfg]\n"},
        {"d.cfg", "[d]\n"},
    };
    GraphResult r;
    run_graph(dl, r);
    REQUIRE(dl.answer_next()); // printer.cfg
    REQUIRE(dl.fail_path("b.cfg"));
    CHECK(r.errors == 0); // a.cfg and c.cfg are still outstanding
    dl.answer_all();

    CHECK(r.completions == 0);
    REQUIRE(r.errors == 1);
    CHECK(r.error.find("b.cfg") != std::string::npos);
    // Nothing new is started once the result is known to be incomplete.
    CHECK(std::count(dl.requested.begin(), dl.requested.end(), "d.cfg") == 0);
}

TEST_CASE("download_include_graph - a download rejected before it returns reports once",
          "[config][includes][include_graph]") {
    std::map<std::string, std::string> server = {
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n"},
        {"a.cfg", "[a]\n"},
        {"b.cfg", "[b]\n"},
    };
    std::vector<std::string> listing;
    for (const auto& [p, _] : server)
        listing.push_back(p);
    int completions = 0;
    int errors = 0;
    download_include_graph(
        listing, "printer.cfg",
        [&server](const std::string& path, helix::SharedCallback<void(std::string)> ok,
                  helix::SharedCallback<void(std::string, bool)> fail) {
            if (path == "printer.cfg")
                ok(server.at(path));
            else
                fail("HTTP request could not be queued", true);
        },
        [&](const std::set<std::string>&, const std::map<std::string, std::string>&) {
            ++completions;
        },
        [&](const std::string&) { ++errors; });

    CHECK(completions == 0);
    CHECK(errors == 1);
}

TEST_CASE("download_include_graph - a full queue is backpressure while downloads are in flight",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n[include c.cfg]\n"},
        {"a.cfg", "[a]\n"},
        {"b.cfg", "[b]\n"},
        {"c.cfg", "[c]\n"},
    };
    // The lane refuses c.cfg once, before returning, as a full HTTP queue does.
    int rejections_left = 1;
    auto deferred = dl.fn();
    GraphResult r;
    download_include_graph(
        dl.listing(), "printer.cfg",
        [&](const std::string& path, helix::SharedCallback<void(std::string)> ok,
            helix::SharedCallback<void(std::string, bool)> fail) {
            if (path == "c.cfg" && rejections_left > 0) {
                --rejections_left;
                dl.requested.push_back(path);
                fail("HTTP request could not be queued", true);
                return;
            }
            deferred(path, std::move(ok), std::move(fail));
        },
        [&r](const std::set<std::string>& active, const std::map<std::string, std::string>& c) {
            ++r.completions;
            r.active = active;
            r.contents = c;
        },
        [&r](const std::string& err) {
            ++r.errors;
            r.error = err;
        });
    dl.answer_all();

    REQUIRE(rejections_left == 0); // the rejection happened
    CHECK(r.errors == 0);
    REQUIRE(r.completions == 1);
    CHECK(r.active == std::set<std::string>{"printer.cfg", "a.cfg", "b.cfg", "c.cfg"});
    CHECK(std::count(dl.requested.begin(), dl.requested.end(), "c.cfg") == 2);
}

TEST_CASE("download_include_graph - an include cycle terminates and fetches each file once",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include a.cfg]\n"},
        {"a.cfg", "[include b.cfg]\n"},
        {"b.cfg", "[include a.cfg]\n"},
    };
    GraphResult r;
    run_graph(dl, r);
    dl.answer_all();

    REQUIRE(r.completions == 1);
    CHECK(dl.requested.size() == 3);
    CHECK(r.active == std::set<std::string>{"printer.cfg", "a.cfg", "b.cfg"});
}

TEST_CASE("download_include_graph - a file included by two parents is fetched once",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n"},
        {"a.cfg", "[include shared.cfg]\n"},
        {"b.cfg", "[include shared.cfg]\n"},
        {"shared.cfg", "[shared]\n"},
    };
    GraphResult r;
    run_graph(dl, r);
    dl.answer_all();

    REQUIRE(r.completions == 1);
    CHECK(std::count(dl.requested.begin(), dl.requested.end(), "shared.cfg") == 1);
    CHECK(r.active.count("shared.cfg") == 1);
}

TEST_CASE("download_include_graph - a request refused for anything but a full queue is not retried",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n"},
        {"a.cfg", "[a]\n"},
        {"b.cfg", "[b]\n"},
    };
    auto deferred = dl.fn();
    GraphResult r;
    download_include_graph(
        dl.listing(), "printer.cfg",
        [&](const std::string& path, helix::SharedCallback<void(std::string)> ok,
            helix::SharedCallback<void(std::string, bool)> fail) {
            if (path == "b.cfg") {
                dl.requested.push_back(path);
                fail("invalid path", false); // before returning, with a.cfg in flight
                return;
            }
            deferred(path, std::move(ok), std::move(fail));
        },
        [&r](const std::set<std::string>&, const std::map<std::string, std::string>&) {
            ++r.completions;
        },
        [&r](const std::string& err) {
            ++r.errors;
            r.error = err;
        });
    dl.answer_all();

    CHECK(r.completions == 0);
    REQUIRE(r.errors == 1);
    CHECK(r.error.find("b.cfg") != std::string::npos);
    CHECK(std::count(dl.requested.begin(), dl.requested.end(), "b.cfg") == 1);
}

// ---------------------------------------------------------------------------
// api_config_download: the walk never reaches a destroyed API
// ---------------------------------------------------------------------------

namespace {

/// One printer whose API the test can destroy mid-walk.
struct TornDownPrinter {
    helix::PrinterState state;
    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPIMock> api = std::make_unique<MoonrakerAPIMock>(client, state);

    TornDownPrinter() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        api->set_config_files({{"printer.cfg", "[include a.cfg]\n[printer]\n"},
                               {"a.cfg", "[include b.cfg]\n[a]\n"},
                               {"b.cfg", "[b]\n"}});
    }

    /// Runs the request queued for the walk's next download.
    static void step() {
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE("api_config_download issues no request once the API is destroyed",
          "[config][includes][include_walk_lifetime]") {
    TornDownPrinter printer;
    int completions = 0;
    std::string error;
    resolve_active_config_files_with_content(
        *printer.api,
        [&](const std::set<std::string>&, const std::map<std::string, std::string>&) {
            ++completions;
        },
        [&](const std::string& err) { error = err; });

    TornDownPrinter::step(); // printer.cfg arrives; a.cfg is requested
    printer.api.reset();     // printer teardown, between two downloads
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(completions == 0);
    CHECK(error.find("disconnected") != std::string::npos);
}

TEST_CASE("api_config_download walks the whole chain while the API lives",
          "[config][includes][include_walk_lifetime]") {
    TornDownPrinter printer;
    std::set<std::string> active;
    std::string error;
    resolve_active_config_files_with_content(
        *printer.api,
        [&](const std::set<std::string>& files, const std::map<std::string, std::string>&) {
            active = files;
        },
        [&](const std::string& err) { error = err; });
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(error.empty());
    CHECK(active == std::set<std::string>{"printer.cfg", "a.cfg", "b.cfg"});
}

TEST_CASE("api_config_download stops when its caller no longer wants the walk",
          "[config][includes][include_walk_lifetime]") {
    TornDownPrinter printer;
    bool wanted = true;
    int completions = 0;
    std::string error;
    download_config_include_graph(
        *printer.api, "printer.cfg", ".cfg",
        [&](const std::set<std::string>&, const std::map<std::string, std::string>&) {
            ++completions;
        },
        [&](const std::string& err) { error = err; },
        api_config_download(*printer.api, [&wanted] { return wanted; }));

    TornDownPrinter::step();
    wanted = false;
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(completions == 0);
    CHECK_FALSE(error.empty());
}

TEST_CASE("download_include_graph - a full queue reported after the call returns is backpressure",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n"},
        {"a.cfg", "[a]\n"},
        {"b.cfg", "[b]\n"},
    };
    // b.cfg is handed off and refused later, while a.cfg is still in flight.
    helix::SharedCallback<void(std::string, bool)> refuse_b;
    auto deferred = dl.fn();
    GraphResult r;
    download_include_graph(
        dl.listing(), "printer.cfg",
        [&](const std::string& path, helix::SharedCallback<void(std::string)> ok,
            helix::SharedCallback<void(std::string, bool)> fail) {
            if (path == "b.cfg" && !refuse_b) {
                dl.requested.push_back(path);
                refuse_b = std::move(fail);
                return;
            }
            deferred(path, std::move(ok), std::move(fail));
        },
        [&r](const std::set<std::string>& active, const std::map<std::string, std::string>&) {
            ++r.completions;
            r.active = active;
        },
        [&r](const std::string& err) {
            ++r.errors;
            r.error = err;
        });
    dl.answer_next(); // printer.cfg: a.cfg and b.cfg are requested
    REQUIRE(refuse_b);
    refuse_b("HTTP request could not be queued", true);
    dl.answer_all();

    CHECK(r.errors == 0);
    REQUIRE(r.completions == 1);
    CHECK(r.active == std::set<std::string>{"printer.cfg", "a.cfg", "b.cfg"});
    CHECK(std::count(dl.requested.begin(), dl.requested.end(), "b.cfg") == 2);
}

namespace {

/// Answers listings from DeferredDownloads::server, recording what was asked for.
struct RecordingLister {
    const DeferredDownloads& dl;
    std::vector<std::string> dirs;
    int whole_trees = 0;

    ConfigListFn fn() {
        return [this](const std::string& dir, bool whole_tree,
                      helix::SharedCallback<void(std::vector<std::string>)> ok,
                      helix::SharedCallback<void(std::string, bool)>) {
            if (whole_tree)
                ++whole_trees;
            else
                dirs.push_back(dir);
            std::vector<std::string> out;
            for (const auto& [path, _] : dl.server) {
                if (whole_tree || config_get_directory(path) == dir)
                    out.push_back(path);
            }
            ok(std::move(out));
        };
    }
};

void run_walk(DeferredDownloads& dl, ConfigListFn list, GraphResult& r) {
    walk_include_graph(
        "printer.cfg", std::move(list), dl.fn(),
        [&r](const std::set<std::string>& active, const std::map<std::string, std::string>& c) {
            ++r.completions;
            r.active = active;
            r.contents = c;
        },
        [&r](const std::string& err) {
            ++r.errors;
            r.error = err;
        });
}

} // namespace

TEST_CASE("walk_include_graph lists only the directories the includes name",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include macros.cfg]\n[include conf.d/*.cfg]\n[include conf.d/z.cfg]\n"},
        {"macros.cfg", "[m]\n"},
        {"conf.d/a.cfg", "[a]\n"},
        {"conf.d/z.cfg", "[z]\n"},
        {"mod/huge/never.cfg", "[never]\n"},
    };
    RecordingLister lister{dl};
    GraphResult r;
    run_walk(dl, lister.fn(), r);
    dl.answer_all();

    CHECK(lister.whole_trees == 0);
    CHECK(std::set<std::string>(lister.dirs.begin(), lister.dirs.end()) ==
          std::set<std::string>{"", "conf.d"});
    CHECK(lister.dirs.size() == 2); // a directory is listed once
    REQUIRE(r.completions == 1);
    CHECK(r.active ==
          std::set<std::string>{"printer.cfg", "macros.cfg", "conf.d/a.cfg", "conf.d/z.cfg"});
}

TEST_CASE("walk_include_graph needs the whole tree only for a wildcard directory",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include */x.cfg]\n[include deep/**/y.cfg]\n"},
        {"a/x.cfg", "[x]\n"},
        {"deep/b/c/y.cfg", "[y]\n"},
    };
    RecordingLister lister{dl};
    GraphResult r;
    run_walk(dl, lister.fn(), r);
    dl.answer_all();

    CHECK(lister.whole_trees == 1);
    REQUIRE(r.completions == 1);
    CHECK(r.active == std::set<std::string>{"printer.cfg", "a/x.cfg", "deep/b/c/y.cfg"});
}

TEST_CASE("walk_include_graph skips an include whose directory holds nothing",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {{"printer.cfg", "[include gone/a.cfg]\n[include ../b.cfg]\n[include /c.cfg]\n"
                                 "[include a..b/d.cfg]\n"}};
    RecordingLister lister{dl};
    GraphResult r;
    run_walk(dl, lister.fn(), r);
    dl.answer_all();

    CHECK(r.errors == 0);
    REQUIRE(r.completions == 1);
    CHECK(r.active == std::set<std::string>{"printer.cfg"});
    // A path no listing can name is never asked about.
    CHECK(std::set<std::string>(lister.dirs.begin(), lister.dirs.end()) ==
          std::set<std::string>{"", "gone"});
}

TEST_CASE("walk_include_graph completes empty when the root file is absent",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {{"other.cfg", "[o]\n"}};
    RecordingLister lister{dl};
    GraphResult r;
    run_walk(dl, lister.fn(), r);

    REQUIRE(r.completions == 1);
    CHECK(r.errors == 0);
    CHECK(r.active.empty());
    CHECK(dl.requested.empty());
}

TEST_CASE("walk_include_graph reports a failed listing as an error",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {{"printer.cfg", "[p]\n"}};
    ConfigListFn list = [](const std::string&, bool,
                           helix::SharedCallback<void(std::vector<std::string>)>,
                           helix::SharedCallback<void(std::string, bool)> fail) {
        fail("connection reset", false);
    };
    GraphResult r;
    run_walk(dl, std::move(list), r);

    CHECK(r.completions == 0);
    REQUIRE(r.errors == 1);
    CHECK(r.error == "Failed to list config files: connection reset");
}

TEST_CASE("walk_include_graph retries a listing refused for a full queue",
          "[config][includes][include_graph]") {
    DeferredDownloads dl;
    dl.server = {
        {"printer.cfg", "[include one/a.cfg]\n[include two/b.cfg]\n"},
        {"one/a.cfg", "[a]\n"},
        {"two/b.cfg", "[b]\n"},
    };
    using Ok = helix::SharedCallback<void(std::vector<std::string>)>;
    std::vector<std::pair<std::string, Ok>> held;
    int two_requests = 0;
    ConfigListFn list = [&](const std::string& dir, bool, Ok ok,
                            helix::SharedCallback<void(std::string, bool)> fail) {
        if (dir == "two" && ++two_requests == 1) {
            fail("queue full", true); // "one" is still outstanding
            return;
        }
        held.emplace_back(dir, std::move(ok));
    };
    GraphResult r;
    run_walk(dl, std::move(list), r);
    auto answer_held = [&] {
        while (!held.empty()) {
            auto [dir, ok] = std::move(held.front());
            held.erase(held.begin());
            std::vector<std::string> out;
            for (const auto& [path, _] : dl.server) {
                if (config_get_directory(path) == dir)
                    out.push_back(path);
            }
            ok(std::move(out));
        }
    };
    answer_held();
    dl.answer_all();
    answer_held();
    dl.answer_all();
    answer_held();
    dl.answer_all();

    CHECK(two_requests == 2);
    CHECK(r.errors == 0);
    REQUIRE(r.completions == 1);
    CHECK(r.active == std::set<std::string>{"printer.cfg", "one/a.cfg", "two/b.cfg"});
}

namespace {

/// Counts every whole-root listing the API layer is asked for.
class ListingSpyFiles : public MoonrakerFileAPIMock {
  public:
    using MoonrakerFileAPIMock::MoonrakerFileAPIMock;
    int whole_root_lists = 0;
    std::vector<std::string> dirs_listed;

    void get_directory(const std::string& root, const std::string& path,
                       FileListCallback on_success, ErrorCallback on_error) override {
        dirs_listed.push_back(path);
        inside_directory_ = true;
        MoonrakerFileAPIMock::get_directory(root, path, std::move(on_success), std::move(on_error));
        inside_directory_ = false;
    }

    void list_files(const std::string& root, const std::string& path, bool recursive,
                    FileListCallback on_success, ErrorCallback on_error) override {
        if (!inside_directory_)
            ++whole_root_lists;
        MoonrakerFileAPIMock::list_files(root, path, recursive, std::move(on_success),
                                         std::move(on_error));
    }

  private:
    bool inside_directory_ = false;
};

class ListingSpyApi : public MoonrakerAPIMock {
  public:
    ListingSpyApi(helix::MoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPIMock(client, state), spy_(client) {}
    MoonrakerFileAPI& files() override {
        return spy_;
    }
    ListingSpyFiles spy_;
};

} // namespace

TEST_CASE("download_config_include_graph never walks the config root for plain includes",
          "[config][includes][include_graph]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    ListingSpyApi api(client, state);
    const std::map<std::string, std::string> files = {
        {"printer.cfg", "[include conf.d/*.cfg]\n"},
        {"conf.d/a.cfg", "[a]\n"},
        {"conf.d/b.txt", "[not a config]\n"},
        {"mod_data/helixscreen/config/z.cfg", "[z]\n"},
    };
    api.set_config_files(files);
    api.spy_.set_config_files(files);

    std::set<std::string> active;
    std::string error;
    download_config_include_graph(
        api, "printer.cfg", ".cfg",
        [&](const std::set<std::string>& a, const std::map<std::string, std::string>&) {
            active = a;
        },
        [&](const std::string& err) { error = err; });
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(error.empty());
    CHECK(active == std::set<std::string>{"printer.cfg", "conf.d/a.cfg"});
    CHECK(api.spy_.whole_root_lists == 0);
    CHECK(std::set<std::string>(api.spy_.dirs_listed.begin(), api.spy_.dirs_listed.end()) ==
          std::set<std::string>{"", "conf.d"});
}
