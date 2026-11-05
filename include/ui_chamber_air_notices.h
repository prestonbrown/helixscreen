// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the screen says when a detachable chamber air accessory is fitted or
// removed: once per printer, a notice that it was found and where its
// controls are, then an offer to redo input shaping; and every time it comes off, a
// warning. The wording comes from the provider (chamber_air::accessory_copy).

namespace helix::ui {

enum class ChamberAirNotice {
    None,
    FirstFit, ///< the accessory was found and this printer has never been told
    Removed,  ///< the accessory came off while connected
};

/// Which notice a presence reading calls for. @p at_startup is the first reading after
/// connecting, which is never a removal. A first fit during a print waits: the caller
/// asks again with @p print_active false once the print ends.
[[nodiscard]] ChamberAirNotice decide_chamber_air_notice(bool present, bool at_startup,
                                                         bool first_fit_shown, bool print_active);

/// Hooks the notices to ChamberAirState's presence edges and the print-active state.
/// Main thread, once, after PrinterState's subjects exist.
void init_chamber_air_notices();

} // namespace helix::ui
