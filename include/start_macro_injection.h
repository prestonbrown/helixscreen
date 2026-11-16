// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

namespace helix {

/// True when the firmware runs its own print-start macro before it streams the
/// first line of the file. virtual_sdcard.file_position then stays at 0 for the
/// whole start sequence, purge included, and leaves 0 once the macro returns.
///
/// @param configfile_settings configfile.settings from Klipper
[[nodiscard]] bool firmware_runs_start_macro_before_file(const json& configfile_settings);

} // namespace helix
