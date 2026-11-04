// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Design-token discovery: expat passes over ui_xml/ that find the <color>, <px>
// and <string> constants, plus the validator for responsive constant sets.

#include "core/data_root_resolver.h"
#include "core/text_io.h"
#include "helix-xml/src/libs/expat/expat.h"
#include "theme_manager.h"
#include "theme_manager_internal.h"
#include "theme_token_table.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace helix::theme_detail {

const char* ui_xml_dir() {
    static const std::string dir = helix::asset_path("ui_xml");
    return dir.c_str();
}

} // namespace helix::theme_detail

// Helper: check if string ends with suffix
static bool ends_with_suffix(const char* str, const char* suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (str_len < suffix_len)
        return false;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

namespace {

/// One element carrying both a name= and a value= attribute, in document order.
struct XmlToken {
    std::string element;
    std::string name;
    std::string value;
};

using XmlTokens = std::vector<XmlToken>;

/// What stat() says about a file. Any field moving means the bytes may have.
struct FileStamp {
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    int64_t mtime_ns = 0;
    int64_t ctime_ns = 0;

    bool operator==(const FileStamp& o) const {
        return dev == o.dev && ino == o.ino && size == o.size && mtime_ns == o.mtime_ns &&
               ctime_ns == o.ctime_ns;
    }
};

struct CachedFile {
    FileStamp stamp;
    // False while the file was modified too recently for its stamp to prove
    // anything: a same-size rewrite inside one timestamp tick leaves every
    // field equal. Such an entry is re-parsed on every lookup.
    bool trusted = false;
    std::shared_ptr<const XmlTokens> tokens;
};

// How old a file's last change must be, when it is parsed, before its stamp is
// trusted to detect the next change. Above the coarsest common timestamp tick:
// FAT and exFAT record mtime in 2s steps.
constexpr int64_t kRacyWindowNs = 3'000'000'000;
std::atomic<int64_t> g_racy_window_ns{kRacyWindowNs};

#if !defined(HELIX_PLATFORM_ESP32)

int64_t to_ns(const struct timespec& ts) {
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

std::optional<FileStamp> stat_file(const char* filepath) {
    struct stat st {};
    if (::stat(filepath, &st) != 0)
        return std::nullopt;
    FileStamp stamp;
    stamp.dev = st.st_dev;
    stamp.ino = st.st_ino;
    stamp.size = st.st_size;
#if defined(__APPLE__)
    stamp.mtime_ns = to_ns(st.st_mtimespec);
    stamp.ctime_ns = to_ns(st.st_ctimespec);
#else
    stamp.mtime_ns = to_ns(st.st_mtim);
    stamp.ctime_ns = to_ns(st.st_ctim);
#endif
    return stamp;
}

int64_t realtime_now_ns() {
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return to_ns(ts);
}
#endif

std::atomic<int> g_token_file_parses{0};

void XMLCALL collect_token_start(void* user_data, const XML_Char* name, const XML_Char** attrs) {
    const char* elem_name = nullptr;
    const char* elem_value = nullptr;
    for (int i = 0; attrs[i]; i += 2) {
        if (strcmp(attrs[i], "name") == 0)
            elem_name = attrs[i + 1];
        else if (strcmp(attrs[i], "value") == 0)
            elem_value = attrs[i + 1];
    }
    if (elem_name && elem_value) {
        static_cast<XmlTokens*>(user_data)->push_back({name, elem_name, elem_value});
    }
}

/// Every name/value element in the file, or nullptr when it cannot be read.
/// A parse error keeps the elements before it.
std::shared_ptr<const XmlTokens> parse_file_tokens(const char* filepath) {
    std::optional<std::string> content = helix::text_io::read_file(filepath);
    if (!content)
        return nullptr;
    g_token_file_parses.fetch_add(1, std::memory_order_relaxed);

    auto tokens = std::make_shared<XmlTokens>();
    if (content->empty())
        return tokens;

    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) {
        spdlog::error("[Theme] Failed to create XML parser for {}", filepath);
        return tokens;
    }
    XML_SetUserData(parser, tokens.get());
    XML_SetElementHandler(parser, collect_token_start, nullptr);
    if (XML_Parse(parser, content->c_str(), static_cast<int>(content->size()), XML_TRUE) ==
        XML_STATUS_ERROR) {
        spdlog::trace("[Theme] XML parse error in {} line {}: {}", filepath,
                      XML_GetCurrentLineNumber(parser), XML_ErrorString(XML_GetErrorCode(parser)));
    }
    XML_ParserFree(parser);
    return tokens;
}

/// The file's tokens, parsed once and reused while the file is unchanged.
///
/// Discovery makes roughly 25 passes over ui_xml/ per theme init or layout
/// refresh, and every LVGL test case refreshes the layout. Reusing the parse
/// makes a pass over an unchanged tree one stat per file, while a live edit
/// still moves the stat and is re-read. ESP32 serves ui_xml from a read-only
/// image, so its entries are never revalidated.
std::shared_ptr<const XmlTokens> file_tokens(const char* filepath) {
    static std::mutex mutex;
    static std::unordered_map<std::string, CachedFile> cache;

#if defined(HELIX_PLATFORM_ESP32)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(filepath);
        if (it != cache.end())
            return it->second.tokens;
    }
    auto tokens = parse_file_tokens(filepath);
    if (tokens) {
        std::lock_guard<std::mutex> lock(mutex);
        cache[filepath] = CachedFile{{}, true, tokens};
    }
    return tokens;
#else
    const std::optional<FileStamp> stamp = stat_file(filepath);
    if (!stamp) {
        std::lock_guard<std::mutex> lock(mutex);
        cache.erase(filepath);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(filepath);
        if (it != cache.end() && it->second.trusted && it->second.stamp == *stamp)
            return it->second.tokens;
    }

    // Stamped before the read: a write that lands mid-read moves the stamp, so
    // the next lookup re-parses rather than keeping what this read saw.
    const int64_t now_ns = realtime_now_ns();
    auto tokens = parse_file_tokens(filepath);
    std::lock_guard<std::mutex> lock(mutex);
    if (!tokens) {
        cache.erase(filepath);
        return nullptr;
    }
    const int64_t last_change_ns = std::max(stamp->mtime_ns, stamp->ctime_ns);
    cache[filepath] = CachedFile{
        *stamp, now_ns - last_change_ns > g_racy_window_ns.load(std::memory_order_relaxed), tokens};
    return tokens;
#endif
}

} // namespace

int64_t helix::theme_detail::set_token_racy_window_ns(int64_t ns) {
    return g_racy_window_ns.exchange(ns, std::memory_order_relaxed);
}

int theme_manager_token_file_parse_count() {
    return g_token_file_parses.load(std::memory_order_relaxed);
}

void theme_manager_parse_xml_file_for_all(
    const char* filepath, const char* element_type,
    std::unordered_map<std::string, std::string>& token_values) {
    if (!filepath || !element_type)
        return;

    const auto tokens = file_tokens(filepath);
    if (!tokens)
        return;

    for (const XmlToken& token : *tokens) {
        if (token.element == element_type)
            token_values[token.name] = token.value;
    }
}

void theme_manager_parse_xml_file_for_suffix(
    const char* filepath, const char* element_type, const char* suffix,
    std::unordered_map<std::string, std::string>& token_values) {
    if (!filepath || !element_type || !suffix) {
        spdlog::trace("[Theme] parse_xml_file_for_suffix: NULL argument");
        return;
    }

    const auto tokens = file_tokens(filepath);
    if (!tokens) {
        spdlog::trace("[Theme] Could not open {} for suffix parsing", filepath);
        return;
    }

    for (const XmlToken& token : *tokens) {
        if (token.element != element_type || !ends_with_suffix(token.name.c_str(), suffix))
            continue;
        // Last-wins: a later element, or a later file, overwrites the base name.
        token_values[token.name.substr(0, token.name.size() - strlen(suffix))] = token.value;
    }
}

std::vector<std::string> theme_manager_find_xml_files(const char* directory, bool recursive) {
    std::vector<std::string> result;

    // Handle NULL directory gracefully
    if (!directory) {
        spdlog::trace("[Theme] find_xml_files: NULL directory");
        return result;
    }

    DIR* dir = opendir(directory);
    if (!dir) {
        spdlog::trace("[Theme] Could not open directory: {}", directory);
        return result;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;

        if (entry->d_type == DT_DIR) {
            if (recursive && filename != "." && filename != "..") {
                const std::string sub = std::string(directory) + "/" + filename;
                auto nested = theme_manager_find_xml_files(sub.c_str(), true);
                result.insert(result.end(), nested.begin(), nested.end());
            }
            continue;
        }

        // Skip suspicious filenames (path traversal defense)
        if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
            continue;
        }

        // Check if file ends with .xml (case-sensitive, lowercase only)
        if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".xml") {
            std::string full_path = std::string(directory) + "/" + filename;
            result.push_back(full_path);
        }
    }
    closedir(dir);

    // Sort alphabetically for deterministic ordering (needed for last-wins)
    std::sort(result.begin(), result.end());

    return result;
}

std::unordered_map<std::string, std::string>
theme_manager_parse_all_xml_for_element(const char* directory, const char* element_type) {
    // Build-time token table: skip the ~28-scan boot storm when the table is
    // enabled, carries this element type, and the caller wants the canonical
    // ui_xml dir (tests, alternate dirs and uncovered types always scan live).
    if (helix::theme_tokens::answers_from_table(helix::theme_tokens::enabled(), element_type,
                                                directory, helix::theme_detail::ui_xml_dir())) {
        return helix::theme_tokens::for_element(element_type);
    }
    std::unordered_map<std::string, std::string> token_values;
    std::vector<std::string> files = theme_manager_find_xml_files(directory);
    for (const auto& filepath : files) {
        theme_manager_parse_xml_file_for_all(filepath.c_str(), element_type, token_values);
    }
    return token_values;
}

std::unordered_map<std::string, std::string>
theme_manager_parse_all_xml_for_suffix(const char* directory, const char* element_type,
                                       const char* suffix) {
    // Build-time token table: same fast-path guard as _for_element above.
    if (helix::theme_tokens::answers_from_table(helix::theme_tokens::enabled(), element_type,
                                                directory, helix::theme_detail::ui_xml_dir())) {
        return helix::theme_tokens::for_suffix(element_type, suffix);
    }

    std::unordered_map<std::string, std::string> token_values;

    // Get sorted list of all XML files
    std::vector<std::string> files = theme_manager_find_xml_files(directory);

    // Parse each file in alphabetical order (last-wins via map overwrite)
    for (const auto& filepath : files) {
        theme_manager_parse_xml_file_for_suffix(filepath.c_str(), element_type, suffix,
                                                token_values);
    }

    return token_values;
}

std::vector<std::string> theme_manager_validate_constant_sets(const char* directory) {
    std::vector<std::string> warnings;

    if (!directory) {
        return warnings;
    }

    // Every layout directory (components/, portrait/, micro/, ...), read from the
    // files themselves: a set split across files there is as broken as one at
    // the top level.
    const std::vector<std::string> files = theme_manager_find_xml_files(directory, true);
    auto suffix_tokens = [&files](const char* element_type, const char* suffix) {
        std::unordered_map<std::string, std::string> tokens;
        for (const auto& filepath : files) {
            theme_manager_parse_xml_file_for_suffix(filepath.c_str(), element_type, suffix, tokens);
        }
        return tokens;
    };

    // Validate responsive px sets (_small/_medium/_large required, _tiny optional)
    {
        auto tiny_tokens = suffix_tokens("px", "_tiny");
        auto small_tokens = suffix_tokens("px", "_small");
        auto medium_tokens = suffix_tokens("px", "_medium");
        auto large_tokens = suffix_tokens("px", "_large");

        // Collect all base names that have at least one responsive suffix
        // _tiny is optional — only _small/_medium/_large are required for a complete set
        std::unordered_map<std::string, int> base_names;
        for (const auto& [name, _] : small_tokens) {
            base_names[name] |= 1; // bit 0 = _small
        }
        for (const auto& [name, _] : medium_tokens) {
            base_names[name] |= 2; // bit 1 = _medium
        }
        for (const auto& [name, _] : large_tokens) {
            base_names[name] |= 4; // bit 2 = _large
        }

        // border_radius_small is a fixed 4px token that only looks like a
        // responsive variant; plugin XML references it by name, so it stays.
        // Exempt only while it is the lone tier: a second tier makes it a set.
        auto border_radius = base_names.find("border_radius");
        if (border_radius != base_names.end() && border_radius->second == 1) {
            base_names.erase(border_radius);
        }

        // Check for incomplete sets (_small/_medium/_large must be complete)
        for (const auto& [base_name, flags] : base_names) {
            if (flags != 7) { // Not all three present (111 in binary)
                std::vector<std::string> found;
                std::vector<std::string> missing;

                if (flags & 1)
                    found.push_back("_small");
                else
                    missing.push_back("_small");

                if (flags & 2)
                    found.push_back("_medium");
                else
                    missing.push_back("_medium");

                if (flags & 4)
                    found.push_back("_large");
                else
                    missing.push_back("_large");

                std::string found_str;
                for (size_t i = 0; i < found.size(); ++i) {
                    if (i > 0)
                        found_str += ", ";
                    found_str += found[i];
                }

                std::string missing_str;
                for (size_t i = 0; i < missing.size(); ++i) {
                    if (i > 0)
                        missing_str += ", ";
                    missing_str += missing[i];
                }

                warnings.push_back("Incomplete responsive set for '" + base_name + "': found " +
                                   found_str + " but missing " + missing_str);
            }
        }

        // Warn about _tiny tokens without corresponding _small (likely a typo)
        for (const auto& [name, _] : tiny_tokens) {
            if (small_tokens.find(name) == small_tokens.end()) {
                warnings.push_back("Token '" + name +
                                   "' has _tiny but no _small (tiny falls back to small)");
            }
        }
    }

    // Validate themed color pairs (_light/_dark)
    {
        auto light_tokens = suffix_tokens("color", "_light");
        auto dark_tokens = suffix_tokens("color", "_dark");

        // Collect all base names that have at least one theme suffix
        std::unordered_map<std::string, int> base_names;
        for (const auto& [name, _] : light_tokens) {
            base_names[name] |= 1; // bit 0 = _light
        }
        for (const auto& [name, _] : dark_tokens) {
            base_names[name] |= 2; // bit 1 = _dark
        }

        // Check for incomplete pairs
        for (const auto& [base_name, flags] : base_names) {
            if (flags != 3) { // Not both present (11 in binary)
                if (flags == 1) {
                    warnings.push_back("Incomplete theme pair for '" + base_name +
                                       "': found _light but missing _dark");
                } else if (flags == 2) {
                    warnings.push_back("Incomplete theme pair for '" + base_name +
                                       "': found _dark but missing _light");
                }
            }
        }
    }

    return warnings;
}
