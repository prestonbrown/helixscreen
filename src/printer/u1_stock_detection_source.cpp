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
    // Both deferred (observe_int_sync): the state and the exception code are
    // parsed in one update_from_status() frame, and the firmware may also send
    // them in separate frames in either order. Re-evaluating on either edge,
    // after the whole frame is parsed, sees the pair however it arrives.
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - U1 stock firmware raises
    // defect detection by pausing, so the edge is the printer's own.
    state_observer_ = helix::ui::observe_int_sync<U1StockSource>(
        state_->get_print_state_enum_subject(), this,
        [](U1StockSource* self, int /*state*/) { self->evaluate(); });
    exception_observer_ = helix::ui::observe_int_sync<U1StockSource>(
        state_->get_print_exception_subject(), this,
        [](U1StockSource* self, int /*code*/) { self->evaluate(); });
}

void U1StockSource::evaluate() {
    // RAW_PRINT_STATE_OK: the printer's own paused state, which is what U1
    // stock firmware raises when it trips defect detection.
    const bool paused = lv_subject_get_int(state_->get_print_state_enum_subject()) ==
                        static_cast<int>(PrintJobState::PAUSED);
    if (!paused) {
        fired_this_pause_ = false;
        return;
    }
    // Gate on confirmed capability: only U1 stock firmware exposes defect_detection.
    // capable_ is set by DetectionManager's post-connect probe, so this stays false
    // (and detection never fires) on non-U1 printers.
    if (!capable_ || fired_this_pause_ || !cb_)
        return;
    const int code = state_->get_print_exception_code();
    if (kind_from_u1_code(code) != DetectionKind::Spaghetti)
        return;
    fired_this_pause_ = true;

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
