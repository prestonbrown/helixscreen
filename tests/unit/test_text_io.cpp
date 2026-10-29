// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_helpers/unique_temp_dir.h"
#include "text_io.h"

#include <atomic>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace tio = helix::text_io;
namespace fs = std::filesystem;

namespace {

struct ScratchDir {
    std::string path = helix::test::unique_temp_dir("helix_text_io");
    ScratchDir() {
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string file(const std::string& name) const {
        return path + "/" + name;
    }
};

std::vector<std::string> split(std::string_view text, char delim = '\n') {
    std::vector<std::string> out;
    for (std::string_view line : tio::lines(text, delim)) {
        out.emplace_back(line);
    }
    return out;
}

std::vector<std::string> read_records(const std::string& path, char delim = '\n') {
    std::vector<std::string> out;
    tio::LineReader reader(path, delim);
    std::string line;
    while (reader.next(line)) {
        out.push_back(line);
    }
    return out;
}

using V = std::vector<std::string>;

} // namespace

TEST_CASE("read_file returns bytes exactly, nullopt only when unopenable", "[text_io]") {
    ScratchDir dir;
    REQUIRE_FALSE(tio::read_file(dir.file("missing")).has_value());

    REQUIRE(tio::write_file(dir.file("empty"), ""));
    auto empty = tio::read_file(dir.file("empty"));
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    const std::string bytes("a\0b\r\nc", 6);
    REQUIRE(tio::write_file(dir.file("bin"), bytes));
    CHECK(tio::read_file(dir.file("bin")) == bytes);

    std::string big(100000, 'x');
    big[54321] = '\0';
    REQUIRE(tio::write_file(dir.file("big"), big));
    CHECK(tio::read_file(dir.file("big")) == big);
}

TEST_CASE("read_file with a cap returns at most that many leading bytes", "[text_io]") {
    ScratchDir dir;
    std::string big(100000, 'x');
    big[0] = 'a';
    big[4999] = 'b';
    REQUIRE(tio::write_file(dir.file("big"), big));
    CHECK(tio::read_file(dir.file("big"), 5000) == big.substr(0, 5000));
    CHECK(tio::read_file(dir.file("big"), 0) == std::string());
    CHECK(tio::read_file(dir.file("big"), 200000) == big);
}

TEST_CASE("read_file reads /proc files whose stat size is 0", "[text_io]") {
    if (!fs::exists("/proc/self/status")) {
        SKIP("no /proc on this host");
    }
    auto text = tio::read_file("/proc/self/status");
    REQUIRE(text.has_value());
    CHECK(text->find("Name:") != std::string::npos);
}

TEST_CASE("read_first_line matches one std::getline", "[text_io]") {
    ScratchDir dir;
    CHECK_FALSE(tio::read_first_line(dir.file("missing")).has_value());

    tio::write_file(dir.file("f"), "wlan0\r\nsecond\n");
    CHECK(tio::read_first_line(dir.file("f")) == "wlan0\r");

    tio::write_file(dir.file("noeol"), "up");
    CHECK(tio::read_first_line(dir.file("noeol")) == "up");

    tio::write_file(dir.file("empty"), "");
    CHECK(tio::read_first_line(dir.file("empty")) == "");
}

TEST_CASE("file_size", "[text_io]") {
    ScratchDir dir;
    CHECK_FALSE(tio::file_size(dir.file("missing")).has_value());
    tio::write_file(dir.file("f"), "12345");
    CHECK(tio::file_size(dir.file("f")) == 5u);
}

TEST_CASE("write_file truncates and fails into a missing directory", "[text_io]") {
    ScratchDir dir;
    REQUIRE(tio::write_file(dir.file("f"), "long content"));
    REQUIRE(tio::write_file(dir.file("f"), "short"));
    CHECK(tio::read_file(dir.file("f")) == "short");

    errno = 0;
    CHECK_FALSE(tio::write_file(dir.file("no/such/dir/f"), "x"));
    CHECK(errno == ENOENT);
}

TEST_CASE("write_file_atomic replaces the target and leaves no tmp", "[text_io]") {
    ScratchDir dir;
    const std::string target = dir.file("settings.json");
    for (auto d : {tio::Durability::None, tio::Durability::Fsync}) {
        REQUIRE(tio::write_file(target, "old"));
        REQUIRE(tio::write_file_atomic(target, "new", d));
        CHECK(tio::read_file(target) == "new");
        CHECK(helix::test::staging_files_beside(target) == 0);
    }
}

TEST_CASE("write_file_atomic failure keeps the target and removes the tmp", "[text_io]") {
    ScratchDir dir;
    const std::string target = dir.file("target");
    REQUIRE(tio::write_file(target, "keep me"));

    // A missing parent directory makes the open fail.
    errno = 0;
    CHECK_FALSE(tio::write_file_atomic(dir.file("no/such/dir/target"), "clobber"));
    CHECK(errno == ENOENT);
    CHECK(tio::read_file(target) == "keep me");

    // A rename onto a non-empty directory fails after the tmp was written.
    const std::string dir_target = dir.file("occupied");
    fs::create_directories(dir_target + "/child");
    errno = 0;
    CHECK_FALSE(tio::write_file_atomic(dir_target, "x"));
    CHECK(errno != 0);
    CHECK(fs::is_directory(dir_target));
    CHECK(helix::test::staging_files_beside(dir_target) == 0);
}

// Writers of one path (thumbnail downloads are not deduped) each stage into
// their own tmp, so every write succeeds and a reader only ever sees one
// writer's whole payload. With a shared staging name, a writer whose tmp was
// renamed away by another fails, and a lagging writer keeps writing into the
// file that is already in place. Detection is probabilistic, so the payload is
// large enough to span many write(2) calls.
TEST_CASE("write_file_atomic concurrent writers of one path never tear it", "[text_io]") {
    ScratchDir dir;
    const std::string target = dir.file("contended");
    constexpr size_t SIZE = 1024 * 1024;
    constexpr int kWriters = 8;
    REQUIRE(tio::write_file_atomic(target, std::string(SIZE, 'a')));

    auto whole = [](const std::string& data) {
        return data.size() == SIZE && data[0] >= 'a' &&
               data[0] < static_cast<char>('a' + kWriters) &&
               data.find_first_not_of(data[0]) == std::string::npos;
    };

    std::atomic<int> failed_writes{0};
    std::atomic<int> torn_reads{0};
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done) {
            const auto data = tio::read_file(target);
            if (data && !whole(*data)) {
                ++torn_reads;
            }
        }
    });
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            const std::string payload(SIZE, static_cast<char>('a' + w));
            for (int rep = 0; rep < 30; ++rep) {
                if (!tio::write_file_atomic(target, payload)) {
                    ++failed_writes;
                }
            }
        });
    }
    for (auto& t : writers) {
        t.join();
    }
    done = true;
    reader.join();

    CHECK(failed_writes == 0);
    CHECK(torn_reads == 0);
    const auto data = tio::read_file(target);
    REQUIRE(data.has_value());
    CHECK(whole(*data));
    CHECK(helix::test::staging_files_beside(target) == 0);
}

TEST_CASE("lines() splits exactly like a std::getline loop", "[text_io]") {
    CHECK(split("") == V{});
    CHECK(split("a") == V{"a"});
    CHECK(split("a\n") == V{"a"});
    CHECK(split("\n") == V{""});
    CHECK(split("a\n\nb") == V{"a", "", "b"});
    CHECK(split("a\n\n") == V{"a", ""});
    CHECK(split("a\r\nb\r\n") == V{"a\r", "b\r"});
    CHECK(split("PLA;PETG;", ';') == V{"PLA", "PETG"});
    CHECK(split(";x", ';') == V{"", "x"});
    CHECK(split(std::string_view("prog\0-a\0b\0", 10), '\0') == V{"prog", "-a", "b"});

    // break and continue work inside the loop body
    int seen = 0;
    for (std::string_view line : tio::lines("1\n2\n3\n4")) {
        if (line == "2") {
            continue;
        }
        if (line == "4") {
            break;
        }
        ++seen;
    }
    CHECK(seen == 2);
}

TEST_CASE("LineReader reads files like std::getline", "[text_io]") {
    ScratchDir dir;
    tio::LineReader missing(dir.file("missing"));
    CHECK_FALSE(missing);
    std::string line = "untouched";
    CHECK_FALSE(missing.next(line));

    tio::write_file(dir.file("empty"), "");
    CHECK(read_records(dir.file("empty")) == V{});

    tio::write_file(dir.file("crlf"), "G28\r\nG1 X1\r\n\nM84");
    CHECK(read_records(dir.file("crlf")) == V{"G28\r", "G1 X1\r", "", "M84"});

    tio::write_file(dir.file("nul"), std::string("a\0b\nc", 5));
    CHECK(read_records(dir.file("nul")) == V{std::string("a\0b", 3), "c"});

    tio::write_file(dir.file("cmdline"), std::string("wpa_supplicant\0-iwlan0\0", 23));
    CHECK(read_records(dir.file("cmdline"), '\0') == V{"wpa_supplicant", "-iwlan0"});

    std::string long_line(20000, 'g');
    tio::write_file(dir.file("long"), long_line + "\nend\n");
    CHECK(read_records(dir.file("long")) == V{long_line, "end"});
}

TEST_CASE("LineReader reports whether the last record was terminated", "[text_io]") {
    ScratchDir dir;
    tio::write_file(dir.file("f"), "a\nb");
    tio::LineReader r(dir.file("f"));
    std::string line;
    REQUIRE(r.next(line));
    CHECK(r.last_had_delimiter());
    REQUIRE(r.next(line));
    CHECK(line == "b");
    CHECK_FALSE(r.last_had_delimiter());
    CHECK_FALSE(r.next(line));

    tio::write_file(dir.file("g"), "a\n");
    tio::LineReader g(dir.file("g"));
    REQUIRE(g.next(line));
    CHECK(g.last_had_delimiter());
    CHECK_FALSE(g.next(line));
}

TEST_CASE("split_ws tokenizes like stream >> token", "[text_io]") {
    using SV = std::vector<std::string_view>;
    CHECK(tio::split_ws("") == SV{});
    CHECK(tio::split_ws(" \t\r\n") == SV{});
    CHECK(tio::split_ws("0.52 0.58 0.59 1/234 5678\n") ==
          SV{"0.52", "0.58", "0.59", "1/234", "5678"});
    CHECK(tio::split_ws("  wlan0\t00000000  0101A8C0 ") == SV{"wlan0", "00000000", "0101A8C0"});
    CHECK(tio::split_ws("x") == SV{"x"});
}

TEST_CASE("parse_int is strict", "[text_io]") {
    CHECK(tio::parse_int("42") == 42);
    CHECK(tio::parse_int("-7") == -7);
    CHECK(tio::parse_int("+7") == 7);
    CHECK_FALSE(tio::parse_int("").has_value());
    CHECK_FALSE(tio::parse_int("+").has_value());
    CHECK_FALSE(tio::parse_int("+-1").has_value());
    CHECK_FALSE(tio::parse_int(" 1").has_value());
    CHECK_FALSE(tio::parse_int("1 ").has_value());
    CHECK_FALSE(tio::parse_int("12abc").has_value());
    CHECK_FALSE(tio::parse_int("1.5").has_value());
    CHECK_FALSE(tio::parse_int("99999999999999999999").has_value());
    CHECK_FALSE(tio::parse_int<int>("3000000000").has_value());
    CHECK_FALSE(tio::parse_int<unsigned>("-1").has_value());
    CHECK(tio::parse_int<std::uint8_t>("255") == 255);
    CHECK_FALSE(tio::parse_int<std::uint8_t>("256").has_value());

    CHECK_FALSE(tio::parse_int("ff").has_value());
    CHECK(tio::parse_int("ff", 16) == 255);
    CHECK(tio::parse_int<unsigned long long>("ffffffffffffffff", 16) ==
          std::numeric_limits<unsigned long long>::max());
    CHECK_FALSE(tio::parse_int("0x1f", 16).has_value());
}

TEST_CASE("parse_double is strict and locale-independent", "[text_io]") {
    // The from_chars form and the strtod form (the ESP32's, and libc++'s) hold one contract.
    using Parse = std::optional<double> (*)(std::string_view);
    const Parse parse =
        GENERATE(as<Parse>{}, &tio::parse_double, &tio::detail::parse_double_strtod);
    CHECK(parse("1.5") == 1.5);
    CHECK(parse("-2e3") == -2000.0);
    CHECK(parse("+0.25") == 0.25);
    CHECK(parse("7") == 7.0);
    CHECK(parse(".5") == 0.5);
    CHECK_FALSE(parse("+-1").has_value());
    CHECK_FALSE(parse("++1").has_value());
    CHECK(std::isinf(*parse("inf")));
    CHECK(std::isnan(*parse("nan")));
    CHECK_FALSE(parse("").has_value());
    CHECK_FALSE(parse(" 1.5").has_value());
    CHECK_FALSE(parse("1.5 ").has_value());
    CHECK_FALSE(parse("1.5mm").has_value());
    CHECK_FALSE(parse("1,5").has_value());
    CHECK_FALSE(parse("0x1p3").has_value());
    CHECK_FALSE(parse("1e999").has_value());
    CHECK_FALSE(parse("1e-400").has_value());
    CHECK_FALSE(parse("-1e-400").has_value());
    CHECK(parse("1e-310") == 1e-310);
    CHECK(parse("0e-400") == 0.0);

    const char* prev = std::setlocale(LC_NUMERIC, nullptr);
    std::string saved = prev ? prev : "C";
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr) {
        SUCCEED("de_DE.UTF-8 not installed; the '.' checks above still ran in C");
        return;
    }
    CHECK(parse("1.5") == 1.5);
    CHECK_FALSE(parse("1,5").has_value());
    std::setlocale(LC_NUMERIC, saved.c_str());
}

namespace {
// What the std:: form returns, or nullopt where it throws.
template <typename T, typename F> std::optional<T> std_or_nullopt(F f) {
    try {
        return static_cast<T>(f());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
} // namespace

TEST_CASE("parse_leading matches std::sto* value for value", "[text_io]") {
    const std::vector<std::string> inputs = {"",
                                             " ",
                                             "42",
                                             "  42abc",
                                             "\t\n-7",
                                             "+7",
                                             "abc",
                                             "-",
                                             "+",
                                             "3.9",
                                             "1e5",
                                             "0x1A",
                                             "0x1p3",
                                             "inf",
                                             "-nan",
                                             "1e400",
                                             "1e-400",
                                             "1e39",
                                             "-1",
                                             "007",
                                             "2147483647",
                                             "2147483648",
                                             "-2147483648",
                                             "-2147483649",
                                             "9223372036854775807",
                                             "9223372036854775808",
                                             "18446744073709551615",
                                             "18446744073709551616",
                                             "4294967295",
                                             "4294967296",
                                             " 12 34",
                                             "1,5"};
    for (const auto& s : inputs) {
        CAPTURE(s);
        CHECK(tio::parse_leading<int>(s) == std_or_nullopt<int>([&] { return std::stoi(s); }));
        CHECK(tio::parse_leading<long>(s) == std_or_nullopt<long>([&] { return std::stol(s); }));
        CHECK(tio::parse_leading<long long>(s) ==
              std_or_nullopt<long long>([&] { return std::stoll(s); }));
        CHECK(tio::parse_leading<unsigned long>(s) ==
              std_or_nullopt<unsigned long>([&] { return std::stoul(s); }));
        CHECK(tio::parse_leading<unsigned long long>(s) ==
              std_or_nullopt<unsigned long long>([&] { return std::stoull(s); }));
        CHECK(tio::parse_leading<unsigned long>(s, 16) ==
              std_or_nullopt<unsigned long>([&] { return std::stoul(s, nullptr, 16); }));

        const auto d = tio::parse_leading<double>(s);
        const auto sd = std_or_nullopt<double>([&] { return std::stod(s); });
        REQUIRE(d.has_value() == sd.has_value());
        if (d && !std::isnan(*d)) {
            CHECK(*d == *sd);
        }
        const auto f = tio::parse_leading<float>(s);
        const auto sf = std_or_nullopt<float>([&] { return std::stof(s); });
        REQUIRE(f.has_value() == sf.has_value());
        if (f && !std::isnan(*f)) {
            CHECK(*f == *sf);
        }
    }
}

TEST_CASE("to_lower and to_upper map ASCII only", "[text_io]") {
    using namespace helix::text_io;
    CHECK(to_lower("AbC-123_xYz") == "abc-123_xyz");
    CHECK(to_upper("AbC-123_xYz") == "ABC-123_XYZ");
    CHECK(to_lower("").empty());
    // High-bit bytes (UTF-8 continuation, Latin-1) pass through whatever the locale.
    CHECK(to_lower("\xC3\x89\xE9") == "\xC3\x89\xE9");
    CHECK(to_upper("\xC3\xA9\xE9") == "\xC3\xA9\xE9");
}

TEST_CASE("contains_ignore_case", "[text_io]") {
    SECTION("Finds exact match") {
        REQUIRE(tio::contains_ignore_case("BED_MESH_CALIBRATE", "BED_MESH"));
    }

    SECTION("Case insensitive - finds lowercase in uppercase") {
        REQUIRE(tio::contains_ignore_case("BED_MESH_CALIBRATE", "bed_mesh"));
    }

    SECTION("Case insensitive - finds uppercase in lowercase") {
        REQUIRE(tio::contains_ignore_case("bed_mesh_calibrate", "BED_MESH"));
    }

    SECTION("Case insensitive - mixed case") {
        REQUIRE(tio::contains_ignore_case("BeD_MeSh_CaLiBrAtE", "bed_MESH"));
    }

    SECTION("Returns false when not found") {
        REQUIRE_FALSE(tio::contains_ignore_case("BED_MESH_CALIBRATE", "QGL"));
    }

    SECTION("Empty needle matches nothing") {
        REQUIRE_FALSE(tio::contains_ignore_case("anything", ""));
    }

    SECTION("Empty haystack doesn't match non-empty needle") {
        REQUIRE_FALSE(tio::contains_ignore_case("", "test"));
    }

    SECTION("Empty needle does not match an empty haystack either") {
        // The empty-needle guard is checked before the size comparison, so this
        // stays false rather than degenerating into std::string::find's
        // "found at 0" convention.
        REQUIRE_FALSE(tio::contains_ignore_case("", ""));
    }

    SECTION("Needle longer than haystack is rejected") {
        REQUIRE_FALSE(tio::contains_ignore_case("BED", "BED_MESH"));
    }

    SECTION("Matches at both ends of the haystack") {
        REQUIRE(tio::contains_ignore_case("bed_mesh_calibrate", "BED"));       // position 0
        REQUIRE(tio::contains_ignore_case("bed_mesh_calibrate", "CALIBRATE")); // final byte
        REQUIRE(tio::contains_ignore_case("G28", "g28"));                      // whole string
    }

    SECTION("Searches by length, not to the first NUL") {
        // std::string_view carries its own size; an embedded NUL must not end
        // the scan the way it would for a C string.
        const std::string_view haystack("BED\0MESH", 8);
        REQUIRE(tio::contains_ignore_case(haystack, std::string_view("\0mesh", 5)));
        REQUIRE_FALSE(tio::contains_ignore_case(haystack, std::string_view("\0qgl", 4)));
    }
}

// contains_ignore_case folds ONLY A-Z/a-z, by arithmetic, on raw bytes. These
// cases pin the contract for bytes >= 0x80, the UTF-8 the AMS backends feed it,
// so a locale-aware, unsigned-char or Unicode-aware fold cannot quietly alter
// which macros and filament names match.

TEST_CASE("contains_ignore_case folds ASCII only over real UTF-8", "[text_io][utf8]") {
    SECTION("High bytes match byte-for-byte and are passed through unfolded") {
        // "Düse" — the u-umlaut is C3 BC in UTF-8; the surrounding ASCII still folds.
        REQUIRE(tio::contains_ignore_case("Düse wechseln", "düse"));
        REQUIRE(tio::contains_ignore_case("AMS Löschen", "ms löschen"));
        REQUIRE(tio::contains_ignore_case("Загрузка филамента", "филамента"));
        REQUIRE(tio::contains_ignore_case("フィラメント交換", "交換"));
    }

    SECTION("No case folding happens above 0x7F") {
        // U+00DC vs U+00FC differ in their second UTF-8 byte (9C vs BC). A
        // Unicode-aware fold would call these equal; this one must not.
        REQUIRE_FALSE(tio::contains_ignore_case("DÜSE", "düse"));
        REQUIRE(tio::contains_ignore_case("DÜSE", "DÜSE"));

        // Cyrillic upper/lower likewise stay distinct.
        REQUIRE_FALSE(tio::contains_ignore_case("ФИЛАМЕНТ", "филамент"));
        REQUIRE(tio::contains_ignore_case("ФИЛАМЕНТ", "ФИЛАМЕНТ"));
    }

    SECTION("Raw high bytes are compared, never folded") {
        // 0xC0/0xE0 are A-with-grave and a-with-grave in Latin-1, i.e. exactly
        // 0x20 apart the way 'A'/'a' are. The fold must not reach them —
        // signed char makes them negative, unsigned char makes them > 'Z', so
        // this holds on x86 and on ARM alike.
        REQUIRE_FALSE(tio::contains_ignore_case("\xC0", "\xE0"));
        REQUIRE(tio::contains_ignore_case("\xC0", "\xC0"));
        REQUIRE(tio::contains_ignore_case("PLA\xFF_DRY", "\xFF_dry"));
    }

    SECTION("The A-Z window is exact at both edges") {
        // '@' is 'A'-1 and '[' is 'Z'+1; folding either would make these match
        // their +0x20 partners '`' and '{'.
        REQUIRE_FALSE(tio::contains_ignore_case("@", "`"));
        REQUIRE_FALSE(tio::contains_ignore_case("[", "{"));
        // 'A' and 'Z' themselves are inside the window.
        REQUIRE(tio::contains_ignore_case("@AZ[", "@az["));
    }

    SECTION("Real macro names with accented parameters still match their ASCII token") {
        REQUIRE(
            tio::contains_ignore_case("SET_FILAMENT_TYPE MATERIAL=Rot-Grün", "set_filament_type"));
        REQUIRE(tio::contains_ignore_case("_UNLOAD_FILAMENT_ÜBERGABE", "unload_filament"));
        REQUIRE_FALSE(tio::contains_ignore_case("SET_FILAMENT_TYPE MATERIAL=Rot-Grün", "grun"));
    }
}

TEST_CASE("equals_ignore_case", "[text_io]") {
    SECTION("Exact match") {
        REQUIRE(tio::equals_ignore_case("SKIP_BED_MESH", "SKIP_BED_MESH"));
    }

    SECTION("Case insensitive match - different cases") {
        REQUIRE(tio::equals_ignore_case("skip_bed_mesh", "SKIP_BED_MESH"));
        REQUIRE(tio::equals_ignore_case("SKIP_BED_MESH", "skip_bed_mesh"));
        REQUIRE(tio::equals_ignore_case("Skip_Bed_Mesh", "SKIP_BED_MESH"));
    }

    SECTION("Returns false for different strings") {
        REQUIRE_FALSE(tio::equals_ignore_case("SKIP_BED_MESH", "SKIP_QGL"));
    }

    SECTION("Returns false for substrings") {
        REQUIRE_FALSE(tio::equals_ignore_case("SKIP_BED_MESH", "SKIP_BED"));
        REQUIRE_FALSE(tio::equals_ignore_case("SKIP_BED", "SKIP_BED_MESH"));
    }

    SECTION("Empty strings equal") {
        REQUIRE(tio::equals_ignore_case("", ""));
    }

    SECTION("No case folding happens above 0x7F") {
        REQUIRE_FALSE(tio::equals_ignore_case("DÜSE", "düse"));
        REQUIRE_FALSE(tio::equals_ignore_case("\xC0", "\xE0"));
        REQUIRE(tio::equals_ignore_case("Düse", "DüSE"));
    }
}

TEST_CASE("utf8_first_codepoint takes the whole lead character", "[text_io]") {
    using helix::text_io::utf8_first_codepoint;
    CHECK(utf8_first_codepoint("").empty());
    CHECK(utf8_first_codepoint("Mon") == "M");
    CHECK(utf8_first_codepoint("\xC3\xA9t\xC3\xA9") == "\xC3\xA9");         // 2 bytes
    CHECK(utf8_first_codepoint("\xE6\x9C\x88") == "\xE6\x9C\x88");          // 3 bytes
    CHECK(utf8_first_codepoint("\xF0\x9F\x98\x80x") == "\xF0\x9F\x98\x80"); // 4 bytes
}
