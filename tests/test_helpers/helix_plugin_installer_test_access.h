// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_plugin_installer.h"

#include <string>

namespace helix {

// Test-only access to where HelixPluginInstaller looks for install.sh and how
// long it lets the script run. A test that reaches the script must point it at
// a stub: the real moonraker-plugin/install.sh edits moonraker.conf.
struct HelixPluginInstallerTestAccess {
    static void set_script_search_root(HelixPluginInstaller& installer, std::string root) {
        installer.script_search_root_ = std::move(root);
    }

    static void set_script_timeout_seconds(HelixPluginInstaller& installer, int seconds) {
        installer.script_timeout_seconds_ = seconds;
    }

    /// A new session: the install prompt may open again.
    static void reset_prompt_session() {
        HelixPluginInstaller::prompt_settled_for_session_ = false;
        HelixPluginInstaller::snooze_checked_this_start_ = false;
    }
};

} // namespace helix
