// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_guided_load_modal.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace helix::ui {

GuidedLoadModal::GuidedLoadModal(std::unique_ptr<GuidedLoadSession> session)
    : session_(std::move(session)) {}

GuidedLoadModal::~GuidedLoadModal() {
    runout_observer_.reset();
    // A session still live here was closed by nobody's choice: it ends with its
    // cleanup and the heater off.
    session_.reset();
    deinit_subjects(); // before the members the subjects point into go
}

GuidedLoadModal::View GuidedLoadModal::view_for(GuidedLoadStage stage) {
    switch (stage) {
    case GuidedLoadStage::Heating:
        return View::Heating;
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
    return open(
        std::make_unique<GuidedLoadSession>(get_printer_state(), std::move(steps), waits_for_push));
}

bool GuidedLoadModal::show_manual(int slot) {
    const ManualLoadRefusal refusal = manual_load_refusal_now(get_printer_state(), slot);
    if (refusal != ManualLoadRefusal::None) {
        const std::string why = manual_load_refusal_text(refusal);
        spdlog::info("[Guided Load] Manual load refused: {}", why);
        NOTIFY_INFO("{}", why);
        return false;
    }
    return open(std::make_unique<GuidedLoadSession>(get_printer_state(), slot));
}

bool GuidedLoadModal::open(std::unique_ptr<GuidedLoadSession> session) {
    auto modal = std::make_unique<GuidedLoadModal>(std::move(session));
    register_callbacks();
    modal->init_subjects();
    modal->publish();
    if (!modal->show(lv_screen_active())) {
        modal->session_.reset();
        modal->deinit_subjects();
        return false;
    }
    GuidedLoadModal* self = modal.get();
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    // Wired after the stack owns it: a session that ends at once (a refused
    // preheat) closes a dialog that is already on the stack.
    self->session_->set_on_changed([self]() { self->publish(); });
    self->session_->begin();
    self->publish();
    return true;
}

void GuidedLoadModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    lv_subject_init_int(&view_subject_, static_cast<int>(View::Push));
    subjects_.publish("guided_load_view", &view_subject_);
    lv_subject_init_string(&heat_text_subject_, heat_text_buf_, nullptr, sizeof(heat_text_buf_),
                           "");
    subjects_.publish("guided_load_heat_text", &heat_text_subject_);
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
    if (session_) {
        session_->cancel();
    }
}

void GuidedLoadModal::on_escape() {
    handle_cancel();
}

void GuidedLoadModal::on_runout_value(int value) {
    const bool edge = last_runout_ == 0 && value == 1;
    last_runout_ = value;
    if (edge && session_) {
        spdlog::info("[{}] Filament detected at the extruder; feeding", get_name());
        session_->filament_detected();
    }
}

void GuidedLoadModal::handle_extrude() {
    if (session_) {
        session_->extrude();
    }
}

void GuidedLoadModal::handle_done() {
    if (session_) {
        session_->done();
    }
}

void GuidedLoadModal::handle_cancel() {
    if (session_) {
        session_->cancel();
    }
    hide(ModalCloseReason::ButtonPress);
}

void GuidedLoadModal::publish() {
    if (!session_) {
        return;
    }
    if (subjects_initialized_) {
        lv_subject_set_int(&view_subject_, static_cast<int>(view_for(session_->stage())));
        const std::string text =
            session_->heat_target_c() > 0
                ? fmt::format("{} / {}°C", session_->heat_current_c(), session_->heat_target_c())
                : std::string();
        lv_subject_copy_string(&heat_text_subject_, text.c_str());
    }
    if (session_->ended() && is_visible()) {
        hide(ModalCloseReason::ButtonPress);
    }
}

} // namespace helix::ui
