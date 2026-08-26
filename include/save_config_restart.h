// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file save_config_restart.h
/// @brief One honest answer to "did SAVE_CONFIG work?", shared by every panel
///        that sends it.
///
/// Klipper's `cmd_SAVE_CONFIG` writes printer.cfg and then calls
/// `request_restart('restart')` as its last act, so it NEVER acknowledges the
/// command -- the connection it would ack through is already going down.
/// Moonraker fails the pending `printer.gcode.script` with 503 "Klippy
/// Disconnected". Every caller therefore sees a FAILED rpc for a save that
/// succeeded, and the only evidence of success is klippy coming back READY.
///
/// Panels that took the rpc error at face value reported "Failed to save
/// configuration" on every successful save, on every printer
/// (prestonbrown/helixscreen#1359). The z-offset panel got it right by hand;
/// this header is that logic lifted out so the other flows share one copy and a
/// panel added later cannot forget the restart half.

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"

#include <functional>
#include <string>

class IMoonrakerAPI;

namespace helix::ui {

/// Track whether the Klipper restart a save triggered has happened, and finished.
///
/// The save's rpc is dropped by `notify_klippy_disconnected()`, so its success
/// callback frequently never fires. Klipper returning READY after a restart that
/// this save triggered is strong evidence the save succeeded.
///
/// Not thread-safe; drive it from the main thread only.
class SaveRestartLatch {
  public:
    /// Clear all state. Call on entering AND leaving the saving state so a
    /// second save in the same session does not inherit the first one's latch.
    void reset() {
        restart_latched_ = false;
        restart_completed_ = false;
    }

    /// Feed an observed klippy readiness transition while a save is in flight.
    void on_klippy_ready(bool ready) {
        if (!ready) {
            restart_latched_ = true;
        } else if (restart_latched_) {
            restart_completed_ = true;
        }
    }

    /// Fold in an external "a restart is expected right now" signal
    /// (EmergencyStopOverlay::is_expected_restart()). Monotonic: once set within
    /// a save it stays set until reset().
    void note_restart_expected(bool expected) {
        if (expected) {
            restart_latched_ = true;
        }
    }

    /// True if a restart was observed or expected at any point since reset().
    bool restart_latched() const {
        return restart_latched_;
    }

    /// True once Klipper returned to READY after a latched restart -- treat the
    /// save as having succeeded even though its rpc was dropped.
    bool restart_completed() const {
        return restart_completed_;
    }

  private:
    bool restart_latched_ = false;
    bool restart_completed_ = false;
};

/// Decide whether a save-in-progress timeout should be extended instead of failing.
///
/// SAVE_CONFIG restarts Klipper, so the save timeout is armed across a window in
/// which no rpc can complete. On some printers stock code chains a *second*
/// config write + restart tens of seconds later (Creality K2 + CFS writes the
/// CFS Tn_data via CXSAVE_CONFIG ~50s after the first SAVE_CONFIG), which pushed
/// the whole sequence past a fixed 30s guard and reported a bogus "timed out"
/// error for a save that actually succeeded.
///
/// Extending is bounded so a genuinely hung save still surfaces an error rather
/// than leaving the panel spinning forever.
///
/// @param restart_latched   SaveRestartLatch::restart_latched() -- whether a
///                          restart was seen at ANY point since the save began.
///                          Must NOT be an instantaneous
///                          EmergencyStopOverlay::is_expected_restart() sample:
///                          the 15s suppression window has always closed by the
///                          time the 30s save guard first fires, so that reads
///                          false and no extension is ever granted.
/// @param extensions_used   How many extensions have already been granted
/// @param max_extensions    Cap on extensions
/// @return true to re-arm the timeout, false to fail the operation
bool should_extend_save_timeout(bool restart_latched, unsigned extensions_used,
                                unsigned max_extensions);

/// Send SAVE_CONFIG and report its real outcome across the restart it triggers.
///
/// Owns the whole contract so a caller cannot implement half of it:
///   - arms the expected-restart suppressions (`begin_expected_klippy_restart`),
///   - sends SAVE_CONFIG,
///   - absorbs the dropped rpc instead of reporting it as a failure,
///   - watches klippy and reports success when it returns READY,
///   - still reports a REAL failure when the save genuinely did not happen.
///
/// A dropped rpc is only absorbed when a restart was actually latched. An rpc
/// error with no restart in sight is a real rejection (bad config, klippy
/// already down) and is passed to @p on_failed unchanged.
///
/// Completion is decided by the klippy state transition, never by a timer:
/// hardware in the field has taken 32s to reach READY while the disconnect
/// suppression window is 15s, so anything window-based re-reports the error on
/// slow boards.
///
/// Hold one of these as a member of the panel that saves. Main-thread only.
class SaveConfigWatch {
  public:
    SaveConfigWatch() = default;
    ~SaveConfigWatch();

    SaveConfigWatch(const SaveConfigWatch&) = delete;
    SaveConfigWatch& operator=(const SaveConfigWatch&) = delete;

    /// Arm the watch and send SAVE_CONFIG.
    ///
    /// @param api                 API to send through; a null api reports failure.
    /// @param initiation_message  Toast wording for the expected restart.
    /// @param on_saved            Called once, on the main thread, when the save
    ///                            is known to have succeeded.
    /// @param on_failed           Called once, on the main thread, with a human
    ///                            message, when the save genuinely failed.
    void begin(IMoonrakerAPI* api, const char* initiation_message, std::function<void()> on_saved,
               std::function<void(const std::string&)> on_failed);

    /// Same contract as begin(), callable from a background thread.
    ///
    /// begin() installs an LVGL observer, so it is main-thread only - but the
    /// natural caller is the success callback of the rpc that precedes the save
    /// (Z_OFFSET_APPLY_PROBE -> SAVE_CONFIG), which lands on the WebSocket
    /// thread. The hop is guarded by this watch's own lifetime, so a watch
    /// destroyed between the call and the hop simply never begins.
    void begin_from_background(IMoonrakerAPI* api, const char* initiation_message,
                               std::function<void()> on_saved,
                               std::function<void(const std::string&)> on_failed);

    /// Drop the klippy watch and forget any in-flight save. Safe to call twice.
    void end();

    /// True between begin() and the terminal callback.
    bool in_flight() const {
        return in_flight_;
    }

    /// Exposed for tests and for panels that drive their own save timeout.
    const SaveRestartLatch& latch() const {
        return latch_;
    }

  private:
    void settle_saved();
    void settle_failed(const std::string& message);

    SaveRestartLatch latch_;
    ObserverGuard klippy_observer_;
    std::function<void()> on_saved_;
    std::function<void(const std::string&)> on_failed_;
    bool in_flight_ = false;

    // The rpc callbacks land on the WebSocket thread. bg_cb() hops them to the
    // main thread and drops them if this watch died first, which is the guarded
    // form the L081 detector requires (never a bare `if (tok.expired()) return`).
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
