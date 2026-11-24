// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

/// Changing the working directory to the install tree. Kept out of board_identity.cpp,
/// which the ESP32 firmware compiles: ESP-IDF has no chdir().
namespace helix::board {

/// chdir() into the install tree (an absolute @p running_exe only) resolve_install_root(@p
/// running_exe) finds, so relative config/ and ui_xml/ paths resolve there. Returns the directory
/// entered; empty when no install tree is found or chdir fails (the working directory is then
/// unchanged). Callers that outlive an in-place update pass a path that is re-resolved on every
/// call (the launcher's binary path), not /proc/self/exe, which keeps naming the replaced tree's
/// moved inode.
std::string chdir_to_install_root(const std::string& running_exe);

} // namespace helix::board
