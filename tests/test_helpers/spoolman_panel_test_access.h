// tests/test_helpers/spoolman_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_spoolman.h"

#include <cstdint>
#include <cstring>
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

    static void archive_spool(SpoolmanPanel& panel, int spool_id) {
        panel.archive_spool(spool_id);
    }

    static void set_active_spool_id(SpoolmanPanel& panel, int spool_id) {
        panel.active_spool_id_ = spool_id;
    }

    /// Opens the context menu the way a tap on a spool row does: the row's
    /// user_data carries the spool id.
    static void open_context_menu(SpoolmanPanel& panel, lv_obj_t* row) {
        panel.handle_spool_clicked(row, lv_point_t{0, 0});
    }

    static void hide_context_menu(SpoolmanPanel& panel) {
        panel.context_menu_.hide();
    }

    static int active_spool_id(const SpoolmanPanel& panel) {
        return panel.active_spool_id_;
    }

    /// Drives the private refresh the queued archive/delete/duplicate
    /// callbacks refetch through (prestonbrown/helixscreen#1402).
    static void refresh_spools(SpoolmanPanel& panel) {
        panel.refresh_spools();
    }

    /// Overwrites the panel-state subject with the recycled-heap shape the
    /// #1402 core dump showed: an uninitialized lv_subject_t whose bytes
    /// happen to pass the INT type check while its observer-list head is a
    /// garbage pointer. This is what a lazily-resurrected shell panel (the
    /// global getter constructed it after StaticPanelRegistry::destroy_all()
    /// freed the previous one) carries when a queued callback drives it
    /// before init_subjects().
    static void poison_state_subject_as_uninitialized(SpoolmanPanel& panel) {
        lv_subject_t& s = panel.panel_state_subject_;
        std::memset(&s, 0xA5, sizeof(s));
        s.type = LV_SUBJECT_TYPE_INT;
        // Non-degenerate bounds so the write passes lv_subject_set_int's
        // clamp and reaches notify (a uniform 0xA5 fill collapses min=max
        // and notify_if_changed skips the walk).
        s.min_value.num = INT32_MIN;
        s.max_value.num = INT32_MAX;
        s.subs_ll.head = reinterpret_cast<lv_ll_node_t*>(0x1);
        s.subs_ll.tail = reinterpret_cast<lv_ll_node_t*>(0x1);
    }

    static const lv_subject_t& state_subject(const SpoolmanPanel& panel) {
        return panel.panel_state_subject_;
    }
};
