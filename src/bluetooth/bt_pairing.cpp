// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_pairing.cpp
 * @brief BlueZ device pairing via D-Bus.
 *
 * Implements helix_bt_pair() and helix_bt_is_paired() using
 * org.bluez.Device1.Pair() and the Device1.Paired property.
 *
 * Most label printers use "Just Works" pairing (no PIN required).
 */

#include "bluetooth_plugin.h"
#include "bt_context.h"

#include <cstdio>
#include <cstring>

/// Mark a device as trusted so future connections don't require re-authorization
static void trust_device(sd_bus* bus, const char* path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r =
        sd_bus_set_property(bus, "org.bluez", path, "org.bluez.Device1", "Trusted", &error, "b", 1);
    if (r < 0) {
        fprintf(stderr, "[bt] failed to set Trusted on %s: %s\n", path,
                error.message ? error.message : strerror(-r));
    } else {
        fprintf(stderr, "[bt] device %s marked as trusted\n", path);
    }
    sd_bus_error_free(&error);
}

extern "C" int helix_bt_pair(helix_bt_context* ctx, const char* mac) {
    if (!ctx)
        return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }
    if (!ctx->bus || !ctx->bus_thread) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    std::string path = mac_to_dbus_path(mac);
    fprintf(stderr, "[bt] pair: starting for %s (dbus=%s)\n", mac, path.c_str());

    int r = 0;
    std::string err;

    try {
        ctx->bus_thread->run_sync([&](sd_bus* bus) {
            // Log device properties pre-pair for diagnostics
            {
                sd_bus_error pe = SD_BUS_ERROR_NULL;
                int paired_pre = 0, connected_pre = 0, trusted_pre = 0, bonded_pre = 0;
                sd_bus_get_property_trivial(bus, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Paired", &pe, 'b', &paired_pre);
                sd_bus_error_free(&pe);
                pe = SD_BUS_ERROR_NULL;
                sd_bus_get_property_trivial(bus, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Connected", &pe, 'b', &connected_pre);
                sd_bus_error_free(&pe);
                pe = SD_BUS_ERROR_NULL;
                sd_bus_get_property_trivial(bus, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Trusted", &pe, 'b', &trusted_pre);
                sd_bus_error_free(&pe);
                pe = SD_BUS_ERROR_NULL;
                sd_bus_get_property_trivial(bus, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Bonded", &pe, 'b', &bonded_pre);
                sd_bus_error_free(&pe);
                fprintf(stderr,
                        "[bt] pair: pre-state for %s: paired=%d connected=%d "
                        "trusted=%d bonded=%d\n",
                        mac, paired_pre, connected_pre, trusted_pre, bonded_pre);
            }

            // Try Pair() first (Classic SPP devices)
            sd_bus_error error = SD_BUS_ERROR_NULL;
            r = sd_bus_call_method(bus, "org.bluez", path.c_str(), "org.bluez.Device1", "Pair",
                                   &error, nullptr, "");
            // HID profile UUID. Explicit ConnectProfile(HID) is required for
            // dual-profile HID scanners that also advertise SPP — bare
            // Device1.Connect() lets BlueZ pick the primary profile, which
            // can end up being SPP (succeeds cleanly but HID never attaches,
            // so the kernel creates no evdev node).
            static constexpr const char* HID_UUID = "00001124-0000-1000-8000-00805f9b34fb";
            auto try_connect_profile = [&](sd_bus* b) {
                // Try HID-specific ConnectProfile first. If the device
                // doesn't advertise HID, BlueZ returns NotSupported /
                // DoesNotExist — fall back to bare Connect() for
                // non-HID devices (label printers etc.).
                sd_bus_error hidErr = SD_BUS_ERROR_NULL;
                fprintf(stderr, "[bt] pair: trying ConnectProfile(HID) for %s\n", mac);
                int hr = sd_bus_call_method(b, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "ConnectProfile", &hidErr, nullptr, "s", HID_UUID);
                if (hr >= 0) {
                    fprintf(stderr, "[bt] pair: ConnectProfile(HID) succeeded for %s\n", mac);
                    sd_bus_error_free(&hidErr);
                    return;
                }
                if (sd_bus_error_has_name(&hidErr, "org.bluez.Error.AlreadyConnected")) {
                    fprintf(stderr, "[bt] pair: HID already connected on %s\n", mac);
                    sd_bus_error_free(&hidErr);
                    return;
                }
                bool is_no_hid = sd_bus_error_has_name(&hidErr, "org.bluez.Error.NotSupported") ||
                                 sd_bus_error_has_name(&hidErr, "org.bluez.Error.DoesNotExist");
                fprintf(stderr,
                        "[bt] pair: ConnectProfile(HID) failed for %s: err_name=%s msg=%s "
                        "(is_no_hid=%d)\n",
                        mac, hidErr.name ? hidErr.name : "(null)",
                        hidErr.message ? hidErr.message : strerror(-hr), is_no_hid);
                sd_bus_error_free(&hidErr);

                sd_bus_error cerr = SD_BUS_ERROR_NULL;
                fprintf(stderr, "[bt] pair: falling back to bare Connect() for %s\n", mac);
                int cr = sd_bus_call_method(b, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Connect", &cerr, nullptr, "");
                if (cr >= 0) {
                    fprintf(stderr, "[bt] pair: Connect() succeeded for %s\n", mac);
                } else if (sd_bus_error_has_name(&cerr, "org.bluez.Error.AlreadyConnected")) {
                    fprintf(stderr, "[bt] pair: device %s already connected\n", mac);
                } else {
                    fprintf(stderr, "[bt] pair: Connect() failed for %s: err_name=%s msg=%s\n", mac,
                            cerr.name ? cerr.name : "(null)",
                            cerr.message ? cerr.message : strerror(-cr));
                }
                sd_bus_error_free(&cerr);
            };

            fprintf(stderr, "[bt] pair: Pair() returned r=%d for %s (err_name=%s msg=%s)\n", r, mac,
                    error.name ? error.name : "(none)", error.message ? error.message : "(none)");

            if (r >= 0) {
                sd_bus_error_free(&error);
                trust_device(bus, path.c_str());
                fprintf(stderr, "[bt] pair: paired successfully with %s\n", mac);
                // Classic HID devices need Connect() after Pair() to bring up
                // the HID profile so the kernel creates an evdev node.
                try_connect_profile(bus);
                r = 0;
                return;
            }

            // AlreadyExists means already paired — treat as success
            if (sd_bus_error_has_name(&error, "org.bluez.Error.AlreadyExists")) {
                fprintf(stderr, "[bt] device %s already paired\n", mac);
                trust_device(bus, path.c_str());
                sd_bus_error_free(&error);
                try_connect_profile(bus);
                r = 0;
                return;
            }

            // BLE devices often don't support traditional Pair() — fall back to Connect()
            // which implicitly bonds on BLE devices. Different BlueZ versions return different
            // errors (UnknownMethod, NotAvailable, BadMessage, ConnectionTimeout), so try
            // Connect() on any Pair() failure.
            fprintf(stderr,
                    "[bt] pair: Pair() failed for %s (err_name=%s msg=%s), "
                    "trying Connect() (BLE fallback)\n",
                    mac, error.name ? error.name : "(null)",
                    error.message ? error.message : strerror(-r));
            sd_bus_error_free(&error);

            sd_bus_error error2 = SD_BUS_ERROR_NULL;
            r = sd_bus_call_method(bus, "org.bluez", path.c_str(), "org.bluez.Device1", "Connect",
                                   &error2, nullptr, "");
            fprintf(stderr, "[bt] pair: Connect() returned r=%d for %s (err_name=%s msg=%s)\n", r,
                    mac, error2.name ? error2.name : "(none)",
                    error2.message ? error2.message : "(none)");

            if (r >= 0) {
                sd_bus_error_free(&error2);
                trust_device(bus, path.c_str());
                fprintf(stderr, "[bt] pair: connected (BLE) successfully with %s\n", mac);
                r = 0;
                return;
            }

            // AlreadyConnected is also success for our purposes
            if (sd_bus_error_has_name(&error2, "org.bluez.Error.AlreadyConnected")) {
                fprintf(stderr, "[bt] pair: device %s already connected\n", mac);
                trust_device(bus, path.c_str());
                sd_bus_error_free(&error2);
                r = 0;
                return;
            }

            fprintf(stderr, "[bt] pair: both Pair() and Connect() failed for %s\n", mac);
            err = error2.message ? error2.message : "connect failed";
            sd_bus_error_free(&error2);
        });
    } catch (const std::exception& e) {
        err = e.what();
        r = -EIO;
    }

    if (r < 0) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = err.empty() ? strerror(-r) : err;
    }
    return r;
}

extern "C" int helix_bt_is_connected(helix_bt_context* ctx, const char* mac) {
    if (!ctx)
        return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }
    if (!ctx->bus || !ctx->bus_thread) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    std::string path = mac_to_dbus_path(mac);
    int r = 0;
    int connected = 0;
    std::string err;

    try {
        ctx->bus_thread->run_sync([&](sd_bus* bus) {
            sd_bus_error error = SD_BUS_ERROR_NULL;
            r = sd_bus_get_property_trivial(bus, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Connected", &error, 'b', &connected);
            if (r < 0) {
                fprintf(stderr, "[bt] is_connected check failed for %s: %s\n", mac,
                        error.message ? error.message : strerror(-r));
                err = error.message ? error.message : "failed to read Connected property";
            }
            sd_bus_error_free(&error);
        });
    } catch (const std::exception& e) {
        err = e.what();
        r = -EIO;
    }

    if (r < 0) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = err.empty() ? strerror(-r) : err;
        return r;
    }
    return connected ? 1 : 0;
}

extern "C" int helix_bt_is_bonded(helix_bt_context* ctx, const char* mac) {
    if (!ctx)
        return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }
    if (!ctx->bus || !ctx->bus_thread) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    std::string path = mac_to_dbus_path(mac);
    int r = 0;
    int bonded = 0;
    std::string err;

    try {
        ctx->bus_thread->run_sync([&](sd_bus* bus) {
            sd_bus_error error = SD_BUS_ERROR_NULL;
            r = sd_bus_get_property_trivial(bus, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Bonded", &error, 'b', &bonded);
            if (r < 0) {
                fprintf(stderr, "[bt] is_bonded check failed for %s: %s\n", mac,
                        error.message ? error.message : strerror(-r));
                err = error.message ? error.message : "failed to read Bonded property";
            }
            sd_bus_error_free(&error);
        });
    } catch (const std::exception& e) {
        err = e.what();
        r = -EIO;
    }

    if (r < 0) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = err.empty() ? strerror(-r) : err;
        return r;
    }
    return bonded ? 1 : 0;
}

extern "C" int helix_bt_is_paired(helix_bt_context* ctx, const char* mac) {
    if (!ctx)
        return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }
    if (!ctx->bus || !ctx->bus_thread) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    std::string path = mac_to_dbus_path(mac);
    int r = 0;
    int paired = 0;
    std::string err;

    try {
        ctx->bus_thread->run_sync([&](sd_bus* bus) {
            sd_bus_error error = SD_BUS_ERROR_NULL;
            r = sd_bus_get_property_trivial(bus, "org.bluez", path.c_str(), "org.bluez.Device1",
                                            "Paired", &error, 'b', &paired);
            if (r < 0) {
                fprintf(stderr, "[bt] is_paired check failed for %s: %s\n", mac,
                        error.message ? error.message : strerror(-r));
                err = error.message ? error.message : "failed to read Paired property";
            }
            sd_bus_error_free(&error);
        });
    } catch (const std::exception& e) {
        err = e.what();
        r = -EIO;
    }

    if (r < 0) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = err.empty() ? strerror(-r) : err;
        return r;
    }
    return paired ? 1 : 0;
}

extern "C" int helix_bt_remove_device(helix_bt_context* ctx, const char* mac) {
    if (!ctx)
        return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }
    if (!ctx->bus || !ctx->bus_thread) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    // RemoveDevice is a method on the adapter (org.bluez.Adapter1), taking
    // the device's object path as its single argument.
    std::string device_path = mac_to_dbus_path(mac);
    const char* adapter_path = "/org/bluez/hci0";
    fprintf(stderr, "[bt] removing device %s (%s)\n", mac, device_path.c_str());

    int r = 0;
    std::string err;

    // BlueZ's default sd_bus call timeout is 25s; RemoveDevice can hang that
    // long when the device is mid-connect or has an active GATT link (BLE).
    // Disconnect first (best-effort, NotConnected = success), then RemoveDevice
    // with a tighter timeout so the UI fails fast instead of stalling.
    constexpr uint64_t CALL_TIMEOUT_US = 5'000'000; // 5s

    auto call_with_timeout = [](sd_bus* bus, const char* path, const char* iface,
                                const char* method, sd_bus_error* error, const char* signature,
                                const char* arg) -> int {
        sd_bus_message* m = nullptr;
        int rc = sd_bus_message_new_method_call(bus, &m, "org.bluez", path, iface, method);
        if (rc < 0)
            return rc;
        if (signature && arg) {
            rc = sd_bus_message_append(m, signature, arg);
            if (rc < 0) {
                sd_bus_message_unref(m);
                return rc;
            }
        }
        sd_bus_message* reply = nullptr;
        rc = sd_bus_call(bus, m, CALL_TIMEOUT_US, error, &reply);
        sd_bus_message_unref(m);
        if (reply)
            sd_bus_message_unref(reply);
        return rc;
    };

    try {
        ctx->bus_thread->run_sync([&](sd_bus* bus) {
            // Best-effort disconnect first — ignore NotConnected / DoesNotExist /
            // any other failure. If the device has an active link, dropping it
            // here unblocks RemoveDevice; if it's already disconnected or the
            // object path is gone, BlueZ returns instantly.
            {
                sd_bus_error derr = SD_BUS_ERROR_NULL;
                int dr = call_with_timeout(bus, device_path.c_str(), "org.bluez.Device1",
                                           "Disconnect", &derr, nullptr, nullptr);
                if (dr < 0) {
                    fprintf(stderr, "[bt] pre-remove Disconnect for %s: %s (continuing)\n", mac,
                            derr.message ? derr.message : strerror(-dr));
                }
                sd_bus_error_free(&derr);
            }

            sd_bus_error error = SD_BUS_ERROR_NULL;
            r = call_with_timeout(bus, adapter_path, "org.bluez.Adapter1", "RemoveDevice", &error,
                                  "o", device_path.c_str());
            if (r < 0) {
                fprintf(stderr, "[bt] RemoveDevice failed for %s: %s\n", mac,
                        error.message ? error.message : strerror(-r));
                err = error.message ? error.message : "RemoveDevice failed";
            }
            sd_bus_error_free(&error);
        });
    } catch (const std::exception& e) {
        err = e.what();
        r = -EIO;
    }

    if (r < 0) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = err.empty() ? strerror(-r) : err;
        return r;
    }

    fprintf(stderr, "[bt] device %s removed successfully\n", mac);
    return 0;
}
