// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_unit_shell.h"

#include "ams_types.h"

#include <cstring>

namespace helix::ui::tray {

namespace {

/// Which hardware a shell draws. A backend that reports only its firmware has no
/// row, because a drawing of the wrong unit is worse than the generic tray.
struct ShellRow {
    const char* backend_slug;
    const char* shell_id;
};

constexpr ShellRow kShellRows[] = {
    {"ace", "ace_pro"},
};

} // namespace

const UnitShell* unit_shell_for(const AmsSystemInfo& info, int unit_index) {
#ifdef ESP_PLATFORM
    // The firmware image carries no unit art and stays on logos.
    (void)info;
    (void)unit_index;
    return nullptr;
#else
    if (unit_index < 0 || unit_index >= static_cast<int>(info.units.size()))
        return nullptr;
    const char* slug = ams_type_telemetry_slug(info.type);
    for (const ShellRow& row : kShellRows) {
        if (std::strcmp(row.backend_slug, slug) == 0)
            return unit_shell_by_id(row.shell_id);
    }
    return nullptr;
#endif
}

} // namespace helix::ui::tray
