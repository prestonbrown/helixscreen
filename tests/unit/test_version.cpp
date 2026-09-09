// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "version.h"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::version;

// ============================================================================
// parse_version() tests
// ============================================================================

TEST_CASE("parse_version() handles valid version strings", "[version][parse]") {
    SECTION("full semver") {
        auto v = parse_version("1.2.3");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 1);
        REQUIRE(v->minor == 2);
        REQUIRE(v->patch == 3);
    }

    SECTION("major only") {
        auto v = parse_version("2");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 2);
        REQUIRE(v->minor == 0);
        REQUIRE(v->patch == 0);
    }

    SECTION("major.minor only") {
        auto v = parse_version("2.5");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 2);
        REQUIRE(v->minor == 5);
        REQUIRE(v->patch == 0);
    }

    SECTION("with v prefix") {
        auto v = parse_version("v1.2.3");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 1);
        REQUIRE(v->minor == 2);
        REQUIRE(v->patch == 3);
    }

    SECTION("with V prefix") {
        auto v = parse_version("V2.0.0");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 2);
        REQUIRE(v->minor == 0);
        REQUIRE(v->patch == 0);
    }

    SECTION("pre-release suffix is captured") {
        auto v = parse_version("1.0.0-beta");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 1);
        REQUIRE(v->minor == 0);
        REQUIRE(v->patch == 0);
        REQUIRE(v->prerelease == "beta");
    }

    SECTION("build metadata is dropped") {
        auto v = parse_version("1.0.0+build123");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 1);
        REQUIRE(v->minor == 0);
        REQUIRE(v->patch == 0);
        REQUIRE(v->prerelease.empty());
    }

    SECTION("with both pre-release and build") {
        auto v = parse_version("2.1.0-rc1+sha.abc1234");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 2);
        REQUIRE(v->minor == 1);
        REQUIRE(v->patch == 0);
        REQUIRE(v->prerelease == "rc1");
    }

    SECTION("zeros are valid") {
        auto v = parse_version("0.0.0");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 0);
        REQUIRE(v->minor == 0);
        REQUIRE(v->patch == 0);
    }

    SECTION("large version numbers") {
        auto v = parse_version("100.200.300");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 100);
        REQUIRE(v->minor == 200);
        REQUIRE(v->patch == 300);
    }
}

TEST_CASE("parse_version() handles invalid version strings", "[version][parse]") {
    SECTION("empty string") {
        auto v = parse_version("");
        REQUIRE_FALSE(v.has_value());
    }

    SECTION("just letters") {
        auto v = parse_version("abc");
        REQUIRE_FALSE(v.has_value());
    }

    SECTION("just v") {
        auto v = parse_version("v");
        REQUIRE_FALSE(v.has_value());
    }
}

// ============================================================================
// Version comparison tests
// ============================================================================

TEST_CASE("Version comparison operators", "[version][comparison]") {
    SECTION("equality") {
        Version a{1, 2, 3};
        Version b{1, 2, 3};
        REQUIRE(a == b);
        REQUIRE_FALSE(a != b);
    }

    SECTION("inequality - different major") {
        Version a{1, 0, 0};
        Version b{2, 0, 0};
        REQUIRE(a != b);
        REQUIRE_FALSE(a == b);
    }

    SECTION("less than - major") {
        Version a{1, 0, 0};
        Version b{2, 0, 0};
        REQUIRE(a < b);
        REQUIRE_FALSE(b < a);
    }

    SECTION("less than - minor") {
        Version a{1, 1, 0};
        Version b{1, 2, 0};
        REQUIRE(a < b);
        REQUIRE_FALSE(b < a);
    }

    SECTION("less than - patch") {
        Version a{1, 2, 1};
        Version b{1, 2, 2};
        REQUIRE(a < b);
        REQUIRE_FALSE(b < a);
    }

    SECTION("greater than") {
        Version a{2, 0, 0};
        Version b{1, 9, 9};
        REQUIRE(a > b);
        REQUIRE_FALSE(b > a);
    }

    SECTION("less than or equal") {
        Version a{1, 2, 3};
        Version b{1, 2, 3};
        Version c{1, 2, 4};
        REQUIRE(a <= b);
        REQUIRE(a <= c);
        REQUIRE_FALSE(c <= a);
    }

    SECTION("greater than or equal") {
        Version a{1, 2, 3};
        Version b{1, 2, 3};
        Version c{1, 2, 2};
        REQUIRE(a >= b);
        REQUIRE(a >= c);
        REQUIRE_FALSE(c >= a);
    }
}

// ============================================================================
// check_version_constraint() tests
// ============================================================================

TEST_CASE("check_version_constraint() with >= operator", "[version][constraint]") {
    SECTION("exact match") {
        REQUIRE(check_version_constraint(">=2.0.0", "2.0.0"));
    }

    SECTION("higher major version") {
        REQUIRE(check_version_constraint(">=2.0.0", "3.0.0"));
    }

    SECTION("higher minor version") {
        REQUIRE(check_version_constraint(">=2.0.0", "2.1.0"));
    }

    SECTION("higher patch version") {
        REQUIRE(check_version_constraint(">=2.0.0", "2.0.1"));
    }

    SECTION("lower major version fails") {
        REQUIRE_FALSE(check_version_constraint(">=2.0.0", "1.9.9"));
    }

    SECTION("lower minor version fails") {
        REQUIRE_FALSE(check_version_constraint(">=2.1.0", "2.0.9"));
    }

    SECTION("lower patch version fails") {
        REQUIRE_FALSE(check_version_constraint(">=2.0.1", "2.0.0"));
    }
}

TEST_CASE("check_version_constraint() with > operator", "[version][constraint]") {
    SECTION("exact match fails") {
        REQUIRE_FALSE(check_version_constraint(">2.0.0", "2.0.0"));
    }

    SECTION("higher version passes") {
        REQUIRE(check_version_constraint(">1.0.0", "1.0.1"));
        REQUIRE(check_version_constraint(">1.0.0", "1.1.0"));
        REQUIRE(check_version_constraint(">1.0.0", "2.0.0"));
    }

    SECTION("lower version fails") {
        REQUIRE_FALSE(check_version_constraint(">2.0.0", "1.9.9"));
    }
}

TEST_CASE("check_version_constraint() with = operator", "[version][constraint]") {
    SECTION("exact match passes") {
        REQUIRE(check_version_constraint("=2.0.0", "2.0.0"));
    }

    SECTION("different version fails") {
        REQUIRE_FALSE(check_version_constraint("=2.0.0", "2.0.1"));
        REQUIRE_FALSE(check_version_constraint("=2.0.0", "1.9.9"));
    }
}

TEST_CASE("check_version_constraint() with no operator (implicit =)", "[version][constraint]") {
    SECTION("exact match passes") {
        REQUIRE(check_version_constraint("2.0.0", "2.0.0"));
    }

    SECTION("different version fails") {
        REQUIRE_FALSE(check_version_constraint("2.0.0", "2.0.1"));
    }
}

TEST_CASE("check_version_constraint() with < operator", "[version][constraint]") {
    SECTION("lower version passes") {
        REQUIRE(check_version_constraint("<3.0.0", "2.9.9"));
        REQUIRE(check_version_constraint("<2.1.0", "2.0.9"));
    }

    SECTION("exact match fails") {
        REQUIRE_FALSE(check_version_constraint("<2.0.0", "2.0.0"));
    }

    SECTION("higher version fails") {
        REQUIRE_FALSE(check_version_constraint("<2.0.0", "2.0.1"));
    }
}

TEST_CASE("check_version_constraint() with <= operator", "[version][constraint]") {
    SECTION("lower version passes") {
        REQUIRE(check_version_constraint("<=2.5.0", "2.4.9"));
    }

    SECTION("exact match passes") {
        REQUIRE(check_version_constraint("<=2.5.0", "2.5.0"));
    }

    SECTION("higher version fails") {
        REQUIRE_FALSE(check_version_constraint("<=2.5.0", "2.5.1"));
    }
}

TEST_CASE("check_version_constraint() edge cases", "[version][constraint][edge]") {
    SECTION("empty constraint matches anything") {
        REQUIRE(check_version_constraint("", "1.0.0"));
        REQUIRE(check_version_constraint("", "999.0.0"));
    }

    SECTION("constraint with spaces") {
        REQUIRE(check_version_constraint(">= 2.0.0", "2.0.0"));
        REQUIRE(check_version_constraint("  >=2.0.0", "2.1.0"));
    }

    SECTION("version with v prefix") {
        REQUIRE(check_version_constraint(">=2.0.0", "v2.0.0"));
    }

    SECTION("constraint with v prefix") {
        REQUIRE(check_version_constraint(">=v2.0.0", "2.0.0"));
    }

    SECTION("invalid constraint returns false") {
        REQUIRE_FALSE(check_version_constraint(">=", "2.0.0"));
        REQUIRE_FALSE(check_version_constraint(">=abc", "2.0.0"));
    }

    SECTION("invalid version returns false") {
        REQUIRE_FALSE(check_version_constraint(">=2.0.0", ""));
        REQUIRE_FALSE(check_version_constraint(">=2.0.0", "invalid"));
    }
}

// ============================================================================
// to_string() tests
// ============================================================================

TEST_CASE("to_string() formats versions correctly", "[version][to_string]") {
    SECTION("regular version") {
        Version v{1, 2, 3};
        REQUIRE(to_string(v) == "1.2.3");
    }

    SECTION("zeros") {
        Version v{0, 0, 0};
        REQUIRE(to_string(v) == "0.0.0");
    }

    SECTION("large numbers") {
        Version v{10, 20, 30};
        REQUIRE(to_string(v) == "10.20.30");
    }
}

// ============================================================================
// Real-world constraint examples from task spec
// ============================================================================

TEST_CASE("Version constraint examples from spec", "[version][constraint][spec]") {
    // Examples from the task specification table
    SECTION(">=2.0.0 with 2.0.0 -> match") {
        REQUIRE(check_version_constraint(">=2.0.0", "2.0.0"));
    }

    SECTION(">=2.0.0 with 2.1.0 -> match") {
        REQUIRE(check_version_constraint(">=2.0.0", "2.1.0"));
    }

    SECTION(">=2.0.0 with 1.9.0 -> no match") {
        REQUIRE_FALSE(check_version_constraint(">=2.0.0", "1.9.0"));
    }

    SECTION(">1.0.0 with 1.0.1 -> match") {
        REQUIRE(check_version_constraint(">1.0.0", "1.0.1"));
    }

    SECTION("=2.0.0 with 2.0.0 -> match") {
        REQUIRE(check_version_constraint("=2.0.0", "2.0.0"));
    }
}

// ============================================================================
// Precedence corpus
// ============================================================================
//
// tests/fixtures/version_precedence.txt is the single ordering corpus. The bats
// test for scripts/version-compare.sh reads the same file, so the two
// implementations of one rule cannot drift apart unnoticed.

namespace {

/// Resolve tests/fixtures/ from __FILE__ so the test does not depend on cwd.
std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

/// The corpus in file order, each entry paired with the line it came from.
/// Blank lines and #-comments are skipped; a line that fails to parse is a
/// failure, never a silent skip.
std::vector<std::pair<std::string, Version>> load_precedence_corpus() {
    const std::string path = fixture_dir() + "version_precedence.txt";
    std::ifstream f(path);
    INFO("corpus missing or unreadable: " << path);
    REQUIRE(f.is_open());

    std::vector<std::pair<std::string, Version>> out;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto v = parse_version(line);
        INFO("corpus line does not parse: " << line);
        REQUIRE(v.has_value());
        out.emplace_back(line, *v);
    }
    return out;
}

} // namespace

TEST_CASE("Precedence corpus orders strictly ascending", "[version][precedence][corpus]") {
    const auto corpus = load_precedence_corpus();

    // An unreadable or truncated corpus would otherwise satisfy every loop below
    // by iterating zero times.
    INFO("corpus entries loaded: " << corpus.size());
    REQUIRE(corpus.size() >= 15);

    for (size_t i = 0; i < corpus.size(); ++i) {
        const std::string& text_i = corpus[i].first;
        const Version& vi = corpus[i].second;

        {
            INFO("reflexive: " << text_i);
            CHECK(vi == vi);
            CHECK_FALSE(vi != vi);
            CHECK_FALSE(vi < vi);
            CHECK(vi <= vi);
            CHECK(vi >= vi);
            // The corpus carries no v prefix and no build metadata, so each line
            // is also the canonical spelling of what it parsed to.
            CHECK(to_string(vi) == text_i);
        }

        // Every later entry outranks this one, not just the adjacent line.
        for (size_t j = i + 1; j < corpus.size(); ++j) {
            const std::string& text_j = corpus[j].first;
            const Version& vj = corpus[j].second;
            INFO(text_i << " must precede " << text_j);
            CHECK(vi < vj);
            CHECK(vj > vi);
            CHECK_FALSE(vj < vi);
            CHECK_FALSE(vi == vj);
            CHECK(vi != vj);
            CHECK(vi <= vj);
            CHECK(vj >= vi);
        }
    }
}

// ============================================================================
// check_version_constraint() ignores the prerelease on purpose
// ============================================================================

TEST_CASE("check_version_constraint() compares the core triple only",
          "[version][constraint][prerelease]") {
    // Ordering and compatibility are different questions. A plugin built against
    // 1.1.0 has to load on a 1.1.0 beta, so the constraint check answers on the
    // triple alone even though operator< ranks the beta lower. Every assertion
    // here flips if the check is ever routed through full precedence.
    SECTION(">= is satisfied by a prerelease of the required triple") {
        REQUIRE(check_version_constraint(">=1.1.0", "1.1.0-beta.5"));
        REQUIRE(check_version_constraint(">=1.1.0", "1.1.0-alpha"));
        REQUIRE(check_version_constraint(">=1.1.0", "1.1.0-rc.1+sha.abc1234"));
    }

    SECTION("> is not satisfied by a prerelease of the required triple") {
        REQUIRE_FALSE(check_version_constraint(">1.1.0", "1.1.0-beta.5"));
    }

    SECTION("< is not satisfied by a prerelease of the boundary triple") {
        REQUIRE_FALSE(check_version_constraint("<1.1.0", "1.1.0-beta.5"));
    }

    SECTION("<= is satisfied by a prerelease of the boundary triple") {
        REQUIRE(check_version_constraint("<=1.1.0", "1.1.0-beta.5"));
    }

    SECTION("= matches across the prerelease on either side") {
        REQUIRE(check_version_constraint("=1.1.0", "1.1.0-beta.5"));
        REQUIRE(check_version_constraint("=1.1.0-beta.5", "1.1.0"));
        REQUIRE(check_version_constraint("1.1.0", "1.1.0-beta.5"));
    }

    SECTION("the triple still decides") {
        REQUIRE_FALSE(check_version_constraint(">=1.1.0", "1.0.9-beta.5"));
        REQUIRE(check_version_constraint(">=1.1.0", "1.1.1-beta.1"));
        REQUIRE_FALSE(check_version_constraint("=1.1.0", "1.1.1-beta.1"));
    }
}

// ============================================================================
// Prerelease parsing and validation
// ============================================================================

TEST_CASE("parse_version() captures prerelease identifiers", "[version][parse][prerelease]") {
    SECTION("multiple identifiers") {
        auto v = parse_version("1.1.0-beta.11");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 1);
        REQUIRE(v->minor == 1);
        REQUIRE(v->patch == 0);
        REQUIRE(v->prerelease == "beta.11");
    }

    SECTION("numeric identifier") {
        auto v = parse_version("1.1.0-1");
        REQUIRE(v.has_value());
        REQUIRE(v->prerelease == "1");
    }

    SECTION("hyphen inside an identifier") {
        auto v = parse_version("1.1.0-x-y.2");
        REQUIRE(v.has_value());
        REQUIRE(v->prerelease == "x-y.2");
    }

    SECTION("build metadata is cut off the prerelease") {
        auto v = parse_version("1.1.0-beta.1+sha.abc1234");
        REQUIRE(v.has_value());
        REQUIRE(v->prerelease == "beta.1");
    }

    SECTION("v prefix and prerelease together") {
        auto v = parse_version("v1.1.0-rc.1");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 1);
        REQUIRE(v->minor == 1);
        REQUIRE(v->prerelease == "rc.1");
    }

    SECTION("a short core still takes a prerelease") {
        auto one = parse_version("1-beta");
        REQUIRE(one.has_value());
        REQUIRE(one->major == 1);
        REQUIRE(one->minor == 0);
        REQUIRE(one->patch == 0);
        REQUIRE(one->prerelease == "beta");

        auto two = parse_version("1.2-beta.3");
        REQUIRE(two.has_value());
        REQUIRE(two->major == 1);
        REQUIRE(two->minor == 2);
        REQUIRE(two->patch == 0);
        REQUIRE(two->prerelease == "beta.3");
    }

    SECTION("trailing whitespace is not part of the prerelease") {
        auto v = parse_version("1.1.0-beta.1  ");
        REQUIRE(v.has_value());
        REQUIRE(v->prerelease == "beta.1");
    }
}

TEST_CASE("parse_version() rejects malformed prereleases", "[version][parse][prerelease]") {
    SECTION("no identifiers at all") {
        REQUIRE_FALSE(parse_version("1.1.0-").has_value());
        REQUIRE_FALSE(parse_version("1.1.0-+sha.abc").has_value());
    }

    SECTION("empty identifier") {
        REQUIRE_FALSE(parse_version("1.1.0-alpha..1").has_value());
        REQUIRE_FALSE(parse_version("1.1.0-.alpha").has_value());
        REQUIRE_FALSE(parse_version("1.1.0-alpha.").has_value());
    }

    SECTION("leading zero on a numeric identifier") {
        REQUIRE_FALSE(parse_version("1.1.0-01").has_value());
        REQUIRE_FALSE(parse_version("1.1.0-beta.01").has_value());
        // A single zero is a legitimate number, and a leading zero on an
        // alphanumeric identifier is not a number at all.
        REQUIRE(parse_version("1.1.0-0").has_value());
        REQUIRE(parse_version("1.1.0-0alpha").has_value());
    }

    SECTION("character outside [0-9A-Za-z-]") {
        REQUIRE_FALSE(parse_version("1.1.0-beta_1").has_value());
        REQUIRE_FALSE(parse_version("1.1.0-beta 1").has_value());
        REQUIRE_FALSE(parse_version("1.1.0-beta/1").has_value());
    }

    SECTION("negatives are still rejected") {
        REQUIRE_FALSE(parse_version("-1.0.0").has_value());
    }
}

// ============================================================================
// Prerelease precedence
// ============================================================================

TEST_CASE("A prerelease ranks below the release of its triple", "[version][precedence]") {
    const Version beta{1, 1, 0, "beta.1"};
    const Version release{1, 1, 0, {}};

    REQUIRE(beta < release);
    REQUIRE(release > beta);
    REQUIRE_FALSE(release < beta);
    REQUIRE(beta != release);
    REQUIRE_FALSE(beta == release);
    REQUIRE(beta <= release);
    REQUIRE_FALSE(release <= beta);

    // The triple still outranks the prerelease rule.
    REQUIRE(release < Version{1, 1, 1, "alpha"});
    REQUIRE(Version{1, 0, 99, {}} < beta);
}

TEST_CASE("Prerelease identifiers compare by kind then value", "[version][precedence]") {
    SECTION("numeric identifiers compare numerically, not lexically") {
        REQUIRE(Version{1, 1, 0, "beta.2"} < Version{1, 1, 0, "beta.11"});
        REQUIRE_FALSE(Version{1, 1, 0, "beta.11"} < Version{1, 1, 0, "beta.2"});
        REQUIRE(Version{1, 1, 0, "9"} < Version{1, 1, 0, "10"});
    }

    SECTION("alphanumeric identifiers compare in ASCII order") {
        REQUIRE(Version{1, 1, 0, "alpha"} < Version{1, 1, 0, "beta"});
        REQUIRE(Version{1, 1, 0, "beta"} < Version{1, 1, 0, "rc"});
        // Uppercase sorts below lowercase, as ASCII order says.
        REQUIRE(Version{1, 1, 0, "RC"} < Version{1, 1, 0, "rc"});
    }

    SECTION("a numeric identifier is lower than an alphanumeric one") {
        REQUIRE(Version{1, 1, 0, "11"} < Version{1, 1, 0, "alpha"});
        REQUIRE_FALSE(Version{1, 1, 0, "alpha"} < Version{1, 1, 0, "11"});
        REQUIRE(Version{1, 1, 0, "alpha.1"} < Version{1, 1, 0, "alpha.beta"});
    }

    SECTION("a longer identifier list wins when every shared one is equal") {
        REQUIRE(Version{1, 1, 0, "beta"} < Version{1, 1, 0, "beta.1"});
        REQUIRE(Version{1, 1, 0, "alpha"} < Version{1, 1, 0, "alpha.1"});
        REQUIRE_FALSE(Version{1, 1, 0, "beta.1"} < Version{1, 1, 0, "beta"});
        // Shared identifiers decide before length does.
        REQUIRE(Version{1, 1, 0, "alpha.9"} < Version{1, 1, 0, "beta"});
    }

    SECTION("identical prereleases are equal, in both directions") {
        const Version a{1, 1, 0, "beta.1"};
        const Version b{1, 1, 0, "beta.1"};
        REQUIRE(a == b);
        REQUIRE_FALSE(a < b);
        REQUIRE_FALSE(b < a);
        REQUIRE(a <= b);
        REQUIRE(a >= b);
    }
}

TEST_CASE("Build metadata does not affect precedence", "[version][precedence]") {
    auto a = parse_version("1.1.0+sha.aaa");
    auto b = parse_version("1.1.0+sha.bbb");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(*a == *b);
    REQUIRE_FALSE(*a < *b);

    auto pre_a = parse_version("1.1.0-beta.1+sha.aaa");
    auto pre_b = parse_version("1.1.0-beta.1+sha.bbb");
    REQUIRE(pre_a.has_value());
    REQUIRE(pre_b.has_value());
    REQUIRE(*pre_a == *pre_b);
    REQUIRE(*pre_a < *a);
}

// ============================================================================
// core() drops the prerelease
// ============================================================================

TEST_CASE("core() keeps the triple and drops the prerelease", "[version][core]") {
    const Version beta{1, 1, 0, "beta.5"};
    REQUIRE(beta.core() == Version{1, 1, 0, {}});
    REQUIRE(beta.core().prerelease.empty());
    REQUIRE(to_string(beta.core()) == "1.1.0");
    // Idempotent, and a release is its own core.
    const Version release{1, 1, 0, {}};
    REQUIRE(release.core() == release);
}

// ============================================================================
// to_string() round-trips the prerelease
// ============================================================================

TEST_CASE("to_string() round-trips a prerelease", "[version][to_string][prerelease]") {
    REQUIRE(to_string(Version{1, 1, 0, "beta.1"}) == "1.1.0-beta.1");
    REQUIRE(to_string(Version{2, 0, 0, "rc.1"}) == "2.0.0-rc.1");
    REQUIRE(to_string(Version{1, 1, 0, "x-y.2"}) == "1.1.0-x-y.2");

    for (const char* text : {"1.1.0-beta.1", "1.1.0", "0.99.118", "1.1.0-alpha.beta"}) {
        auto v = parse_version(text);
        INFO("round-tripping " << text);
        REQUIRE(v.has_value());
        CHECK(to_string(*v) == text);
    }
}
