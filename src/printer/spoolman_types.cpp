// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_types.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <vector>

// ============================================================================
// Fuzzy String Matching
// ============================================================================

namespace {

/// Levenshtein edit distance between two strings.
/// Uses a single-row DP approach (O(min(a,b)) space).
size_t levenshtein(const std::string& a, const std::string& b) {
    const size_t m = a.size();
    const size_t n = b.size();
    if (m == 0)
        return n;
    if (n == 0)
        return m;

    std::vector<size_t> prev(n + 1);
    for (size_t j = 0; j <= n; ++j)
        prev[j] = j;

    for (size_t i = 1; i <= m; ++i) {
        size_t prev_diag = prev[0];
        prev[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            size_t tmp = prev[j];
            if (a[i - 1] == b[j - 1]) {
                prev[j] = prev_diag;
            } else {
                prev[j] = 1 + std::min({prev_diag, prev[j], prev[j - 1]});
            }
            prev_diag = tmp;
        }
    }
    return prev[n];
}

/// Maximum edit distance allowed for a given search term length.
/// <=4 chars: 1 edit (tight for short words). >4 chars: 2 edits.
size_t fuzzy_threshold(size_t term_length) {
    return (term_length <= 4) ? 1 : 2;
}

/// Check if a term looks like a numeric ID (digits, or # followed by digits).
/// Fuzzy matching is skipped for these since typos in numbers are semantically different.
bool is_numeric_term(const std::string& term) {
    if (term.empty())
        return false;
    size_t start = (term[0] == '#') ? 1 : 0;
    if (start >= term.size())
        return false;
    return std::all_of(term.begin() + static_cast<ptrdiff_t>(start), term.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

/// Check if a search term fuzzy-matches any word in the searchable text.
/// Words are split on spaces. Returns true if any word is within edit distance threshold.
bool fuzzy_match_any_word(const std::string& term, const std::string& searchable) {
    // Don't fuzzy-match numeric terms (IDs) — exact substring is sufficient
    if (is_numeric_term(term))
        return false;

    size_t threshold = fuzzy_threshold(term.size());

    std::istringstream words(searchable);
    std::string word;
    while (words >> word) {
        // Skip words with very different lengths — they can't be typo matches
        auto len_diff =
            (term.size() > word.size()) ? term.size() - word.size() : word.size() - term.size();
        if (len_diff > threshold)
            continue;

        if (levenshtein(term, word) <= threshold) {
            return true;
        }
    }
    return false;
}

} // namespace

// ============================================================================
// Spool Filtering
// ============================================================================

std::vector<SpoolInfo> filter_spools(const std::vector<SpoolInfo>& spools,
                                     const std::string& query) {
    // Empty or whitespace-only query returns all spools.
    // The stream >> term loop skips whitespace, so terms will be empty for whitespace-only input.
    if (query.empty()) {
        return spools;
    }

    // Split query into lowercase terms (space-separated)
    std::vector<std::string> terms;
    std::istringstream stream(query);
    std::string term;
    while (stream >> term) {
        std::transform(term.begin(), term.end(), term.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        terms.push_back(std::move(term));
    }

    if (terms.empty()) {
        return spools;
    }

    std::vector<SpoolInfo> result;
    result.reserve(spools.size());

    for (const auto& spool : spools) {
        // Build searchable text: "#ID vendor material filament_name location"
        std::string searchable = "#" + std::to_string(spool.id) + " " + spool.vendor + " " +
                                 spool.material + " " + spool.filament_name + " " + spool.location;

        // Lowercase the searchable text
        std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // All terms must match (AND logic): exact substring first, fuzzy fallback
        bool all_match =
            std::all_of(terms.begin(), terms.end(), [&searchable](const std::string& t) {
                // Fast path: exact substring match
                if (searchable.find(t) != std::string::npos) {
                    return true;
                }
                // Slow path: fuzzy match against individual words
                return fuzzy_match_any_word(t, searchable);
            });

        if (all_match) {
            result.push_back(spool);
        }
    }

    return result;
}

#if defined(ESP_PLATFORM)
// newlib on ESP-IDF does not provide timegm(). Pure days-from-civil conversion
// (Howard Hinnant's algorithm): UTC by definition, thread-safe, no TZ churn.
static int64_t timegm(std::tm* tm) {
    int64_t y = tm->tm_year + 1900;
    const int m = tm->tm_mon + 1;
    const int d = tm->tm_mday;
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u +
                         static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}
#endif

std::optional<int64_t> parse_spool_timestamp(const std::string& ts) {
    // Shortest accepted form is "YYYY-MM-DDTHH:MM:SS" (19 chars).
    if (ts.size() < 19)
        return std::nullopt;

    std::tm tm{};
    // strptime stops after seconds; the remainder (fractional seconds, zone) is
    // handled below from the same offset.
    const char* rest = strptime(ts.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (rest == nullptr)
        return std::nullopt;

    // timegm() interprets the fields as UTC; a trailing zone offset is applied
    // afterwards. A naive timestamp (no zone) is treated as UTC — Spoolman is
    // internally consistent, so every spool in one list gets the same treatment
    // and relative ordering is preserved either way.
    int64_t epoch = static_cast<int64_t>(timegm(&tm));

    // Skip fractional seconds: ".123456"
    if (*rest == '.') {
        ++rest;
        while (*rest != '\0' && std::isdigit(static_cast<unsigned char>(*rest)))
            ++rest;
    }

    // Trailing zone: "Z" (already UTC) or "+HH:MM" / "-HH:MM" to subtract.
    if (*rest == '+' || *rest == '-') {
        const int sign = (*rest == '+') ? 1 : -1;
        // Need at least "HH:MM" or "HHMM" after the sign.
        const std::string zone(rest + 1);
        if (zone.size() >= 4) {
            const bool colon = (zone.size() >= 5 && zone[2] == ':');
            const std::string hh = zone.substr(0, 2);
            const std::string mm = colon ? zone.substr(3, 2) : zone.substr(2, 2);
            if (std::all_of(hh.begin(), hh.end(),
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) &&
                std::all_of(mm.begin(), mm.end(),
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                const int offset_s = ((std::stoi(hh) * 60) + std::stoi(mm)) * 60;
                epoch -= sign * offset_s; // local -> UTC
            }
        }
    }

    return epoch;
}

int64_t spool_recency_key(const SpoolInfo& spool) {
    const auto used = parse_spool_timestamp(spool.last_used);
    const auto made = parse_spool_timestamp(spool.registered);

    if (used && made)
        return std::max(*used, *made);
    if (used)
        return *used;
    if (made)
        return *made;
    return SPOOL_RECENCY_NONE;
}

void sort_spools_by_recency(std::vector<SpoolInfo>& spools) {
    // Single descending key: the spool's most recent activity of either kind
    // (#1071). A never-used spool ranks on its registration date rather than
    // being pushed below every used spool, so a spool added today appears at the
    // top even though it has never been printed with.
    std::sort(spools.begin(), spools.end(), [](const SpoolInfo& a, const SpoolInfo& b) {
        const int64_t ka = spool_recency_key(a);
        const int64_t kb = spool_recency_key(b);
        if (ka != kb)
            return ka > kb;
        return a.id > b.id; // deterministic tie-break; no churn across refreshes
    });
}
