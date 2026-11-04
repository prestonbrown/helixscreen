// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_bed_drying_modal.h"

#include "ui_event_safety.h"
#include "ui_toast_manager.h"
#include "ui_unload_lane_choice.h"

#include "ams_state.h"
#include "app_globals.h"
#include "bed_drying_controller.h"
#include "display_manager.h"
#include "filament_op_execute.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "sound_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <memory>

namespace helix::ui {

using namespace bed_drying;

std::string BedDryingModal::material_label(const Material& m, int bed_c) {
    return fmt::format("{}  {}°C  {}h", m.name, bed_c, m.hours);
}

bool BedDryingModal::show_owned() {
    auto* ctrl = get_bed_drying_controller();
    if (!ctrl) {
        spdlog::warn("[BedDryingModal] no controller");
        return false;
    }
    auto modal = std::make_unique<BedDryingModal>();
    if (!modal->show(lv_screen_active())) {
        return false;
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

void BedDryingModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    auto* ctrl = get_bed_drying_controller();
    if (ctrl) {
        lv_subject_set_int(ctrl->get_ack_subject(), 0);
    }
    lv_obj_t* dropdown = find_widget("material_dropdown");
    if (!ctrl || !dropdown) {
        return;
    }
    std::string options;
    for (const auto& m : kMaterials) {
        if (!options.empty()) {
            options += '\n';
        }
        options += material_label(m, ctrl->bed_temp_for(m));
    }
    lv_dropdown_set_options(dropdown, options.c_str());
    ctrl->describe_chamber(kMaterials[0]);
}

void BedDryingModal::on_ok() {
    lv_obj_t* dropdown = find_widget("material_dropdown");
    const auto index = dropdown ? lv_dropdown_get_selected(dropdown) : 0;
    lv_obj_t* appliance = find_widget("appliance_switch");
    const bool with_appliance = appliance && lv_obj_has_state(appliance, LV_STATE_CHECKED);
    const Material material = kMaterials[index < kMaterials.size() ? index : 0];
    hide();
    start_bed_drying_flow(material, with_appliance);
}

namespace {

void show_place_prompt() {
    ConfirmOptions opts;
    opts.cancel_text = lv_tr("No spools placed");
    opts.on_cancel = [] {
        if (auto* ctrl = get_bed_drying_controller()) {
            ctrl->cancel_placement();
        }
    };
    // Any other way out leaves the spools possibly on the plate, so the latch
    // stays and the user confirms them off instead.
    opts.on_dismiss = [] { show_bed_drying_remove_prompt(); };
    modal_confirm(
        lv_tr("Place the spools"),
        lv_tr("Clear the area above and below the plate. Lay the spools on the "
              "plate, cover them with a box (a printed lid or the filament's "
              "packaging) and close the door."),
        ModalSeverity::Warning, lv_tr("Start drying"),
        [] {
            auto* ctrl = get_bed_drying_controller();
            if (ctrl && !ctrl->confirm_placed()) {
                ToastManager::instance().show(
                    ToastSeverity::ERROR,
                    lv_tr("Could not save the drying state: nothing was heated"), 6000);
            }
        },
        opts);
}

void prepare_plate(const Material& material, bool with_appliance) {
    auto* ctrl = get_bed_drying_controller();
    if (!ctrl) {
        return;
    }
    ctrl->prepare(material, with_appliance, show_place_prompt, [](const std::string& msg) {
        ToastManager::instance().show(
            ToastSeverity::ERROR,
            fmt::format("{}: {}", lv_tr("Could not move the plate"), msg).c_str(), 6000);
    });
}

} // namespace

void unload_before_drying(BedDryingController& ctrl, helix::SharedCallback<void()> then) {
    AmsBackend* backend = AmsState::instance().get_backend();
    AmsSystemInfo info;
    int slot = -1;
    if (backend) {
        info = backend->get_system_info();
        slot = info.current_slot;
    }
    const bool loaded = read_unload_target_loaded(backend, info, slot);

    FilamentOpSurface surface;
    surface.log_tag = "[BedDrying]";
    // A filament system has no reply to wait on, so its action is watched
    // instead. on_begin runs inline, before the backend is asked, for this tier.
    surface.on_begin = [&ctrl, then](const FilamentOpPlan& plan) {
        if (plan.tier != FilamentTier::AmsBackend) {
            return;
        }
        ctrl.await_unload(then, [](bool started) {
            // Once it started, an ERROR edge has its own error surface.
            if (!started) {
                ToastManager::instance().show(
                    ToastSeverity::ERROR,
                    lv_tr("The unload did not start: unload by hand, then start again"), 6000);
            }
        });
    };
    surface.on_failed = [&ctrl](const FilamentOpPlan&, const AmsError&, bool&) {
        ctrl.cancel_unload_wait();
    };
    surface.on_async_success = then;
    // A preheat that ends without unloading leaves the flow with nothing to wait
    // on, so say so rather than stall.
    surface.on_preheat_ended = []() {
        ToastManager::instance().show(
            ToastSeverity::ERROR,
            lv_tr("The unload did not start: unload by hand, then start again"), 6000);
    };
    surface.on_refused = [then](const FilamentOpPlan&) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("No filament loaded to unload"),
                                      3000);
        then();
    };
    // A backend that cannot name the lane in the toolhead asks first; a
    // Cancel there leaves the drying unstarted.
    if (slot < 0 && ask_lane_then_unload(backend, nullptr, surface)) {
        return;
    }
    execute_filament_unload(backend, slot, loaded, surface);
}

void start_bed_drying_flow(const Material& material, bool with_appliance) {
    auto* ctrl = get_bed_drying_controller();
    if (!ctrl || ctrl->state() != BedDryingController::State::Idle) {
        return;
    }
    const UnloadOffer offer = unload_offer(ctrl->toolhead_loaded());
    if (offer == UnloadOffer::None) {
        prepare_plate(material, with_appliance);
        return;
    }
    auto unload_then_prepare = [material, with_appliance] {
        if (auto* c = get_bed_drying_controller()) {
            unload_before_drying(
                *c, [material, with_appliance] { prepare_plate(material, with_appliance); });
        }
    };
    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Skip");
    opts.on_cancel = [material, with_appliance] { prepare_plate(material, with_appliance); };
    const char* message =
        offer == UnloadOffer::Recommended
            ? lv_tr("Filament is loaded at the toolhead. Unload it first, so it does not "
                    "soften in the extruder while the bed heats.")
            : lv_tr("Make sure no filament is loaded at the toolhead: it can soften in the "
                    "extruder while the bed heats.");
    modal_confirm(lv_tr("Unload filament?"), message,
                  offer == UnloadOffer::Recommended ? ModalSeverity::Warning : ModalSeverity::Info,
                  lv_tr("Unload filament"), unload_then_prepare, opts);
}

void show_bed_drying_remove_prompt() {
    modal_confirm(lv_tr("Remove the spools"),
                  lv_tr("The bed has cooled. Take the spools off the plate, then confirm."),
                  ModalSeverity::Info, lv_tr("Spools removed"), [] {
                      if (auto* ctrl = get_bed_drying_controller()) {
                          ctrl->confirm_removed();
                      }
                  });
}

void show_spools_on_bed_print_alarm() {
    if (auto* dm = DisplayManager::instance()) {
        dm->wake_display();
    }
    SoundManager::instance().play("error_alert");
    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Dismiss");
    modal_confirm(
        lv_tr("Spools on the bed!"),
        lv_tr("A print started while spools are marked as on the bed. If they are "
              "still there, stop the printer now with the E-stop."),
        ModalSeverity::Error, lv_tr("Spools are off"),
        [] {
            if (auto* ctrl = get_bed_drying_controller()) {
                ctrl->confirm_removed();
            }
        },
        opts);
}

void on_bed_drying_banner_clicked() {
    auto* ctrl = get_bed_drying_controller();
    if (!ctrl) {
        return;
    }
    switch (ctrl->state()) {
    case BedDryingController::State::Running:
        modal_confirm(lv_tr("Stop drying?"),
                      lv_tr("The bed turns off. The spools stay latched on the bed until you "
                            "confirm they are off."),
                      ModalSeverity::Warning, lv_tr("Stop"), [] {
                          if (auto* c = get_bed_drying_controller()) {
                              c->stop();
                          }
                      });
        break;
    case BedDryingController::State::Cooling:
        modal_confirm(lv_tr("Remove the spools now?"),
                      lv_tr("The plate and the spools are still hot. Use gloves."),
                      ModalSeverity::Warning, lv_tr("Spools removed"), [] {
                          if (auto* c = get_bed_drying_controller()) {
                              c->confirm_removed();
                          }
                      });
        break;
    case BedDryingController::State::ReadyToRemove:
        show_bed_drying_remove_prompt();
        break;
    case BedDryingController::State::Placing:
        show_place_prompt();
        break;
    case BedDryingController::State::Unloading:
    case BedDryingController::State::Preparing:
        modal_confirm(
            lv_tr("Stop drying?"),
            ctrl->state() == BedDryingController::State::Unloading
                ? lv_tr("The unload finishes on its own, but the plate will not move.")
                : lv_tr("The plate finishes its current move, but drying will not start."),
            ModalSeverity::Warning, lv_tr("Stop"), [] {
                if (auto* c = get_bed_drying_controller()) {
                    c->cancel_preparation();
                }
            });
        break;
    case BedDryingController::State::Idle:
        break;
    }
}

namespace {

void on_bed_drying_banner_clicked_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BedDrying] banner clicked");
    on_bed_drying_banner_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void on_bed_drying_material_changed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BedDrying] material changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const auto index = lv_dropdown_get_selected(dropdown);
    if (auto* ctrl = get_bed_drying_controller(); ctrl && index < kMaterials.size()) {
        ctrl->describe_chamber(kMaterials[index]);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void on_bed_drying_start_clicked_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BedDrying] start clicked");
    // A flow already under way is the banner's to answer; a second start would
    // race it for the plate.
    auto* ctrl = get_bed_drying_controller();
    if (ctrl && ctrl->state() != BedDryingController::State::Idle) {
        on_bed_drying_banner_clicked();
    } else {
        BedDryingModal::show_owned();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

void register_bed_drying_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_bed_drying_banner_clicked",
                             on_bed_drying_banner_clicked_cb);
    lv_xml_register_event_cb(nullptr, "on_bed_drying_start_clicked",
                             on_bed_drying_start_clicked_cb);
    lv_xml_register_event_cb(nullptr, "on_bed_drying_material_changed",
                             on_bed_drying_material_changed_cb);
}

} // namespace helix::ui
