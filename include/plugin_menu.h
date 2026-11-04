// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "plugin_menu_kind.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// The app menus' only view of plugin menu items. An app menu asks whether any plugin
/// declares items for it, resolves them against a plain JSON context when it opens, and
/// activates the entry the user taps. With no plugin host running (or plugins compiled
/// out) every call answers empty, false or nothing.
namespace helix::plugin_menu {

using MenuKind = helix::plugin::MenuKind;
using json = nlohmann::json;

/// One row a plugin adds to a menu, already resolved against the menu's context.
struct Entry {
    std::string plugin_id;
    std::string plugin_name;
    std::string item_id;
    std::string label;  ///< translated
    std::string icon;   ///< empty when the item names none
    std::string reason; ///< translated; why the row is disabled, empty otherwise
    bool enabled = true;
    uint64_t gen = 0; ///< the plugin's load generation; a stale entry does nothing on activate
};

/// True when a loaded plugin (only `plugin_id`, when given) declares an item for `kind`.
/// Runs no check.
bool any_declared(MenuKind kind, std::string_view plugin_id = {});

/// The visible items for `kind`, plugins in load order and items in manifest order, each
/// item's check run against `context`. At most row_cap(kind) entries; rows past it are
/// dropped. `only_plugin` limits the result to one plugin.
std::vector<Entry> resolve(MenuKind kind, const json& context, std::string_view only_plugin = {});

/// Runs the entry's handler with `context`. Does nothing when its plugin has since been
/// unloaded, faulted or reloaded.
void activate(const Entry& e, const json& context);

/// The most plugin rows one opening of `kind` shows, across every plugin.
int row_cap(MenuKind kind);

} // namespace helix::plugin_menu
