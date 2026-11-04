// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_backend.h"

#ifdef HELIX_ENABLE_MOCKS
#include "usb_backend_mock.h"
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#include "usb_backend_linux.h"
#endif

#include <spdlog/spdlog.h>

std::unique_ptr<UsbBackend> UsbBackend::create(bool force_mock) {
#ifdef HELIX_ENABLE_MOCKS
    if (force_mock) {
        spdlog::debug("[UsbBackend] Creating mock backend (force_mock=true)");
        return std::make_unique<UsbBackendMock>();
    }
#else
    (void)force_mock;
#endif

#if defined(__linux__) && !defined(__ANDROID__)
    // Returned unstarted: the owner attaches its event callback first, and
    // UsbManager::start() reports a backend that fails to start.
    spdlog::debug("[UsbBackend] Linux platform detected - using native backend");
    return std::make_unique<UsbBackendLinux>();
#elif defined(__APPLE__)
    // macOS: No native USB backend implemented
    spdlog::info("[UsbBackend] macOS platform - USB support not available");
    return nullptr;
#else
    // Android and other unsupported platforms
    spdlog::info("[UsbBackend] Platform does not support native USB");
    return nullptr;
#endif
}
