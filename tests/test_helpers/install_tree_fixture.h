// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Install trees under a scratch probe root, for resolving "where is my install"
// from an executable path. Each tree holds bin/helix-screen, ui_xml/ and
// moonraker-plugin/install.sh, the parts the resolver and the plugin installer
// look for.

#include "unique_temp_dir.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace helix::test {

struct InstallTreeFixture {
    InstallTreeFixture() : root(unique_temp_dir("helix_install_tree")) {
        std::filesystem::create_directories(root);
    }
    ~InstallTreeFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    InstallTreeFixture(const InstallTreeFixture&) = delete;
    InstallTreeFixture& operator=(const InstallTreeFixture&) = delete;

    /// An install tree at <root>/@p rel; returns its bin/helix-screen.
    std::filesystem::path make_tree(const std::string& rel) const {
        const std::filesystem::path dir = std::filesystem::path(root) / rel;
        std::filesystem::create_directories(dir / "bin");
        std::filesystem::create_directories(dir / "ui_xml");
        std::filesystem::create_directories(dir / "moonraker-plugin");
        std::ofstream(dir / "bin/helix-screen") << "elf";
        std::ofstream(dir / "moonraker-plugin/install.sh") << "#!/bin/sh\nexit 0\n";
        return dir / "bin/helix-screen";
    }

    /// PAXX's layout: the package unpacked into oem/apps/helixscreen/@p version with
    /// `latest` naming it, and the lmd hook's bind mount of its binary over
    /// usr/bin/gui. A hard link gives usr/bin/gui the same device and inode the bind
    /// mount does. Returns the usr/bin/gui path the process sees as /proc/self/exe.
    std::filesystem::path make_paxx_bind_mount(const std::string& version) const {
        const std::filesystem::path binary = make_tree("oem/apps/helixscreen/" + version);
        std::filesystem::create_directory_symlink(version, std::filesystem::path(root) /
                                                               "oem/apps/helixscreen/latest");
        const std::filesystem::path gui = std::filesystem::path(root) / "usr/bin/gui";
        std::filesystem::create_directories(gui.parent_path());
        std::filesystem::create_hard_link(binary, gui);
        return gui;
    }

    std::string root;
};

} // namespace helix::test
