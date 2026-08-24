// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "upgrade_nudge.h"

namespace helix {

// Test-only seam. UpgradeNudge declares this class as a friend so the common
// visibility gate can be exercised directly. Must live in namespace helix to
// match that friend declaration.
//
// The two public queries are not substitutes: should_show_settings_badge()
// additionally requires Intensity >= Normal, and should_show_banner() requires
// Aggressive plus an undismissed version. Both ship as Off by default, so
// asserting the print-state policy through them would need config setup whose
// failure looks identical to the policy working.
class UpgradeNudgeTestAccess {
  public:
    static bool is_update_visible_now(const UpgradeNudge& n) {
        return n.is_update_visible_now();
    }
};

} // namespace helix
