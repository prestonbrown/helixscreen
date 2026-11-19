// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plugin_menu_kind.h"
#include "plugin_permissions.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hv/json.hpp" // JSON_HPP_OK: default_value held by value

namespace helix::plugin {

using json = nlohmann::json;

enum class SettingType { Bool, Int, Float, Enum, String, Action, Info };

constexpr int kMaxWidgetCells = 8;
constexpr size_t kMaxWidgetsPerPlugin = 32;

/// One entry of the manifest's `settings` array.
struct SettingDecl {
    std::string key;
    std::string label;
    SettingType type = SettingType::Bool;
    json default_value;               ///< null when the manifest gives none
    double min = 0;                   ///< Int and Float
    double max = 0;                   ///< Int and Float
    std::vector<std::string> options; ///< Enum
    std::string callback;             ///< Action: handler name, owned by the plugin
    std::string subject;              ///< Info: subject name, owned by the plugin
};

/// One entry of the manifest's `widgets` array. Spans are in grid tracks, half
/// a cell each; the manifest writes them in cells.
struct WidgetDecl {
    std::string id; ///< <plugin>__<name>
    std::string name;
    std::string icon; ///< app icon name or one of `icons`; empty when absent
    std::string description;
    std::string component; ///< <plugin>__<name>, a file in ui/
    int colspan = 2;
    int rowspan = 2;
    int max_colspan = 0; ///< 0: not resizable on this axis
    int max_rowspan = 0;
    bool half_cells = false; ///< resizes and may be authored in half cells
};

constexpr size_t kMaxIconsPerPlugin = 16;
constexpr const char* kDefaultPluginIcon = "puzzle_outline";

/// One entry of the manifest's `icons` map: a plugin-local PNG drawn as an icon.
struct IconDecl {
    std::string name;     ///< <plugin>__<name>
    std::string src;      ///< relative to the plugin folder, ends in ".png"
    std::string fallback; ///< app icon name drawn when the image is unavailable
};

constexpr int kMaxMenuItemsPerMenu = 4; ///< per plugin, per menu
constexpr int kMaxMenuItems = 16;       ///< per manifest
constexpr size_t kMaxMenuLabelBytes = 48;

/// One entry of the manifest's `menu_items` array. `handler` and `check` are local
/// `helix.ui.on` names; `check` and `icon` are empty when absent.
struct MenuItemDecl {
    std::string id;
    std::string label;
    std::string icon;
    std::string handler;
    std::string check;
    MenuKind menu = MenuKind::PrintFile;
};

struct Manifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string helix_version; ///< version constraint, empty when absent
    PermissionSet permissions;
    int memory_mb = 2;
    std::vector<SettingDecl> settings;
    std::string settings_overlay; ///< XML component name, empty when absent
    std::vector<IconDecl> icons;
    std::vector<WidgetDecl> widgets;
    std::vector<MenuItemDecl> menu_items;
};

/// `manifest` is set only when `errors` is empty.
struct ManifestParse {
    std::optional<Manifest> manifest;
    std::vector<std::string> errors;
};

ManifestParse parse_manifest(const std::string& text);

/// `^[a-z][a-z0-9-]{1,31}$`. No underscore, so the id cannot contain the
/// ownership separator.
bool is_valid_plugin_id(std::string_view id);

/// A name a plugin registers without its id prefix (`helix.ui.on`): 1 to 48 of
/// `[a-z0-9_-]`.
bool is_valid_local_name(std::string_view name);

/// Separator between a plugin id and the rest of a name that plugin owns.
/// Doubled because app names share the single-underscore space (`ams_*`,
/// `extruder_target`, `settings_*`) and no app name contains `__`: only the
/// double underscore proves the name is the plugin's.
constexpr std::string_view kPluginNameSeparator = "__";

/// The full form of a name a plugin owns: `<id>__<rest>`.
std::string plugin_owned_name(std::string_view id, std::string_view rest);

/// True for `<id>__<rest>` with a non-empty rest.
bool is_owned_name(std::string_view id, std::string_view name);

/// The owning plugin id of `<id>__<rest>`; empty when the name is not in that
/// form.
std::string_view owner_of(std::string_view name);

/// The `<rest>` of `<id>__<rest>`; empty when the name is not in that form. Header-only
/// so code built without the plugin runtime can split a name too.
constexpr std::string_view local_part_of(std::string_view name) {
    const size_t pos = name.find(kPluginNameSeparator);
    return pos == std::string_view::npos ? std::string_view{}
                                         : name.substr(pos + kPluginNameSeparator.size());
}

} // namespace helix::plugin
