// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_plugin.cpp
 * @brief Plugin core — C ABI exports for init/deinit/info/error.
 *
 * All exported functions use extern "C" to match bluetooth_plugin.h typedefs.
 * Logging uses fprintf(stderr) — NOT spdlog (plugin is a standalone .so).
 */

#include "bluetooth_plugin.h"
#include "bt_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Shared helper: MAC to D-Bus path
// ---------------------------------------------------------------------------

std::string helix::bluetooth::device_dbus_path(sd_bus* bus, const char* mac) {
    std::string path = find_adapter_path(bus);
    if (path.empty())
        path = "/org/bluez/hci0";
    path += "/dev_";
    if (mac) {
        std::string m(mac);
        std::replace(m.begin(), m.end(), ':', '_');
        path += m;
    }
    return path;
}

// ---------------------------------------------------------------------------
// Plugin info
// ---------------------------------------------------------------------------

static helix_bt_plugin_info plugin_info = {
    .api_version = HELIX_BT_API_VERSION,
    .name = "helix-bluetooth",
    .has_classic = true,
    .has_ble = true,
};

extern "C" helix_bt_plugin_info* helix_bt_get_info(void) {
    return &plugin_info;
}

// ---------------------------------------------------------------------------
// Init / Deinit
// ---------------------------------------------------------------------------

extern "C" helix_bt_context* helix_bt_init(void) {
    auto* ctx = new (std::nothrow) helix_bt_context;
    if (!ctx) {
        fprintf(stderr, "[bt] failed to allocate context\n");
        return nullptr;
    }

    int r = sd_bus_open_system(&ctx->bus);
    if (r < 0) {
        fprintf(stderr, "[bt] failed to open system bus: %s\n", strerror(-r));
        delete ctx;
        return nullptr;
    }

    // Set a 5-second D-Bus method call timeout (default is 25 seconds which
    // freezes the UI when is_paired/is_connected checks are called synchronously)
    sd_bus_set_method_call_timeout(ctx->bus, 5 * 1000000ULL); // microseconds

    ctx->bus_thread = std::make_unique<helix::bluetooth::BusThread>(ctx->bus);
    ctx->bus_thread->start();

    // Register a BlueZ agent so that Pair() can exchange link keys.
    // Without an agent, "Just Works" pairing completes at the protocol level
    // but doesn't bond — the kernel's input plugin then refuses HID.
    helix_bt_register_agent(ctx);

    fprintf(stderr, "[bt] plugin initialized (api_version=%d)\n", HELIX_BT_API_VERSION);
    return ctx;
}

extern "C" void helix_bt_deinit(helix_bt_context* ctx) {
    if (!ctx)
        return;

    // Unregister the pairing agent before tearing down the bus thread.
    helix_bt_unregister_agent(ctx);

    // Route any remaining sd_bus_slot_unref calls through the bus thread
    // BEFORE stopping it — all sd_bus_* calls must happen on the bus thread.
    if (ctx->bus_thread) {
        try {
            ctx->bus_thread->run_sync([ctx](sd_bus* /*bus*/) {
                std::lock_guard<std::mutex> lock(ctx->ble_mutex);
                for (auto& conn : ctx->ble_connections) {
                    if (conn && conn->notify_slot) {
                        sd_bus_slot_unref(conn->notify_slot);
                        conn->notify_slot = nullptr;
                    }
                }
            });
        } catch (const std::exception&) {
            // Best-effort — if the thread already stopped, slots get cleaned up
            // by bus teardown.
        }

        if (ctx->discovery_slot) {
            try {
                ctx->bus_thread->run_sync([ctx](sd_bus* /*bus*/) {
                    if (ctx->discovery_slot) {
                        sd_bus_slot_unref(ctx->discovery_slot);
                        ctx->discovery_slot = nullptr;
                    }
                });
            } catch (const std::exception&) {
            }
        }

        ctx->bus_thread->stop();
        ctx->bus_thread.reset();
    }

    // Close BLE acquired fds (slots were already freed on the bus thread above).
    {
        std::lock_guard<std::mutex> lock(ctx->ble_mutex);
        for (auto& conn : ctx->ble_connections) {
            if (!conn)
                continue;
            if (conn->acquired_fd >= 0) {
                close(conn->acquired_fd);
                conn->acquired_fd = -1;
            }
            if (conn->notify_fd >= 0) {
                close(conn->notify_fd);
                conn->notify_fd = -1;
            }
            conn->active = false;
        }
        ctx->ble_connections.clear();
    }

    // Close tracked RFCOMM fds
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (int fd : ctx->rfcomm_fds) {
            close(fd);
        }
        ctx->rfcomm_fds.clear();
    }

    if (ctx->bus) {
        sd_bus_flush_close_unref(ctx->bus);
        ctx->bus = nullptr;
    }

    fprintf(stderr, "[bt] plugin deinitialized\n");
    delete ctx;
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

extern "C" const char* helix_bt_last_error(helix_bt_context* ctx) {
    if (!ctx)
        return "null context";
    std::lock_guard<std::mutex> lock(ctx->mutex);
    return ctx->last_error.c_str();
}
