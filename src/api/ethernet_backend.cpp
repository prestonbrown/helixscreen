// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_backend.h"

#ifdef HELIX_ENABLE_MOCKS
#include "ethernet_backend_mock.h"
#endif
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#ifdef __APPLE__
#include "ethernet_backend_macos.h"
#elif !defined(__ANDROID__)
#include "ethernet_backend_linux.h"
#include "ethernet_backend_netd.h"
#include "netd_protocol.h"
#endif

std::unique_ptr<EthernetBackend> EthernetBackend::create() {
    // In test mode, always use mock unless --real-ethernet was specified
#ifdef HELIX_ENABLE_MOCKS
    if (get_runtime_config()->should_mock_ethernet()) {
        spdlog::debug("[EthernetBackend] Test mode: using mock backend");
        return std::make_unique<EthernetBackendMock>();
    }
#endif

#ifdef __APPLE__
    // macOS: Use native backend (handles missing interface gracefully)
    spdlog::debug("[EthernetBackend] Creating macOS backend");
    auto backend = std::make_unique<EthernetBackendMacOS>();

    if (backend->has_interface()) {
        spdlog::debug("[EthernetBackend] macOS backend initialized (interface found)");
    } else {
        spdlog::info("[EthernetBackend] No Ethernet interface found");
    }
    return backend;
#elif defined(__ANDROID__)
    // Android: Ethernet managed by the OS
    spdlog::info("[EthernetBackend] Android platform - Ethernet not managed natively");
    return nullptr;
#else
    // Linux: when the printer's network daemon owns the network, interface
    // truth comes from it (a daemon enforcing single-transport downs eth0,
    // which ioctls then misread as "no cable"); otherwise ask the OS.
    if (helix::netd::available()) {
        spdlog::debug("[EthernetBackend] Creating netd backend");
        auto backend = std::make_unique<EthernetBackendNetd>();

        if (backend->has_interface()) {
            spdlog::debug("[EthernetBackend] netd backend initialized (interface found)");
        } else {
            spdlog::info("[EthernetBackend] No Ethernet interface found");
        }
        return backend;
    }

    // Linux: Use native backend (handles missing interface gracefully)
    spdlog::debug("[EthernetBackend] Creating Linux backend");
    auto backend = std::make_unique<EthernetBackendLinux>();

    if (backend->has_interface()) {
        spdlog::debug("[EthernetBackend] Linux backend initialized (interface found)");
    } else {
        spdlog::info("[EthernetBackend] No Ethernet interface found");
    }
    return backend;
#endif
}
