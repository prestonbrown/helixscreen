// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_plugin_install_modal.h"

#include <functional>
#include <utility>

namespace helix {

// Test-only access to the install modal's Install button and to where it
// sends install.sh to run.
struct PluginInstallModalTestAccess {
    /// Install work goes to @p submitter instead of HttpExecutor::slow().
    static void set_install_submitter(PluginInstallModal& modal,
                                      std::function<void(std::function<void()>)> submitter) {
        modal.install_submitter_ = std::move(submitter);
    }

    static void click_install(PluginInstallModal& modal) {
        modal.on_install_clicked();
    }
};

} // namespace helix
