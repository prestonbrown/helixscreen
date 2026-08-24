// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_keyboard_alternates.cpp
 * @brief Long-press alternate characters and session MRU reordering
 *
 * Covers KeyboardManager::find_alternatives() and promote_alternative(): the
 * shipped mapping, and the "last character you picked becomes this key's default"
 * behaviour that backs the multi-character ',' and '.' keys.
 *
 * KeyboardManager is a process-wide singleton and alt_order_ persists for the
 * lifetime of the test binary, so every assertion here is deliberately written to
 * be **independent of the starting order**. A test that assumed the shipped order
 * would pass or fail depending on which other test ran first.
 */

#include "ui_keyboard_manager.h"

#include <algorithm>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Alternates for a key as a std::string ("" when the key has none).
std::string alts(char key) {
    const char* a = KeyboardManager::instance().find_alternatives(key);
    return a ? std::string(a) : std::string();
}

/// Character multiset of a key's alternates, for order-independent comparison.
std::multiset<char> alt_set(char key) {
    std::string s = alts(key);
    return std::multiset<char>(s.begin(), s.end());
}

} // namespace

TEST_CASE("Keyboard alternates: letters carry their documented character", "[ui][keyboard_alt]") {
    // The mapping the user guide publishes. If these change, the table in
    // docs/user/guide/getting-started.md is wrong and must change with them.
    REQUIRE(alts('f') == "_");
    REQUIRE(alts('h') == "-");
    REQUIRE(alts('j') == "+");
    REQUIRE(alts('a') == "@");
    REQUIRE(alts('q') == "1");
    REQUIRE(alts('p') == "0");
    REQUIRE(alts('m') == "?");
}

TEST_CASE("Keyboard alternates: uppercase maps the same as lowercase", "[ui][keyboard_alt]") {
    // Shift must not change which alternate a key offers.
    for (char lower = 'a'; lower <= 'z'; ++lower) {
        const char upper = static_cast<char>(lower - 'a' + 'A');
        INFO("key " << lower);
        REQUIRE(alts(lower) == alts(upper));
        REQUIRE_FALSE(alts(lower).empty());
    }
}

TEST_CASE("Keyboard alternates: punctuation keys carry three characters each",
          "[ui][keyboard_alt]") {
    // ',' and '.' are the only multi-character keys. Compared as sets because an
    // earlier test may have promoted one of them.
    REQUIRE(alt_set(',') == std::multiset<char>({'=', '<', '>'}));
    REQUIRE(alt_set('.') == std::multiset<char>({'/', '[', ']'}));
}

TEST_CASE("Keyboard alternates: unmapped keys have none", "[ui][keyboard_alt]") {
    REQUIRE(KeyboardManager::instance().find_alternatives(' ') == nullptr);
    REQUIRE(KeyboardManager::instance().find_alternatives('1') == nullptr);
    REQUIRE(KeyboardManager::instance().find_alternatives(0) == nullptr);
}

TEST_CASE("Keyboard MRU: promoting moves a character to the front", "[ui][keyboard_alt]") {
    auto& kb = KeyboardManager::instance();

    kb.promote_alternative(',', '<');
    REQUIRE(alts(',').front() == '<');

    kb.promote_alternative(',', '>');
    REQUIRE(alts(',').front() == '>');

    kb.promote_alternative(',', '=');
    REQUIRE(alts(',').front() == '=');
}

TEST_CASE("Keyboard MRU: promoting never loses or duplicates a character", "[ui][keyboard_alt]") {
    auto& kb = KeyboardManager::instance();
    const std::multiset<char> before = alt_set(',');

    kb.promote_alternative(',', '<');
    REQUIRE(alt_set(',') == before);
    kb.promote_alternative(',', '>');
    REQUIRE(alt_set(',') == before);
    kb.promote_alternative(',', '<');
    REQUIRE(alt_set(',') == before);

    REQUIRE(alts(',').size() == 3);
}

TEST_CASE("Keyboard MRU: promoting the current default is a no-op", "[ui][keyboard_alt]") {
    auto& kb = KeyboardManager::instance();

    kb.promote_alternative(',', '<');
    const std::string after_first = alts(',');

    kb.promote_alternative(',', '<'); // already front
    REQUIRE(alts(',') == after_first);
}

TEST_CASE("Keyboard MRU: promoting a character the key does not offer is a no-op",
          "[ui][keyboard_alt]") {
    auto& kb = KeyboardManager::instance();
    const std::string before = alts(',');

    kb.promote_alternative(',', 'Z'); // not one of = < >
    REQUIRE(alts(',') == before);

    kb.promote_alternative(',', 0);
    REQUIRE(alts(',') == before);
}

TEST_CASE("Keyboard MRU: promotion is per-key", "[ui][keyboard_alt]") {
    // Regression guard: a shared or global "last pick" would let a promotion on one
    // key change another. Verified at runtime too ('.' kept '/' after ',' moved).
    auto& kb = KeyboardManager::instance();

    const std::string dot_before = alts('.');
    kb.promote_alternative(',', '>');
    REQUIRE(alts('.') == dot_before);

    const std::string comma_before = alts(',');
    kb.promote_alternative('.', ']');
    REQUIRE(alts(',') == comma_before);
    REQUIRE(alts('.').front() == ']');
}

TEST_CASE("Keyboard MRU: single-alternate keys are unaffected", "[ui][keyboard_alt]") {
    // Letters have exactly one alternate, so there is nothing to reorder. Promoting
    // must neither change them nor invent an entry.
    auto& kb = KeyboardManager::instance();

    kb.promote_alternative('f', '_'); // already the only one
    REQUIRE(alts('f') == "_");

    kb.promote_alternative('f', '@'); // belongs to 'a', not 'f'
    REQUIRE(alts('f') == "_");
}

TEST_CASE("Keyboard MRU: promoting a key with no alternates does not create one",
          "[ui][keyboard_alt]") {
    auto& kb = KeyboardManager::instance();

    kb.promote_alternative(' ', 'x');
    REQUIRE(kb.find_alternatives(' ') == nullptr);
}
