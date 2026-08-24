// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme_loader.h"

#include "border_radius_sizes.h"
#include "data_root_resolver.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

#include "hv/json.hpp"

namespace helix {

// ============================================================================
// ModePalette implementation (new dual-palette system)
// ============================================================================

const std::array<const char*, 16>& ModePalette::color_names() {
    static const std::array<const char*, 16> names = {
        "screen_bg",  "overlay_bg",  "card_bg", "elevated_bg", "border",   "text",
        "text_muted", "text_subtle", "primary", "secondary",   "tertiary", "info",
        "success",    "warning",     "danger",  "focus"};
    return names;
}

const std::string& ModePalette::at(size_t index) const {
    switch (index) {
    case 0:
        return screen_bg;
    case 1:
        return overlay_bg;
    case 2:
        return card_bg;
    case 3:
        return elevated_bg;
    case 4:
        return border;
    case 5:
        return text;
    case 6:
        return text_muted;
    case 7:
        return text_subtle;
    case 8:
        return primary;
    case 9:
        return secondary;
    case 10:
        return tertiary;
    case 11:
        return info;
    case 12:
        return success;
    case 13:
        return warning;
    case 14:
        return danger;
    case 15:
        return focus;
    default:
        throw std::out_of_range("ModePalette index out of range");
    }
}

std::string& ModePalette::at(size_t index) {
    return const_cast<std::string&>(static_cast<const ModePalette*>(this)->at(index));
}

bool ModePalette::is_valid() const {
    for (size_t i = 0; i < 16; ++i) {
        const auto& color = at(i);
        if (color.empty() || color[0] != '#' || color.size() != 7) {
            return false;
        }
    }
    return true;
}

bool ThemeData::is_valid() const {
    if (name.empty()) {
        return false;
    }
    // At least one of dark/light must be valid
    return dark.is_valid() || light.is_valid();
}

bool ThemeData::supports_dark() const {
    return dark.is_valid();
}

bool ThemeData::supports_light() const {
    return light.is_valid();
}

ThemeModeSupport ThemeData::get_mode_support() const {
    bool has_dark = dark.is_valid();
    bool has_light = light.is_valid();

    if (has_dark && has_light) {
        return ThemeModeSupport::DUAL_MODE;
    } else if (has_dark) {
        return ThemeModeSupport::DARK_ONLY;
    } else {
        return ThemeModeSupport::LIGHT_ONLY;
    }
}

ThemeData get_default_nord_theme() {
    ThemeData theme;
    theme.name = "Nord";
    theme.filename = "nord";

    // NEW: Populate dual palette system (dark mode)
    theme.dark.screen_bg = "#2e3440";
    theme.dark.overlay_bg = "#3b4252";
    theme.dark.card_bg = "#434c5e";
    theme.dark.elevated_bg = "#4c566a";
    theme.dark.border = "#616e88";
    theme.dark.text = "#eceff4";
    theme.dark.text_muted = "#d8dee9";
    theme.dark.text_subtle = "#b8c2d1";
    theme.dark.primary = "#88c0d0";   // nord8 - frost cyan
    theme.dark.secondary = "#81a1c1"; // nord9 - frost blue
    theme.dark.tertiary = "#5e81ac";  // nord10 - frost dark blue
    theme.dark.info = "#b48ead";      // nord15 - aurora purple
    theme.dark.success = "#a3be8c";   // nord14 - aurora green
    theme.dark.warning = "#ebcb8b";   // nord13 - aurora yellow
    theme.dark.danger = "#bf616a";    // nord11 - aurora red
    theme.dark.focus = "#8fbcbb";     // nord7 - frost teal

    // NEW: Populate dual palette system (light mode)
    theme.light.screen_bg = "#eceff4";
    theme.light.overlay_bg = "#e5e9f0";
    theme.light.card_bg = "#ffffff";
    theme.light.elevated_bg = "#edeff6";
    theme.light.border = "#cbd5e1";
    theme.light.text = "#2e3440";
    theme.light.text_muted = "#3b4252";
    theme.light.text_subtle = "#64748b";
    theme.light.primary = "#5e81ac";   // nord10 - darker frost for light bg
    theme.light.secondary = "#81a1c1"; // nord9 - frost blue
    theme.light.tertiary = "#4c566a";  // nord3 - polar night for contrast
    theme.light.info = "#b48ead";      // nord15 - aurora purple
    theme.light.success = "#3fa47d";   // adjusted green for light bg
    theme.light.warning = "#b08900";   // adjusted yellow for light bg
    theme.light.danger = "#b23a48";    // adjusted red for light bg
    theme.light.focus = "#8fbcbb";     // nord7 - frost teal

    theme.properties.border_radius_size = 3; // "Soft"
    theme.properties.border_width = 1;
    theme.properties.border_opacity = 40;
    theme.properties.shadow_intensity = 0;
    theme.properties.shadow_opa = 0;
    theme.properties.shadow_offset_y = 2;

    return theme;
}

/**
 * @brief Helper to parse a ModePalette from a JSON object
 *
 * A key that is missing OR present-but-empty falls back to the corresponding
 * color from the built-in default theme, so no palette field is ever left empty.
 * Empty fields used to flow straight into the color parser, flooding the log with
 * "[Theme] Invalid hex color string:" (one per widget, per redraw) and rendering
 * those widgets black (prestonbrown/helixscreen#989).
 */
static void parse_mode_palette(const nlohmann::json& palette_json, ModePalette& palette,
                               const std::string& filename, const std::string& mode_name) {
    auto& names = ModePalette::color_names();
    const ThemeData defaults = get_default_nord_theme();
    const ModePalette& default_palette = (mode_name == "light") ? defaults.light : defaults.dark;

    // A palette key present as null (or any non-object) resolves to the same
    // all-defaults result via the contains() checks below, but doing it here
    // costs one log line instead of sixteen.
    if (!palette_json.is_object()) {
        spdlog::warn("[ThemeLoader] '{}' palette in {} is not an object, using defaults", mode_name,
                     filename);
        for (size_t i = 0; i < 16; ++i) {
            palette.at(i) = default_palette.at(i);
        }
        return;
    }

    for (size_t i = 0; i < 16; ++i) {
        const char* name = names[i];
        std::string value;
        if (palette_json.contains(name) && palette_json[name].is_string()) {
            value = palette_json[name].get<std::string>();
        }
        if (value.empty()) {
            spdlog::warn("[ThemeLoader] Missing/empty '{}' in {}.{}, using default", name, filename,
                         mode_name);
            value = default_palette.at(i);
        }
        palette.at(i) = value;
    }
}

ThemeData parse_theme_json(const std::string& json_str, const std::string& filename) {
    ThemeData theme;
    theme.filename = filename;

    // Remove .json extension if present
    if (theme.filename.size() > 5 && theme.filename.substr(theme.filename.size() - 5) == ".json") {
        theme.filename = theme.filename.substr(0, theme.filename.size() - 5);
    }

    // The try covers the parse and nothing else, deliberately. Syntactically
    // broken JSON is the one failure where "this file is not a theme" is true
    // and falling back to built-in Nord is right. Every read below degrades on
    // its own instead: they go through the safe_* helpers, because .value()
    // throws type_error.302 on a key that is PRESENT with a null value (a
    // missing key is fine) and type_error.306 when the receiver is not an
    // object. Under the old wide try, one `"shadow_intensity": null` threw into
    // the catch and replaced the user's ENTIRE theme — both palettes, every
    // property — with built-in Nord. A bad property is now worth exactly that
    // property.
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("[ThemeLoader] Failed to parse {}: {}", filename, e.what());
        return get_default_nord_theme();
    }

    if (!json.is_object()) {
        spdlog::error("[ThemeLoader] {} is not a JSON object", filename);
        return get_default_nord_theme();
    }

    theme.name = helix::json_util::safe_string(json, "name", "Unnamed Theme");

    // Theme must have "dark" and/or "light" palette objects
    bool has_dark = json.contains("dark");
    bool has_light = json.contains("light");

    if (!has_dark && !has_light) {
        spdlog::error("[ThemeLoader] No 'dark' or 'light' palette in {}", filename);
        return get_default_nord_theme();
    }

    spdlog::trace("[ThemeLoader] Parsing {} with dark={}, light={}", filename, has_dark, has_light);

    if (has_dark) {
        parse_mode_palette(json["dark"], theme.dark, filename, "dark");
    }
    if (has_light) {
        parse_mode_palette(json["light"], theme.light, filename, "light");
    }

    // Parse properties with defaults
    // New format: border_radius_size (0-7 index)
    // Old format: border_radius (raw pixels) — auto-migrate
    //
    // Note the migration branch keys off contains(), so a null border_radius
    // _size takes the new-format branch and safe_int yields 3 ("Soft"), the
    // same value the old default argument carried. It does NOT fall through to
    // the legacy border_radius migration, which is correct: the key is present,
    // so this is a new-format theme with one unreadable field.
    if (json.contains("border_radius_size")) {
        theme.properties.border_radius_size =
            std::clamp(helix::json_util::safe_int(json, "border_radius_size", 3), 0,
                       helix::BorderRadiusSizes::count() - 1);
    } else {
        int raw = helix::json_util::safe_int(json, "border_radius", 12);
        theme.properties.border_radius_size = helix::BorderRadiusSizes::nearest_size_index(raw);
        spdlog::info("[ThemeLoader] Migrated border_radius={} -> border_radius_size={} ({})", raw,
                     theme.properties.border_radius_size,
                     helix::BorderRadiusSizes::name(theme.properties.border_radius_size));
    }
    theme.properties.border_width = helix::json_util::safe_int(json, "border_width", 1);
    theme.properties.border_opacity = helix::json_util::safe_int(json, "border_opacity", 40);
    theme.properties.shadow_intensity = helix::json_util::safe_int(json, "shadow_intensity", 0);
    theme.properties.shadow_opa = helix::json_util::safe_int(json, "shadow_opa", 0);
    theme.properties.shadow_offset_y = helix::json_util::safe_int(json, "shadow_offset_y", 2);
    theme.properties.handle_style = helix::json_util::safe_string(json, "handle_style", "round");
    theme.properties.handle_color = helix::json_util::safe_string(json, "handle_color", "primary");

    return theme;
}

ThemeData load_theme_from_file(const std::string& filepath_or_name) {
    std::string filepath = filepath_or_name;
    std::string filename;

    // Check if this is a full path or just a theme name
    if (filepath_or_name.find('/') == std::string::npos) {
        // Just a theme name - look up in directories
        std::string themes_dir = get_themes_directory();
        std::string defaults_dir = get_default_themes_directory();

        // Add .json extension if not present
        std::string name_with_ext = filepath_or_name;
        if (name_with_ext.size() < 5 || name_with_ext.substr(name_with_ext.size() - 5) != ".json") {
            name_with_ext += ".json";
        }

        // Check user themes directory first
        std::string user_path = themes_dir + "/" + name_with_ext;
        struct stat st;
        if (stat(user_path.c_str(), &st) == 0) {
            filepath = user_path;
            spdlog::debug("[ThemeLoader] Loading user theme from {}", filepath);
        } else {
            // Fall back to defaults directory
            std::string defaults_path = defaults_dir + "/" + name_with_ext;
            if (stat(defaults_path.c_str(), &st) == 0) {
                filepath = defaults_path;
                spdlog::debug("[ThemeLoader] Loading default theme from {}", filepath);
            } else {
                spdlog::error("[ThemeLoader] Theme '{}' not found in themes or defaults",
                              filepath_or_name);
                return {};
            }
        }
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
        spdlog::error("[ThemeLoader] Failed to open {}", filepath);
        return {};
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    // Extract filename from path
    filename = filepath;
    size_t slash = filepath.rfind('/');
    if (slash != std::string::npos) {
        filename = filepath.substr(slash + 1);
    }

    return parse_theme_json(buffer.str(), filename);
}

/**
 * @brief Helper to serialize a ModePalette to JSON
 */
static nlohmann::json serialize_mode_palette(const ModePalette& palette) {
    nlohmann::json result;
    auto& names = ModePalette::color_names();
    for (size_t i = 0; i < 16; ++i) {
        result[names[i]] = palette.at(i);
    }
    return result;
}

bool save_theme_to_file(const ThemeData& theme, const std::string& filepath) {
    nlohmann::json json;

    json["name"] = theme.name;

    // Save dark and/or light palettes
    if (theme.dark.is_valid()) {
        json["dark"] = serialize_mode_palette(theme.dark);
    }
    if (theme.light.is_valid()) {
        json["light"] = serialize_mode_palette(theme.light);
    }

    // Properties
    json["border_radius_size"] = theme.properties.border_radius_size;
    json["border_width"] = theme.properties.border_width;
    json["border_opacity"] = theme.properties.border_opacity;
    json["shadow_intensity"] = theme.properties.shadow_intensity;
    json["shadow_opa"] = theme.properties.shadow_opa;
    json["shadow_offset_y"] = theme.properties.shadow_offset_y;
    json["handle_style"] = theme.properties.handle_style;
    json["handle_color"] = theme.properties.handle_color;

    // Ensure parent directory exists — writable config dir may not yet have
    // a themes/ subdir on a fresh HELIX_CONFIG_DIR baseline.
    {
        std::error_code ec;
        std::filesystem::path p(filepath);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }

    // Write with pretty formatting
    std::ofstream file(filepath);
    if (!file.is_open()) {
        spdlog::error("[ThemeLoader] Failed to write {}", filepath);
        return false;
    }

    file << json.dump(2);
    return true;
}

std::string get_themes_directory() {
    return writable_path("themes");
}

std::string get_default_themes_directory() {
#if defined(HELIX_PLATFORM_ESP32)
    // Firmware: read-only theme defaults ship in the packed /assets container.
    // The staged tree keeps the SAME layout as desktop's data dir — config lives
    // under assets/config/ — and asset_root is the "/assets" mount, so the full
    // path is /assets/assets/config/themes/defaults (the doubled "assets" is
    // mount-point + staged dir, NOT a typo). The relpath here must match desktop's
    // "assets/config/themes/defaults"; dropping the leading "assets/" points one
    // level short (/assets/config/...), finds no JSON, and silently falls back to
    // the compiled-in Nord palette.
    return asset_path("assets/config/themes/defaults");
#else
    return get_data_dir() + "/assets/config/themes/defaults";
#endif
}

bool has_default_theme(const std::string& filename) {
    std::string defaults_dir = get_default_themes_directory();

    // Add .json extension if not present
    std::string name_with_ext = filename;
    if (name_with_ext.size() < 5 || name_with_ext.substr(name_with_ext.size() - 5) != ".json") {
        name_with_ext += ".json";
    }

    std::string defaults_path = defaults_dir + "/" + name_with_ext;
    struct stat st;
    return stat(defaults_path.c_str(), &st) == 0;
}

std::optional<ThemeData> reset_theme_to_default(const std::string& filename) {
    // Check if a default exists
    if (!has_default_theme(filename)) {
        spdlog::debug("[ThemeLoader] No default theme for '{}', cannot reset", filename);
        return std::nullopt;
    }

    std::string themes_dir = get_themes_directory();
    std::string defaults_dir = get_default_themes_directory();

    // Add .json extension if not present
    std::string name_with_ext = filename;
    if (name_with_ext.size() < 5 || name_with_ext.substr(name_with_ext.size() - 5) != ".json") {
        name_with_ext += ".json";
    }

    // Delete user override if it exists
    std::string user_path = themes_dir + "/" + name_with_ext;
    struct stat st;
    if (stat(user_path.c_str(), &st) == 0) {
        if (std::remove(user_path.c_str()) != 0) {
            spdlog::error("[ThemeLoader] Failed to delete user theme override: {}", user_path);
            return std::nullopt;
        }
        spdlog::info("[ThemeLoader] Deleted user theme override: {}", user_path);
    }

    // Load and return the default theme
    std::string defaults_path = defaults_dir + "/" + name_with_ext;
    return load_theme_from_file(defaults_path);
}

bool ensure_themes_directory(const std::string& themes_dir) {
    struct stat st;

    // First ensure parent config directory exists. Use the writable config dir
    // (honors $HELIX_CONFIG_DIR — e.g. /config on ESP32) rather than a literal
    // relative "config", which fails when the CWD isn't the config root.
    std::string config_dir = get_user_config_dir();
    if (stat(config_dir.c_str(), &st) != 0) {
        if (mkdir(config_dir.c_str(), 0750) != 0) {
            spdlog::error("[ThemeLoader] Failed to create config directory {}: {}", config_dir,
                          strerror(errno));
            return false;
        }
        spdlog::info("[ThemeLoader] Created config directory: {}", config_dir);
    }

    // Then create themes directory if it doesn't exist
    if (stat(themes_dir.c_str(), &st) != 0) {
        if (mkdir(themes_dir.c_str(), 0750) != 0) {
            spdlog::error("[ThemeLoader] Failed to create themes directory {}: {}", themes_dir,
                          strerror(errno));
            return false;
        }
        spdlog::info("[ThemeLoader] Created themes directory: {}", themes_dir);
    }

    // Check if nord.json exists, create if missing
    std::string nord_path = themes_dir + "/nord.json";
    if (stat(nord_path.c_str(), &st) != 0) {
        auto nord = get_default_nord_theme();
        if (!save_theme_to_file(nord, nord_path)) {
            spdlog::error("[ThemeLoader] Failed to create default nord.json");
            return false;
        }
        spdlog::info("[ThemeLoader] Created default theme: {}", nord_path);
    }

    return true;
}

std::vector<ThemeInfo> discover_themes(const std::string& themes_dir) {
    std::vector<ThemeInfo> themes;
    std::set<std::string> seen_filenames;

    // Helper lambda to scan a directory
    auto scan_directory = [&](const std::string& dir_path, bool is_defaults) {
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) {
            if (!is_defaults) {
                // User themes dir not existing is fine
                spdlog::debug("[ThemeLoader] Themes directory doesn't exist yet: {}", dir_path);
            }
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;

            // Skip non-json files
            if (filename.size() <= 5 || filename.substr(filename.size() - 5) != ".json") {
                continue;
            }

            // Skip hidden files
            if (filename[0] == '.') {
                continue;
            }

            std::string base_name = filename.substr(0, filename.size() - 5); // Remove .json

            // Skip if we've already seen this theme (user overrides take precedence)
            if (seen_filenames.count(base_name) > 0) {
                continue;
            }

            std::string filepath = dir_path + "/" + filename;
            auto theme = load_theme_from_file(filepath);

            if (theme.is_valid()) {
                ThemeInfo info;
                info.filename = base_name;
                info.display_name = theme.name;
                themes.push_back(info);
                seen_filenames.insert(base_name);
            }
        }

        closedir(dir);
    };

    // First scan user themes directory (takes precedence)
    scan_directory(themes_dir, false);

    // Then scan defaults directory
    std::string defaults_dir = get_default_themes_directory();
    scan_directory(defaults_dir, true);

    // Sort alphabetically by display name
    std::sort(themes.begin(), themes.end(), [](const ThemeInfo& a, const ThemeInfo& b) {
        return a.display_name < b.display_name;
    });

    spdlog::debug("[ThemeLoader] Discovered {} themes (user + defaults)", themes.size());
    return themes;
}

} // namespace helix
