// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fonts.h"

#include "../ui_test_utils.h"
#include "theme_manager.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

// Helper to extract RGB from lv_color_t (masks out alpha channel)
// lv_color_to_u32() returns 0xAARRGGBB, we only care about 0x00RRGGBB
#define COLOR_RGB(color) (lv_color_to_u32(color) & 0x00FFFFFF)

// ============================================================================
// Color Parsing Tests
// ============================================================================

TEST_CASE("UI Theme: Parse valid hex color", "[ui_theme][color]") {
    lv_color_t color = theme_manager_parse_hex_color("#FF0000");

    // Red channel should be max
    REQUIRE(COLOR_RGB(color) == 0xFF0000);
}

TEST_CASE("UI Theme: Parse various colors", "[ui_theme][color]") {
    SECTION("Black") {
        lv_color_t color = theme_manager_parse_hex_color("#000000");
        REQUIRE(COLOR_RGB(color) == 0x000000);
    }

    SECTION("White") {
        lv_color_t color = theme_manager_parse_hex_color("#FFFFFF");
        REQUIRE(COLOR_RGB(color) == 0xFFFFFF);
    }

    SECTION("Red") {
        lv_color_t color = theme_manager_parse_hex_color("#FF0000");
        REQUIRE(COLOR_RGB(color) == 0xFF0000);
    }

    SECTION("Green") {
        lv_color_t color = theme_manager_parse_hex_color("#00FF00");
        REQUIRE(COLOR_RGB(color) == 0x00FF00);
    }

    SECTION("Blue") {
        lv_color_t color = theme_manager_parse_hex_color("#0000FF");
        REQUIRE(COLOR_RGB(color) == 0x0000FF);
    }
}

TEST_CASE("UI Theme: Parse lowercase hex", "[ui_theme][color]") {
    lv_color_t color1 = theme_manager_parse_hex_color("#ff0000");
    lv_color_t color2 = theme_manager_parse_hex_color("#FF0000");

    REQUIRE(COLOR_RGB(color1) == COLOR_RGB(color2));
}

TEST_CASE("UI Theme: Parse mixed case hex", "[ui_theme][color]") {
    lv_color_t color = theme_manager_parse_hex_color("#AbCdEf");

    REQUIRE(COLOR_RGB(color) == 0xABCDEF);
}

TEST_CASE("UI Theme: Parse typical UI colors", "[ui_theme][color]") {
    SECTION("Primary color (example)") {
        lv_color_t color = theme_manager_parse_hex_color("#2196F3");
        REQUIRE(COLOR_RGB(color) == 0x2196F3);
    }

    SECTION("Success green") {
        lv_color_t color = theme_manager_parse_hex_color("#4CAF50");
        REQUIRE(COLOR_RGB(color) == 0x4CAF50);
    }

    SECTION("Warning orange") {
        lv_color_t color = theme_manager_parse_hex_color("#FF9800");
        REQUIRE(COLOR_RGB(color) == 0xFF9800);
    }

    SECTION("Error red") {
        lv_color_t color = theme_manager_parse_hex_color("#F44336");
        REQUIRE(COLOR_RGB(color) == 0xF44336);
    }

    SECTION("Gray") {
        lv_color_t color = theme_manager_parse_hex_color("#9E9E9E");
        REQUIRE(COLOR_RGB(color) == 0x9E9E9E);
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("UI Theme: Handle invalid color strings", "[ui_theme][color][error]") {
    SECTION("NULL pointer") {
        lv_color_t color = theme_manager_parse_hex_color(nullptr);
        // Should return black (0x000000) as fallback
        REQUIRE(COLOR_RGB(color) == 0x000000);
    }

    SECTION("Missing # prefix") {
        lv_color_t color = theme_manager_parse_hex_color("FF0000");
        // Should return black as fallback
        REQUIRE(COLOR_RGB(color) == 0x000000);
    }

    SECTION("Empty string") {
        lv_color_t color = theme_manager_parse_hex_color("");
        // Should return black as fallback
        REQUIRE(COLOR_RGB(color) == 0x000000);
    }

    SECTION("Just # symbol") {
        lv_color_t color = theme_manager_parse_hex_color("#");
        // Should parse as 0 (black)
        REQUIRE(COLOR_RGB(color) == 0x000000);
    }
}

TEST_CASE("UI Theme: Handle malformed hex strings", "[ui_theme][color][error]") {
    SECTION("Too short") {
        lv_color_t color = theme_manager_parse_hex_color("#FF");
        // Should parse as 0xFF (255)
        REQUIRE(COLOR_RGB(color) == 0x0000FF);
    }

    SECTION("Invalid hex characters") {
        lv_color_t color = theme_manager_parse_hex_color("#GGGGGG");
        // Invalid hex, should parse as 0
        REQUIRE(COLOR_RGB(color) == 0x000000);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("UI Theme: Color parsing edge cases", "[ui_theme][color][edge]") {
    SECTION("All zeros") {
        lv_color_t color = theme_manager_parse_hex_color("#000000");
        REQUIRE(COLOR_RGB(color) == 0x000000);
    }

    SECTION("All ones") {
        lv_color_t color = theme_manager_parse_hex_color("#111111");
        REQUIRE(COLOR_RGB(color) == 0x111111);
    }

    SECTION("All Fs") {
        lv_color_t color = theme_manager_parse_hex_color("#FFFFFF");
        REQUIRE(COLOR_RGB(color) == 0xFFFFFF);
    }

    SECTION("Leading zeros") {
        lv_color_t color = theme_manager_parse_hex_color("#000001");
        REQUIRE(COLOR_RGB(color) == 0x000001);
    }
}

// ============================================================================
// Consistency Tests
// ============================================================================

TEST_CASE("UI Theme: Multiple parses of same color", "[ui_theme][color]") {
    const char* color_str = "#2196F3";

    lv_color_t color1 = theme_manager_parse_hex_color(color_str);
    lv_color_t color2 = theme_manager_parse_hex_color(color_str);
    lv_color_t color3 = theme_manager_parse_hex_color(color_str);

    REQUIRE(COLOR_RGB(color1) == COLOR_RGB(color2));
    REQUIRE(COLOR_RGB(color2) == COLOR_RGB(color3));
}

// ============================================================================
// Integration Tests with LVGL
// ============================================================================

TEST_CASE("UI Theme: Parsed colors work with LVGL", "[ui_theme][integration]") {
    lv_init_safe();

    lv_color_t red = theme_manager_parse_hex_color("#FF0000");
    lv_color_t green = theme_manager_parse_hex_color("#00FF00");
    lv_color_t blue = theme_manager_parse_hex_color("#0000FF");

    // Create a simple object and set its background color
    lv_obj_t* obj = lv_obj_create(lv_screen_active());
    REQUIRE(obj != nullptr);

    lv_obj_set_style_bg_color(obj, red, 0);
    lv_obj_set_style_bg_color(obj, green, 0);
    lv_obj_set_style_bg_color(obj, blue, 0);

    // Cleanup
    lv_obj_delete(obj);
}

// ============================================================================
// Color Comparison Tests
// ============================================================================

TEST_CASE("UI Theme: Color equality", "[ui_theme][color]") {
    lv_color_t color1 = theme_manager_parse_hex_color("#FF0000");
    lv_color_t color2 = theme_manager_parse_hex_color("#FF0000");
    lv_color_t color3 = theme_manager_parse_hex_color("#00FF00");

    REQUIRE(COLOR_RGB(color1) == COLOR_RGB(color2));
    REQUIRE(COLOR_RGB(color1) != COLOR_RGB(color3));
}

// ============================================================================
// Real-world Color Examples
// ============================================================================

TEST_CASE("UI Theme: Parse colors from globals.xml", "[ui_theme][color][integration]") {
    // These are typical colors that might appear in globals.xml

    SECTION("Primary colors") {
        lv_color_t primary_light = theme_manager_parse_hex_color("#2196F3");
        lv_color_t primary_dark = theme_manager_parse_hex_color("#1976D2");

        REQUIRE(COLOR_RGB(primary_light) == 0x2196F3);
        REQUIRE(COLOR_RGB(primary_dark) == 0x1976D2);
    }

    SECTION("Background colors") {
        lv_color_t bg_light = theme_manager_parse_hex_color("#FFFFFF");
        lv_color_t bg_dark = theme_manager_parse_hex_color("#121212");

        REQUIRE(COLOR_RGB(bg_light) == 0xFFFFFF);
        REQUIRE(COLOR_RGB(bg_dark) == 0x121212);
    }

    SECTION("Text colors") {
        lv_color_t text_light = theme_manager_parse_hex_color("#000000");
        lv_color_t text_dark = theme_manager_parse_hex_color("#FFFFFF");

        REQUIRE(COLOR_RGB(text_light) == 0x000000);
        REQUIRE(COLOR_RGB(text_dark) == 0xFFFFFF);
    }

    SECTION("State colors") {
        lv_color_t success = theme_manager_parse_hex_color("#4CAF50");
        lv_color_t warning = theme_manager_parse_hex_color("#FF9800");
        lv_color_t error = theme_manager_parse_hex_color("#F44336");

        REQUIRE(COLOR_RGB(success) == 0x4CAF50);
        REQUIRE(COLOR_RGB(warning) == 0xFF9800);
        REQUIRE(COLOR_RGB(error) == 0xF44336);
    }
}

// ============================================================================
// Responsive Breakpoint Tests
// ============================================================================

TEST_CASE("UI Theme: Breakpoint suffix detection", "[ui_theme][responsive]") {
    SECTION("Micro breakpoint (≤272px)") {
        // The tier 480x272 lands in: the argument is responsive_dimension(), the
        // SMALLER of width and height, so a 480x272 panel resolves on 272. It was
        // untested until now because the header documented only five of the seven
        // tiers this function returns, omitting _micro entirely - and _tiny's own
        // case probes 320, which is above the micro boundary and so never noticed.
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(272), "_micro") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(240), "_micro") == 0);
    }

    SECTION("Tiny breakpoint (height ≤390px)") {
        // Heights at or below 390 should select _tiny variants
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(320), "_tiny") == 0);
    }

    SECTION("Small breakpoint (height 391-460px)") {
        // Heights between 391 and 460 should select _small variants
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(400), "_small") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(440), "_small") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(460), "_small") == 0);
    }

    SECTION("Medium breakpoint (height 461-550px)") {
        // Heights between 461 and 550 should select _medium variants
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(461), "_medium") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(480), "_medium") == 0);
    }

    SECTION("Large breakpoint (height 551-700px)") {
        // Heights between 551 and 700 should select _large variants
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(600), "_large") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(700), "_large") == 0);
    }

    SECTION("XLarge breakpoint (height 701-1000px)") {
        // Heights between 701 and 1000 should select _xlarge variants
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(701), "_xlarge") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(720), "_xlarge") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(1000), "_xlarge") == 0);
    }

    SECTION("XXLarge breakpoint (height >1000px)") {
        // Heights above 1000 should select _xxlarge variants
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(1001), "_xxlarge") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(1080), "_xxlarge") == 0);
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(2160), "_xxlarge") == 0);
    }
}

TEST_CASE("UI Theme: Breakpoint boundary conditions", "[ui_theme][responsive]") {
    SECTION("Exact boundary: 272 → micro") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(272), "_micro") == 0);
    }

    SECTION("Exact boundary: 273 → tiny") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(273), "_tiny") == 0);
    }

    SECTION("Exact boundary: 460 → small") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(460), "_small") == 0);
    }

    SECTION("Exact boundary: 461 → medium") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(461), "_medium") == 0);
    }

    SECTION("Exact boundary: 700 → large") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(700), "_large") == 0);
    }

    SECTION("Exact boundary: 701 → xlarge") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(701), "_xlarge") == 0);
    }
}

TEST_CASE("UI Theme: Target hardware resolutions", "[ui_theme][responsive]") {
    // Test against the specific target hardware — breakpoint uses screen HEIGHT
    SECTION("480x320 (tiny screen) → TINY") {
        // height=320 ≤390 → TINY
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(320), "_tiny") == 0);
    }

    SECTION("480x400 (K1 screen) → SMALL") {
        // height=400, 391-460 → SMALL
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(400), "_small") == 0);
    }

    SECTION("1920x440 (ultra-wide) → SMALL") {
        // height=440, 391-460 → SMALL
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(440), "_small") == 0);
    }

    SECTION("800x480 (AD5M screen) → MEDIUM") {
        // height=480, 461-550 → MEDIUM
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(480), "_medium") == 0);
    }

    SECTION("1024x600 (medium screen) → LARGE") {
        // height=600, 551-700 → LARGE
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(600), "_large") == 0);
    }

    SECTION("1280x720 (large screen) → XLARGE") {
        // height=720 >700 → XLARGE
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(720), "_xlarge") == 0);
    }
}

TEST_CASE("UI Theme: Font height helper", "[ui_theme][responsive]") {
    // Test that font height helper returns valid values for project fonts
    // Note: This project uses noto_sans_* fonts instead of lv_font_montserrat_*
    SECTION("Valid fonts return positive height") {
        REQUIRE(theme_manager_get_font_height(&noto_sans_12) > 0);
        REQUIRE(theme_manager_get_font_height(&noto_sans_16) > 0);
        REQUIRE(theme_manager_get_font_height(&noto_sans_20) > 0);
    }

    SECTION("NULL font returns 0") {
        REQUIRE(theme_manager_get_font_height(nullptr) == 0);
    }

    SECTION("Larger fonts have larger heights") {
        int32_t h12 = theme_manager_get_font_height(&noto_sans_12);
        int32_t h16 = theme_manager_get_font_height(&noto_sans_16);
        int32_t h20 = theme_manager_get_font_height(&noto_sans_20);

        REQUIRE(h12 < h16);
        REQUIRE(h16 < h20);
    }
}

// ============================================================================
// Multi-File Responsive Constants Tests
// ============================================================================
// These tests verify the extension of responsive constants (_small/_medium/_large)
// to work with ALL XML files, not just globals.xml.
// Functions under test: parse_xml_file_for_suffix(), find_xml_files(),
// parse_all_xml_for_suffix()

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <vector>

// Test fixture helper: creates a temp directory and cleans up on destruction
class TempXmlDirectory {
  public:
    TempXmlDirectory() {
        // Create unique temp directory for test isolation
        // Use atomic counter + thread ID to avoid race conditions in parallel tests
        static std::atomic<int> counter{0};
        std::ostringstream oss;
        oss << "/tmp/helix_test_xml_" << counter.fetch_add(1) << "_" << std::this_thread::get_id();
        m_path = oss.str();
        std::filesystem::create_directories(m_path);
    }

    ~TempXmlDirectory() {
        // Cleanup temp directory and all contents
        std::filesystem::remove_all(m_path);
    }

    const std::string& path() const {
        return m_path;
    }

    // Create an XML file with given content
    void create_file(const std::string& filename, const std::string& content) {
        std::string filepath = m_path + "/" + filename;
        std::ofstream out(filepath);
        out << content;
        out.close();
    }

  private:
    std::string m_path;
};

TEST_CASE("Parse XML file for suffix: extracts name and value", "[ui_theme][responsive]") {
    TempXmlDirectory temp_dir;

    SECTION("Extracts px constants with _small suffix") {
        temp_dir.create_file("test_component.xml", R"(
            <component>
                <consts>
                    <px name="button_height_small" value="32"/>
                    <px name="button_height_medium" value="40"/>
                    <px name="button_height_large" value="48"/>
                    <px name="icon_size_small" value="16"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/test_component.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "px", "_small", results);

        // Should extract base name "button_height" with value "32"
        REQUIRE(results.size() == 2);
        REQUIRE(results["button_height"] == "32");
        REQUIRE(results["icon_size"] == "16");
    }

    SECTION("Extracts color constants with _light suffix") {
        temp_dir.create_file("theme.xml", R"(
            <component>
                <consts>
                    <color name="card_bg_light" value="#FFFFFF"/>
                    <color name="card_bg_dark" value="#1A1A1A"/>
                    <color name="text_primary_light" value="#000000"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/theme.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "color", "_light", results);

        REQUIRE(results.size() == 2);
        REQUIRE(results["card_bg"] == "#FFFFFF");
        REQUIRE(results["text_primary"] == "#000000");
    }

    SECTION("Extracts string constants with suffix") {
        temp_dir.create_file("strings.xml", R"(
            <component>
                <consts>
                    <string name="font_body_small" value="noto_sans_14"/>
                    <string name="font_body_medium" value="noto_sans_18"/>
                    <string name="font_body_large" value="noto_sans_20"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/strings.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "string", "_medium", results);

        REQUIRE(results.size() == 1);
        REQUIRE(results["font_body"] == "noto_sans_18");
    }

    SECTION("Ignores elements without matching suffix") {
        temp_dir.create_file("mixed.xml", R"(
            <component>
                <consts>
                    <px name="padding_small" value="4"/>
                    <px name="padding" value="8"/>
                    <px name="other_thing" value="100"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/mixed.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "px", "_small", results);

        // Only "padding_small" should match
        REQUIRE(results.size() == 1);
        REQUIRE(results["padding"] == "4");
    }
}

TEST_CASE("Parse XML file for suffix: handles missing files gracefully", "[ui_theme][responsive]") {
    std::unordered_map<std::string, std::string> results;

    SECTION("Non-existent file does not crash") {
        // Should not throw or crash, just return empty results
        theme_manager_parse_xml_file_for_suffix("/nonexistent/path/file.xml", "px", "_small",
                                                results);
        REQUIRE(results.empty());
    }

    SECTION("NULL filepath does not crash") {
        theme_manager_parse_xml_file_for_suffix(nullptr, "px", "_small", results);
        REQUIRE(results.empty());
    }
}

TEST_CASE("Parse XML file for suffix: handles malformed XML gracefully", "[ui_theme][responsive]") {
    TempXmlDirectory temp_dir;

    SECTION("Truncated XML does not crash") {
        temp_dir.create_file("truncated.xml", R"(
            <component>
                <consts>
                    <px name="test_small" value="10"
        )");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/truncated.xml";

        // Should not throw or crash
        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "px", "_small", results);
        // May or may not extract partial data, but should not crash
    }

    SECTION("Empty file does not crash") {
        temp_dir.create_file("empty.xml", "");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/empty.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "px", "_small", results);
        REQUIRE(results.empty());
    }

    SECTION("Non-XML content does not crash") {
        temp_dir.create_file("not_xml.xml", "This is not XML content at all!");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/not_xml.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "px", "_small", results);
        REQUIRE(results.empty());
    }

    SECTION("Missing name attribute is skipped") {
        temp_dir.create_file("no_name.xml", R"(
            <component>
                <consts>
                    <px value="10"/>
                    <px name="valid_small" value="20"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/no_name.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "px", "_small", results);
        REQUIRE(results.size() == 1);
        REQUIRE(results["valid"] == "20");
    }

    SECTION("Missing value attribute is skipped") {
        temp_dir.create_file("no_value.xml", R"(
            <component>
                <consts>
                    <px name="missing_value_small"/>
                    <px name="valid_small" value="30"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results;
        std::string filepath = temp_dir.path() + "/no_value.xml";

        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), "px", "_small", results);
        REQUIRE(results.size() == 1);
        REQUIRE(results["valid"] == "30");
    }
}

TEST_CASE("Find XML files: returns sorted list", "[ui_theme][responsive]") {
    TempXmlDirectory temp_dir;

    SECTION("Returns files in alphabetical order") {
        // Create files in non-alphabetical order
        temp_dir.create_file("zebra.xml", "<component/>");
        temp_dir.create_file("apple.xml", "<component/>");
        temp_dir.create_file("mango.xml", "<component/>");

        std::vector<std::string> files = theme_manager_find_xml_files(temp_dir.path().c_str());

        REQUIRE(files.size() == 3);
        // Should be sorted alphabetically
        REQUIRE(files[0].find("apple.xml") != std::string::npos);
        REQUIRE(files[1].find("mango.xml") != std::string::npos);
        REQUIRE(files[2].find("zebra.xml") != std::string::npos);
    }

    SECTION("Returns full paths") {
        temp_dir.create_file("test.xml", "<component/>");

        std::vector<std::string> files = theme_manager_find_xml_files(temp_dir.path().c_str());

        REQUIRE(files.size() == 1);
        // Should contain the directory path
        REQUIRE(files[0].find(temp_dir.path()) != std::string::npos);
        REQUIRE(files[0].find("test.xml") != std::string::npos);
    }

    SECTION("Empty directory returns empty list") {
        std::vector<std::string> files = theme_manager_find_xml_files(temp_dir.path().c_str());
        REQUIRE(files.empty());
    }

    SECTION("Non-existent directory returns empty list") {
        std::vector<std::string> files =
            theme_manager_find_xml_files("/nonexistent/directory/path");
        REQUIRE(files.empty());
    }

    SECTION("NULL directory returns empty list") {
        std::vector<std::string> files = theme_manager_find_xml_files(nullptr);
        REQUIRE(files.empty());
    }
}

TEST_CASE("Find XML files: filters non-XML files", "[ui_theme][responsive]") {
    TempXmlDirectory temp_dir;

    SECTION("Only includes .xml files") {
        temp_dir.create_file("component.xml", "<component/>");
        temp_dir.create_file("readme.txt", "text content");
        temp_dir.create_file("style.css", "css content");
        temp_dir.create_file("another.xml", "<component/>");
        temp_dir.create_file("data.json", "{}");

        std::vector<std::string> files = theme_manager_find_xml_files(temp_dir.path().c_str());

        REQUIRE(files.size() == 2);
        // Both should be XML files
        for (const auto& file : files) {
            REQUIRE(file.find(".xml") != std::string::npos);
        }
    }

    SECTION("Case sensitivity for .xml extension") {
        temp_dir.create_file("lower.xml", "<component/>");
        temp_dir.create_file("upper.XML", "<component/>");
        temp_dir.create_file("mixed.Xml", "<component/>");

        std::vector<std::string> files = theme_manager_find_xml_files(temp_dir.path().c_str());

        // Implementation should handle this consistently
        // At minimum, lowercase .xml should be included
        REQUIRE(files.size() >= 1);
        bool has_lowercase = false;
        for (const auto& f : files) {
            if (f.find("lower.xml") != std::string::npos)
                has_lowercase = true;
        }
        REQUIRE(has_lowercase);
    }

    SECTION("Does not recurse into subdirectories") {
        temp_dir.create_file("root.xml", "<component/>");
        // Create subdirectory with XML file
        std::string subdir = temp_dir.path() + "/subdir";
        std::filesystem::create_directories(subdir);
        std::ofstream(subdir + "/nested.xml") << "<component/>";

        std::vector<std::string> files = theme_manager_find_xml_files(temp_dir.path().c_str());

        // Should only find root.xml, not nested.xml
        REQUIRE(files.size() == 1);
        REQUIRE(files[0].find("root.xml") != std::string::npos);
    }
}

TEST_CASE("Multi-file aggregation: component overrides global", "[ui_theme][responsive]") {
    TempXmlDirectory temp_dir;

    SECTION("Later file overrides earlier file (last-wins)") {
        // Create globals.xml (processed first due to alphabetical order)
        temp_dir.create_file("globals.xml", R"(
            <component>
                <consts>
                    <px name="button_height_small" value="32"/>
                    <px name="card_padding_small" value="8"/>
                </consts>
            </component>
        )");

        // Create widget.xml (processed after globals.xml)
        temp_dir.create_file("widget.xml", R"(
            <component>
                <consts>
                    <px name="button_height_small" value="28"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_small");

        // button_height should be overridden by widget.xml (28, not 32)
        REQUIRE(results["button_height"] == "28");
        // card_padding should still have globals.xml value
        REQUIRE(results["card_padding"] == "8");
    }

    SECTION("Multiple files contribute unique tokens") {
        temp_dir.create_file("a_first.xml", R"(
            <component>
                <consts>
                    <px name="token_a_small" value="10"/>
                </consts>
            </component>
        )");

        temp_dir.create_file("b_second.xml", R"(
            <component>
                <consts>
                    <px name="token_b_small" value="20"/>
                </consts>
            </component>
        )");

        temp_dir.create_file("c_third.xml", R"(
            <component>
                <consts>
                    <px name="token_c_small" value="30"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> results =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_small");

        REQUIRE(results.size() == 3);
        REQUIRE(results["token_a"] == "10");
        REQUIRE(results["token_b"] == "20");
        REQUIRE(results["token_c"] == "30");
    }

    SECTION("Empty directory returns empty map") {
        std::unordered_map<std::string, std::string> results =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_small");

        REQUIRE(results.empty());
    }
}

TEST_CASE("Multi-file aggregation: incomplete triplets ignored", "[ui_theme][responsive]") {
    // This test verifies that tokens without complete _small/_medium/_large
    // triplets are handled correctly (either ignored or partially used)

    TempXmlDirectory temp_dir;

    SECTION("Token with only _small variant") {
        temp_dir.create_file("partial.xml", R"(
            <component>
                <consts>
                    <px name="incomplete_token_small" value="10"/>
                </consts>
            </component>
        )");

        // Query for _small suffix - should find it
        std::unordered_map<std::string, std::string> small_results =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_small");
        REQUIRE(small_results["incomplete_token"] == "10");

        // Query for _medium suffix - should be empty (no _medium variant defined)
        std::unordered_map<std::string, std::string> medium_results =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_medium");
        REQUIRE(medium_results.find("incomplete_token") == medium_results.end());

        // Query for _large suffix - should be empty (no _large variant defined)
        std::unordered_map<std::string, std::string> large_results =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_large");
        REQUIRE(large_results.find("incomplete_token") == large_results.end());
    }

    SECTION("Complete triplet across multiple files") {
        temp_dir.create_file("file_a.xml", R"(
            <component>
                <consts>
                    <px name="spacing_small" value="4"/>
                    <px name="spacing_medium" value="8"/>
                </consts>
            </component>
        )");

        temp_dir.create_file("file_b.xml", R"(
            <component>
                <consts>
                    <px name="spacing_large" value="12"/>
                </consts>
            </component>
        )");

        // All three variants should be findable
        std::unordered_map<std::string, std::string> small =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_small");
        std::unordered_map<std::string, std::string> medium =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_medium");
        std::unordered_map<std::string, std::string> large =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_large");

        REQUIRE(small["spacing"] == "4");
        REQUIRE(medium["spacing"] == "8");
        REQUIRE(large["spacing"] == "12");
    }

    SECTION("Mix of complete and incomplete triplets") {
        temp_dir.create_file("mixed.xml", R"(
            <component>
                <consts>
                    <!-- Complete triplet -->
                    <px name="complete_small" value="10"/>
                    <px name="complete_medium" value="20"/>
                    <px name="complete_large" value="30"/>

                    <!-- Incomplete - only small and large -->
                    <px name="partial_small" value="5"/>
                    <px name="partial_large" value="15"/>
                </consts>
            </component>
        )");

        std::unordered_map<std::string, std::string> small =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_small");
        std::unordered_map<std::string, std::string> medium =
            theme_manager_parse_all_xml_for_suffix(temp_dir.path().c_str(), "px", "_medium");

        // Complete triplet - all present
        REQUIRE(small["complete"] == "10");
        REQUIRE(medium["complete"] == "20");

        // Partial triplet - small exists, medium does not
        REQUIRE(small["partial"] == "5");
        REQUIRE(medium.find("partial") == medium.end());
    }
}
