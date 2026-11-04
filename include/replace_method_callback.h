// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "i_moonraker_client.h"
#include "json_fwd.h"

#include <functional>
#include <string>
#include <utility>

namespace helix {

/// Subscribe @p cb as (@p method, @p handler_name), dropping a handler already
/// registered under that name. The client keeps the FIRST registration of a repeated
/// name, so a plain re-register on a reconnect would leave the earlier handler, and
/// whatever it captured, live. Features that re-attach on every discovery go through
/// this, which makes attach idempotent.
inline void replace_method_callback(IMoonrakerClient& client, const std::string& method,
                                    const std::string& handler_name,
                                    std::function<void(const nlohmann::json&)> cb) {
    client.unregister_method_callback(method, handler_name);
    client.register_method_callback(method, handler_name, std::move(cb));
}

} // namespace helix
