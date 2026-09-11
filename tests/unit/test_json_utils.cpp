// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json_utils.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace ju = helix::json_util;

// ============================================================================
// safe_string tests
// ============================================================================

TEST_CASE("safe_string returns value for normal string", "[json_utils]") {
    json j = {{"name", "PLA Red"}};
    CHECK(helix::json_util::safe_string(j, "name") == "PLA Red");
}

TEST_CASE("safe_string returns default for null field", "[json_utils]") {
    json j = {{"name", nullptr}};
    CHECK(helix::json_util::safe_string(j, "name") == "");
    CHECK(helix::json_util::safe_string(j, "name", "fallback") == "fallback");
}

TEST_CASE("safe_string returns default for missing field", "[json_utils]") {
    json j = {{"other", "value"}};
    CHECK(helix::json_util::safe_string(j, "name") == "");
    CHECK(helix::json_util::safe_string(j, "name", "default") == "default");
}

TEST_CASE("safe_string returns default for non-string type", "[json_utils]") {
    json j = {{"name", 42}};
    CHECK(helix::json_util::safe_string(j, "name") == "");
}

TEST_CASE("safe_string handles empty string", "[json_utils]") {
    json j = {{"name", ""}};
    CHECK(helix::json_util::safe_string(j, "name") == "");
}

// ============================================================================
// safe_int tests
// ============================================================================

TEST_CASE("safe_int returns value for normal int", "[json_utils]") {
    json j = {{"id", 42}};
    CHECK(helix::json_util::safe_int(j, "id") == 42);
}

TEST_CASE("safe_int returns default for null field", "[json_utils]") {
    json j = {{"id", nullptr}};
    CHECK(helix::json_util::safe_int(j, "id") == 0);
    CHECK(helix::json_util::safe_int(j, "id", -1) == -1);
}

TEST_CASE("safe_int returns default for missing field", "[json_utils]") {
    json j = {{"other", 1}};
    CHECK(helix::json_util::safe_int(j, "id") == 0);
    CHECK(helix::json_util::safe_int(j, "id", 99) == 99);
}

TEST_CASE("safe_int parses string integers", "[json_utils]") {
    json j = {{"id", "123"}};
    CHECK(helix::json_util::safe_int(j, "id") == 123);
}

TEST_CASE("safe_int returns default for non-numeric string", "[json_utils]") {
    json j = {{"id", "not-a-number"}};
    CHECK(helix::json_util::safe_int(j, "id") == 0);
    CHECK(helix::json_util::safe_int(j, "id", -1) == -1);
}

TEST_CASE("safe_int parses leading digits from mixed string", "[json_utils]") {
    // stoi("3d-fuel...") parses the leading "3" — this is expected behavior
    json j = {{"id", "3d-fuel_pla+_almond"}};
    CHECK(helix::json_util::safe_int(j, "id") == 3);
}

TEST_CASE("safe_int handles float JSON values", "[json_utils]") {
    json j = {{"id", 3.7}};
    CHECK(helix::json_util::safe_int(j, "id") == 3);
}

// ============================================================================
// safe_float tests
// ============================================================================

TEST_CASE("safe_float returns value for normal float", "[json_utils]") {
    json j = {{"density", 1.24f}};
    CHECK(helix::json_util::safe_float(j, "density") == Catch::Approx(1.24f));
}

TEST_CASE("safe_float returns default for null field", "[json_utils]") {
    json j = {{"density", nullptr}};
    CHECK(helix::json_util::safe_float(j, "density") == 0.0f);
    CHECK(helix::json_util::safe_float(j, "density", 1.0f) == 1.0f);
}

TEST_CASE("safe_float returns default for missing field", "[json_utils]") {
    json j = {{"other", 1}};
    CHECK(helix::json_util::safe_float(j, "density") == 0.0f);
}

TEST_CASE("safe_float parses string floats", "[json_utils]") {
    json j = {{"density", "1.24"}};
    CHECK(helix::json_util::safe_float(j, "density") == Catch::Approx(1.24f));
}

TEST_CASE("safe_float returns default for non-numeric string", "[json_utils]") {
    json j = {{"density", "unknown"}};
    CHECK(helix::json_util::safe_float(j, "density") == 0.0f);
}

TEST_CASE("safe_float returns default for NaN value", "[json_utils]") {
    json j;
    j["weight"] = std::numeric_limits<float>::quiet_NaN();
    CHECK(helix::json_util::safe_float(j, "weight") == 0.0f);
    CHECK(helix::json_util::safe_float(j, "weight", -1.0f) == -1.0f);
}

TEST_CASE("safe_float returns default for infinity value", "[json_utils]") {
    json j;
    j["weight"] = std::numeric_limits<float>::infinity();
    CHECK(helix::json_util::safe_float(j, "weight") == 0.0f);
    j["weight"] = -std::numeric_limits<float>::infinity();
    CHECK(helix::json_util::safe_float(j, "weight") == 0.0f);
}

TEST_CASE("safe_float returns default for inf string", "[json_utils]") {
    json j = {{"weight", "inf"}};
    CHECK(helix::json_util::safe_float(j, "weight") == 0.0f);
}

// ============================================================================
// safe_double tests
// ============================================================================

TEST_CASE("safe_double returns value for normal double", "[json_utils]") {
    json j = {{"weight", 1000.5}};
    CHECK(helix::json_util::safe_double(j, "weight") == Catch::Approx(1000.5));
}

TEST_CASE("safe_double returns default for null field", "[json_utils]") {
    json j = {{"weight", nullptr}};
    CHECK(helix::json_util::safe_double(j, "weight") == 0.0);
    CHECK(helix::json_util::safe_double(j, "weight", -1.0) == -1.0);
}

TEST_CASE("safe_double parses string doubles", "[json_utils]") {
    json j = {{"weight", "1000.5"}};
    CHECK(helix::json_util::safe_double(j, "weight") == Catch::Approx(1000.5));
}

TEST_CASE("safe_double returns default for NaN value", "[json_utils]") {
    json j;
    j["weight"] = std::numeric_limits<double>::quiet_NaN();
    CHECK(helix::json_util::safe_double(j, "weight") == 0.0);
    CHECK(helix::json_util::safe_double(j, "weight", -1.0) == -1.0);
}

TEST_CASE("safe_double returns default for infinity value", "[json_utils]") {
    json j;
    j["weight"] = std::numeric_limits<double>::infinity();
    CHECK(helix::json_util::safe_double(j, "weight") == 0.0);
    j["weight"] = -std::numeric_limits<double>::infinity();
    CHECK(helix::json_util::safe_double(j, "weight") == 0.0);
}

// ============================================================================
// Non-object receiver — every helper must survive `j` itself not being an
// object. nlohmann's .value() throws type_error.306 here; contains() returns
// false without throwing, which is what the helpers are built on.
// ============================================================================

TEST_CASE("helpers return default when receiver is not an object", "[json_utils]") {
    const json nul = json(nullptr);
    const json arr = json::array({1, 2, 3});
    const json str = json("a bare string");
    const json num = json(42);

    for (const json& j : {nul, arr, str, num}) {
        CHECK(ju::safe_string(j, "k", "d") == "d");
        CHECK(ju::safe_int(j, "k", -1) == -1);
        CHECK(ju::safe_float(j, "k", -1.0f) == -1.0f);
        CHECK(ju::safe_double(j, "k", -1.0) == -1.0);
        CHECK(ju::safe_bool(j, "k", true) == true);
        CHECK(ju::safe_int64(j, "k", -1) == -1);
        CHECK(ju::safe_uint64(j, "k", 7u) == 7u);
        CHECK(ju::safe_size_t(j, "k", 7u) == 7u);
    }
}

TEST_CASE("default-constructed json is a safe receiver", "[json_utils]") {
    // TipsManager::get_version() reads a member `json data` that is still null
    // when init() took the file-not-found early return. .value() throws 306
    // there; the helper must not.
    const json data;
    CHECK(ju::safe_string(data, "version", "unknown") == "unknown");
}

// ============================================================================
// safe_bool tests
// ============================================================================

TEST_CASE("safe_bool returns value for real booleans", "[json_utils]") {
    json j = {{"yes", true}, {"no", false}};
    CHECK(ju::safe_bool(j, "yes") == true);
    CHECK(ju::safe_bool(j, "no", true) == false);
}

TEST_CASE("safe_bool returns default for null field", "[json_utils]") {
    // The Klipper case: filament_detected is published as null before the
    // sensor's first read.
    json j = {{"filament_detected", nullptr}};
    CHECK(ju::safe_bool(j, "filament_detected") == false);
    CHECK(ju::safe_bool(j, "filament_detected", true) == true);
}

TEST_CASE("safe_bool returns default for missing field", "[json_utils]") {
    json j = {{"other", true}};
    CHECK(ju::safe_bool(j, "flag") == false);
    CHECK(ju::safe_bool(j, "flag", true) == true);
}

TEST_CASE("safe_bool coerces numbers by zero/non-zero", "[json_utils]") {
    json j = {{"a", 0}, {"b", 1}, {"c", -3}, {"d", 0.0}, {"e", 0.5}};
    CHECK(ju::safe_bool(j, "a", true) == false);
    CHECK(ju::safe_bool(j, "b") == true);
    CHECK(ju::safe_bool(j, "c") == true);
    CHECK(ju::safe_bool(j, "d", true) == false);
    CHECK(ju::safe_bool(j, "e") == true);
}

TEST_CASE("safe_bool returns default for non-finite numbers", "[json_utils]") {
    json j;
    j["flag"] = std::numeric_limits<double>::quiet_NaN();
    CHECK(ju::safe_bool(j, "flag", true) == true);
    CHECK(ju::safe_bool(j, "flag", false) == false);
    j["flag"] = std::numeric_limits<double>::infinity();
    CHECK(ju::safe_bool(j, "flag", true) == true);
}

TEST_CASE("safe_bool honors the whitelisted string spellings", "[json_utils]") {
    for (const char* s : {"true", "TRUE", "True", "1", "yes", "YES", "on", "On"}) {
        json j = {{"flag", s}};
        INFO("spelling: " << s);
        CHECK(ju::safe_bool(j, "flag", false) == true);
    }
    for (const char* s : {"false", "FALSE", "False", "0", "no", "NO", "off", "Off"}) {
        json j = {{"flag", s}};
        INFO("spelling: " << s);
        CHECK(ju::safe_bool(j, "flag", true) == false);
    }
}

TEST_CASE("safe_bool does NOT treat an arbitrary non-empty string as true", "[json_utils]") {
    // This is the whole point of the closed whitelist: the naive `!s.empty()`
    // shorthand reads "false" as TRUE. An unrecognized spelling must fall back
    // to the caller's default instead of being guessed at.
    for (const char* s : {"maybe", "enabled", "null", "2", "-1", "", "  "}) {
        json j = {{"flag", s}};
        INFO("spelling: " << s);
        CHECK(ju::safe_bool(j, "flag", false) == false);
        CHECK(ju::safe_bool(j, "flag", true) == true);
    }
}

TEST_CASE("safe_bool returns default for object and array types", "[json_utils]") {
    json j = {{"o", json::object()}, {"a", json::array({1})}};
    CHECK(ju::safe_bool(j, "o", true) == true);
    CHECK(ju::safe_bool(j, "a", true) == true);
}

// ============================================================================
// safe_int range guard
// ============================================================================

TEST_CASE("safe_int returns default for values outside int range", "[json_utils]") {
    json j;
    j["big"] = static_cast<std::int64_t>(5000000000);    // > INT32_MAX
    j["small"] = static_cast<std::int64_t>(-5000000000); // < INT32_MIN
    CHECK(ju::safe_int(j, "big", -1) == -1);
    CHECK(ju::safe_int(j, "small", -1) == -1);

    // Boundaries still pass through.
    j["max"] = std::numeric_limits<int>::max();
    j["min"] = std::numeric_limits<int>::min();
    CHECK(ju::safe_int(j, "max") == std::numeric_limits<int>::max());
    CHECK(ju::safe_int(j, "min") == std::numeric_limits<int>::min());
}

TEST_CASE("safe_int returns default for out-of-range numeric strings", "[json_utils]") {
    json j = {{"big", "99999999999999999999"}, {"ok", "-42"}};
    CHECK(ju::safe_int(j, "big", -1) == -1);
    CHECK(ju::safe_int(j, "ok") == -42);
}

TEST_CASE("safe_int returns default for non-finite float", "[json_utils]") {
    json j;
    j["v"] = std::numeric_limits<double>::quiet_NaN();
    CHECK(ju::safe_int(j, "v", -1) == -1);
    j["v"] = std::numeric_limits<double>::infinity();
    CHECK(ju::safe_int(j, "v", -1) == -1);
}

// ============================================================================
// safe_int64 tests
// ============================================================================

TEST_CASE("safe_int64 round-trips values int cannot hold", "[json_utils]") {
    json j;
    j["v"] = static_cast<std::int64_t>(5000000000);
    CHECK(ju::safe_int64(j, "v") == 5000000000LL);
    j["v"] = std::numeric_limits<std::int64_t>::max();
    CHECK(ju::safe_int64(j, "v") == std::numeric_limits<std::int64_t>::max());
    j["v"] = std::numeric_limits<std::int64_t>::min();
    CHECK(ju::safe_int64(j, "v") == std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("safe_int64 handles null, missing and wrong type", "[json_utils]") {
    json j = {{"n", nullptr}, {"s", "not-a-number"}, {"o", json::object()}};
    CHECK(ju::safe_int64(j, "n", -7) == -7);
    CHECK(ju::safe_int64(j, "absent", -7) == -7);
    CHECK(ju::safe_int64(j, "s", -7) == -7);
    CHECK(ju::safe_int64(j, "o", -7) == -7);
}

TEST_CASE("safe_int64 parses numeric strings", "[json_utils]") {
    json j = {{"v", "-9007199254740993"}};
    CHECK(ju::safe_int64(j, "v") == -9007199254740993LL);
}

TEST_CASE("safe_int64 rejects an unsigned value above INT64_MAX", "[json_utils]") {
    json j;
    j["v"] = std::numeric_limits<std::uint64_t>::max();
    CHECK(ju::safe_int64(j, "v", -1) == -1);
}

TEST_CASE("safe_int64 rejects out-of-range and non-finite floats", "[json_utils]") {
    json j;
    j["v"] = 1e30;
    CHECK(ju::safe_int64(j, "v", -1) == -1);
    j["v"] = -1e30;
    CHECK(ju::safe_int64(j, "v", -1) == -1);
    j["v"] = std::numeric_limits<double>::quiet_NaN();
    CHECK(ju::safe_int64(j, "v", -1) == -1);
    j["v"] = std::numeric_limits<double>::infinity();
    CHECK(ju::safe_int64(j, "v", -1) == -1);
}

// ============================================================================
// safe_uint64 tests
// ============================================================================

TEST_CASE("safe_uint64 round-trips large unsigned values", "[json_utils]") {
    json j;
    j["v"] = std::numeric_limits<std::uint64_t>::max();
    CHECK(ju::safe_uint64(j, "v") == std::numeric_limits<std::uint64_t>::max());
    j["v"] = static_cast<std::uint64_t>(0);
    CHECK(ju::safe_uint64(j, "v", 99u) == 0u);
}

TEST_CASE("safe_uint64 handles null, missing and wrong type", "[json_utils]") {
    json j = {{"n", nullptr}, {"s", "nope"}, {"a", json::array()}};
    CHECK(ju::safe_uint64(j, "n", 5u) == 5u);
    CHECK(ju::safe_uint64(j, "absent", 5u) == 5u);
    CHECK(ju::safe_uint64(j, "s", 5u) == 5u);
    CHECK(ju::safe_uint64(j, "a", 5u) == 5u);
}

TEST_CASE("safe_uint64 rejects negative numbers rather than wrapping", "[json_utils]") {
    json j = {{"v", -1}};
    CHECK(ju::safe_uint64(j, "v", 5u) == 5u);
    j["v"] = -0.5;
    CHECK(ju::safe_uint64(j, "v", 5u) == 5u);
}

TEST_CASE("safe_uint64 rejects a negative numeric STRING rather than wrapping", "[json_utils]") {
    // std::stoull("-1") returns 18446744073709551615 without throwing. Guarding
    // this is the reason the string path scans for a sign first.
    json j = {{"v", "-1"}};
    CHECK(ju::safe_uint64(j, "v", 5u) == 5u);
    j["v"] = "   -42";
    CHECK(ju::safe_uint64(j, "v", 5u) == 5u);
    j["v"] = "42";
    CHECK(ju::safe_uint64(j, "v", 5u) == 42u);
}

TEST_CASE("safe_uint64 rejects out-of-range and non-finite floats", "[json_utils]") {
    json j;
    j["v"] = 1e30;
    CHECK(ju::safe_uint64(j, "v", 5u) == 5u);
    j["v"] = std::numeric_limits<double>::quiet_NaN();
    CHECK(ju::safe_uint64(j, "v", 5u) == 5u);
    j["v"] = std::numeric_limits<double>::infinity();
    CHECK(ju::safe_uint64(j, "v", 5u) == 5u);
}

// ============================================================================
// safe_size_t tests
// ============================================================================

TEST_CASE("safe_size_t behaves as safe_uint64 within range", "[json_utils]") {
    json j = {{"v", 4096}};
    CHECK(ju::safe_size_t(j, "v") == static_cast<std::size_t>(4096));
    CHECK(ju::safe_size_t(j, "absent", 8u) == static_cast<std::size_t>(8));
    json n = {{"v", nullptr}};
    CHECK(ju::safe_size_t(n, "v", 8u) == static_cast<std::size_t>(8));
}

TEST_CASE("safe_size_t does not truncate a value wider than size_t", "[json_utils]") {
    // On 64-bit hosts size_t == uint64_t so this passes through; on the 32-bit
    // cross targets (AD5M/MIPS32, K1 armv7) it must fall back to the default
    // rather than silently truncating. Assert whichever the platform implies so
    // the test is meaningful on both.
    json j;
    j["v"] = std::numeric_limits<std::uint64_t>::max();
    if (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        CHECK(ju::safe_size_t(j, "v", 5u) == static_cast<std::size_t>(5));
    } else {
        CHECK(ju::safe_size_t(j, "v", 5u) == std::numeric_limits<std::size_t>::max());
    }
}

TEST_CASE("safe_size_t rejects negatives rather than wrapping", "[json_utils]") {
    json j = {{"v", -1}};
    CHECK(ju::safe_size_t(j, "v", 5u) == static_cast<std::size_t>(5));
    j["v"] = "-1";
    CHECK(ju::safe_size_t(j, "v", 5u) == static_cast<std::size_t>(5));
}

// ============================================================================
// Notification payload
// ============================================================================
// A method callback is handed the whole JSON-RPC message, and Moonraker wraps a
// notification's payload in a single-element params array. Three subscribers
// walk down to it, so the walk is one function.

TEST_CASE("notification_payload finds the object inside params", "[json_utils][notification]") {
    const nlohmann::json msg = {
        {"jsonrpc", "2.0"},
        {"method", "notify_history_changed"},
        {"params", nlohmann::json::array({nlohmann::json{{"action", "finished"}}})}};

    const nlohmann::json* payload = helix::json_util::notification_payload(msg);
    REQUIRE(payload != nullptr);
    CHECK(helix::json_util::safe_string(*payload, "action") == "finished");
    CHECK(helix::json_util::notification_action(msg) == "finished");
}

TEST_CASE("notification_payload rejects frames shaped any other way",
          "[json_utils][notification]") {
    using helix::json_util::notification_action;
    using helix::json_util::notification_payload;

    // No params at all.
    CHECK(notification_payload(nlohmann::json{{"method", "notify_history_changed"}}) == nullptr);
    // params present but empty - Moonraker sends this for parameterless events.
    CHECK(notification_payload(nlohmann::json{{"params", nlohmann::json::array()}}) == nullptr);
    // params is an object rather than the array a notification uses.
    CHECK(notification_payload(nlohmann::json{{"params", {{"action", "finished"}}}}) == nullptr);
    // params[0] is a scalar, so there are no fields to read.
    CHECK(notification_payload(nlohmann::json{{"params", nlohmann::json::array({7})}}) == nullptr);

    // An unreadable frame reports no action rather than throwing, which is what
    // lets a caller treat "not the action I want" and "not a frame I understand"
    // the same way.
    CHECK(notification_action(nlohmann::json{{"method", "x"}}).empty());
    // A payload with no action field, and one whose action is JSON null.
    CHECK(notification_action(nlohmann::json{{"params", nlohmann::json::array({nlohmann::json{
                                                            {"job", nlohmann::json::object()}}})}})
              .empty());
    CHECK(notification_action(nlohmann::json{{"params", nlohmann::json::array({nlohmann::json{
                                                            {"action", nullptr}}})}})
              .empty());
}

// ============================================================================
// safe_dump
// ============================================================================

namespace {

/// A string no UTF-8 decoder can accept: 0xFF is not a legal lead byte in any
/// position. Free-form text reaching a save path — SSIDs, printer and tool
/// names, file names, macro text, gcode responses — is not validated anywhere,
/// so a byte like this can be sitting in any stored string.
const std::string kInvalidUtf8 = std::string("Voron \xff\xfe 2.4");

} // namespace

TEST_CASE("safe_dump serializes invalid UTF-8 instead of throwing", "[json_utils][safe_dump]") {
    json j = {{"name", kInvalidUtf8}, {"port", 7125}};

    // The default handler is error_handler_t::strict, which is what makes an
    // unguarded save path fatal.
    CHECK_THROWS_AS(j.dump(), nlohmann::json::type_error);

    std::string out;
    REQUIRE_NOTHROW(out = ju::safe_dump(j));

    // The rest of the document survives — that is the whole point of replacing
    // rather than catching.
    json round_trip = json::parse(out);
    CHECK(round_trip["port"] == 7125);
    CHECK(round_trip["name"].get<std::string>().rfind("Voron ", 0) == 0);
    // The offending bytes become U+FFFD, so they are gone from the output.
    CHECK(round_trip["name"].get<std::string>().find('\xff') == std::string::npos);
}

TEST_CASE("safe_dump matches dump() byte for byte on valid input", "[json_utils][safe_dump]") {
    json j = {{"name", "Vorön ünicode ✓"}, {"nested", {{"a", 1}, {"b", json::array({1, 2})}}}};

    CHECK(ju::safe_dump(j) == j.dump());
    CHECK(ju::safe_dump(j, 2) == j.dump(2));
    CHECK(ju::safe_dump(j, 4) == j.dump(4));
}

TEST_CASE("safe_dump honors the indent argument", "[json_utils][safe_dump]") {
    json j = {{"name", kInvalidUtf8}};

    // Compact by default, pretty-printed on request — a caller replacing
    // `doc.dump(2)` must not silently lose its formatting.
    CHECK(ju::safe_dump(j).find('\n') == std::string::npos);
    CHECK(ju::safe_dump(j, 2).find("\n  ") != std::string::npos);
}

// ============================================================================
// UTF-8 decoder table
// ============================================================================

TEST_CASE("dump() escapes multi-byte UTF-8 through the real decoder table",
          "[json_utils][safe_dump]") {
    // The DFA every dump() walks its strings with is a 400-entry table declared
    // as a function-local static inside a template, which the compiler emits as
    // a STB_GNU_UNIQUE symbol. A linker that drops that symbol leaves the
    // reference to it resolving to the image base, so the decoder indexes the
    // ELF header: the link is silent, the binary runs, and every string it
    // serializes comes out mis-escaped (prestonbrown/helixscreen#1584).
    //
    // So these expectations are absolute rather than dump() compared against
    // dump(). Both sides of such a comparison read the same table and agree
    // with each other whatever it holds, which is why the cases above this one
    // cannot see a wrong table. ensure_ascii is what forces the multi-byte
    // path; with it off the bytes are copied through unexamined.
    CHECK(json("\xC3\xA9").dump(-1, ' ', true) == "\"\\u00e9\"");                // U+00E9 é
    CHECK(json("\xE2\x82\xAC").dump(-1, ' ', true) == "\"\\u20ac\"");            // U+20AC €
    CHECK(json("\xF0\x9F\x98\x80").dump(-1, ' ', true) == "\"\\ud83d\\ude00\""); // U+1F600

    // The reject state has to be reachable too, or safe_dump()'s replacement
    // handler has nothing to act on: 0xFF is a legal byte in no position.
    CHECK_THROWS_AS(json("\xff").dump(-1, ' ', true), nlohmann::json::type_error);
}
