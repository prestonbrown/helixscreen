// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "print_history_data.h"

#include "hv/json.hpp"

namespace helix {

/// Parse one job object out of a Moonraker history payload.
///
/// `server.history.list` entries and the `job` record attached to a
/// notify_history_changed notification carry the identical schema, including
/// the `exists` flag Moonraker recomputes against the file manager for both.
/// Declared here so the notification path folds a job into the cache through
/// the same parser the list response goes through, rather than a second
/// spelling that would drift field by field.
///
/// Every field is read through json_util::safe_* or a null-checked numeric
/// accessor: nlohmann's .value() throws type_error.302 on a present-but-null
/// field, and Moonraker writes a null `filename` for a job whose source file
/// was deleted.
///
/// Runs on the WebSocket thread in both callers, and touches no LVGL state.
[[nodiscard]] PrintHistoryJob parse_history_job(const nlohmann::json& job_json);

} // namespace helix
