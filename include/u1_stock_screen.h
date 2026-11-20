// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file u1_stock_screen.h
 * @brief What the Snapmaker U1 platform hook hands the app about SSH and the
 *        stock screen on stock firmware.
 *
 * On stock U1 firmware HelixScreen disables the stock screen, which is also
 * what starts SSH at boot. The platform hook (hooks-snapmaker-u1.sh) takes
 * that job over and reports through the environment, which reaches
 * helix-screen from the launcher's platform_pre_start call. The decisions are
 * pure functions of those values so tests feed them directly.
 */
namespace helix::u1 {

/// Set to "1" by ensure_root_ssh when Root Access is on but dropbear did not
/// start.
inline constexpr const char* kRootSshDownEnv = "HELIX_U1_ROOT_SSH_DOWN";

/// True when @p flag (the value of HELIX_U1_ROOT_SSH_DOWN, may be null) says
/// SSH did not come up although Root Access is on.
bool root_ssh_down(const char* flag);

/// Read HELIX_U1_ROOT_SSH_DOWN and, when it is set, show a warning: the
/// user should learn SSH is gone before they need it. Called once at startup;
/// before the toast system is up the warning waits with the other boot
/// warnings.
void surface_root_ssh_down_from_hook();

} // namespace helix::u1
