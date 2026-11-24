// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file bluetooth_plugin.h
 * @brief C ABI for the Bluetooth plugin shared library (libhelix-bluetooth.so)
 *
 * This header defines the interface between the main HelixScreen binary and the
 * optional Bluetooth plugin. The main binary loads the plugin via dlopen() and
 * resolves these function pointers via dlsym().
 *
 * The plugin handles all BlueZ/D-Bus interaction — the main binary never links
 * libsystemd or libbluetooth for Bluetooth support.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HELIX_BT_API_VERSION 3

/// Plugin metadata returned by helix_bt_get_info()
typedef struct {
    int api_version;
    const char* name;
    bool has_classic; ///< Supports SPP/RFCOMM
    bool has_ble;     ///< Supports BLE GATT
} helix_bt_plugin_info;

/// Discovered Bluetooth device
typedef struct {
    const char* mac;  ///< "AA:BB:CC:DD:EE:FF"
    const char* name; ///< Human-readable name
    bool paired;
    bool is_ble;              ///< false=Classic, true=BLE
    const char* service_uuid; ///< Primary service UUID (SPP or vendor)
    bool is_scanner;          ///< true if device is a HID barcode scanner, not a printer
} helix_bt_device;

/// Opaque context — allocated by plugin, freed by plugin
typedef struct helix_bt_context helix_bt_context;

/// Discovery callback — called per device found. user_data is pass-through.
typedef void (*helix_bt_discover_cb)(const helix_bt_device* dev, void* user_data);

// Function pointer typedefs for dlsym() resolution

/// Return plugin metadata. Must return api_version == HELIX_BT_API_VERSION.
typedef helix_bt_plugin_info* (*helix_bt_get_info_fn)(void);

/// Initialize plugin. Starts internal sd-bus event loop thread.
/// Returns context on success, NULL on failure.
typedef helix_bt_context* (*helix_bt_init_fn)(void);

/// Shut down plugin. Stops event loop, frees resources.
typedef void (*helix_bt_deinit_fn)(helix_bt_context*);

/// Start discovery. Calls cb per device found. Stops after timeout_ms, once *cancel becomes
/// nonzero (read atomically; null for none), or on stop_discovery(). Scans on one context run
/// one at a time, and a cancel set before this scan starts ends it at once. cancel ends only
/// this scan; stop_discovery() ends every scan on the context.
/// Returns 0 on success (including cancelled), negative on error.
typedef int (*helix_bt_discover_fn)(helix_bt_context*, int timeout_ms, helix_bt_discover_cb cb,
                                    void* user_data, const int* cancel);

/// Stop every in-progress or queued discovery on the context early.
typedef void (*helix_bt_stop_discovery_fn)(helix_bt_context*);

/// Enumerate devices BlueZ already knows about (paired or previously seen) without
/// starting an active radio scan. Calls cb once per matching device, then returns.
/// Returns 0 on success, negative on error.
typedef int (*helix_bt_enumerate_known_fn)(helix_bt_context*, helix_bt_discover_cb cb,
                                           void* user_data);

/// Pair with a device. Blocks until pairing completes or fails.
/// Returns 0 on success, negative on error.
typedef int (*helix_bt_pair_fn)(helix_bt_context*, const char* mac);

/// Check if device is paired. Returns 1=paired, 0=not paired, negative=error.
typedef int (*helix_bt_is_paired_fn)(helix_bt_context*, const char* mac);

/// Check if device is bonded (paired with a persistent Long-Term Key).
/// Just-Works SSP can produce paired-but-not-bonded sessions, which BlueZ's
/// input plugin rejects for HID profiles (hidp_add_connection "!bonded").
/// Returns 1=bonded, 0=not bonded, negative=error.
typedef int (*helix_bt_is_bonded_fn)(helix_bt_context*, const char* mac);

/// Check if device is connected. Returns 1=connected, 0=not connected, negative=error.
typedef int (*helix_bt_is_connected_fn)(helix_bt_context*, const char* mac);

/// Connect via RFCOMM (SPP). Returns fd on success, negative on error.
typedef int (*helix_bt_connect_rfcomm_fn)(helix_bt_context*, const char* mac, int channel);

/// Connect via BLE GATT. Returns handle on success, negative on error.
typedef int (*helix_bt_connect_ble_fn)(helix_bt_context*, const char* mac, const char* write_uuid);

/// Write data to BLE GATT characteristic. Handles chunking to MTU internally.
/// Returns 0 on success, negative on error.
typedef int (*helix_bt_ble_write_fn)(helix_bt_context*, int handle, const uint8_t* data, int len);

/// Read data from BLE GATT notifications. Blocks up to timeout_ms.
/// Returns bytes read on success, 0 on timeout, negative on error.
typedef int (*helix_bt_ble_read_fn)(helix_bt_context*, int handle, uint8_t* buf, int buf_len,
                                    int timeout_ms);

/// Disconnect a connection (RFCOMM fd or BLE handle).
typedef void (*helix_bt_disconnect_fn)(helix_bt_context*, int handle);

/// Get human-readable error string for last failure.
typedef const char* (*helix_bt_last_error_fn)(helix_bt_context*);

/// Remove (unpair) a device from the adapter. Returns 0 on success,
/// negative errno on failure. Sets ctx->last_error on failure.
typedef int (*helix_bt_remove_device_fn)(helix_bt_context* ctx, const char* mac);

/// Look up the RFCOMM channel for a given service UUID via SDP.
/// Returns 0 on success and writes channel (1..30) to *out_channel.
/// Returns negative on failure; *out_channel untouched on failure.
/// Sets ctx->last_error on failure.
typedef int (*helix_bt_sdp_find_rfcomm_channel_fn)(helix_bt_context* ctx, const char* mac,
                                                   uint16_t uuid16, int* out_channel);

// Symbol name macros for dlsym()

#define HELIX_BT_SYM_GET_INFO "helix_bt_get_info"
#define HELIX_BT_SYM_INIT "helix_bt_init"
#define HELIX_BT_SYM_DEINIT "helix_bt_deinit"
#define HELIX_BT_SYM_DISCOVER "helix_bt_discover"
#define HELIX_BT_SYM_STOP_DISCOVERY "helix_bt_stop_discovery"
#define HELIX_BT_SYM_ENUMERATE_KNOWN "helix_bt_enumerate_known"
#define HELIX_BT_SYM_PAIR "helix_bt_pair"
#define HELIX_BT_SYM_IS_PAIRED "helix_bt_is_paired"
#define HELIX_BT_SYM_IS_BONDED "helix_bt_is_bonded"
#define HELIX_BT_SYM_IS_CONNECTED "helix_bt_is_connected"
#define HELIX_BT_SYM_CONNECT_RFCOMM "helix_bt_connect_rfcomm"
#define HELIX_BT_SYM_CONNECT_BLE "helix_bt_connect_ble"
#define HELIX_BT_SYM_BLE_WRITE "helix_bt_ble_write"
#define HELIX_BT_SYM_BLE_READ "helix_bt_ble_read"
#define HELIX_BT_SYM_DISCONNECT "helix_bt_disconnect"
#define HELIX_BT_SYM_LAST_ERROR "helix_bt_last_error"
#define HELIX_BT_SYM_REMOVE_DEVICE "helix_bt_remove_device"
#define HELIX_BT_SYM_SDP_FIND_RFCOMM_CHANNEL "helix_bt_sdp_find_rfcomm_channel"

/// LZO1X-1 compress data. Returns compressed size on success, negative on error.
/// out_buf must be at least (in_len + in_len/16 + 64 + 3) bytes.
typedef int (*helix_bt_lzo_compress_fn)(const uint8_t* in, int in_len, uint8_t* out, int out_len);

#define HELIX_BT_SYM_LZO_COMPRESS "helix_bt_lzo_compress"

#ifdef __cplusplus
}
#endif
