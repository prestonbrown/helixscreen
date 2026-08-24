// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_config_path_candidates.cpp
 * @brief Resolving a filename Moonraker reported to a path the file API takes
 *
 * server.config names each loaded file relative to the ROOT config's parent
 * directory, and falls back to the full absolute path for anything outside it.
 * The file API addresses files relative to the file manager's "config" root.
 * Those two are the same thing on a stock install and diverge on three of the
 * four firmwares measured on 2026-08-14:
 *
 *   K1    root /usr/data/printer_data/config   reports "moonraker.conf"   — same
 *   U1    root /oem/printer_data/config        reports a relative subpath — same
 *   K2    root /mnt/UDISK/printer_data/config  reports "moonraker.conf", which the
 *                                              file API 404s (config really lives
 *                                              in /usr/share/moonraker)
 *   AD5M  root /opt/config                     reports ABSOLUTE paths under
 *                                              /root/printer_data/config, which is
 *                                              a different tree — yet the same
 *                                              files ARE served under the root by
 *                                              their tail (symlink/bind mount)
 *
 * So a path-only test cannot decide reachability. This produces ranked candidates;
 * verify_config_reachable() proves the winner by content before anything is written.
 */

#include "moonraker_config_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::MoonrakerConfigManager;

namespace {

std::vector<std::string> candidates(const std::string& reported, const std::string& root = "") {
    return MoonrakerConfigManager::candidate_config_paths(reported, root);
}

} // namespace

// ============================================================================
// Case 1 — already relative. Every stock install, unchanged by the root.
// ============================================================================

TEST_CASE("K1: a bare relative name is the only candidate", "[config_candidates]") {
    CHECK(candidates("moonraker.conf", "/usr/data/printer_data/config") ==
          std::vector<std::string>{"moonraker.conf"});
}

TEST_CASE("U1: a relative subpath is passed through intact", "[config_candidates]") {
    CHECK(candidates("extended/moonraker/04_remote_screen.cfg", "/oem/printer_data/config") ==
          std::vector<std::string>{"extended/moonraker/04_remote_screen.cfg"});
}

TEST_CASE("K2: a bare relative name is the only candidate here too", "[config_candidates]") {
    // Reachability is not this function's job — the file API 404s this one, and
    // verify_config_reachable() is what discovers that.
    CHECK(candidates("moonraker.conf", "/mnt/UDISK/printer_data/config") ==
          std::vector<std::string>{"moonraker.conf"});
}

TEST_CASE("a relative name resolves the same with no root at all", "[config_candidates]") {
    CHECK(candidates("moonraker.conf") == std::vector<std::string>{"moonraker.conf"});
    CHECK(candidates("sub/helixscreen.conf") == std::vector<std::string>{"sub/helixscreen.conf"});
}

// ============================================================================
// Case 2 — absolute, and under the config root. Strip the prefix.
// ============================================================================

TEST_CASE("an absolute path under the config root strips to its remainder", "[config_candidates]") {
    CHECK(candidates("/mnt/UDISK/printer_data/config/helixscreen.conf",
                     "/mnt/UDISK/printer_data/config") ==
          std::vector<std::string>{"helixscreen.conf"});
}

TEST_CASE("stripping keeps a subdirectory under the root", "[config_candidates]") {
    CHECK(candidates("/usr/data/printer_data/config/sub/dir/x.conf",
                     "/usr/data/printer_data/config") ==
          std::vector<std::string>{"sub/dir/x.conf"});
}

TEST_CASE("a trailing slash on the root does not change the strip", "[config_candidates]") {
    CHECK(candidates("/opt/config/helixscreen.conf", "/opt/config/") ==
          std::vector<std::string>{"helixscreen.conf"});
}

// ============================================================================
// Case 3 — absolute, outside the root. Speculate from the tail.
//
// This is the AD5M. Moonraker reports the unresolved path through one tree while
// the file manager serves the same file through another; only the tail is common.
// ============================================================================

TEST_CASE("AD5M: an absolute path in a foreign tree yields its post-config tail",
          "[config_candidates]") {
    CHECK(candidates("/root/printer_data/config/moonraker.conf", "/opt/config") ==
          std::vector<std::string>{"moonraker.conf"});
}

TEST_CASE("AD5M: the nested user config keeps its subdirectory, basename second",
          "[config_candidates]") {
    // "mod_data/user.moonraker.conf" is the one that actually resolves on the
    // device; the bare basename follows as a weaker guess.
    CHECK(candidates("/root/printer_data/config/mod_data/user.moonraker.conf", "/opt/config") ==
          std::vector<std::string>{"mod_data/user.moonraker.conf", "user.moonraker.conf"});
}

TEST_CASE("an absolute path with no config component falls back to the basename",
          "[config_candidates]") {
    CHECK(candidates("/usr/share/moonraker/moonraker.conf", "/mnt/UDISK/printer_data/config") ==
          std::vector<std::string>{"moonraker.conf"});
}

TEST_CASE("the last config component wins when a path has several", "[config_candidates]") {
    CHECK(candidates("/config/printer_data/config/mod_data/x.conf", "/opt/config") ==
          std::vector<std::string>{"mod_data/x.conf", "x.conf"});
}

TEST_CASE("speculation still happens when no config root was reported", "[config_candidates]") {
    // Without a root, case 2 cannot apply — but the tail is still the file API's
    // best guess, and content-verification is what decides.
    CHECK(candidates("/root/printer_data/config/moonraker.conf") ==
          std::vector<std::string>{"moonraker.conf"});
}

// ============================================================================
// The sibling trap, and why the answer is safe
// ============================================================================

TEST_CASE("the config_backup sibling never prefix-strips, but does speculate",
          "[config_candidates]") {
    // ".../config_backup/" must not be swallowed by the ".../config" prefix, so
    // case 2 correctly declines. Case 3 then offers the bare basename.
    //
    // That is deliberate and safe: a candidate is never written on the strength of
    // its path. verify_config_reachable() downloads "helixscreen.conf" from the
    // config root and grades it with classify_section_match() against the sections
    // Moonraker reported for the config_backup file. A genuinely different file
    // fails that and is refused, exactly as before. A file that DOES match is
    // the same content reached by another name — which is precisely the AD5M
    // symlink case this whole fallback exists for.
    CHECK(candidates("/mnt/UDISK/printer_data/config_backup/helixscreen.conf",
                     "/mnt/UDISK/printer_data/config") ==
          std::vector<std::string>{"helixscreen.conf"});
}

// ============================================================================
// Nothing usable
// ============================================================================

TEST_CASE("'..' never survives into a candidate", "[config_candidates]") {
    // Relative, escaping.
    CHECK(candidates("../outside.conf", "/mnt/UDISK/printer_data/config").empty());
    // Absolute under the root, escaping after the strip.
    CHECK(
        candidates("/mnt/UDISK/printer_data/config/../evil.conf", "/mnt/UDISK/printer_data/config")
            .empty());
    // Absolute outside the root: the tail and the basename are both discarded
    // rather than sanitised, so a traversal can never become a write target.
    CHECK(candidates("/root/printer_data/config/../evil.conf", "/opt/config").empty());
    CHECK(candidates("/root/printer_data/config/mod_data/../../evil.conf", "/opt/config").empty());
}

TEST_CASE("an empty or nameless report yields no candidates", "[config_candidates]") {
    CHECK(candidates("", "/opt/config").empty());
    CHECK(candidates("   ", "/opt/config").empty());
    // The config root directory itself, not a file in it.
    CHECK(candidates("/opt/config", "/opt/config").empty());
    CHECK(candidates("/opt/config/", "/opt/config").empty());
    // A path that is nothing but slashes has no basename to fall back to.
    CHECK(candidates("/", "/opt/config").empty());
}

TEST_CASE("candidates never repeat", "[config_candidates]") {
    // The post-config tail and the basename coincide whenever the file sits
    // directly in a config directory; one entry, not two.
    auto c = candidates("/root/printer_data/config/moonraker.conf", "/opt/config");
    REQUIRE(c.size() == 1);
}

TEST_CASE("every candidate is usable as a file-API path", "[config_candidates]") {
    // Whatever comes back must be relative and non-empty, or the upload would
    // address something other than the config root.
    const char* reported[] = {"moonraker.conf", "extended/moonraker/04_remote_screen.cfg",
                              "/opt/config/helixscreen.conf",
                              "/root/printer_data/config/mod_data/user.moonraker.conf",
                              "/usr/share/moonraker/moonraker.conf"};
    for (const char* r : reported) {
        for (const auto& c : candidates(r, "/opt/config")) {
            INFO("reported=" << r << " candidate=" << c);
            CHECK_FALSE(c.empty());
            CHECK(c.front() != '/');
            CHECK(c.find("..") == std::string::npos);
        }
    }
}

// ============================================================================
// Speculative vs derived
//
// A guessed path may not lean on drift tolerance: the two compound into a
// confident write to an unrelated file of the same name. candidates_are_speculative()
// is the whole-list verdict SpoolmanOverlay::verify_config_reachable() keys that on.
// ============================================================================

namespace {
bool speculative(const std::string& reported, const std::string& root = "") {
    return MoonrakerConfigManager::candidates_are_speculative(reported, root);
}
} // namespace

TEST_CASE("a relative name is derived, never speculative", "[config_candidates][speculative]") {
    CHECK_FALSE(speculative("moonraker.conf", "/opt/config"));
    CHECK_FALSE(speculative("extended/moonraker/04_remote_screen.cfg", "/opt/config"));
    // With no root at all it is still the form the file API takes verbatim.
    CHECK_FALSE(speculative("moonraker.conf"));
}

TEST_CASE("an absolute path the root contains is derived", "[config_candidates][speculative]") {
    CHECK_FALSE(speculative("/opt/config/moonraker.conf", "/opt/config"));
    CHECK_FALSE(speculative("/opt/config/sub/moonraker.conf", "/opt/config"));
    // A trailing slash on the root does not change the verdict.
    CHECK_FALSE(speculative("/opt/config/moonraker.conf", "/opt/config/"));
}

TEST_CASE("AD5M and K2: an absolute path outside the root is speculative",
          "[config_candidates][speculative]") {
    // AD5M — the same file served under another tree, which is why speculating works.
    CHECK(speculative("/root/printer_data/config/moonraker.conf", "/opt/config"));
    // K2 — a vendor config the file manager does not serve at all, which is why
    // speculating needs an exact content match before anything is written.
    CHECK(speculative("/usr/share/moonraker/moonraker.conf", "/mnt/UDISK/printer_data/config"));
}

TEST_CASE("a lookalike sibling of the root is speculative, not derived",
          "[config_candidates][speculative]") {
    // The prefix test is component-wise, so config_backup is outside /opt/config.
    CHECK(speculative("/opt/config_backup/moonraker.conf", "/opt/config"));
}

TEST_CASE("an absolute path with no root reported is speculative",
          "[config_candidates][speculative]") {
    CHECK(speculative("/root/printer_data/config/moonraker.conf"));
}

TEST_CASE("inputs that yield no candidates are not reported as speculative",
          "[config_candidates][speculative]") {
    // Nothing to grade — candidate_config_paths() returns an empty list for each.
    CHECK_FALSE(speculative("", "/opt/config"));
    CHECK_FALSE(speculative("   ", "/opt/config"));
    CHECK_FALSE(speculative("/opt/config/../etc/passwd", "/opt/config"));
    CHECK_FALSE(speculative("/opt/config", "/opt/config"));
    CHECK_FALSE(speculative("/opt/config/", "/opt/config"));
}

TEST_CASE("speculative agrees with which branch of candidate_config_paths ran",
          "[config_candidates][speculative]") {
    // The two functions must never disagree about how a name was resolved: a
    // candidate equal to the reported name, or to it minus the root prefix, was
    // derived; anything else was inferred from the tail.
    struct Case {
        const char* reported;
        const char* root;
    };
    const Case cases[] = {
        {"moonraker.conf", "/opt/config"},
        {"sub/moonraker.conf", "/opt/config"},
        {"/opt/config/moonraker.conf", "/opt/config"},
        {"/opt/config/sub/moonraker.conf", "/opt/config"},
        {"/root/printer_data/config/moonraker.conf", "/opt/config"},
        {"/usr/share/moonraker/moonraker.conf", "/opt/config"},
        {"/opt/config_backup/moonraker.conf", "/opt/config"},
        {"/root/printer_data/config/moonraker.conf", ""},
    };
    for (const auto& c : cases) {
        const auto list = candidates(c.reported, c.root);
        if (list.empty())
            continue;
        const std::string reported = c.reported;
        const std::string root = c.root;
        const bool derived =
            list.front() == reported || (!root.empty() && reported.size() > root.size() + 1 &&
                                         reported.compare(0, root.size() + 1, root + "/") == 0 &&
                                         list.front() == reported.substr(root.size() + 1));
        INFO("reported=" << c.reported << " root=" << c.root << " first=" << list.front());
        CHECK(speculative(c.reported, c.root) == !derived);
    }
}
