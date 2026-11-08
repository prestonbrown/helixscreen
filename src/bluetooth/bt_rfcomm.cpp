// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_rfcomm.cpp
 * @brief RFCOMM socket connection for Bluetooth Classic SPP.
 *
 * Used by Brother QL printers that expose an SPP serial port profile.
 * Returns an fd that can be written to directly (same as a TCP socket).
 */

#include "bluetooth_plugin.h"
#include "bt_context.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

extern "C" int helix_bt_connect_rfcomm(helix_bt_context* ctx, const char* mac, int channel) {
    if (!ctx)
        return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }

    fprintf(stderr, "[bt] RFCOMM connecting to %s channel %d\n", mac, channel);

    int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (fd < 0) {
        int err = errno;
        fprintf(stderr, "[bt] socket() failed: %s\n", strerror(err));
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = std::string("socket() failed: ") + strerror(err);
        return -err;
    }

    struct timeval tv = {};
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_rc addr = {};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = static_cast<uint8_t>(channel);
    str2ba(mac, &addr.rc_bdaddr);

    int r = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (r < 0) {
        int err = errno;
        fprintf(stderr, "[bt] RFCOMM connect failed for %s: %s\n", mac, strerror(err));
        close(fd);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = std::string("RFCOMM connect failed: ") + strerror(err);
        return -err;
    }

    if (fd & helix::bluetooth::kBleHandleTag) {
        close(fd);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "RFCOMM fd collides with the BLE handle range";
        return -EMFILE;
    }

    // Track the fd for safe cleanup on deinit
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->rfcomm_fds.insert(fd);
    }

    fprintf(stderr, "[bt] RFCOMM connected to %s (fd=%d)\n", mac, fd);
    return fd;
}
