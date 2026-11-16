// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

#include <string>

namespace helix {

class IMoonrakerClient;

namespace batch_feeding {

/// Clears the macro's `doing` interlock and restores the hotend targets the
/// batch snapshotted at its START. The command alias is uppercased regardless
/// of the config's spelling; the status object key is not (see
/// macro_config_name()). Every site that ends a firmware batch sends this, so
/// a copy drifting strands the interlock.
inline constexpr const char* kEndGcode = "AUTO_FEEDING_BATCH ACTION=END";

/**
 * @brief Connect-time cleanup of a stranded AUTO_FEEDING_BATCH interlock
 *
 * A session that dies mid-batch leaves the macro's `doing` save-variable set,
 * and PRINT_PRESTART_CHECK refuses every print over it. Sends ACTION=END when
 * the discovery snapshot shows `doing` true with no print in flight and every
 * filament_feed channel at rest; a printing or paused print, an active
 * virtual_sdcard, or a channel mid-load/unload (a batch another client is
 * running) owns the interlock legitimately, because ACTION=END restores the
 * hotend targets the batch snapshotted at START - the right cleanup when
 * idle, the wrong targets mid-print or mid-feed. A snapshot without feed
 * channels cannot vouch for that and is left alone. Runs on the WebSocket
 * thread beside the screws-tilt reconcile; touches no LVGL.
 *
 * @param macro_object  The macro's status object key as written in
 *        printer.cfg ("gcode_macro <name>"). Klipper preserves the config's
 *        case in object keys, so the caller supplies the discovered spelling
 *        (PrinterDiscovery::macro_config_name()) rather than a guessed one.
 * @param local_batch_active  Whether THIS process has a batch it dispatched
 *        and has not seen complete (AmsBackend::filament_batch_in_flight()).
 *        True means the interlock belongs to a live batch of ours and is
 *        left alone. False after a process restart mid-batch, where the
 *        reconcile clears an interlock the still-running batch owns —
 *        accepted, since the restart already orphaned the plan that tracked
 *        it, and refusing would strand the interlock forever.
 */
void reconcile_on_connect(IMoonrakerClient& client, const nlohmann::json& initial_status,
                          const std::string& macro_object, bool local_batch_active = false);

} // namespace batch_feeding
} // namespace helix
