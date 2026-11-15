// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tokenized logging for the ESP32 firmware (#1915). A log call's format literal
// lives in the ELF's non-loaded `.helix_logfmt` section, never in the flashed
// image; the device prints the literal's offset in that section (the token) and
// its arguments as one base64 record line, and scripts/detokenize_log.py turns
// the line back into text with the token database the build extracts from the
// same ELF (scripts/esp32_logtok_db.py).
//
// Record: `$` + base64 (no padding) of
//   level byte (spdlog numbering; kTruncatedFlag set when arguments were cut)
//   token, unsigned LEB128
//   per argument: one ArgTag byte, then its payload (see ArgTag)
//
// This header is plain C++17 with no ESP-IDF dependency, so the host test build
// compiles it too (tests/unit/test_esp32_logtok.cpp). The spdlog shim defines
// the `info(...)` / `warn(...)` / `error(...)` / `critical(...)` macros on top
// of HELIX_LOGTOK_ARGS below.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef FMT_VERSION
#include <spdlog/include/spdlog/fmt/bundled/format.h> // the repo's bundled fmt via -isystem lib
#endif

namespace helix::logtok {

// One record's encoded bytes, before base64. A line is 1 + 4/3 of this.
constexpr std::size_t kMaxRecordBytes = 192;
// Base64 of a full record, plus the leading '$' and the terminating NUL.
constexpr std::size_t kMaxLineChars = 1 + (kMaxRecordBytes * 4 + 2) / 3 + 1;
// Set in the level byte when an argument did not fit.
constexpr std::uint8_t kTruncatedFlag = 0x80;
// The header line repeats this often, so a capture that starts mid-session
// still names its build.
constexpr unsigned kHeaderEvery = 200;
// `$HLT1:<first 9 hex digits of the ELF SHA-256>` names the build whose token
// database decodes the records that follow.
constexpr const char* kHeaderPrefix = "$HLT1:";

enum class ArgTag : std::uint8_t {
    kInt = 1,         // zigzag LEB128
    kUint = 2,        // LEB128
    kDouble = 3,      // 8 bytes, little-endian IEEE 754
    kFloat = 4,       // 4 bytes, little-endian IEEE 754
    kBool = 5,        // 1 byte
    kChar = 6,        // 1 byte
    kString = 7,      // LEB128 length + bytes
    kStringCut = 8,   // as kString; the device cut the value to fit the record
    kPointer = 9,     // LEB128 of the address
    kUnsupported = 10 // no payload: a value the encoder cannot carry
};

// Appends to a fixed buffer; once a write does not fit, every later write is
// refused and full() stays true.
class ByteWriter {
  public:
    ByteWriter(std::uint8_t* out, std::size_t cap) : out_(out), cap_(cap) {}

    bool put(std::uint8_t b);
    bool put_bytes(const void* p, std::size_t n);
    bool put_uleb(std::uint64_t v);
    bool put_sleb_zigzag(std::int64_t v);

    std::size_t size() const {
        return len_;
    }
    std::size_t remaining() const {
        return cap_ - len_;
    }
    bool full() const {
        return full_;
    }

  private:
    std::uint8_t* out_;
    std::size_t cap_;
    std::size_t len_ = 0;
    bool full_ = false;
};

// Bytes put_uleb() writes for @p v.
std::size_t uleb_size(std::uint64_t v);

// Encodes one record into @p out (at most @p cap bytes) and returns its length.
// Arguments that do not fit are dropped and kTruncatedFlag is set; a string
// argument is cut to the space left first (ArgTag::kStringCut).
std::size_t encode_record(int level, std::uint32_t token, fmt::format_args args, std::uint8_t* out,
                          std::size_t cap);

// Standard-alphabet base64 without padding into @p out (NUL-terminated).
// Returns the characters written, or 0 when @p cap cannot hold them.
std::size_t base64_encode(const std::uint8_t* in, std::size_t n, char* out, std::size_t cap);

// True when @p s (the stringized macro argument) is one or more adjacent plain
// "..." literals and nothing else: the only shape whose text the token database
// can resolve. A prefixed (u8"", L""), raw (R"()") or macro-built literal is
// false, and so is any expression.
constexpr bool is_literal_run(const char* s) {
    bool any = false;
    for (std::size_t i = 0;;) {
        while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            ++i;
        }
        if (s[i] == '\0') {
            return any;
        }
        if (s[i] != '"') {
            return false;
        }
        ++i;
        while (s[i] != '"') {
            if (s[i] == '\0') {
                return false;
            }
            if (s[i] == '\\') {
                if (s[i + 1] == '\0') {
                    return false;
                }
                ++i;
            }
            ++i;
        }
        ++i;
        any = true;
    }
}

// A format literal and its token. The log-level overloads in the spdlog shim
// read only the token, so the literal itself is never referenced and the
// compiler leaves it out of the image. A call to some other function that
// happens to be named info/warn/error/critical gets its string back through
// the conversions.
template <std::size_t N> struct Site {
    const char (&text)[N];
    std::uint32_t token;

    operator const char*() const {
        return text;
    }
    operator std::string() const {
        return std::string(text, N - 1);
    }
    operator std::string_view() const {
        return std::string_view(text, N - 1);
    }
};

template <bool kLiteral> struct Wrap;
template <> struct Wrap<true> {
    template <std::size_t N>
    static constexpr Site<N> apply(const char (&text)[N], std::uint32_t token) {
        return Site<N>{text, token};
    }
};
template <> struct Wrap<false> {
    template <typename T> static constexpr T&& apply(T&& value, std::uint32_t) {
        return std::forward<T>(value);
    }
};

} // namespace helix::logtok

#define HELIX_LOGTOK_STR_(x) #x
#define HELIX_LOGTOK_STR(x) HELIX_LOGTOK_STR_(x)

// The token of the literal @p x: the address of its stringized text in the
// non-loaded section, or 0 when @p x is not a literal run, which then gets no
// entry at all (the discarded branch of a generic lambda is never instantiated).
// Each site gets its own input section name, since GCC refuses one name shared by
// a function-local static in an inline function and one in an ordinary function
// ("section type conflict"). `used` keeps the section under LTO, which otherwise
// moves an inline function's static into .rodata.
#define HELIX_LOGTOK_TOKEN(x, is_literal)                                                          \
    ([](auto literal) -> ::std::uint32_t {                                                         \
        if constexpr (decltype(literal)::value) {                                                  \
            static const char kText[]                                                              \
                __attribute__((section(".helix_logfmt." HELIX_LOGTOK_STR(__COUNTER__)), used)) =   \
                    #x;                                                                            \
            return static_cast<::std::uint32_t>(reinterpret_cast<::std::uintptr_t>(kText));        \
        } else {                                                                                   \
            return 0;                                                                              \
        }                                                                                          \
    }(::std::integral_constant<bool, is_literal>{}))

#define HELIX_LOGTOK_WRAP(x)                                                                       \
    ::helix::logtok::Wrap<::helix::logtok::is_literal_run(#x)>::apply(                             \
        x, HELIX_LOGTOK_TOKEN(x, ::helix::logtok::is_literal_run(#x)))

#define HELIX_LOGTOK_ARGS_(x, ...) HELIX_LOGTOK_WRAP(x) __VA_OPT__(, ) __VA_ARGS__

// A call's argument list with its first argument wrapped; an empty list stays
// empty, so `x.error()` is untouched.
#define HELIX_LOGTOK_ARGS(...) __VA_OPT__(HELIX_LOGTOK_ARGS_(__VA_ARGS__))
