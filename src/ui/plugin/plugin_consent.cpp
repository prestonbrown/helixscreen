// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_consent.h"

#include "ui_modal.h"

#include <algorithm>

namespace helix::plugin {

namespace {

/// The approved wording per permission, already through lv_tr (returning the
/// literal itself is safe: it has static storage, and the translation pack's
/// copy outlives the call). The wording is approved as written; rewording it is
/// a product change, not a copy edit.
const char* permission_line(Permission p) {
    switch (p) {
    case Permission::Gcode:
        return lv_tr("Send any G-code command. This is full control of the printer: every "
                     "macro, including ones that run shell commands, is reachable.");
    case Permission::MoonrakerWrite:
        return lv_tr("Change printer settings through Moonraker, including uploading, "
                     "rewriting and deleting files in the printer's config folder.");
    case Permission::Http:
        return lv_tr("Connect to servers on your network and the internet.");
    case Permission::Storage:
        return lv_tr("Keep its own data on this screen (up to 256 KB).");
    }
    return "";
}

const char* menu_target_name(MenuKind k) {
    switch (k) {
    case MenuKind::PrintFile:
        return lv_tr("Print details");
    case MenuKind::FilamentSlot:
        return lv_tr("Filament slot menu");
    case MenuKind::Spool:
        return lv_tr("Spool menu");
    case MenuKind::Tile:
        return lv_tr("Its home tile");
    }
    return "";
}

} // namespace

std::string menu_targets_line(const Manifest& m) {
    std::string names;
    for (MenuKind k : kMenuKinds) {
        const bool declared = std::any_of(m.menu_items.begin(), m.menu_items.end(),
                                          [k](const MenuItemDecl& d) { return d.menu == k; });
        if (!declared)
            continue;
        if (!names.empty())
            names += ", ";
        names += menu_target_name(k);
    }
    return names.empty() ? names : std::string(lv_tr("Adds to:")) + " " + names;
}

std::vector<std::string> consent_lines(const PermissionSet& perms) {
    std::vector<std::string> lines;
    for (Permission p : perms) // PermissionSet iterates in enum order
        lines.push_back(permission_line(p));
    return lines;
}

std::string consent_message(const Manifest& m, const std::vector<Permission>& grown) {
    std::string msg = m.name + " " + m.version;
    if (!m.author.empty())
        msg += " " + std::string(lv_tr("by")) + " " + m.author;
    msg += "\n";

    if (!grown.empty()) {
        msg += std::string(lv_tr("This update asks for new permissions:")) + "\n";
        for (Permission p : grown)
            msg += std::string(permission_line(p)) + "\n";
    } else if (m.permissions.empty()) {
        msg += lv_tr("This plugin asks for no special permissions.");
    } else {
        for (const std::string& line : consent_lines(m.permissions))
            msg += line + "\n";
    }
    if (!msg.empty() && msg.back() == '\n')
        msg.pop_back();
    // Menu items need no permission of their own; the line says where they appear.
    if (const std::string targets = menu_targets_line(m); !targets.empty())
        msg += "\n" + targets;
    return msg;
}

void show_consent(const Manifest& m, const std::vector<Permission>& grown,
                  std::function<void()> on_yes) {
    // What the user is approving: the growth on an update, else the whole set.
    const PermissionSet approving =
        grown.empty() ? m.permissions : PermissionSet(grown.begin(), grown.end());
    const bool printer_control =
        approving.count(Permission::Gcode) || approving.count(Permission::MoonrakerWrite);
    const std::string msg = consent_message(m, grown);
    helix::ui::modal_confirm(lv_tr("Enable plugin?"), msg.c_str(),
                             printer_control ? ModalSeverity::Warning : ModalSeverity::Info,
                             lv_tr("Enable"), std::move(on_yes));
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
