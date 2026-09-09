// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "version.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace helix::version {

namespace {

/// Split a prerelease on '.' into its identifiers. An empty prerelease has none.
std::vector<std::string_view> split_identifiers(const std::string& pre) {
    std::vector<std::string_view> ids;
    if (pre.empty()) {
        return ids;
    }
    const std::string_view all(pre);
    size_t pos = 0;
    while (true) {
        const size_t dot = all.find('.', pos);
        if (dot == std::string_view::npos) {
            ids.push_back(all.substr(pos));
            return ids;
        }
        ids.push_back(all.substr(pos, dot - pos));
        pos = dot + 1;
    }
}

bool identifier_is_numeric(std::string_view id) {
    return !id.empty() && id.find_first_not_of("0123456789") == std::string_view::npos;
}

/// A digit run with its leading zeros removed, so the remaining length orders
/// magnitude and equal lengths compare lexically. "0" and "000" both empty out.
std::string_view significant_digits(std::string_view id) {
    const size_t first = id.find_first_not_of('0');
    if (first == std::string_view::npos) {
        return {};
    }
    return id.substr(first);
}

/// -1, 0 or +1 for a before, equal to, or after b under Semantic Versioning
/// 2.0.0 precedence rules 3 and 4. Both sides must be non-empty prereleases.
int compare_prerelease(const std::string& a, const std::string& b) {
    const auto ia = split_identifiers(a);
    const auto ib = split_identifiers(b);

    const size_t shared = std::min(ia.size(), ib.size());
    for (size_t i = 0; i < shared; ++i) {
        const bool numeric_a = identifier_is_numeric(ia[i]);
        const bool numeric_b = identifier_is_numeric(ib[i]);
        if (numeric_a != numeric_b) {
            return numeric_a ? -1 : 1;
        }
        if (numeric_a) {
            const std::string_view da = significant_digits(ia[i]);
            const std::string_view db = significant_digits(ib[i]);
            if (da.size() != db.size()) {
                return da.size() < db.size() ? -1 : 1;
            }
            if (da != db) {
                return da < db ? -1 : 1;
            }
        } else if (ia[i] != ib[i]) {
            return ia[i] < ib[i] ? -1 : 1;
        }
    }

    if (ia.size() != ib.size()) {
        return ia.size() < ib.size() ? -1 : 1;
    }
    return 0;
}

/// Validate a prerelease as dot-separated identifiers of [0-9A-Za-z-]. An empty
/// identifier, a character outside that set, or a leading zero on a numeric
/// identifier is rejected.
bool prerelease_is_valid(const std::string& pre) {
    if (pre.empty()) {
        return false;
    }
    for (std::string_view id : split_identifiers(pre)) {
        if (id.empty()) {
            return false;
        }
        for (char c : id) {
            const bool allowed = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                                 (c >= 'a' && c <= 'z') || c == '-';
            if (!allowed) {
                return false;
            }
        }
        if (identifier_is_numeric(id) && id.size() > 1 && id.front() == '0') {
            return false;
        }
    }
    return true;
}

} // namespace

bool Version::operator<(const Version& other) const {
    if (major != other.major) {
        return major < other.major;
    }
    if (minor != other.minor) {
        return minor < other.minor;
    }
    if (patch != other.patch) {
        return patch < other.patch;
    }
    if (prerelease.empty() || other.prerelease.empty()) {
        // A prerelease ranks below the release of the same triple; two releases
        // of one triple are equal.
        return !prerelease.empty() && other.prerelease.empty();
    }
    return compare_prerelease(prerelease, other.prerelease) < 0;
}

std::optional<Version> parse_version(const std::string& version_str) {
    if (version_str.empty()) {
        return std::nullopt;
    }

    // Skip leading 'v' or 'V' if present (e.g., "v1.2.3")
    const char* start = version_str.c_str();
    if (*start == 'v' || *start == 'V') {
        start++;
    }

    Version v{};
    int* components[] = {&v.major, &v.minor, &v.patch};
    int component_idx = 0;

    const char* end = version_str.c_str() + version_str.length();

    while (start < end && component_idx < 3) {
        // Skip any leading whitespace
        while (start < end && (*start == ' ' || *start == '\t')) {
            start++;
        }

        if (start >= end) {
            break;
        }

        // Stop at pre-release (-) or build metadata (+)
        if (*start == '-' || *start == '+') {
            break;
        }

        // Parse the number (strtol instead of from_chars for GCC 7 compat)
        char* parse_end = nullptr;
        long value = std::strtol(start, &parse_end, 10);

        if (parse_end == start) {
            // Failed to parse - if we got at least major, that's ok
            if (component_idx == 0) {
                return std::nullopt;
            }
            break;
        }
        const char* ptr = parse_end;

        if (value < 0) {
            return std::nullopt; // Negative versions not allowed
        }

        *components[component_idx] = static_cast<int>(value);
        component_idx++;
        start = ptr;

        // Skip the dot separator
        if (start < end && *start == '.') {
            start++;
        } else if (start < end && *start != '-' && *start != '+' && *start != '\0') {
            // Invalid character
            break;
        }
    }

    // Must have at least major version
    if (component_idx == 0) {
        return std::nullopt;
    }

    // Prerelease: everything from the '-' up to build metadata or end of input.
    // Anything else that stopped the component loop leaves the version bare.
    if (start < end && *start == '-') {
        const char* pre_begin = start + 1;
        const char* pre_end = pre_begin;
        while (pre_end < end && *pre_end != '+') {
            pre_end++;
        }
        while (pre_end > pre_begin && (pre_end[-1] == ' ' || pre_end[-1] == '\t')) {
            pre_end--;
        }
        std::string pre(pre_begin, pre_end);
        if (!prerelease_is_valid(pre)) {
            return std::nullopt;
        }
        v.prerelease = std::move(pre);
    }

    return v;
}

bool check_version_constraint(const std::string& constraint, const std::string& version) {
    if (constraint.empty()) {
        // Empty constraint matches anything
        return true;
    }

    auto current = parse_version(version);
    if (!current) {
        spdlog::warn("[version] Failed to parse version: {}", version);
        return false;
    }

    // Parse operator and required version from constraint
    const char* c = constraint.c_str();

    // Skip leading whitespace
    while (*c == ' ' || *c == '\t') {
        c++;
    }

    enum class Op { EQ, GT, GE, LT, LE };
    Op op = Op::EQ;

    if (c[0] == '>' && c[1] == '=') {
        op = Op::GE;
        c += 2;
    } else if (c[0] == '<' && c[1] == '=') {
        op = Op::LE;
        c += 2;
    } else if (c[0] == '>') {
        op = Op::GT;
        c += 1;
    } else if (c[0] == '<') {
        op = Op::LT;
        c += 1;
    } else if (c[0] == '=') {
        op = Op::EQ;
        c += 1;
    }
    // else: no operator means equality

    // Skip whitespace after operator
    while (*c == ' ' || *c == '\t') {
        c++;
    }

    auto required = parse_version(c);
    if (!required) {
        spdlog::warn("[version] Failed to parse constraint version: {}", constraint);
        return false;
    }

    spdlog::debug("[version] Checking {} against constraint {} (op={}, required={}.{}.{})", version,
                  constraint, static_cast<int>(op), required->major, required->minor,
                  required->patch);

    // Compatibility is a question about the triple: a plugin built for 1.1.0
    // must load on 1.1.0-beta.5, which precedence would refuse.
    const Version have = current->core();
    const Version want = required->core();

    switch (op) {
    case Op::EQ:
        return have == want;
    case Op::GT:
        return have > want;
    case Op::GE:
        return have >= want;
    case Op::LT:
        return have < want;
    case Op::LE:
        return have <= want;
    }

    return false;
}

std::string to_string(const Version& v) {
    std::string out =
        std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    if (!v.prerelease.empty()) {
        out += "-" + v.prerelease;
    }
    return out;
}

} // namespace helix::version
