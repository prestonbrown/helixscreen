// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_plugin_installer.h"
#include "helix_plugin_installer_test_access.h"
#include "unique_temp_dir.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace helix::test {

/// Confines an installer's install.sh lookup to an empty temp dir and points it
/// at a local Moonraker, so nothing a test does can reach the repository's
/// moonraker-plugin/install.sh, which edits moonraker.conf. write_script() puts
/// a stub there that records its arguments and then runs the given body.
struct PluginScriptSandbox {
    explicit PluginScriptSandbox(HelixPluginInstaller& installer)
        : root(unique_temp_dir("helix_plugin_installer")), installer_(installer) {
        std::filesystem::create_directories(root + "/moonraker-plugin");
        HelixPluginInstallerTestAccess::set_script_search_root(installer, root);
        installer.set_websocket_url("ws://localhost:7125/websocket");
    }

    ~PluginScriptSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    PluginScriptSandbox(const PluginScriptSandbox&) = delete;
    PluginScriptSandbox& operator=(const PluginScriptSandbox&) = delete;

    std::string script_path() const {
        return root + "/moonraker-plugin/install.sh";
    }

    void write_script(const std::string& body) const {
        std::ofstream(script_path()) << "#!/bin/sh\n"
                                     << "printf '%s ' \"$@\" > '" << root << "/args'\n"
                                     << body << "\n";
    }

    /// A stub that outlives the installer's script timeout, cut to one second.
    void write_script_past_timeout() const {
        HelixPluginInstallerTestAccess::set_script_timeout_seconds(installer_, 1);
        write_script("exec sleep 30");
    }

    /// The arguments the stub was run with, space-separated; empty if it never ran.
    std::string script_args() const {
        std::ifstream in(root + "/args");
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::string root;

  private:
    HelixPluginInstaller& installer_;
};

} // namespace helix::test
