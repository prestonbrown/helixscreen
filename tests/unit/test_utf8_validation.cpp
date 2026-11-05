// SPDX-License-Identifier: GPL-3.0-or-later

// The one UTF-8 sequence decoder, and the two validators built on it.

#include "moonraker_validation.h"
#include "utf8_validation.h"

#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

using helix::text_io::decode_utf8;
using helix::text_io::is_valid_utf8;
using moonraker_internal::is_safe_object_name;

TEST_CASE("decode_utf8 reads one well-formed sequence", "[utf8]") {
    const std::string s = "a\xC3\xA9\xE6\xB5\x8B\xF0\x9F\x98\x80";
    auto a = decode_utf8(s, 0);
    REQUIRE(a);
    CHECK(a->codepoint == 'a');
    CHECK(a->length == 1);
    auto e = decode_utf8(s, 1);
    REQUIRE(e);
    CHECK(e->codepoint == 0xE9);
    CHECK(e->length == 2);
    auto ce = decode_utf8(s, 3);
    REQUIRE(ce);
    CHECK(ce->codepoint == 0x6D4B);
    CHECK(ce->length == 3);
    auto emoji = decode_utf8(s, 6);
    REQUIRE(emoji);
    CHECK(emoji->codepoint == 0x1F600);
    CHECK(emoji->length == 4);
    CHECK_FALSE(decode_utf8(s, s.size()));
}

TEST_CASE("decode_utf8 refuses every malformed shape", "[utf8]") {
    CHECK_FALSE(decode_utf8("\x80", 0));             // stray continuation
    CHECK_FALSE(decode_utf8("\xC3", 0));             // truncated
    CHECK_FALSE(decode_utf8("\xC3\x41", 0));         // bad continuation
    CHECK_FALSE(decode_utf8("\xC0\xAF", 0));         // overlong
    CHECK_FALSE(decode_utf8("\xED\xA0\x80", 0));     // surrogate
    CHECK_FALSE(decode_utf8("\xF4\x90\x80\x80", 0)); // past U+10FFFF
    CHECK_FALSE(decode_utf8("\xF8\x88\x80\x80", 0)); // no such lead
}

TEST_CASE("both validators reject what the decoder rejects", "[utf8]") {
    // A label, not the bytes: Catch2 passes a surrogate into its XML report verbatim.
    const std::pair<const char*, std::string> kBad[] = {
        {"overlong", "x\xC0\xAF"}, {"surrogate", "x\xED\xA0\x80"}, {"truncated", "x\xC3"}};
    for (const auto& [label, bad] : kBad) {
        INFO(label);
        CHECK_FALSE(is_valid_utf8(bad));
        CHECK_FALSE(is_safe_object_name(bad));
    }
    CHECK(is_valid_utf8("plain \xE6\xB5\x8B"));
    CHECK(is_safe_object_name("\xE6\xB5\x8B\xE8\xAF\x95.stl_id_0"));
    INFO("the object-name validator keeps its own printable filter on top");
    CHECK(is_valid_utf8("\xC2\x85"));
    CHECK_FALSE(is_safe_object_name("a\xC2\x85"));
}
