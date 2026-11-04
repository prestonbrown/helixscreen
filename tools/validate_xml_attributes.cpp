// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file validate_xml_attributes.cpp
 * @brief CLI tool to validate XML attributes against LVGL widget definitions
 *
 * Scans LVGL parser sources and custom widget registrations to build a database
 * of valid attributes for each widget type, then validates XML files against
 * this database.
 *
 * Usage: validate-xml-attributes [options] [files...]
 *
 * Options:
 *   --warn-only    Print warnings but exit 0
 *   --verbose      Show all files checked, not just errors
 *   -h, --help     Show this help message
 *
 * Arguments:
 *   files          XML files to validate (default: ui_xml/)
 *
 * Exit codes:
 *   0 - All attributes valid (or --warn-only)
 *   1 - Found unknown attributes
 */

#include "xml_attribute_validator.h"

#include <cstring>
#include <expat.h>
#include <filesystem>
#include <fstream>
#include <glob.h>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// Cache file location
static const std::string kCacheFile = "build/.xml_attr_cache";

// LVGL XML structure elements that are not widgets - skip validation
static const std::unordered_set<std::string> kNonWidgetElements = {
    "component", "api",   "view",    "prop",     "consts",    "px",
    "styles",    "style", "subject", "subjects", "gradients", "gradient",
    "images",    "fonts", "font",    "const",    "percentage"};

// XML-specific attributes that should not be validated as widget attributes
static const std::unordered_set<std::string> kXmlBuiltinAttrs = {"xmlns", "version", "encoding"};

// Common attributes that all widgets inherit from lv_obj
// These are extracted from lv_xml_obj.c but we define them here as a fallback
// NOTE: scroll_dir is NOT supported by LVGL XML parser - do not include it
static const std::unordered_set<std::string> kCommonLvObjAttrs = {"name",
                                                                  "x",
                                                                  "y",
                                                                  "width",
                                                                  "height",
                                                                  "align",
                                                                  "hidden",
                                                                  "clickable",
                                                                  "click_focusable",
                                                                  "checkable",
                                                                  "scrollable",
                                                                  "scroll_snap_x",
                                                                  "scroll_snap_y",
                                                                  "flex_grow",
                                                                  "flex_flow",
                                                                  "grid_cell_row_pos",
                                                                  "grid_cell_row_span",
                                                                  "grid_cell_column_pos",
                                                                  "grid_cell_column_span",
                                                                  "grid_cell_x_align",
                                                                  "grid_cell_y_align"};

// Part suffixes used in LVGL style selectors (e.g., style_arc_width_indicator -> style_arc_width)
static const std::vector<std::string> kPartSuffixes = {"_main", "_indicator", "_knob"};

/**
 * @brief Read entire file content into a string.
 */
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/**
 * @brief Find files matching a glob pattern.
 */
static std::vector<std::string> find_files(const std::string& pattern) {
    std::vector<std::string> files;
    glob_t glob_result;
    if (glob(pattern.c_str(), GLOB_TILDE, nullptr, &glob_result) == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            files.push_back(glob_result.gl_pathv[i]);
        }
    }
    globfree(&glob_result);
    return files;
}

/**
 * @brief Context for XML parsing.
 */
struct ParseContext {
    XML_Parser parser;
    std::string current_file;
    std::vector<std::string> errors;
    const std::unordered_map<std::string, std::unordered_set<std::string>>* valid_attrs;
    bool verbose;
};

/**
 * @brief Expat start element callback.
 */
static void XMLCALL start_element(void* data, const char* name, const char** attrs) {
    ParseContext* ctx = static_cast<ParseContext*>(data);
    int line = XML_GetCurrentLineNumber(ctx->parser);

    std::string widget_name = name;

    // Skip non-widget XML structure elements
    if (kNonWidgetElements.count(widget_name) > 0) {
        return;
    }

    // Find valid attrs for this widget
    auto it = ctx->valid_attrs->find(widget_name);
    const std::unordered_set<std::string>* widget_attrs = nullptr;
    if (it != ctx->valid_attrs->end()) {
        widget_attrs = &it->second;
    }

    // Check each attribute
    for (int i = 0; attrs[i]; i += 2) {
        std::string attr_name = attrs[i];

        // Skip XML-specific attributes
        if (kXmlBuiltinAttrs.count(attr_name) > 0) {
            continue;
        }

        // Skip xmlns:* prefixed attributes
        if (attr_name.substr(0, 6) == "xmlns:") {
            continue;
        }

        // If we don't know this widget, skip validation (unknown custom widget)
        if (!widget_attrs) {
            continue;
        }

        // Handle style selector syntax: style_*:selector -> style_*
        // e.g., style_text_color:checked -> style_text_color
        //       style_arc_width:indicator -> style_arc_width
        std::string normalized_attr = attr_name;
        if (attr_name.substr(0, 6) == "style_") {
            size_t colon_pos = attr_name.find(':');
            if (colon_pos != std::string::npos) {
                normalized_attr = attr_name.substr(0, colon_pos);
            }
        }

        // Handle part-specific style suffixes: style_arc_opa_main -> style_arc_opa
        // LVGL uses _main, _indicator, _knob suffixes for part-specific styles
        if (normalized_attr.substr(0, 6) == "style_") {
            for (const auto& suffix : kPartSuffixes) {
                if (normalized_attr.size() > suffix.size() &&
                    normalized_attr.substr(normalized_attr.size() - suffix.size()) == suffix) {
                    normalized_attr =
                        normalized_attr.substr(0, normalized_attr.size() - suffix.size());
                    break;
                }
            }
        }

        // Handle flag_ prefix: flag_clickable -> clickable
        if (normalized_attr.substr(0, 5) == "flag_") {
            normalized_attr = normalized_attr.substr(5);
        }

        // Check if attribute is valid for this widget
        if (widget_attrs->count(normalized_attr) == 0) {
            std::ostringstream oss;
            oss << ctx->current_file << ":" << line << ": Unknown attribute '" << attr_name
                << "' on " << widget_name;
            ctx->errors.push_back(oss.str());
        }
    }
}

/**
 * @brief Expat end element callback (unused but required).
 */
static void XMLCALL end_element(void* /*data*/, const char* /*name*/) {
    // Nothing to do
}

/**
 * @brief Validate a single XML file.
 */
static std::vector<std::string> validate_xml_file(
    const std::string& filepath,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& valid_attrs,
    bool verbose) {
    std::vector<std::string> errors;

    std::string content = read_file(filepath);
    if (content.empty()) {
        if (verbose) {
            std::cerr << "Warning: Could not read " << filepath << std::endl;
        }
        return errors;
    }

    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) {
        std::cerr << "Error: Could not create XML parser" << std::endl;
        return errors;
    }

    ParseContext ctx;
    ctx.parser = parser;
    ctx.current_file = filepath;
    ctx.valid_attrs = &valid_attrs;
    ctx.verbose = verbose;

    XML_SetUserData(parser, &ctx);
    XML_SetElementHandler(parser, start_element, end_element);

    if (XML_Parse(parser, content.c_str(), static_cast<int>(content.size()), XML_TRUE) ==
        XML_STATUS_ERROR) {
        std::ostringstream oss;
        oss << filepath << ":" << XML_GetCurrentLineNumber(parser)
            << ": XML parse error: " << XML_ErrorString(XML_GetErrorCode(parser));
        errors.push_back(oss.str());
    } else {
        errors = std::move(ctx.errors);
    }

    XML_ParserFree(parser);
    return errors;
}

/**
 * @brief Get the newest modification time from source directories.
 */
static std::time_t get_newest_source_mtime() {
    std::time_t newest = 0;

    auto check_files = [&newest](const std::string& pattern) {
        auto files = find_files(pattern);
        for (const auto& path : files) {
            try {
                auto mtime = fs::last_write_time(path);
                auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
                    mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t t = std::chrono::system_clock::to_time_t(sctp);
                if (t > newest) {
                    newest = t;
                }
            } catch (...) {
                // Ignore errors
            }
        }
    };

    check_files("lib/lvgl/src/xml/parsers/*.c");
    check_files("lib/lvgl/src/xml/lv_xml.c");
    check_files("src/ui/*.cpp");
    check_files("src/ui/*/*.cpp");
    check_files("ui_xml/*.xml");

    return newest;
}

/**
 * @brief Save the widget database to cache file.
 */
static void save_cache(const xml_validator::WidgetDatabase& db, std::time_t source_mtime) {
    std::ofstream f(kCacheFile);
    if (!f) {
        return; // Can't write cache, not fatal
    }

    f << "CACHE_VERSION 1\n";
    f << "SOURCE_MTIME " << source_mtime << "\n";

    for (const auto& [widget, attrs] : db.widget_attrs) {
        f << "WIDGET " << widget;
        for (const auto& attr : attrs) {
            f << " " << attr;
        }
        f << "\n";
    }

    for (const auto& [widget, parent] : db.inheritance) {
        f << "INHERIT " << widget << " " << parent << "\n";
    }
}

/**
 * @brief Load the widget database from cache file if valid.
 * @return true if cache was loaded successfully, false if cache is stale/missing
 */
static bool load_cache(xml_validator::WidgetDatabase& db, std::time_t current_source_mtime,
                       bool verbose) {
    std::ifstream f(kCacheFile);
    if (!f) {
        return false; // No cache file
    }

    std::string line;

    // Check version
    if (!std::getline(f, line) || line != "CACHE_VERSION 1") {
        if (verbose) {
            std::cout << "Cache version mismatch, rebuilding..." << std::endl;
        }
        return false;
    }

    // Check source mtime
    if (!std::getline(f, line) || line.substr(0, 13) != "SOURCE_MTIME ") {
        return false;
    }
    std::time_t cached_mtime = std::stoll(line.substr(13));
    if (cached_mtime < current_source_mtime) {
        if (verbose) {
            std::cout << "Cache outdated, rebuilding..." << std::endl;
        }
        return false;
    }

    // Load widgets and inheritance
    while (std::getline(f, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "WIDGET") {
            std::string widget;
            iss >> widget;
            std::unordered_set<std::string> attrs;
            std::string attr;
            while (iss >> attr) {
                attrs.insert(attr);
            }
            db.widget_attrs[widget] = std::move(attrs);
        } else if (type == "INHERIT") {
            std::string widget, parent;
            iss >> widget >> parent;
            db.inheritance[widget] = parent;
        }
    }

    if (verbose) {
        std::cout << "Loaded " << db.widget_attrs.size() << " widgets from cache" << std::endl;
    }
    return true;
}

/**
 * @brief Build the widget attribute database from LVGL sources and components.
 */
static xml_validator::WidgetDatabase build_widget_database(bool verbose) {
    xml_validator::WidgetDatabase db;

    // 1. Scan LVGL parser sources for widget attributes
    auto parser_files = find_files("lib/lvgl/src/xml/parsers/*.c");
    if (verbose) {
        std::cout << "Scanning " << parser_files.size() << " LVGL parser files..." << std::endl;
    }

    for (const auto& path : parser_files) {
        std::string content = read_file(path);
        if (content.empty()) {
            continue;
        }

        // Extract widget name from filename (e.g., lv_xml_label_parser.c -> lv_label)
        fs::path p(path);
        std::string filename = p.stem().string(); // e.g., "lv_xml_label_parser"
        std::string widget_name;

        // Handle special case: lv_xml_obj_parser.c -> lv_obj
        if (filename == "lv_xml_obj_parser") {
            widget_name = "lv_obj";
        } else if (filename.substr(0, 7) == "lv_xml_" && filename.size() > 14 &&
                   filename.substr(filename.size() - 7) == "_parser") {
            // Convert lv_xml_label_parser -> lv_label
            // Strip "lv_xml_" prefix (7 chars) and "_parser" suffix (7 chars)
            widget_name = "lv_" + filename.substr(7, filename.size() - 14);
        } else {
            continue; // Not a widget parser file
        }

        auto attrs = xml_validator::extract_attributes_from_parser(content, widget_name);
        if (!attrs.empty()) {
            db.widget_attrs[widget_name] = std::move(attrs);
            if (verbose) {
                std::cout << "  " << widget_name << ": " << db.widget_attrs[widget_name].size()
                          << " attributes" << std::endl;
            }
        }

        // LVGL widgets always inherit from lv_obj (they all call lv_xml_obj_apply)
        // Set this directly instead of trying to detect from code
        if (widget_name != "lv_obj") {
            db.inheritance[widget_name] = "lv_obj";
        }
    }

    // 2. Scan LVGL lv_xml.c for widget registrations (inheritance info)
    std::string lv_xml_content = read_file("lib/lvgl/src/xml/lv_xml.c");
    if (!lv_xml_content.empty()) {
        auto registrations = xml_validator::extract_widget_registration(lv_xml_content);
        if (verbose) {
            std::cout << "Found " << registrations.size() << " LVGL widget registrations"
                      << std::endl;
        }
        // Note: Registration data could be used for more sophisticated inheritance
        // For now, we assume lv_obj is the base for all LVGL widgets
        for (const auto& [widget, apply_fn] : registrations) {
            if (widget != "lv_obj" && db.inheritance.count(widget) == 0) {
                db.inheritance[widget] = "lv_obj";
            }
        }
    }

    // 3. Scan custom widget registrations in src/ui/ and its one level of group directories
    auto ui_sources = find_files("src/ui/*.cpp");
    for (auto& path : find_files("src/ui/*/*.cpp")) {
        ui_sources.push_back(std::move(path));
    }
    if (verbose) {
        std::cout << "Scanning " << ui_sources.size() << " UI source files for custom widgets..."
                  << std::endl;
    }

    for (const auto& path : ui_sources) {
        std::string content = read_file(path);
        if (content.empty()) {
            continue;
        }

        auto registrations = xml_validator::extract_widget_registration(content);
        for (const auto& [widget, apply_fn] : registrations) {
            // Detect inheritance by looking for lv_xml_*_apply calls in the file
            // If the apply function calls lv_xml_label_apply, it inherits from lv_label, etc.
            std::string parent = "lv_obj"; // Default parent

            // Look for lv_xml_*_apply calls (but not lv_xml_obj_apply which is the base)
            std::regex apply_call_regex(R"(lv_xml_(\w+)_apply\s*\()");
            auto apply_begin =
                std::sregex_iterator(content.begin(), content.end(), apply_call_regex);
            auto apply_end = std::sregex_iterator();

            for (auto it = apply_begin; it != apply_end; ++it) {
                std::string base_widget = (*it)[1].str();
                if (base_widget != "obj") {
                    parent = "lv_" + base_widget;
                    break; // Use the first non-obj parent found
                }
            }

            if (db.inheritance.count(widget) == 0) {
                db.inheritance[widget] = parent;
            }

            // Try to extract attributes from the same file (apply function)
            auto attrs = xml_validator::extract_attributes_from_parser(content, widget);
            if (!attrs.empty()) {
                db.widget_attrs[widget] = std::move(attrs);
                if (verbose) {
                    std::cout << "  " << widget << " (inherits " << parent
                              << "): " << db.widget_attrs[widget].size() << " custom attributes"
                              << std::endl;
                }
            } else {
                // Ensure widget has at least an empty entry so it's validated
                if (db.widget_attrs.count(widget) == 0) {
                    db.widget_attrs[widget] = {};
                }
                if (verbose) {
                    std::cout << "  " << widget << " (inherits " << parent << ")" << std::endl;
                }
            }
        }
    }

    // 4. Scan XML component files for props and extends
    auto xml_files = find_files("ui_xml/*.xml");
    if (verbose) {
        std::cout << "Scanning " << xml_files.size() << " XML component files..." << std::endl;
    }

    for (const auto& path : xml_files) {
        std::string content = read_file(path);
        if (content.empty()) {
            continue;
        }

        auto component_info = xml_validator::extract_component_props(content);
        if (component_info.extends.empty() && component_info.props.empty()) {
            continue; // Not a component definition
        }

        // Extract component name from filename (e.g., icon.xml -> icon)
        fs::path p(path);
        std::string component_name = p.stem().string();

        // Set inheritance
        if (!component_info.extends.empty()) {
            db.inheritance[component_name] = component_info.extends;
        } else {
            db.inheritance[component_name] = "lv_obj";
        }

        // Add props as valid attributes
        db.widget_attrs[component_name] = std::move(component_info.props);

        if (verbose) {
            std::cout << "  " << component_name << " extends " << db.inheritance[component_name]
                      << " with " << db.widget_attrs[component_name].size() << " props"
                      << std::endl;
        }
    }

    // 5. Ensure lv_obj has basic common attributes as fallback
    if (db.widget_attrs.count("lv_obj") == 0 || db.widget_attrs["lv_obj"].empty()) {
        db.widget_attrs["lv_obj"] = kCommonLvObjAttrs;
    }

    return db;
}

/**
 * @brief Print usage information.
 */
static void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options] [files...]\n"
              << "\n"
              << "Validates XML attributes against LVGL widget definitions.\n"
              << "\n"
              << "Options:\n"
              << "  --warn-only    Print warnings but exit 0\n"
              << "  --verbose      Show all files checked, not just errors\n"
              << "  -h, --help     Show this help message\n"
              << "\n"
              << "Arguments:\n"
              << "  files          XML files to validate (default: ui_xml/*.xml)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    bool warn_only = false;
    bool verbose = false;
    std::vector<std::string> files;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--warn-only") {
            warn_only = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        } else {
            files.push_back(arg);
        }
    }

    // Default to all XML files in ui_xml/
    if (files.empty()) {
        files = find_files("ui_xml/*.xml");
    }

    if (files.empty()) {
        std::cerr << "No XML files found to validate" << std::endl;
        return 1;
    }

    // Try to load from cache first
    xml_validator::WidgetDatabase db;
    std::time_t source_mtime = get_newest_source_mtime();

    if (!load_cache(db, source_mtime, verbose)) {
        // Cache miss - build from scratch
        if (verbose) {
            std::cout << "Building widget attribute database..." << std::endl;
        }
        db = build_widget_database(verbose);
        save_cache(db, source_mtime);
        if (verbose) {
            std::cout << "Saved database to cache" << std::endl;
        }
    }

    // Build inheritance tree to get complete attribute sets
    auto valid_attrs = xml_validator::build_inheritance_tree(db);

    if (verbose) {
        std::cout << "\nValidating " << files.size() << " XML files..." << std::endl;
    }

    // Validate each file
    std::vector<std::string> all_errors;
    for (const auto& file : files) {
        if (verbose) {
            std::cout << "Checking " << file << "..." << std::endl;
        }

        auto errors = validate_xml_file(file, valid_attrs, verbose);
        for (auto& err : errors) {
            all_errors.push_back(std::move(err));
        }
    }

    // Print errors
    for (const auto& error : all_errors) {
        std::cerr << error << std::endl;
    }

    // Summary
    if (!all_errors.empty()) {
        std::cerr << "Found " << all_errors.size() << " unknown attribute(s)" << std::endl;
    } else if (verbose) {
        std::cout << "All attributes valid" << std::endl;
    }

    // Exit code
    if (all_errors.empty() || warn_only) {
        return 0;
    }
    return 1;
}
