// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file version.h
 * @brief Semantic version parsing, precedence and constraint checking
 *
 * Parses semantic version strings and orders them by Semantic Versioning 2.0.0
 * precedence, so a prerelease ranks below the release of the same triple
 * (`1.1.0-beta.1` < `1.1.0`). The in-app updater orders channel versions with
 * these operators; the plugin system checks manifest constraints with
 * check_version_constraint(), which answers on the core triple alone.
 *
 * tests/fixtures/version_precedence.txt is the ordering corpus, shared with
 * scripts/version-compare.sh so the two implementations cannot drift.
 *
 * Supports constraint operators: >=, >, =, <, <=
 *
 * @example
 * // Parse a version string
 * auto v = parse_version("2.1.0-rc.1");   // 2.1.0, prerelease "rc.1"
 *
 * // Check if constraint is satisfied
 * bool ok = check_version_constraint(">=2.0.0", "2.1.0"); // true
 */

#include <optional>
#include <string>

namespace helix::version {

/**
 * @brief Semantic version components
 *
 * @c prerelease holds the dot-separated identifiers after the `-`, without the
 * leading `-`, and is empty for a release. Build metadata (`+sha`) does not
 * affect precedence and is not kept.
 */
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease{};

    /// The version without its prerelease, for the compatibility question
    /// check_version_constraint() asks.
    Version core() const {
        return Version{major, minor, patch, {}};
    }

    bool operator==(const Version& other) const {
        return major == other.major && minor == other.minor && patch == other.patch &&
               prerelease == other.prerelease;
    }

    /// Semantic Versioning 2.0.0 precedence: the triple numerically, then a
    /// prerelease below the same triple without one, then identifier by
    /// identifier (numeric ones numerically and below alphanumeric ones), and
    /// finally the shorter identifier list first.
    bool operator<(const Version& other) const;

    bool operator>(const Version& other) const {
        return other < *this;
    }

    bool operator<=(const Version& other) const {
        return !(other < *this);
    }

    bool operator>=(const Version& other) const {
        return !(*this < other);
    }

    bool operator!=(const Version& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Parse a semantic version string
 *
 * Accepts formats:
 * - "1" -> 1.0.0
 * - "1.2" -> 1.2.0
 * - "1.2.3" -> 1.2.3
 * - "1.2.3-beta.1" -> 1.2.3 with prerelease "beta.1"
 * - "1.2.3+build" -> 1.2.3 (build metadata ignored)
 * - "1.2.3-beta.1+sha.abc" -> 1.2.3 with prerelease "beta.1"
 *
 * A leading "v" or "V" is skipped. A prerelease must be dot-separated
 * identifiers of [0-9A-Za-z-]; an empty identifier or a leading zero on a
 * numeric identifier is rejected, as is a negative component.
 *
 * @param version_str Version string to parse
 * @return Parsed version, or nullopt if invalid
 */
std::optional<Version> parse_version(const std::string& version_str);

/**
 * @brief Check if a version satisfies a constraint
 *
 * Constraint format: [operator]version
 * - ">=2.0.0" - version must be >= 2.0.0
 * - ">1.0.0" - version must be > 1.0.0
 * - "=2.0.0" or "2.0.0" - version must equal 2.0.0
 * - "<3.0.0" - version must be < 3.0.0
 * - "<=2.5.0" - version must be <= 2.5.0
 *
 * Compares the core triple only, so a plugin declaring ">=1.1.0" loads on
 * 1.1.0-beta.5. Compatibility and ordering are different questions: applying
 * precedence here would make every beta refuse the plugins built for the
 * release it is a beta of.
 *
 * @param constraint Version constraint string
 * @param version Version string to check
 * @return true if version satisfies constraint, false otherwise
 */
bool check_version_constraint(const std::string& constraint, const std::string& version);

/**
 * @brief Convert Version to string
 *
 * @param v Version to convert
 * @return String representation, prerelease included (e.g. "1.2.3-beta.1")
 */
std::string to_string(const Version& v);

} // namespace helix::version
