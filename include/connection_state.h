// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {

/**
 * @brief Connection state for the Moonraker transport
 *
 * Lives in its own header so UI/observer code can bind to connection state
 * without including the concrete client (which drags libhv on desktop).
 */
enum class ConnectionState {
    DISCONNECTED, // Not connected
    CONNECTING,   // Connection in progress
    CONNECTED,    // Connected and ready
    RECONNECTING, // Automatic reconnection in progress
    FAILED        // Connection failed (max retries exceeded)
};

} // namespace helix
