// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file device_identity.h
 * @brief The telemetry device identity file and the one rule for moving it.
 */

#include <string>

namespace helix::telemetry {

/// The identity's file name inside its directory (the state root).
inline constexpr const char* kDeviceIdentityFile = "telemetry_device.json";

/// Move a device identity left at @p from to @p to, once. The copy is written
/// and synced before @p from goes, so an interrupted move leaves two identical
/// files, which the next start settles. A readable identity already at @p to is
/// never replaced; @p from is then removed only when it is the same identity. An
/// unreadable one at @p to is set aside beside it, and a readable @p from takes
/// its place. @p to's directory must exist.
void move_device_identity(const std::string& from, const std::string& to);

} // namespace helix::telemetry
