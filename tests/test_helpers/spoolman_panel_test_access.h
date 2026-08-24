// tests/test_helpers/spoolman_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_spoolman.h"

#include <vector>

// Test-only access to SpoolmanPanel's private spool-assignment path. Lets a
// consumer-mirroring test drive the ACTUAL set_active_spool() the context
// menu dispatches (never a mirror) and observe the stores it must have
// routed through AmsState::commit_external_spool_edit(): settings, the
// Spoolman server's active spool, and the identity cache.
class SpoolmanPanelTestAccess {
  public:
    static void set_active_spool(SpoolmanPanel& panel, int spool_id) {
        panel.set_active_spool(spool_id);
    }

    static void seed_cached_spools(SpoolmanPanel& panel, std::vector<SpoolInfo> spools) {
        panel.cached_spools_ = std::move(spools);
    }

    static int active_spool_id(const SpoolmanPanel& panel) {
        return panel.active_spool_id_;
    }
};
