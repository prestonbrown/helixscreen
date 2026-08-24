// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_error.h"

#include <functional>
#include <string>

class IMoonrakerAPI;

namespace helix {

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
///      what users expect to see happen.
///   2. **Local exec** of `$INSTALL_DIR/bin/helix-recover.sh` (this class
///      `fork()` + `execvp()`s it). The per-platform installer
///      (`scripts/lib/installer/recovery.sh`) writes the script with the
///      right `/etc/init.d/...` invocation baked in. Works even when
///      klippy is fully dead — that's the whole point. No-op on platforms
///      where the installer didn't ship a script (stock systemd Pi etc.).
///   3. `machine.services.restart klipper` — last resort. Only works on
///      hosts where Moonraker's `[machine] provider` is wired to a real
///      service manager (systemd_dbus / systemd_cli / supervisord_cli).
///
/// Frontend stays platform-blind: this class never learns platform names.
/// All platform knowledge lives in the shipped helix-recover.sh.
///
/// Lifetime: stateless free-standing helper, owned wherever it's invoked from
/// (typically the EmergencyStopOverlay or a toast action callback).
class PrinterRecoveryService {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    explicit PrinterRecoveryService(IMoonrakerAPI* api) : api_(api) {}

    /// Run the full recovery chain (see class doc).
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
    static void run_local_recovery(SuccessCallback on_success, ErrorCallback on_error);

    IMoonrakerAPI* api_;
};

} // namespace helix
