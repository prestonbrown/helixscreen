// SPDX-License-Identifier: GPL-3.0-or-later
// Ground truth: U1 stock fw print_stats.exception={...,code:2(noodle),...} + state=paused
// on a spaghetti fire; code 2 unique vs runout(0). See defect_detection in
// docs/devel/printers/SNAPMAKER_U1_SUPPORT.md.
#include "u1_stock_detection_source.h"

#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix::detection {

void U1StockSource::start() {
    if (!state_)
        return;
    // Deferred (observe_int_sync) rather than immediate: PrinterPrintState sets the
    // print-state-enum subject (line ~284) BEFORE it parses print_stats.exception
    // (line ~343) within a single update_from_status() frame. A synchronous observer
    // would read a stale exception code on the paused edge. Deferring via the
    // UpdateQueue runs on_print_state() after the whole frame is parsed, so the
    // exception is latched. The callback fires on the main thread (queue drain).
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - U1 stock firmware raises
    // defect detection by pausing, so the edge is the printer's own.
    state_observer_ = helix::ui::observe_int_sync<U1StockSource>(
        state_->get_print_state_enum_subject(), this,
        [](U1StockSource* self, int value) { self->on_print_state(value); });
}

void U1StockSource::on_print_state(int state_enum) {
    const int prev = last_state_;
    last_state_ = state_enum;

    // Gate on confirmed capability: only U1 stock firmware exposes defect_detection.
    // capable_ is set by DetectionManager's post-connect probe, so this stays false
    // (and detection never fires) on non-U1 printers.
    if (!capable_)
        return;
    // RAW_PRINT_STATE_OK: an edge INTO the printer's own paused state, which is
    // what U1 stock firmware raises when it trips defect detection.
    if (state_enum != static_cast<int>(PrintJobState::PAUSED))
        return;
    if (prev == static_cast<int>(PrintJobState::PAUSED))
        return;
    if (!cb_)
        return;

    const int code = state_->get_print_exception_code();
    if (kind_from_u1_code(code) != DetectionKind::Spaghetti)
        return;

    DetectionEvent e;
    e.source_id = id();
    e.kind = DetectionKind::Spaghetti;
    e.attributable = true;
    e.already_paused = true;
    e.message = state_->get_print_exception_message();
    spdlog::info("[U1StockSource] spaghetti detected (code 2): {}", e.message);
    cb_(e);
}

} // namespace helix::detection
