// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_env_refusal_notice.cpp
 * @brief Unit tests for the launcher's helixscreen.env refusal handoff
 *        (prestonbrown/helixscreen#1712)
 *
 * The launcher exports HELIX_ENV_FILE_REFUSED (kind|detail|expected|path) /
 * HELIX_ENV_LINES_SKIPPED; the app parses them, picks the user-facing copy
 * for the refusal kind, and notifies through the boot-warning toast channel.
 * The parsers and the copy mapping are pure functions; the surface function
 * is observed through the test notification hooks, which replace
 * ui_notification_warning() / _sticky() in the test binary (the
 * PendingStartupWarnings defer sits inside the real implementations, so the
 * strings asserted here are the ones the production channel receives). The
 * whole-file refusal must take the sticky channel (it never auto-dismisses);
 * the skipped-lines notice takes the timed one.
 */

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "env_refusal_notice.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// parse_env_lines_skipped
// ============================================================================

TEST_CASE("env-refusal: empty and absent values parse to no entries", "[1712]") {
    CHECK(parse_env_lines_skipped("").empty());
    CHECK(parse_env_lines_skipped("||||").empty());
}

TEST_CASE("env-refusal: one entry splits on the first colon", "[1712]") {
    const auto entries = parse_env_lines_skipped("LD_PRELOAD:not a setting this file may change");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].label == "LD_PRELOAD");
    CHECK(entries[0].reason == "not a setting this file may change");
}

TEST_CASE("env-refusal: a reason may itself contain colons", "[1712]") {
    const auto entries =
        parse_env_lines_skipped("HELIX_ALSA_DEVICE:must be default, sysdefault:... or hw:...");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].label == "HELIX_ALSA_DEVICE");
    CHECK(entries[0].reason == "must be default, sysdefault:... or hw:...");
}

TEST_CASE("env-refusal: many entries split on the separator", "[1712]") {
    const auto entries =
        parse_env_lines_skipped("line 3:malformed line|LD_PRELOAD:not a setting this file may "
                                "change|MOONRAKER_HOST:unterminated quote");
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].label == "line 3");
    CHECK(entries[0].reason == "malformed line");
    CHECK(entries[1].label == "LD_PRELOAD");
    CHECK(entries[2].reason == "unterminated quote");
}

TEST_CASE("env-refusal: malformed entries are dropped, not fatal", "[1712]") {
    // No colon, empty label, empty reason, empty entry between separators,
    // trailing separator: the survivors are the well-formed entries.
    const auto entries =
        parse_env_lines_skipped("no-colon-here|:empty-label|KEY:|KEY:ok||KEY2:also-ok|");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].label == "KEY");
    CHECK(entries[0].reason == "ok");
    CHECK(entries[1].label == "KEY2");
    CHECK(entries[1].reason == "also-ok");
}

// ============================================================================
// parse_env_file_refused
// ============================================================================

TEST_CASE("env-refusal: the refusal record splits into its four fields", "[1712]") {
    const EnvFileRefusal r = parse_env_file_refused("owner|1000|root:root|/etc/helixscreen/env");
    REQUIRE(r.valid);
    CHECK(r.kind == "owner");
    CHECK(r.detail == "1000");
    CHECK(r.expected == "root:root");
    CHECK(r.path == "/etc/helixscreen/env");
}

TEST_CASE("env-refusal: a mode refusal carries empty middle fields", "[1712]") {
    const EnvFileRefusal r = parse_env_file_refused("mode|||/etc/helixscreen/env");
    REQUIRE(r.valid);
    CHECK(r.kind == "mode");
    CHECK(r.detail.empty());
    CHECK(r.expected.empty());
    CHECK(r.path == "/etc/helixscreen/env");
}

TEST_CASE("env-refusal: a path may itself contain the separator", "[1712]") {
    const EnvFileRefusal r = parse_env_file_refused("mode|||/odd|path");
    REQUIRE(r.valid);
    CHECK(r.path == "/odd|path");
}

TEST_CASE("env-refusal: unknown kinds and missing fields decide to no refusal", "[1712]") {
    CHECK_FALSE(parse_env_file_refused("").valid);
    CHECK_FALSE(parse_env_file_refused("mode|/etc/helixscreen/env").valid);
    CHECK_FALSE(parse_env_file_refused("mode|||").valid);
    CHECK_FALSE(parse_env_file_refused("garbage|||/etc/helixscreen/env").valid);
    CHECK_FALSE(
        parse_env_file_refused("it is writable by other users (/etc/env). chmod 644").valid);
}

// ============================================================================
// env_refusal_copy: one line, one command, chosen by the kind
// ============================================================================

TEST_CASE("env-refusal: a mode refusal names only the chmod", "[1712]") {
    const EnvRefusalCopy copy =
        env_refusal_copy(parse_env_file_refused("mode|||/etc/helixscreen/helixscreen.env"));
    CHECK(copy.message == "helixscreen.env ignored: writable by other users");
    CHECK(copy.fix == "Fix: chmod 644 /etc/helixscreen/helixscreen.env, then restart");
    CHECK(copy.fix.find("chown") == std::string::npos);
}

TEST_CASE("env-refusal: an owner refusal names only the chown", "[1712]") {
    const EnvRefusalCopy copy = env_refusal_copy(
        parse_env_file_refused("owner|1000|root:root|/etc/helixscreen/helixscreen.env"));
    CHECK(copy.message == "helixscreen.env ignored: owned by uid 1000");
    CHECK(copy.fix == "Fix: chown root:root /etc/helixscreen/helixscreen.env, then restart");
    CHECK(copy.fix.find("chmod") == std::string::npos);
}

TEST_CASE("env-refusal: a chain refusal points at the log", "[1712]") {
    const EnvRefusalCopy copy =
        env_refusal_copy(parse_env_file_refused("chain|||/etc/helixscreen/helixscreen.env"));
    CHECK(copy.message == "helixscreen.env ignored: untrusted symlink chain");
    CHECK(copy.fix == "See the log");
}

TEST_CASE("env-refusal: an other refusal carries the launcher's short reason", "[1712]") {
    const EnvRefusalCopy copy = env_refusal_copy(parse_env_file_refused(
        "other|its permissions could not be read||/etc/helixscreen/helixscreen.env"));
    CHECK(copy.message == "helixscreen.env ignored: its permissions could not be read");
    CHECK(copy.fix == "See the log");
}

// ============================================================================
// decide_env_refusal_notice
// ============================================================================

TEST_CASE("env-refusal: null and empty handoffs decide to nothing", "[1712]") {
    const auto notice = decide_env_refusal_notice(nullptr, nullptr);
    CHECK_FALSE(notice.refusal.valid);
    CHECK(notice.skipped_lines == 0);

    const auto empty = decide_env_refusal_notice("", "");
    CHECK_FALSE(empty.refusal.valid);
    CHECK(empty.skipped_lines == 0);
}

TEST_CASE("env-refusal: skipped lines are counted from parsed entries only", "[1712]") {
    const auto notice = decide_env_refusal_notice(nullptr, "KEY:ok||||KEY2:also-ok");
    CHECK_FALSE(notice.refusal.valid);
    REQUIRE(notice.skipped_lines == 2);
    REQUIRE(notice.skipped.size() == 2);
    CHECK(notice.skipped[1].label == "KEY2");
}

TEST_CASE("env-refusal: garbage in either handoff decides to no notice", "[1712]") {
    const auto notice = decide_env_refusal_notice("not-a-record", ":::|:x|y:");
    CHECK_FALSE(notice.refusal.valid);
    CHECK(notice.skipped_lines == 0);
}

namespace {

/// A HELIX_ENV_LINES_SKIPPED value at the launcher's cap: 12 real entries
/// closed by the sentinel row the launcher appends once it starts dropping
/// lines (scripts/helix-launcher.sh helix_env_note_skip).
std::string capped_handoff_value() {
    std::string value;
    for (int i = 1; i <= 12; ++i) {
        value += "NOT_A_SETTING_" + std::to_string(i) + ":not a setting this file may change|";
    }
    return value + "more skipped:not every skipped line is listed";
}

} // namespace

TEST_CASE("env-refusal: the sentinel entry marks the handoff truncated", "[1712]") {
    const auto notice = decide_env_refusal_notice(nullptr, capped_handoff_value().c_str());
    CHECK_FALSE(notice.refusal.valid);
    // 13 parsed rows, but only 12 are skipped lines: the last is the sentinel.
    REQUIRE(notice.skipped_lines == 13);
    CHECK(notice.skipped_truncated);
    CHECK(notice.skipped.back().label == "more skipped");
}

TEST_CASE("env-refusal: a complete handoff stays exact at the cap size", "[1712]") {
    // The same 12 entries WITHOUT the sentinel: nothing was dropped, so the
    // count is exact and the wording must not claim a lower bound.
    const std::string value = capped_handoff_value();
    const auto notice =
        decide_env_refusal_notice(nullptr, value.substr(0, value.rfind('|')).c_str());
    REQUIRE(notice.skipped_lines == 12);
    CHECK_FALSE(notice.skipped_truncated);
}

// ============================================================================
// surface_env_refusal_from_launcher: the glue, observed at the notification
// hooks for ui_notification_warning() and ui_notification_warning_sticky().
// Both hand the hook the JOINED "message - detail" history row.
// ============================================================================

namespace {

/// Set an env var for the duration of one test and restore the old value.
struct EnvVarGuard {
    std::string name;
    std::string old;
    bool was_set;

    explicit EnvVarGuard(const char* var, const char* value) : name(var) {
        const char* prev = std::getenv(var);
        was_set = prev != nullptr;
        if (was_set) {
            old = prev;
        }
        ::setenv(var, value, 1);
    }
    ~EnvVarGuard() {
        if (was_set) {
            ::setenv(name.c_str(), old.c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

/// Collect everything the app surfaces through the warning channels while $1
/// runs, split by channel: timed (auto-dismissing) vs sticky (stays until
/// closed). The hooks run on the calling thread, so no drain is needed.
struct Surfaced {
    std::vector<std::string> timed;
    std::vector<std::string> sticky;
};

template <typename Fn> Surfaced surfaced_warnings(Fn&& fn) {
    Surfaced out;
    helix::ui::set_test_notification_warning_hook(
        [&](const std::string& msg) { out.timed.push_back(msg); });
    helix::ui::set_test_notification_sticky_warning_hook(
        [&](const std::string& msg) { out.sticky.push_back(msg); });
    fn();
    helix::ui::set_test_notification_warning_hook(nullptr);
    helix::ui::set_test_notification_sticky_warning_hook(nullptr);
    return out;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "env-refusal: a mode refusal becomes one sticky warning naming the chmod",
                 "[1712]") {
    const Surfaced surfaced = surfaced_warnings([] {
        EnvVarGuard refused("HELIX_ENV_FILE_REFUSED", "mode|||/etc/helixscreen/helixscreen.env");
        surface_env_refusal_from_launcher();
    });
    // The whole file is being ignored, so the warning stays until closed.
    REQUIRE(surfaced.sticky.size() == 1);
    CHECK(surfaced.timed.empty());
    CHECK(surfaced.sticky[0].find("helixscreen.env ignored: writable by other users") !=
          std::string::npos);
    CHECK(surfaced.sticky[0].find("Fix: chmod 644 /etc/helixscreen/helixscreen.env, "
                                  "then restart") != std::string::npos);
    CHECK(surfaced.sticky[0].find("chown") == std::string::npos);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "env-refusal: skipped lines become one timed warning naming every entry",
                 "[1712]") {
    const Surfaced surfaced = surfaced_warnings([] {
        EnvVarGuard skipped(
            "HELIX_ENV_LINES_SKIPPED",
            "LD_PRELOAD:not a setting this file may change|HELIX_NICE:must be 0-19");
        surface_env_refusal_from_launcher();
    });
    // Line skips still load the rest of the file, so this one auto-dismisses.
    REQUIRE(surfaced.timed.size() == 1);
    CHECK(surfaced.sticky.empty());
    CHECK(surfaced.timed[0].find("2 lines in helixscreen.env were ignored") != std::string::npos);
    CHECK(surfaced.timed[0].find("LD_PRELOAD") != std::string::npos);
    CHECK(surfaced.timed[0].find("HELIX_NICE") != std::string::npos);
}

TEST_CASE_METHOD(HelixTestFixture, "env-refusal: one skipped line uses the singular wording",
                 "[1712]") {
    const Surfaced surfaced = surfaced_warnings([] {
        EnvVarGuard skipped("HELIX_ENV_LINES_SKIPPED", "HELIX_NICE:must be 0-19");
        surface_env_refusal_from_launcher();
    });
    REQUIRE(surfaced.timed.size() == 1);
    CHECK(surfaced.timed[0].find("1 line in helixscreen.env was ignored") != std::string::npos);
    CHECK(surfaced.timed[0].find("lines in helixscreen.env were ignored") == std::string::npos);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "env-refusal: a truncated handoff words the count as a lower bound", "[1712]") {
    const Surfaced surfaced = surfaced_warnings([] {
        EnvVarGuard skipped("HELIX_ENV_LINES_SKIPPED", capped_handoff_value().c_str());
        surface_env_refusal_from_launcher();
    });
    // The sentinel says lines were dropped, so the message must not name 13
    // (a count nobody has) and must hedge: "At least 12".
    REQUIRE(surfaced.timed.size() == 1);
    CHECK(surfaced.timed[0].find("At least 12 lines in helixscreen.env were ignored") !=
          std::string::npos);
    CHECK(surfaced.timed[0].find("13 lines in helixscreen.env were ignored") == std::string::npos);
    CHECK(surfaced.timed[0].find("more skipped: not every skipped line is listed") !=
          std::string::npos);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "env-refusal: a handoff at the cap exactly keeps the exact count", "[1712]") {
    const Surfaced surfaced = surfaced_warnings([] {
        const std::string value = capped_handoff_value();
        EnvVarGuard skipped("HELIX_ENV_LINES_SKIPPED", value.substr(0, value.rfind('|')).c_str());
        surface_env_refusal_from_launcher();
    });
    REQUIRE(surfaced.timed.size() == 1);
    CHECK(surfaced.timed[0].find("12 lines in helixscreen.env were ignored") != std::string::npos);
    CHECK(surfaced.timed[0].find("At least") == std::string::npos);
}

TEST_CASE("env-refusal: no handoff means no warning", "[1712]") {
    const Surfaced surfaced = surfaced_warnings([] {
        ::unsetenv("HELIX_ENV_FILE_REFUSED");
        ::unsetenv("HELIX_ENV_LINES_SKIPPED");
        surface_env_refusal_from_launcher();
    });
    CHECK(surfaced.timed.empty());
    CHECK(surfaced.sticky.empty());
}
