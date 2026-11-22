// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file printer_pending_restore.h
 * @brief A setting the app changed on the printer and still owes back, kept in
 *        the active printer's settings so a restart or a lost link cannot
 *        leave it changed.
 *
 * Write the restore before sending the change, clear it once the setting is
 * back. Each feature owns one key.
 */

#include <string>

namespace helix::pending_restore {

/// The restore stored under @p key for the active printer; empty when none. Main thread.
[[nodiscard]] std::string read(const char* key);

/// Store @p value under @p key for the active printer (empty clears it). Main thread.
void write(const char* key, const std::string& value);

} // namespace helix::pending_restore
