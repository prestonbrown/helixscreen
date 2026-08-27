// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "save_config_restart.h"

#include "ui_emergency_stop.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "moonraker_types.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

bool should_extend_save_timeout(bool restart_latched, unsigned extensions_used,
                                unsigned max_extensions) {
    // Only stall the clock if this save actually triggered a restart. A save
    // that never restarted Klipper and still has not completed is a real hang
    // and must be reported.
    return restart_latched && extensions_used < max_extensions;
}

SaveConfigWatch::~SaveConfigWatch() {
    // The observer must go before the guard's generation bumps, so a klippy
    // transition mid-teardown cannot land on a half-destroyed watch.
    klippy_observer_.reset();
}

void SaveConfigWatch::begin(IMoonrakerAPI* api, const char* initiation_message,
                            std::function<void()> on_saved,
                            std::function<void(const std::string&)> on_failed) {
    if (in_flight_) {
        spdlog::warn("[SaveConfigWatch] begin() while a save is already in flight - ignoring");
        return;
    }

    on_saved_ = std::move(on_saved);
    on_failed_ = std::move(on_failed);

    if (!api) {
        settle_failed("No connection to the printer");
        return;
    }

    latch_.reset(); // Never inherit a previous save's latch
    in_flight_ = true;

    // Watch klippy for the whole save. This, not the rpc, is what tells us the
    // save worked: SAVE_CONFIG's reply is dropped by the restart it causes.
    klippy_observer_ = observe_int_sync<SaveConfigWatch>(
        get_printer_state().get_klippy_state_subject(), this, [](SaveConfigWatch* self, int state) {
            if (!self->in_flight_) {
                return; // Stale fire after this save settled
            }
            self->latch_.on_klippy_ready(static_cast<KlippyState>(state) == KlippyState::READY);

            if (self->latch_.restart_completed()) {
                spdlog::info("[SaveConfigWatch] Klipper back READY after the save's restart - "
                             "treating SAVE_CONFIG as succeeded (its rpc was dropped)");
                // Settle on the next tick, not from inside this observer's own
                // callback: settle_saved() resets the observer we are standing in.
                self->lifetime_.defer("SaveConfigWatch::settle_after_restart",
                                      [self]() { self->settle_saved(); });
            }
        });

    // Arms the recovery-dialog and disconnect-modal suppressions. It does not
    // touch the rpc error path, which is what this class adds.
    helix::ui::begin_expected_klippy_restart(initiation_message);

    // Through the API's named operation rather than a bare gcode string: which
    // command persists the config is the network layer's business, not ours.
    api->advanced().save_config(
        lifetime_.bg_cb("SaveConfigWatch::rpc_ok",
                        [this]() {
                            // Rare: an ack that beat the restart. Either way the
                            // config is written by the time Klipper replies.
                            spdlog::debug("[SaveConfigWatch] SAVE_CONFIG acked before the restart");
                            settle_saved();
                        }),
        lifetime_.bg_cb("SaveConfigWatch::rpc_error", [this](const MoonrakerError& err) {
            if (!in_flight_) {
                return;
            }
            // An expected restart may already be flagged even though no klippy
            // transition has been observed yet, because the suppression is armed
            // synchronously before the send.
            latch_.note_restart_expected(EmergencyStopOverlay::instance().is_expected_restart());

            if (latch_.restart_latched()) {
                // The dropped rpc, not a failure. Keep waiting for READY; the
                // observer above decides the outcome.
                spdlog::debug("[SaveConfigWatch] SAVE_CONFIG rpc dropped by the restart it "
                              "triggered ({}) - waiting for Klipper to return",
                              err.message);
                return;
            }

            // No restart in sight: Klipper genuinely rejected the save, or was
            // already down before we sent it. That is a real failure.
            spdlog::error("[SaveConfigWatch] SAVE_CONFIG failed with no restart in progress: {}",
                          err.message);
            settle_failed(err.message);
        }));
}

void SaveConfigWatch::begin_from_background(IMoonrakerAPI* api, const char* initiation_message,
                                            std::function<void()> on_saved,
                                            std::function<void(const std::string&)> on_failed) {
    lifetime_.bg_cb("SaveConfigWatch::begin_from_background",
                    [this, api, initiation_message, on_saved = std::move(on_saved),
                     on_failed = std::move(on_failed)]() mutable {
                        begin(api, initiation_message, std::move(on_saved), std::move(on_failed));
                    })();
}

void SaveConfigWatch::end() {
    in_flight_ = false;
    klippy_observer_.reset();
    latch_.reset();
    on_saved_ = nullptr;
    on_failed_ = nullptr;
}

void SaveConfigWatch::settle_saved() {
    if (!in_flight_) {
        return;
    }
    auto cb = on_saved_; // end() clears the members; call the copy
    end();
    if (cb) {
        cb();
    }
}

void SaveConfigWatch::settle_failed(const std::string& message) {
    if (!in_flight_ && !on_failed_) {
        return;
    }
    auto cb = on_failed_;
    end();
    if (cb) {
        cb(message);
    }
}

} // namespace helix::ui
