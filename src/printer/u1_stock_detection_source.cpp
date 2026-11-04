// SPDX-License-Identifier: GPL-3.0-or-later
// Ground truth: U1 stock fw print_stats.exception={id:532,code:2(noodle)|1(dirty bed),...}
// + state=paused on a defect fire; runout is id 523 code 0. See defect_detection in
// docs/devel/printers/SNAPMAKER_U1_SUPPORT.md.
#include "u1_stock_detection_source.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "i_moonraker_client.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "hv/json.hpp"

namespace helix::detection {

void U1StockSource::on_objects_listed(const std::vector<std::string>& objects) {
    capable_ = std::find(objects.begin(), objects.end(), "defect_detection") != objects.end();
    spdlog::info("[U1StockSource] defect_detection capability = {}", capable_);
}

void U1StockSource::tune() {
    // Null callbacks, not empty lambdas: a non-null error_cb reads as "this
    // caller reports the failure itself", which would suppress Klipper's `!!`
    // broadcast for a rejected DEFECT_DETECTION_CONFIG and leave the user with
    // nothing.
    get_moonraker_client()->send_jsonrpc(
        "printer.gcode.script", json{{"script", "DEFECT_DETECTION_CONFIG NOODLE_SENSITIVITY=low"}},
        nullptr, nullptr);
}

void U1StockSource::read_bed_check(BedCheckReadCallback done) {
    json params = {{"objects", {{"defect_detection", nullptr}}}};
    get_moonraker_client()->send_jsonrpc(
        "printer.objects.query", params,
        [done](const json& resp) {
            const json* module = nullptr;
            if (auto r = resp.find("result"); r != resp.end() && r->is_object()) {
                if (auto st = r->find("status"); st != r->end() && st->is_object()) {
                    if (auto dd = st->find("defect_detection");
                        dd != st->end() && dd->is_object()) {
                        module = &*dd;
                    }
                }
            }
            const json* clean_bed = nullptr;
            if (module) {
                if (auto cb = module->find("clean_bed"); cb != module->end() && cb->is_object()) {
                    clean_bed = &*cb;
                }
            }
            const json* enable = nullptr;
            if (clean_bed) {
                if (auto en = clean_bed->find("enable");
                    en != clean_bed->end() && en->is_boolean()) {
                    enable = &*en;
                }
            }
            if (!enable) {
                done(std::nullopt);
                return;
            }
            BedCheckState state;
            state.enabled = enable->get<bool>();
            if (auto main = module->find("main_enable");
                main != module->end() && main->is_boolean()) {
                state.module_enabled = main->get<bool>();
            }
            done(state);
        },
        [done](const MoonrakerError& err) {
            spdlog::warn("[U1StockSource] bed check state unreadable: {}", err.message);
            done(std::nullopt);
        });
}

void U1StockSource::set_bed_check(bool enabled, BedCheckSetCallback done) {
    const std::string script =
        std::string("DEFECT_DETECTION_CONFIG CLEAN_BED_ENABLE=") + (enabled ? "1" : "0");
    // The error callback carries Klipper's reason to the caller, which shows
    // it with its own revert; that is why it is not left null.
    get_moonraker_client()->send_jsonrpc(
        "printer.gcode.script", json{{"script", script}}, [done](const json&) { done(true, {}); },
        [done](const MoonrakerError& err) {
            spdlog::warn("[U1StockSource] bed check switch refused: {}", err.message);
            done(false, err.message);
        });
}

DetectionKind U1StockSource::latched_kind() const {
    return kind_from_u1_exception(state_->print_state().get_print_exception_id(),
                                  state_->print_state().get_print_exception_code());
}

bool U1StockSource::explains_current_pause() const {
    if (!capable_ || !state_) {
        return false;
    }
    // RAW_PRINT_STATE_OK: the printer's own paused state, which is what U1
    // stock firmware raises when it trips defect detection.
    const bool paused = lv_subject_get_int(state_->print_state().get_print_state_enum_subject()) ==
                        static_cast<int>(PrintJobState::PAUSED);
    return paused && latched_kind() != DetectionKind::Unknown;
}

void U1StockSource::start() {
    if (!state_)
        return;
    // Both deferred (observe<int>): the state and the exception code are
    // parsed in one update_from_status() frame, and the firmware may also send
    // them in separate frames in either order. Re-evaluating on either edge,
    // after the whole frame is parsed, sees the pair however it arrives.
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - U1 stock firmware raises
    // defect detection by pausing, so the edge is the printer's own.
    state_observer_ = helix::ui::observe<int>(
        state_->print_state().get_print_state_enum_subject(), this,
        [](U1StockSource* self, int /*state*/) { self->evaluate(); },
        state_->get_subjects_lifetime());
    exception_observer_ = helix::ui::observe<int>(
        state_->print_state().get_print_exception_subject(), this,
        [](U1StockSource* self, int /*code*/) { self->evaluate(); },
        state_->get_subjects_lifetime());
}

void U1StockSource::evaluate() {
    // RAW_PRINT_STATE_OK: the printer's own paused state, which is what U1
    // stock firmware raises when it trips defect detection.
    const bool paused = lv_subject_get_int(state_->print_state().get_print_state_enum_subject()) ==
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
    const DetectionKind kind = latched_kind();
    if (kind != DetectionKind::Spaghetti && kind != DetectionKind::DirtyBed)
        return;
    fired_this_pause_ = true;

    DetectionEvent e;
    e.source_id = id();
    e.kind = kind;
    e.attributable = true;
    e.already_paused = true;
    e.message = state_->print_state().get_print_exception_message();
    spdlog::info("[U1StockSource] defect detected (code {}): {}",
                 state_->print_state().get_print_exception_code(), e.message);
    cb_(e);
}

} // namespace helix::detection
