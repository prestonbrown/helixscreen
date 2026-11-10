// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "layer_pause.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <functional>
#include <string>

namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

namespace helix::ui {

/**
 * @file ui_print_layer_pause.h
 * @brief Scheduling a pause into the running print from the print status panel (#1703).
 *
 * The layer readout in the metadata strip opens a modal offering "pause at the
 * next layer" and "pause at layer N" (bounded to the layers still ahead), and
 * cancelling whatever is pending. Everything printer-specific is behind
 * helix::layer_pause; the per-print state and the pending layer the strip and
 * progress ticks show live in PrinterPrintState.
 *
 * Helper owned by PrintStatusPanel, like PrintLightTimelapseControls.
 *
 * @threading Main thread only (LVGL)
 */
class PrintLayerPauseControls {
  public:
    PrintLayerPauseControls() = default;
    ~PrintLayerPauseControls();

    PrintLayerPauseControls(const PrintLayerPauseControls&) = delete;
    PrintLayerPauseControls& operator=(const PrintLayerPauseControls&) = delete;

    /// Registers layer_pause_status_text and the XML callbacks.
    void init_subjects(helix::PrinterState& printer_state);
    void deinit_subjects();

    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    void open_modal();
    void schedule_next_layer();
    void choose_layer();
    void schedule_at_layer(int layer);
    void cancel_pending();

    /// Recompute the modal's status line from print state.
    void refresh_status_text();

    /// The print reached complete, cancelled or error. The macros arm on an
    /// exact layer match and only CANCEL_PRINT clears them, so a pause this
    /// screen armed that never fired would pause the next print: disarm both.
    void on_print_ended();

  private:
    /// False when nothing was sent (no API).
    bool send(const std::string& gcode, const char* what, std::function<void()> on_sent);

    helix::IMoonrakerAPI* api_ = nullptr;
    helix::PrinterState* printer_state_ = nullptr;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;
    lv_subject_t status_text_subject_{};
    char status_text_buf_[192] = "";

    ObserverGuard availability_observer_;
    ObserverGuard pending_observer_;
    ObserverGuard layer_observer_;
    ObserverGuard total_observer_;
    ObserverGuard language_observer_;
    ObserverGuard job_state_observer_;

    /// This screen armed a pause during the current print.
    bool armed_this_print_ = false;

    helix::AsyncLifetimeGuard lifetime_;
};

/**
 * @brief The modal's status line: where the print is, what is pending, or why
 *        nothing can be scheduled. Translated.
 * @param pending_layer The layer the pending pause lands on, 0 = none.
 */
std::string layer_pause_status_text(helix::layer_pause::Availability availability,
                                    int current_layer, int total_layers, int pending_layer);

} // namespace helix::ui
