// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: exercises the shipped ESP32 tokenized-log encoder unmodified --
//                 firmware/helixscreen-esp32/components/helixnet/shim/log_token.{h,cpp}.
//                 That is production firmware code; this gate only scans include/ and
//                 src/, so a firmware/ include reads to it as no include at all.

/**
 * @file test_esp32_logtok.cpp
 * @brief The ESP32 tokenized-log record encoder (#1915), on the host.
 *
 * The corpus pins every record against tests/fixtures/logtok/corpus.json, which
 * also holds fmt::format's text for each; tests/python/test_detokenize_log.py
 * decodes the same records and must reproduce that text. Regenerate the fixture
 * after an intended format change with HELIX_LOGTOK_REGEN=1.
 */

// The encoder is one firmware .cpp with no ESP-IDF dependency; compiling it into
// this unit keeps the firmware tree out of the test build's source lists.
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "firmware/helixscreen-esp32/components/helixnet/shim/log_token.cpp"
#include "hv/json.hpp"

using namespace helix::logtok;

namespace {

std::string corpus_path() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : "";
    return root + "tests/fixtures/logtok/corpus.json";
}

template <typename... Args>
std::string record_line(int level, uint32_t token, const Args&... args) {
    uint8_t rec[kMaxRecordBytes];
    const size_t n = encode_record(level, token, fmt::make_format_args(args...), rec, sizeof rec);
    char line[kMaxLineChars];
    line[0] = '$';
    base64_encode(rec, n, line + 1, sizeof line - 1);
    return line;
}

template <typename... Args>
std::vector<uint8_t> record_bytes(int level, uint32_t token, size_t cap, const Args&... args) {
    std::vector<uint8_t> rec(cap);
    rec.resize(encode_record(level, token, fmt::make_format_args(args...), rec.data(), cap));
    return rec;
}

struct Entry {
    std::string fmt;
    uint32_t token;
    std::string line;
    std::string text;
};

template <typename... Args>
Entry entry(uint32_t token, fmt::format_string<Args...> f, const Args&... args) {
    return {std::string(f.get().data(), f.get().size()), token, record_line(2, token, args...),
            fmt::format(f, args...)};
}

// Real format strings from the tree (and their spec shapes, ranked by use),
// plus the value edges fmt's `{}` treats specially.
std::vector<Entry> corpus() {
    const std::string fan = "part_fan";
    const std::string_view method = "printer.gcode.script";
    return {
        entry(0, "[FavoriteMacroConfigModal] Selected color: 0x{:06X}", 0x1a2b3c),
        entry(57, "[GCode::Builder]     Raw toolpath segments:    {:>8}", size_t{123456}),
        entry(130, "[SPIN] start={:3d} end={:3d} sweep={:3d} [range: {:3d}-{:3d}]", 5, -90, 270, 0,
              360),
        entry(201, "[MoonrakerClientMock] SET_FAN_SPEED FAN={} SPEED={:.2f}", fan, 0.5),
        entry(300, "[K2StockSource] capable: poll every {} s at prob >= {:.3}", 30, 0.8125),
        entry(415, "[LabelPrinterSettings] set_usb_vid(0x{:04x})", uint16_t{0x4b43}),
        entry(16383, "[MoonrakerClientMock] T{} offsets calibrated: x={:.4f} y={:.4f} z={:.4f}", 1,
              -0.0125, 3.14159, 0.0f),
        entry(16384, "[Fbdev Backend] Cleared framebuffer to 0x{:08X} ({}x{}, {}bpp)",
              uint32_t{0xff000000u}, 800, 480, 32),
        entry(70000, "{} Refusing homing G-code during active print (state={}): '{}'", "[Gcode]",
              "printing", "G28"),
        entry(70001, "[BeltDsp] {}-point PSD at {} Hz bandwidth took {:.1f} ms - {}", 4096, 3200.0,
              12.345f, true),
        entry(70002, "[Moonraker API] {}: {}", method, "Klipper reports: \"Must home axis first\""),
        entry(70003,
              "[Temp] extruder {}\xc2\xb0"
              "C target {} ({:+} / {:+.1f})",
              215.5, 100.0, 5, -2.25),
        entry(70004, "flag={} c={} min={} max={}", false, 'x', std::numeric_limits<int64_t>::min(),
              std::numeric_limits<uint64_t>::max()),
        entry(70005, "ratio={} eps={} big={} third={}", 0.1f, 1e-7, 1e20, 1.0 / 3.0),
        entry(70006, "nan={} inf={} neg0={}", std::nan(""),
              -std::numeric_limits<double>::infinity(), -0.0),
        entry(70007, "braces {{literal}} {1} before {0}", "a", "b"),
        entry(70008, "[{:>{}}|{:<6}|{:^7}|{:*^9}]", "x", 4, "ab", "mid", 42),
        entry(70009, "ptr={} hex={:#x} oct={:o} bin={:b}",
              reinterpret_cast<const void*>(0x3fc8a000), 255, 8, 5u),
        entry(70010, "byte={} short={} char-as-int={:d}", uint8_t{200}, int16_t{-300}, 'A'),
        entry(70011, "no arguments at all"),
    };
}

} // namespace

TEST_CASE("logtok: LEB128 and zigzag edges", "[tokenlog]") {
    struct Case {
        uint64_t v;
        std::vector<uint8_t> bytes;
    };
    for (const Case& c : {Case{0, {0x00}}, Case{127, {0x7f}}, Case{128, {0x80, 0x01}},
                          Case{16383, {0xff, 0x7f}}, Case{16384, {0x80, 0x80, 0x01}},
                          Case{std::numeric_limits<uint64_t>::max(),
                               {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}}}) {
        uint8_t buf[16];
        ByteWriter w(buf, sizeof buf);
        REQUIRE(w.put_uleb(c.v));
        CHECK(std::vector<uint8_t>(buf, buf + w.size()) == c.bytes);
        CHECK(uleb_size(c.v) == c.bytes.size());
    }
    struct ZCase {
        int64_t v;
        uint8_t first;
    };
    for (const ZCase& c : {ZCase{0, 0}, ZCase{-1, 1}, ZCase{1, 2}, ZCase{-64, 127}}) {
        uint8_t buf[16];
        ByteWriter w(buf, sizeof buf);
        REQUIRE(w.put_sleb_zigzag(c.v));
        CHECK(w.size() == 1);
        CHECK(buf[0] == c.first);
    }
    uint8_t buf[16];
    ByteWriter w(buf, sizeof buf);
    REQUIRE(w.put_sleb_zigzag(std::numeric_limits<int64_t>::min()));
    CHECK(w.size() == 10);
    CHECK(buf[0] == 0xff);
    CHECK(buf[9] == 0x01);
}

TEST_CASE("logtok: a write that does not fit refuses and stays refused", "[tokenlog]") {
    uint8_t buf[2];
    ByteWriter w(buf, sizeof buf);
    CHECK(w.put(1));
    CHECK_FALSE(w.put_uleb(300));
    CHECK(w.full());
    CHECK_FALSE(w.put(2));
    CHECK(w.size() == 1);
}

TEST_CASE("logtok: base64 without padding", "[tokenlog]") {
    const std::vector<std::pair<std::string, std::string>> vectors = {
        {"", ""},           {"f", "Zg"},          {"fo", "Zm8"},          {"foo", "Zm9v"},
        {"foob", "Zm9vYg"}, {"fooba", "Zm9vYmE"}, {"foobar", "Zm9vYmFy"}, {"\xff\xfe", "//4"}};
    for (const auto& [in, want] : vectors) {
        char out[16];
        const size_t n =
            base64_encode(reinterpret_cast<const uint8_t*>(in.data()), in.size(), out, sizeof out);
        CHECK(std::string(out, n) == want);
    }
    char tiny[4];
    CHECK(base64_encode(reinterpret_cast<const uint8_t*>("foo"), 3, tiny, sizeof tiny) == 0);
}

TEST_CASE("logtok: each argument type gets its tag", "[tokenlog]") {
    const std::string s = "ok";
    const auto rec = record_bytes(3, 300, kMaxRecordBytes, -2, 3u, 1.5, 2.5f, true, 'z', s,
                                  static_cast<const void*>(nullptr));
    const std::vector<uint8_t> want = {
        3, 0xac, 0x02,                               // level, token 300
        1, 3,                                        // int -2 (zigzag 3)
        2, 3,                                        // unsigned 3
        3, 0,    0,    0,    0,    0, 0, 0xf8, 0x3f, // double 1.5
        4, 0,    0,    0x20, 0x40,                   // float 2.5
        5, 1,                                        // bool
        6, 'z',                                      // char
        7, 2,    'o',  'k',                          // string
        9, 0,                                        // pointer
    };
    CHECK(rec == want);
}

TEST_CASE("logtok: a string longer than the record is cut and flagged", "[tokenlog]") {
    const std::string longer(500, 'q');
    const auto rec = record_bytes(2, 5, kMaxRecordBytes, 7, longer);
    REQUIRE(rec.size() == kMaxRecordBytes);
    CHECK(rec[0] == (2 | kTruncatedFlag));
    // level, token, int tag + value, then the cut string's tag and 2-byte length.
    CHECK(rec[4] == static_cast<uint8_t>(ArgTag::kStringCut));
    CHECK(((rec[5] & 0x7f) | (rec[6] << 7)) == static_cast<int>(kMaxRecordBytes - 7));
    CHECK(rec.back() == 'q');
}

TEST_CASE("logtok: an argument with no room is dropped and flagged", "[tokenlog]") {
    const auto rec = record_bytes(4, 1, 6, 1, 2.0);
    // The double needs 9 bytes; only the int fits after level and token.
    CHECK(rec == std::vector<uint8_t>{4 | kTruncatedFlag, 1, 1, 2});
}

TEST_CASE("logtok: only a run of plain literals is a literal", "[tokenlog]") {
    static_assert(is_literal_run("\"temp: {}\""));
    static_assert(is_literal_run("\"a\" \"b\""));
    static_assert(is_literal_run("\"esc \\\" quote\"   \"\\\\\""));
    static_assert(!is_literal_run("msg"));
    static_assert(!is_literal_run("\"a\" PREFIX"));
    static_assert(!is_literal_run("u8\"x\""));
    static_assert(!is_literal_run("R\"(x)\""));
    static_assert(!is_literal_run("fmt::runtime(s)"));
    static_assert(!is_literal_run(""));
    static_assert(!is_literal_run("\"unterminated"));
    CHECK(is_literal_run("\"a\"\n\t\"b\""));
    CHECK_FALSE(is_literal_run("\"a\" + b"));
}

#if defined(__ELF__)
namespace {
template <size_t N> int classify(Site<N>&&) {
    return 1;
}
int classify(const std::string&) {
    return 2;
}
std::string take(const std::string& s) {
    return s;
}
int no_args() {
    return 7;
}
} // namespace

TEST_CASE("logtok: the call-site wrapper", "[tokenlog]") {
    const std::string s = "runtime";
    CHECK(classify(HELIX_LOGTOK_ARGS("lit {}")) == 1);
    CHECK(classify(HELIX_LOGTOK_ARGS("a"
                                     "b")) == 1);
    CHECK(classify(HELIX_LOGTOK_ARGS(s)) == 2);
    // A function that is not a log call still gets its string back.
    CHECK(take(HELIX_LOGTOK_ARGS("abc")) == "abc");
    CHECK(no_args(HELIX_LOGTOK_ARGS()) == 7);
    // Two sites with the same text are two tokens: each site has its own entry.
    auto a = HELIX_LOGTOK_ARGS("same");
    auto b = HELIX_LOGTOK_ARGS("same");
    CHECK(a.token != b.token);
    // A non-literal gets no section entry, and so no token.
    CHECK(HELIX_LOGTOK_TOKEN(s, false) == 0);
    CHECK(HELIX_LOGTOK_TOKEN("lit", true) != 0);
}
#endif

TEST_CASE("logtok: the corpus matches the checked-in fixture", "[tokenlog]") {
    const auto entries = corpus();
    nlohmann::json want = nlohmann::json::array();
    for (const Entry& e : entries) {
        want.push_back({{"fmt", e.fmt}, {"token", e.token}, {"line", e.line}, {"text", e.text}});
    }
    if (std::getenv("HELIX_LOGTOK_REGEN") != nullptr) {
        std::ofstream(corpus_path()) << want.dump(1) << "\n";
    }
    std::ifstream in(corpus_path());
    REQUIRE(in.good());
    const auto have = nlohmann::json::parse(in, nullptr, false);
    REQUIRE_FALSE(have.is_discarded());
    REQUIRE(have.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        INFO(want[i]["fmt"].get<std::string>());
        CHECK(have[i] == want[i]);
    }
}
