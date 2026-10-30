// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_layer_pause.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "i_moonraker_api.h"
#include "lvgl.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <utility>

namespace helix::ui {

namespace lp = helix::layer_pause;

namespace {

PrintLayerPauseControls* g_layer_pause_controls = nullptr;

void with_controls(void (PrintLayerPauseControls::*fn)()) {
    if (g_layer_pause_controls) {
        (g_layer_pause_controls->*fn)();
    }
}

/// Close the modal the tapped button belongs to; it is the top one.
void close_layer_pause_modal() {
    if (lv_obj_t* top = Modal::get_top()) {
        Modal::hide(top);
    }
}

} // namespace

std::string layer_pause_status_text(lp::Availability availability, int current_layer,
                                    int total_layers, int pending_layer) {
    std::string where;
    switch (availability) {
    case lp::Availability::Hidden:
        return {};
    case lp::Availability::NoLayerData:
        where = lv_tr("This print does not report layers, so no layer pause can be scheduled.");
        break;
    case lp::Availability::LastLayer:
        where = lv_tr("The print is on its last layer.");
        break;
    case lp::Availability::Ready:
        where = fmt::format(fmt::runtime(lv_tr("Layer {} of {}.")), current_layer, total_layers);
        break;
    }
    const std::string pending =
        pending_layer > 0 ? fmt::format(fmt::runtime(lv_tr("Pausing at layer {}.")), pending_layer)
                          : std::string(lv_tr("No pause scheduled."));
    return where + "\n" + pending;
}

PrintLayerPauseControls::~PrintLayerPauseControls() {
    deinit_subjects();
}

void PrintLayerPauseControls::init_subjects(helix::PrinterState& printer_state) {
    if (subjects_initialized_) {
        return;
    }
    printer_state_ = &printer_state;
    g_layer_pause_controls = this;

    UI_MANAGED_SUBJECT_STRING(status_text_subject_, status_text_buf_, "", "layer_pause_status_text",
                              subjects_);

    auto& print = printer_state.print_state();
    const auto lifetime = printer_state.get_subjects_lifetime();
    auto refresh = [](PrintLayerPauseControls* self, int) { self->refresh_status_text(); };
    availability_observer_ = helix::ui::observe<int>(print.get_layer_pause_availability_subject(),
                                                     this, refresh, lifetime);
    pending_observer_ =
        helix::ui::observe<int>(print.get_layer_pause_layer_subject(), this, refresh, lifetime);
    layer_observer_ =
        helix::ui::observe<int>(print.get_print_layer_current_subject(), this, refresh, lifetime);
    total_observer_ =
        helix::ui::observe<int>(print.get_print_layer_total_subject(), this, refresh, lifetime);
    // RAW_PRINT_STATE_OK: the macros stay armed until the printer's own job
    // ends, so the wire's end states are the question, not the app lifecycle.
    job_state_observer_ = helix::ui::observe_print_state(
        print.get_print_state_enum_subject(), this,
        [](PrintLayerPauseControls* self, PrintJobState state) {
            if (job_has_ended(state)) {
                self->on_print_ended();
            }
        },
        lifetime);
    language_observer_ = helix::ui::observe_language_change(
        this, [](PrintLayerPauseControls* self) { self->refresh_status_text(); });

    register_xml_callbacks({
        {"on_layer_pause_open",
         [](lv_event_t*) { with_controls(&PrintLayerPauseControls::open_modal); }},
        {"on_layer_pause_next",
         [](lv_event_t*) {
             close_layer_pause_modal();
             with_controls(&PrintLayerPauseControls::schedule_next_layer);
         }},
        {"on_layer_pause_choose",
         [](lv_event_t*) {
             close_layer_pause_modal();
             with_controls(&PrintLayerPauseControls::choose_layer);
         }},
        {"on_layer_pause_cancel",
         [](lv_event_t*) {
             close_layer_pause_modal();
             with_controls(&PrintLayerPauseControls::cancel_pending);
         }},
        {"on_layer_pause_close", [](lv_event_t*) { close_layer_pause_modal(); }},
    });

    subjects_initialized_ = true;
    refresh_status_text();
}

void PrintLayerPauseControls::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    if (g_layer_pause_controls == this) {
        g_layer_pause_controls = nullptr;
    }
    availability_observer_.reset();
    pending_observer_.reset();
    layer_observer_.reset();
    total_observer_.reset();
    language_observer_.reset();
    job_state_observer_.reset();
    lifetime_.invalidate();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrintLayerPauseControls::refresh_status_text() {
    if (!subjects_initialized_ || !printer_state_) {
        return;
    }
    auto& print = printer_state_->print_state();
    const std::string text =
        layer_pause_status_text(static_cast<lp::Availability>(lv_subject_get_int(
                                    print.get_layer_pause_availability_subject())),
                                print.slicer_current_layer(), print.slicer_total_layers(),
                                lv_subject_get_int(print.get_layer_pause_layer_subject()));
    std::snprintf(status_text_buf_, sizeof(status_text_buf_), "%s", text.c_str());
    lv_subject_copy_string(&status_text_subject_, status_text_buf_);
}

void PrintLayerPauseControls::open_modal() {
    refresh_status_text();
    Modal::show("layer_pause_modal");
}

bool PrintLayerPauseControls::send(const std::string& gcode, const char* what,
                                   std::function<void()> on_sent) {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("Cannot schedule a pause: printer not connected"));
        return false;
    }
    spdlog::info("[LayerPause] {}: {}", what, gcode);
    api_->execute_gcode(gcode, lifetime_.bg_cb("PrintLayerPauseControls::send", std::move(on_sent)),
                        [](const MoonrakerError& err) {
                            spdlog::error("[LayerPause] Command failed: {}", err.message);
                            helix::ui::notify_error_tr(TR_NOOP("Failed to schedule the pause: {}"),
                                                       err);
                        });
    return true;
}

void PrintLayerPauseControls::schedule_next_layer() {
    armed_this_print_ |= send(lp::schedule_next_layer_command(), "Pause at next layer", [this]() {
        if (printer_state_) {
            printer_state_->print_state().note_layer_pause_next_layer(true);
        }
    });
}

void PrintLayerPauseControls::schedule_at_layer(int layer) {
    if (printer_state_ && !printer_state_->print_state().file_may_report_layer(layer)) {
        // The macro fires only on an exact CURRENT_LAYER match; a layer the
        // file never reports would stay armed into the next print.
        spdlog::info("[LayerPause] Refusing layer {}: the file never reports it", layer);
        NOTIFY_WARNING(
            "{}", fmt::format(fmt::runtime(lv_tr("This print never reports layer {}.")), layer));
        return;
    }
    armed_this_print_ |=
        send(lp::schedule_at_layer_command(layer), "Pause at layer", [this, layer]() {
            if (printer_state_) {
                printer_state_->print_state().note_layer_pause_at_layer(layer);
            }
        });
}

void PrintLayerPauseControls::on_print_ended() {
    if (!armed_this_print_) {
        return;
    }
    armed_this_print_ = false;
    // Disarming an idle macro is harmless, so both go out whatever read-back
    // says: a printer without read-back cannot tell us what is still armed.
    send(lp::cancel_next_layer_command(), "Disarm next-layer pause at print end", [this]() {
        if (printer_state_) {
            printer_state_->print_state().note_layer_pause_next_layer(false);
        }
    });
    send(lp::cancel_at_layer_command(), "Disarm layer pause at print end", [this]() {
        if (printer_state_) {
            printer_state_->print_state().note_layer_pause_at_layer(0);
        }
    });
}

void PrintLayerPauseControls::choose_layer() {
    if (!printer_state_) {
        return;
    }
    auto& print = printer_state_->print_state();
    if (lv_subject_get_int(print.get_layer_pause_availability_subject()) !=
        static_cast<int>(lp::Availability::Ready)) {
        return;
    }
    const int current = print.slicer_current_layer();
    const int total = print.slicer_total_layers();
    const auto bounds = lp::layer_bounds(current, total);
    const auto pending = print.get_layer_pause_schedule();

    ui_keypad_config_t config{};
    config.initial_value =
        static_cast<float>(pending.at_layer > current ? pending.at_layer : bounds.min_layer);
    config.min_value = static_cast<float>(bounds.min_layer);
    config.max_value = static_cast<float>(bounds.max_layer);
    config.title_label = lv_tr("Pause Layer");
    config.unit_label = "";
    config.allow_decimal = false;
    config.allow_negative = false;
    config.callback = [](float value, void* user_data) {
        static_cast<PrintLayerPauseControls*>(user_data)->schedule_at_layer(
            static_cast<int>(value));
    };
    config.user_data = this;
    ui_keypad_show(&config);
}

void PrintLayerPauseControls::cancel_pending() {
    if (!printer_state_) {
        return;
    }
    const auto pending = printer_state_->print_state().get_layer_pause_schedule();
    if (pending.next_layer) {
        send(lp::cancel_next_layer_command(), "Cancel next-layer pause", [this]() {
            if (printer_state_) {
                printer_state_->print_state().note_layer_pause_next_layer(false);
            }
        });
    }
    if (pending.at_layer > 0) {
        send(lp::cancel_at_layer_command(), "Cancel layer pause", [this]() {
            if (printer_state_) {
                printer_state_->print_state().note_layer_pause_at_layer(0);
            }
        });
    }
}

} // namespace helix::ui
