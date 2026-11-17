// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_error.h"

#include <cstdint>
#include <functional>
#include <string>

namespace helix {
class IMoonrakerAPI;

/// What a successful recovery established.
enum class RecoveryOutcome {
    KLIPPY_READY,   ///< printer.info reported `ready` after the restart.
    RESTART_ISSUED, ///< A restart was started; Klippy's state comes from its notifications.
};

/// Cadence and bounds of the recovery chain.
struct RecoveryTiming {
    uint32_t poll_interval_ms = 2000;
    /// How long a Klippy answering anything but `startup` or `ready` is given
    /// before the script runs; restarts after the script runs.
    uint32_t verify_window_ms = 45000;
    /// Measured from recover(): past it the chain stops polling and ends. A
    /// script already running is still waited for, up to script_timeout_ms.
    uint32_t chain_deadline_ms = 120000;
    /// helix-recover.sh is killed if it runs longer than this.
    uint32_t script_timeout_ms = 90000;
};

/// Platform-blind printer recovery orchestrator.
///
/// "Recovery" means: bring a Klipper instance back to a usable state when
/// `firmware_restart` alone won't do it (e.g. K2's `klipper_mcu` daemon needs
/// to be bounced because a CFS retract error left the rpi MCU shut down —
/// see `key298: Can not update MCU rpi config as it is shutdown`, or the
/// Snapmaker U1 case where Moonraker has `[machine] provider: none` and
/// klippy_uds dropped so neither firmware_restart nor services.restart can
/// reach klippy).
///
/// **Chain** — each step only fires if the previous returns a recoverable
/// error (transport down / klippy disconnected / method unavailable):
///   1. `printer.firmware_restart` — proxies through klippy_uds. Always
///      tried first; on a healthy klippy it's the cheapest restart and
///      what users expect to see happen. Moonraker accepting it proves only
///      that the restart began, so the chain then polls `printer.info` for a
///      bounded window: `ready` completes, while `error` (a startup failure
///      such as key298) or no `ready` before the window ends runs step 2 once.
///      While Klippy keeps reporting `startup` the window stretches to the
///      chain's deadline instead, since a slow boot is not a failure; the
///      deadline ends the chain with no script. After the script exits 0 the
///      same poll runs once more, under the same deadline and without
///      escalating again, so a recovery that brings Klippy back reports it
///      ready. Without a local script the chain ends
///      at the first escalation: a startup error on a host with no platform
///      knowledge is a config fault no service restart clears, and the
///      app's klippy state already shows it.
///   2. **Local exec** of `$INSTALL_DIR/bin/helix-recover.sh` (this class
///      `fork()` + `execvp()`s it). The per-platform installer
///      (`scripts/lib/installer/recovery.sh`) writes the script with the
///      right `/etc/init.d/...` invocation baked in. Works even when
///      klippy is fully dead — that's the whole point. No-op on platforms
///      where the installer didn't ship a script (stock systemd Pi etc.).
///   3. `machine.services.restart klipper` — last resort. Only works on
///      hosts where Moonraker's `[machine] provider` is wired to a real
///      service manager (systemd_dbus / systemd_cli / supervisord_cli).
///      Elsewhere Moonraker refuses it, and on_error carries a translated
///      message telling the user to power-cycle the printer, never
///      Moonraker's own text.
///
/// Frontend stays platform-blind: this class never learns platform names.
/// All platform knowledge lives in the shipped helix-recover.sh.
///
/// Lifetime: stateless free-standing helper, owned wherever it's invoked from
/// (typically the EmergencyStopOverlay or a toast action callback). The
/// chain outlives it; a chain whose Moonraker client is destroyed (printer
/// switch, soft restart) stops without calling either callback.
///
/// Single-flight: one chain runs at a time, process-wide. recover() while one
/// is running refuses through on_error.
class PrinterRecoveryService {
  public:
    using SuccessCallback = std::function<void(RecoveryOutcome)>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    explicit PrinterRecoveryService(IMoonrakerAPI* api, RecoveryTiming timing = {})
        : api_(api), timing_(timing) {}

    /// Run the full recovery chain (see class doc). Either callback may run
    /// on the WebSocket thread or the main thread, and exactly one runs unless
    /// the Moonraker client is destroyed mid-chain.
    void recover(SuccessCallback on_success, ErrorCallback on_error);

    /// True iff `$INSTALL_DIR/bin/helix-recover.sh` exists and is executable.
    /// Cheap (single access(2) call). Exposed for tests + callers that want
    /// to gate UI on availability.
    static bool local_recovery_available();

    /// Resolved absolute path to the local recovery script. Empty when no
    /// installer-managed `INSTALL_DIR` could be derived (dev builds run from
    /// the source tree). Exposed for testing.
    static std::string local_recovery_script_path();

  private:
    /// Step 2 in the chain: fork+execvp the platform recovery script. Submits
    /// to the HttpExecutor slow lane so the worker doesn't block on the
    /// process wait (which can take ~30s on K2's klipper_mcu bounce).
    /// Callbacks are dispatched on the main thread via queue_update().
    ///
    /// Static — doesn't touch any instance state and the recover() chain
    /// captures lambdas that outlive `this` (the holding widget can be
    /// destroyed mid-chain; [L072]).
    static void run_local_recovery(uint32_t timeout_ms, SuccessCallback on_success,
                                   ErrorCallback on_error);

    struct RestartVerification;

    IMoonrakerAPI* api_;
    RecoveryTiming timing_;
};

namespace detail {
/// Script path local_recovery_script_path() returns instead of the installed
/// one when non-empty. The test binary has no install root, which would leave
/// the local-exec step permanently dark.
inline std::string& local_recovery_script_override_ref() {
    static std::string path;
    return path;
}
} // namespace detail

} // namespace helix
