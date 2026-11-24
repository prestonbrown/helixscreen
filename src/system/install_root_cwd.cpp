// SPDX-License-Identifier: GPL-3.0-or-later

#include "install_root_cwd.h"

#include "board_identity.h"

#include <unistd.h>

namespace helix::board {

std::string chdir_to_install_root(const std::string& running_exe) {
    // A relative path would stop naming the binary once the cwd moves.
    if (running_exe.empty() || running_exe[0] != '/') {
        return {};
    }
    std::string root = resolve_install_root(running_exe);
    if (root.empty() || chdir(root.c_str()) != 0) {
        return {};
    }
    return root;
}

} // namespace helix::board
