// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_status_icon.h"

#include "ui_emergency_stop.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "i_moonraker_client.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::ui::observe;

// ============================================================================
// SINGLETON INSTANCE
// ============================================================================

PrinterStatusIcon& PrinterStatusIcon::instance() {
    static PrinterStatusIcon instance;
    return instance;
}

// ============================================================================
// PRINTER STATUS ICON IMPLEMENTATION
// ============================================================================

void PrinterStatusIcon::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[PrinterStatusIcon] Subjects already initialized");
        return;
    }

    spdlog::trace("[PrinterStatusIcon] Initializing printer icon subjects...");

    // Printer starts disconnected (gray)
    UI_MANAGED_SUBJECT_INT(printer_icon_state_subject_,
                           static_cast<int>(PrinterIconState::DISCONNECTED), "printer_icon_state",
                           subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "PrinterStatusIconSubjects", []() { PrinterStatusIcon::instance().deinit_subjects(); });

    spdlog::trace("[PrinterStatusIcon] Subjects initialized and registered");
}

void PrinterStatusIcon::init() {
    if (initialized_) {
        spdlog::warn("[PrinterStatusIcon] Already initialized");
        return;
    }

    spdlog::debug("[PrinterStatusIcon] init() called");

    // Ensure subjects are initialized
    if (!subjects_initialized_) {
        init_subjects();
    }

    // Observe printer states from PrinterState
    PrinterState& printer_state = get_printer_state();

    // Printer connection observer
    lv_subject_t* conn_subject =
        printer_state.network_state().get_printer_connection_state_subject();
    spdlog::trace("[PrinterStatusIcon] Registering observer on printer_connection_state_subject at "
                  "{}",
                  (void*)conn_subject);
    connection_observer_ = observe<int>(
        conn_subject, this,
        [](PrinterStatusIcon* self, int val) {
            self->cached_connection_state_ = val;
            spdlog::trace("[PrinterStatusIcon] Connection state changed to: {}",
                          self->cached_connection_state_);
            self->update_icon_state();
        },
        printer_state.get_subjects_lifetime());

    // Klippy state observer
    lv_subject_t* klippy_subject = printer_state.network_state().get_klippy_state_subject();
    spdlog::trace("[PrinterStatusIcon] Registering observer on klippy_state_subject at {}",
                  (void*)klippy_subject);
    klippy_observer_ = observe<int>(
        klippy_subject, this,
        [](PrinterStatusIcon* self, int val) {
            self->cached_klippy_state_ = val;
            spdlog::trace("[PrinterStatusIcon] Klippy state changed to: {}",
                          self->cached_klippy_state_);
            self->update_icon_state();
        },
        printer_state.get_subjects_lifetime());

    initialized_ = true;
    spdlog::debug("[PrinterStatusIcon] Initialization complete");
}

PrinterIconState PrinterStatusIcon::compute_state(int connection_state, int klippy_state,
                                                  bool ever_connected, bool expected_restart) {
    if (connection_state == static_cast<int>(ConnectionState::CONNECTED)) {
        switch (klippy_state) {
        case static_cast<int>(KlippyState::STARTUP):
            return PrinterIconState::WARNING;
        case static_cast<int>(KlippyState::SHUTDOWN):
            // A SAVE_CONFIG or user-initiated restart bounces Klipper through a
            // transient SHUTDOWN. During an expected restart show WARNING (amber
            // "restarting") instead of ERROR (red) so the icon doesn't flash red
            // mid-calibration. A genuine SHUTDOWN (no restart pending) still maps
            // to ERROR, as does KlippyState::ERROR regardless of the window.
            return expected_restart ? PrinterIconState::WARNING : PrinterIconState::ERROR;
        case static_cast<int>(KlippyState::ERROR):
            return PrinterIconState::ERROR;
        case static_cast<int>(KlippyState::READY):
        default:
            return PrinterIconState::READY;
        }
    }
    if (connection_state == static_cast<int>(ConnectionState::FAILED)) {
        return PrinterIconState::ERROR;
    }
    // DISCONNECTED, CONNECTING, RECONNECTING
    return ever_connected ? PrinterIconState::WARNING : PrinterIconState::DISCONNECTED;
}

void PrinterStatusIcon::update_icon_state() {
    const bool expected_restart = EmergencyStopOverlay::instance().is_expected_restart();
    PrinterIconState new_state =
        compute_state(cached_connection_state_, cached_klippy_state_,
                      get_printer_state().network_state().was_ever_connected(), expected_restart);

    spdlog::debug("[PrinterStatusIcon] conn={} klippy={} expected_restart={} -> icon state {}",
                  cached_connection_state_, cached_klippy_state_, expected_restart,
                  static_cast<int>(new_state));

    if (subjects_initialized_) {
        lv_subject_set_int(&printer_icon_state_subject_, static_cast<int>(new_state));
    }
}

void PrinterStatusIcon::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // Clear observers before deinit to prevent callbacks during teardown
    connection_observer_.reset();
    klippy_observer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
    initialized_ = false;
    spdlog::debug("[PrinterStatusIcon] Subjects deinitialized");
}
