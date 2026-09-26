// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace helix::mock {

/// True when a HELIX_MOCK_PRINTER value names a mock HARDWARE persona: the
/// persona publishes the Klipper objects and status a real machine runs, and a
/// production backend is meant to drive them rather than a mock backend. The
/// CLI's --real-ams implication and the mock client's persona queries both
/// route through this rule so the two cannot drift. Exact and case-sensitive,
/// like the persona selection itself (MoonrakerManager matches each
/// HELIX_MOCK_PRINTER token verbatim), so a spelling the selector would not
/// recognise implies nothing here either.
[[nodiscard]] inline bool is_hardware_persona(std::string_view persona) {
    return persona == "creator5_zmod";
}

} // namespace helix::mock
