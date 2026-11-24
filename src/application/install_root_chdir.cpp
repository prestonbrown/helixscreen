// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "data_root_resolver.h"

#include <unistd.h>

namespace helix {

std::string chdir_to_install_root(const std::string& exe_path) {
    if (exe_path.empty() || exe_path[0] != '/') {
        return {};
    }
    std::string root = resolve_install_root(exe_path);
    if (root.empty() || chdir(root.c_str()) != 0) {
        return {};
    }
    return root;
}

} // namespace helix
