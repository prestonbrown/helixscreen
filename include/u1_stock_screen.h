// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

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

/// The restore-stock trigger file's path. Exported by the hook only on stock
/// firmware; the hook acts on the file at the next HelixScreen start.
inline constexpr const char* kRestoreTriggerEnv = "HELIX_U1_RESTORE_TRIGGER";

/// True when Settings offers Restore Stock Screen: the hook named a trigger
/// path (@p trigger_path, the value of HELIX_U1_RESTORE_TRIGGER, may be null).
bool restore_stock_screen_offered(const char* trigger_path);

/// What tapping Restore Stock Screen does.
enum class RestoreStockVerdict {
    NotOffered,   ///< No trigger path: not a stock-firmware U1
    StateUnknown, ///< Whether a job holds the machine is not known yet
    JobActive,    ///< A job holds the machine; the restore reboots, so refuse
    Proceed,
};

/// Decide a Restore Stock Screen request from the trigger path (may be null),
/// power_job_state_known() and power_job_active(). The restore reboots, so it
/// fails closed: an unknown job state refuses like an active job. Pure.
RestoreStockVerdict decide_restore_stock(const char* trigger_path, bool job_state_known,
                                         bool job_active);

/// Create the trigger file at @p path. False when it could not be written, in
/// which case nothing will restore the stock screen.
bool write_restore_trigger(const std::string& path);

} // namespace helix::u1
