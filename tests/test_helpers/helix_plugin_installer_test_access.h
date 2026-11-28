// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "helix_plugin_installer.h"
namespace helix {
// Reaches the installer's two process-wide prompt flags. In the running app
// only a new process clears them, so a test that stands for the app starting
// again clears them itself.
struct HelixPluginInstallerTestAccess {
    /// A new process: the install prompt may open again, and the next snooze
    /// check counts as the first of this start.
    static void reset_prompt_session() {
        HelixPluginInstaller::prompt_settled_for_session_ = false;
        HelixPluginInstaller::snooze_checked_this_start_ = false;
    }
};
} // namespace helix
