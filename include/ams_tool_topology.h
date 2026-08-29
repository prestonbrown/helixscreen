// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend.h"
#include "tool_state.h"

#include <optional>

namespace helix {

/**
 * @brief Build a ToolTopology from a backend that owns a tool->slot table.
 *
 * Returns nullopt when the backend owns no such table, which is the signal to
 * leave ToolState enumerating extruders instead. That is the right answer for a
 * machine whose tools ARE extruders: the Snapmaker U1 carries out every remap
 * the user picks, through its pre-print send, and still owns no table to adopt.
 * Gating this on remap capability would hand ToolState a four-lane topology for
 * a machine with four independent extruders.
 *
 * Falls back to a 1:1 mapping from the slot count when a table-owning backend
 * returns an empty vector.
 *
 * Declared here rather than left file-static in ams_state.cpp so the gate can be
 * tested against each backend directly. It is otherwise reachable only through a
 * full AmsState sync, which is why nothing pinned which backends it fires for.
 */
[[nodiscard]] std::optional<ToolTopology> build_ams_topology(AmsBackend* backend,
                                                             int backend_index);

} // namespace helix
