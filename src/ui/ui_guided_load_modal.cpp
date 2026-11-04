// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_guided_load_modal.h"

#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace helix::ui {

GuidedLoadModal::GuidedLoadModal(filament_macros::GuidedLoad steps, bool waits_for_push)
    : flow_(std::move(steps), waits_for_push) {}

GuidedLoadModal::~GuidedLoadModal() {
    runout_observer_.reset();
    deinit_subjects(); // before the members the subjects point into go
}

GuidedLoadModal::View GuidedLoadModal::view_for(GuidedLoadStage stage) {
    switch (stage) {
    case GuidedLoadStage::AwaitPush:
        return View::Push;
    case GuidedLoadStage::ConfirmOutput:
        return View::Confirm;
    default:
        return View::Working;
    }
}

bool GuidedLoadModal::opens_during(PrintState lifecycle) {
    return !job_holds_machine(lifecycle);
}

bool GuidedLoadModal::show_owned(filament_macros::GuidedLoad steps, bool waits_for_push) {
    const PrintState lifecycle = get_printer_state().print_state().get_print_lifecycle();
    if (!opens_during(lifecycle)) {
        spdlog::info("[Guided Load] Not opening: a print job holds the machine");
        return false;
    }
    auto modal = std::make_unique<GuidedLoadModal>(std::move(steps), waits_for_push);
    register_callbacks();
    modal->init_subjects();
    modal->publish();
    if (!modal->show(lv_screen_active())) {
        modal->deinit_subjects();
        return false;
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

void GuidedLoadModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    lv_subject_init_int(&view_subject_, static_cast<int>(View::Push));
    subjects_.publish("guided_load_view", &view_subject_);
    subjects_initialized_ = true;
}

void GuidedLoadModal::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void GuidedLoadModal::register_callbacks() {
    static bool registered = false;
    if (registered) {
        return;
    }
    lv_xml_register_event_cb(nullptr, "on_guided_load_extrude", [](lv_event_t* e) {
        if (auto* self = Modal::from_event<GuidedLoadModal>(e)) {
            self->handle_extrude();
        }
    });
    lv_xml_register_event_cb(nullptr, "on_guided_load_done", [](lv_event_t* e) {
        if (auto* self = Modal::from_event<GuidedLoadModal>(e)) {
            self->handle_done();
        }
    });
    lv_xml_register_event_cb(nullptr, "on_guided_load_cancel", [](lv_event_t* e) {
        if (auto* self = Modal::from_event<GuidedLoadModal>(e)) {
            self->handle_cancel();
        }
    });
    registered = true;
}

void GuidedLoadModal::on_show() {
    auto& sensors = FilamentSensorManager::instance();
    lv_subject_t* runout = sensors.get_runout_detected_subject();
    if (!runout) {
        return;
    }
    // The first notification is the value at attach; only a later empty-to-detected
    // edge means the user pushed filament in.
    last_runout_ = lv_subject_get_int(runout);
    runout_observer_ = observe<int>(
        runout, this, [](GuidedLoadModal* self, int value) { self->on_runout_value(value); },
        sensors.subjects_lifetime());
}

void GuidedLoadModal::on_hide() {
    runout_observer_.reset();
}

void GuidedLoadModal::on_dismissed() {
    // A close nobody chose through the buttons still leaves the nozzle hot and
    // the sensor off.
    send(flow_.cancel(), /*report_outcome=*/false);
}

void GuidedLoadModal::on_escape() {
    handle_cancel();
}

void GuidedLoadModal::on_runout_value(int value) {
    const bool edge = last_runout_ == 0 && value == 1;
    last_runout_ = value;
    if (edge) {
        spdlog::info("[{}] Filament detected at the extruder; feeding", get_name());
        send(flow_.filament_detected(), /*report_outcome=*/true);
    }
}

void GuidedLoadModal::handle_extrude() {
    send(flow_.extrude(), /*report_outcome=*/true);
}

void GuidedLoadModal::handle_done() {
    send(flow_.done(), /*report_outcome=*/true);
}

void GuidedLoadModal::handle_cancel() {
    send(flow_.cancel(), /*report_outcome=*/false);
    hide(ModalCloseReason::ButtonPress);
}

void GuidedLoadModal::send(const std::string& script, bool report_outcome) {
    publish();
    if (script.empty()) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API; not sending: {}", get_name(), script);
        if (report_outcome) {
            on_command_finished(false);
        }
        return;
    }
    spdlog::info("[{}] Sending: {}", get_name(), script);
    if (!report_outcome) {
        // Cleanup outlives this dialog: nothing is left to tell about it.
        api->execute_gcode(
            script, []() {}, [](const MoonrakerError&) {}, IMoonrakerAPI::kExtrusionTimeoutMs,
            /*silent=*/false, /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);
        return;
    }
    // hide() invalidates lifetime_ before the stack frees this instance, so a
    // completion deferred through its token never reaches a freed `this`.
    auto tok = lifetime_.token();
    api->execute_gcode(
        script,
        [tok, this]() {
            tok.defer("GuidedLoadModal::sent", [this]() { on_command_finished(true); });
        },
        [tok, this](const MoonrakerError&) {
            tok.defer("GuidedLoadModal::failed", [this]() { on_command_finished(false); });
        },
        IMoonrakerAPI::kExtrusionTimeoutMs, /*silent=*/false, /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/false);
}

void GuidedLoadModal::on_command_finished(bool ok) {
    const std::string follow_up = flow_.command_finished(ok);
    send(follow_up, /*report_outcome=*/false);
    if (flow_.ended()) {
        hide(ModalCloseReason::ButtonPress);
    }
}

void GuidedLoadModal::publish() {
    if (subjects_initialized_) {
        lv_subject_set_int(&view_subject_, static_cast<int>(view_for(flow_.stage())));
    }
}

} // namespace helix::ui
