// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_data_source.h"
#include "test_helpers/unique_temp_dir.h"

#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

namespace {

// Helper to create a temp file with content
class TempFile {
  public:
    explicit TempFile(const std::string& content) {
        path_ = helix::test::unique_temp_file("test_datasource", "gcode");
        std::ofstream file(path_);
        file << content;
    }

    ~TempFile() {
        std::remove(path_.c_str());
    }

    const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
};

const std::string SAMPLE_GCODE = R"(; Sample G-code
G28 ; Home
G1 Z10 F1000
G1 X50 Y50 F3000
G1 Z0.2
G1 X100 Y50 E10 F1500
G1 X100 Y100 E20
G1 X50 Y100 E30
G1 X50 Y50 E40
)";

} // namespace

TEST_CASE("FileDataSource basic operations", "[gcode][datasource]") {
    TempFile temp(SAMPLE_GCODE);
    FileDataSource source(temp.path());

    SECTION("opens valid file") {
        REQUIRE(source.is_valid());
        REQUIRE(source.file_size() > 0);
        REQUIRE(source.file_size() == SAMPLE_GCODE.size());
    }

    SECTION("reports correct source name") {
        REQUIRE(source.source_name() == temp.path());
        REQUIRE(source.filepath() == temp.path());
    }

    SECTION("supports range requests") {
        REQUIRE(source.supports_range_requests());
    }

    SECTION("reads entire file") {
        auto data = source.read_all();
        REQUIRE(data.size() == SAMPLE_GCODE.size());
        std::string content(data.begin(), data.end());
        REQUIRE(content == SAMPLE_GCODE);
    }

    SECTION("reads byte range from start") {
        auto data = source.read_range(0, 20);
        REQUIRE(data.size() == 20);
        std::string content(data.begin(), data.end());
        REQUIRE(content == SAMPLE_GCODE.substr(0, 20));
    }

    SECTION("reads byte range from middle") {
        auto data = source.read_range(10, 15);
        REQUIRE(data.size() == 15);
        std::string content(data.begin(), data.end());
        REQUIRE(content == SAMPLE_GCODE.substr(10, 15));
    }

    SECTION("clamps read past end of file") {
        size_t offset = SAMPLE_GCODE.size() - 10;
        auto data = source.read_range(offset, 100);
        REQUIRE(data.size() == 10);
    }

    SECTION("returns empty for offset past end") {
        auto data = source.read_range(SAMPLE_GCODE.size() + 100, 10);
        REQUIRE(data.empty());
    }
}

TEST_CASE("FileDataSource read_line", "[gcode][datasource]") {
    TempFile temp(SAMPLE_GCODE);
    FileDataSource source(temp.path());

    SECTION("reads first line") {
        auto line = source.read_line(0);
        REQUIRE(line.has_value());
        REQUIRE(*line == "; Sample G-code");
    }

    SECTION("reads subsequent lines") {
        // Find second line (after first newline)
        size_t pos = SAMPLE_GCODE.find('\n') + 1;
        auto line = source.read_line(pos);
        REQUIRE(line.has_value());
        REQUIRE(*line == "G28 ; Home");
    }
}

TEST_CASE("FileDataSource invalid file", "[gcode][datasource]") {
    FileDataSource source("/nonexistent/path/file.gcode");

    SECTION("reports invalid") {
        REQUIRE_FALSE(source.is_valid());
    }

    SECTION("returns zero size") {
        REQUIRE(source.file_size() == 0);
    }

    SECTION("read_range returns empty") {
        auto data = source.read_range(0, 100);
        REQUIRE(data.empty());
    }
}

TEST_CASE("FileDataSource move semantics", "[gcode][datasource]") {
    TempFile temp(SAMPLE_GCODE);

    FileDataSource source1(temp.path());
    REQUIRE(source1.is_valid());

    // Move construct
    FileDataSource source2(std::move(source1));
    REQUIRE(source2.is_valid());

    // Original should be invalid after move
    REQUIRE_FALSE(source1.is_valid()); // NOLINT - testing moved-from state

    // Moved-to should work
    auto data = source2.read_range(0, 10);
    REQUIRE(data.size() == 10);
}

TEST_CASE("MemoryDataSource from string", "[gcode][datasource]") {
    MemoryDataSource source(SAMPLE_GCODE, "test-gcode");

    SECTION("is always valid") {
        REQUIRE(source.is_valid());
    }

    SECTION("reports correct size") {
        REQUIRE(source.file_size() == SAMPLE_GCODE.size());
    }

    SECTION("reports source name") {
        REQUIRE(source.source_name() == "test-gcode");
    }

    SECTION("supports range requests") {
        REQUIRE(source.supports_range_requests());
    }

    SECTION("reads entire content") {
        auto data = source.read_all();
        std::string content(data.begin(), data.end());
        REQUIRE(content == SAMPLE_GCODE);
    }

    SECTION("reads byte range") {
        auto data = source.read_range(5, 10);
        REQUIRE(data.size() == 10);
        std::string content(data.begin(), data.end());
        REQUIRE(content == SAMPLE_GCODE.substr(5, 10));
    }
}

TEST_CASE("MemoryDataSource from vector", "[gcode][datasource]") {
    std::vector<char> bytes = {'H', 'e', 'l', 'l', 'o'};
    MemoryDataSource source(bytes);

    SECTION("has correct size") {
        REQUIRE(source.file_size() == 5);
    }

    SECTION("reads content") {
        auto data = source.read_range(0, 5);
        REQUIRE(data.size() == 5);
        REQUIRE(std::string(data.begin(), data.end()) == "Hello");
    }
}

TEST_CASE("MemoryDataSource empty content", "[gcode][datasource]") {
    MemoryDataSource source("");

    SECTION("is valid even when empty") {
        REQUIRE(source.is_valid());
    }

    SECTION("has zero size") {
        REQUIRE(source.file_size() == 0);
    }

    SECTION("read_range returns empty") {
        auto data = source.read_range(0, 10);
        REQUIRE(data.empty());
    }

    SECTION("read_line returns empty string at offset 0") {
        auto line = source.read_line(0);
        REQUIRE(line.has_value());
        REQUIRE(line->empty());
    }
}
