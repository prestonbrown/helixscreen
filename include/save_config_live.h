// SPDX-License-Identifier: GPL-3.0-or-later

/// @file save_config_live.h
/// @brief Whether a SAVE_CONFIG may skip the Klipper restart, from what it is saving.
///
/// A firmware whose SAVE_CONFIG takes RESTART=0
/// (PrinterDiscovery::save_config_takes_restart_param()) writes printer.cfg and
/// stays up, but writing the file changes nothing in the running Klipper: a
/// value it reads once at startup (a probe's z_offset, an endstop position)
/// keeps its old value until a restart. Skipping the restart is only safe when
/// every pending item (configfile.save_config_pending_items) is already in
/// effect, because the command that staged it also applied it. The rows that
/// say so, each citing the firmware code that applies it live, are in
/// src/printer/save_config_live.cpp. Anything else restarts.

#pragma once

#include "json_fwd.h"

namespace helix {

class PrinterDiscovery;

namespace save_config {

/// True when this save must restart Klipper: always on a firmware without
/// RESTART=0, and otherwise unless every item in @p pending_items
/// ({section: {option: value} or null for a removed section}) is already live.
bool restart_required(const PrinterDiscovery& hw, const nlohmann::json& pending_items);

} // namespace save_config
} // namespace helix
