// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "i_moonraker_client.h"
#include "moonraker_events.h"
#include "moonraker_manager.h"

#include <memory>

namespace helix {

class MoonrakerManagerTestAccess {
  public:
    static void present_event(MoonrakerManager& mgr, const MoonrakerEvent& evt) {
        mgr.present_event(evt);
    }

    /// Makes `client` the manager's transport and marks it initialized, as init() would,
    /// without building a mock printer or an API behind it.
    static void install_client(MoonrakerManager& mgr, std::unique_ptr<IMoonrakerClient> client) {
        mgr.m_client = std::move(client);
        mgr.m_initialized = true;
    }
};

} // namespace helix
