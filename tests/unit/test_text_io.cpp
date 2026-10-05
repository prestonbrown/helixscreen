// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_helpers/unique_temp_dir.h"
#include "text_io.h"

#include <clocale>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <sys/stat.h>
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
        CHECK_FALSE(fs::exists(target + ".tmp"));
    }
}

TEST_CASE("write_file_atomic failure keeps the target and removes the tmp", "[text_io]") {
    ScratchDir dir;
    const std::string target = dir.file("target");
    REQUIRE(tio::write_file(target, "keep me"));

    // A directory where the tmp should go makes the open fail.
    fs::create_directories(target + ".tmp");
    errno = 0;
    CHECK_FALSE(tio::write_file_atomic(target, "clobber"));
    CHECK(errno != 0);
    CHECK(tio::read_file(target) == "keep me");
    fs::remove_all(target + ".tmp");

    // A rename onto a non-empty directory fails after the tmp was written.
    const std::string dir_target = dir.file("occupied");
    fs::create_directories(dir_target + "/child");
    errno = 0;
    CHECK_FALSE(tio::write_file_atomic(dir_target, "x"));
    CHECK(errno != 0);
    CHECK_FALSE(fs::exists(dir_target + ".tmp"));
    CHECK(fs::is_directory(dir_target));
}

TEST_CASE("open_file + write_all + close stream a file", "[text_io]") {
    ScratchDir dir;
    auto f = tio::open_file(dir.file("out"), "wb");
    REQUIRE(f);
    CHECK(tio::write_all(f.get(), "line1\n"));
    CHECK(tio::write_all(f.get(), ""));
    CHECK(tio::write_all(f.get(), std::string_view("a\0b", 3)));
    CHECK(tio::close(f));
    CHECK_FALSE(f);
    CHECK(tio::read_file(dir.file("out")) == std::string("line1\na\0b", 9));

    CHECK_FALSE(tio::open_file(dir.file("no/dir/f"), "wb"));

    auto a = tio::open_file(dir.file("out"), "ab");
    REQUIRE(a);
    tio::write_all(a.get(), "!");
    CHECK(tio::close(a));
    CHECK(tio::read_file(dir.file("out"))->back() == '!');
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
    CHECK(tio::parse_double("1.5") == 1.5);
    CHECK(tio::parse_double("-2e3") == -2000.0);
    CHECK(tio::parse_double("+0.25") == 0.25);
    CHECK(tio::parse_double("7") == 7.0);
    CHECK(tio::parse_double(".5") == 0.5);
    CHECK(std::isinf(*tio::parse_double("inf")));
    CHECK(std::isnan(*tio::parse_double("nan")));
    CHECK_FALSE(tio::parse_double("").has_value());
    CHECK_FALSE(tio::parse_double(" 1.5").has_value());
    CHECK_FALSE(tio::parse_double("1.5 ").has_value());
    CHECK_FALSE(tio::parse_double("1.5mm").has_value());
    CHECK_FALSE(tio::parse_double("1,5").has_value());
    CHECK_FALSE(tio::parse_double("0x1p3").has_value());
    CHECK_FALSE(tio::parse_double("1e999").has_value());

    const char* prev = std::setlocale(LC_NUMERIC, nullptr);
    std::string saved = prev ? prev : "C";
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr) {
        SUCCEED("de_DE.UTF-8 not installed; the '.' checks above still ran in C");
        return;
    }
    CHECK(tio::parse_double("1.5") == 1.5);
    CHECK_FALSE(tio::parse_double("1,5").has_value());
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
