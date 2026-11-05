// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_chamber_air_notices.h"

#include "ui_fan_control_overlay.h"
#include "ui_modal.h"
#include "ui_panel_input_shaper.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "chamber_air_modes.h"
#include "config.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <string>

namespace helix::ui {

ChamberAirNotice decide_chamber_air_notice(bool present, bool at_startup, bool first_fit_shown,
                                           bool print_active) {
    if (!present) {
        return at_startup ? ChamberAirNotice::None : ChamberAirNotice::Removed;
    }
    if (first_fit_shown || print_active) {
        return ChamberAirNotice::None;
    }
    return ChamberAirNotice::FirstFit;
}

namespace {

/// Anchor for the print-active observer; the notices have no object of their own.
int s_anchor = 0;
ObserverGuard s_print_active_observer;
/// A first fit seen during a print, shown when the print ends.
bool s_first_fit_pending = false;

std::string shown_key() {
    return Config::get_instance()->df() + "chamber_air/first_fit_shown";
}

bool first_fit_shown() {
    Config* cfg = Config::get_instance();
    return cfg && cfg->get<bool>(shown_key(), false);
}

void mark_first_fit_shown() {
    if (Config* cfg = Config::get_instance()) {
        cfg->set<bool>(shown_key(), true);
        cfg->save();
    }
}

bool print_active() {
    return lv_subject_get_int(get_printer_state().print_state().get_print_active_subject()) != 0;
}

lv_obj_t* screen() {
    return lv_display_get_screen_active(nullptr);
}

void offer_shaper_recalibration(const chamber_air::AccessoryCopy& copy) {
    const std::string message = fmt::format(
        lv_tr("After connecting the {}, we recommend recalibrating input shaping to ensure "
              "print quality."),
        lv_tr(copy.name));
    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Later");
    modal_confirm(
        lv_tr("Recalibrate input shaping?"), message.c_str(), ModalSeverity::Info,
        lv_tr("Calibrate now"), []() { get_global_input_shaper_panel().show(screen()); }, opts);
}

void show_first_fit(const chamber_air::AccessoryCopy& copy) {
    s_first_fit_pending = false;
    // Whichever way the notice closes, it has been shown, and the next step follows.
    auto after = [copy]() {
        mark_first_fit_shown();
        if (copy.suggests_shaper_recalibration) {
            offer_shaper_recalibration(copy);
        }
    };
    ConfirmOptions opts;
    opts.cancel_text = lv_tr("OK");
    opts.on_cancel = after;
    opts.on_dismiss = after;
    const std::string title = fmt::format(lv_tr("{} detected"), lv_tr(copy.name));
    modal_confirm(
        title.c_str(), lv_tr(copy.detected), ModalSeverity::Info, lv_tr("Open Fans"),
        [after]() {
            open_fan_control_overlay(screen());
            after();
        },
        opts);
}

void show_removed(const chamber_air::AccessoryCopy& copy) {
    const std::string title = fmt::format(lv_tr("{} disconnected"), lv_tr(copy.name));
    modal_alert(title.c_str(), lv_tr(copy.removed), ModalSeverity::Warning, lv_tr("OK"));
}

void handle(bool present, bool at_startup) {
    const auto copy = chamber_air::accessory_copy(get_printer_state().get_discovery());
    if (!copy) {
        return;
    }
    const bool shown = first_fit_shown();
    const bool printing = print_active();
    switch (decide_chamber_air_notice(present, at_startup, shown, printing)) {
    case ChamberAirNotice::FirstFit:
        spdlog::info("[ChamberAirNotices] {} found for the first time", copy->name);
        show_first_fit(*copy);
        return;
    case ChamberAirNotice::Removed:
        s_first_fit_pending = false;
        spdlog::info("[ChamberAirNotices] {} removed", copy->name);
        show_removed(*copy);
        return;
    case ChamberAirNotice::None:
        s_first_fit_pending = present && !shown && printing;
        return;
    }
}

} // namespace

void init_chamber_air_notices() {
    auto& ps = get_printer_state();
    // The edge arrives from the status path; the modal opens on the next tick,
    // outside it.
    ps.chamber_air().set_presence_edge_handler([](bool present, bool at_startup) {
        queue_update("ChamberAirNotices::edge",
                     [present, at_startup]() { handle(present, at_startup); });
    });
    s_print_active_observer = observe<int>(
        ps.print_state().get_print_active_subject(), &s_anchor,
        [](int*, int active) {
            if (active == 0 && s_first_fit_pending &&
                get_printer_state().chamber_air().available()) {
                handle(true, false);
            }
        },
        ps.get_subjects_lifetime());
    StaticSubjectRegistry::instance().register_deinit("ChamberAirNotices", []() {
        s_print_active_observer.reset();
        get_printer_state().chamber_air().set_presence_edge_handler(nullptr);
        s_first_fit_pending = false;
    });
}

} // namespace helix::ui
