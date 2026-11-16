// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bluetooth_plugin.h"

#include <mutex>
#include <string>

namespace helix::bluetooth {

/// Whether to load the plugin, given HELIX_BLUETOOTH's value (null when unset). `0` always
/// skips it and `1` always allows it. Otherwise production loads it and a --test run does
/// not, so a dev run never registers a BlueZ agent on the developer's machine.
bool bluetooth_enabled(const char* env_value, bool test_mode);

/// Runtime loader for libhelix-bluetooth.so.
/// Checks for BT hardware, loads plugin via dlopen, resolves function pointers.
/// Zero overhead when BT unavailable — is_available() returns false, all ops are no-ops.
class BluetoothLoader {
  public:
    static BluetoothLoader& instance();

    /// Check if BT hardware exists AND plugin loaded successfully.
    [[nodiscard]] bool is_available() const;

    /// Plugin function pointers — only valid when is_available() == true.
    /// Caller must check is_available() before using these.
    helix_bt_init_fn init = nullptr;
    helix_bt_deinit_fn deinit = nullptr;
    helix_bt_discover_fn discover = nullptr;
    helix_bt_stop_discovery_fn stop_discovery = nullptr;
    helix_bt_enumerate_known_fn enumerate_known = nullptr;
    helix_bt_pair_fn pair = nullptr;
    helix_bt_is_paired_fn is_paired = nullptr;
    helix_bt_is_bonded_fn is_bonded = nullptr;
    helix_bt_is_connected_fn is_connected = nullptr;
    helix_bt_remove_device_fn remove_device = nullptr;
    helix_bt_connect_rfcomm_fn connect_rfcomm = nullptr;
    helix_bt_sdp_find_rfcomm_channel_fn sdp_find_rfcomm_channel = nullptr;
    helix_bt_connect_ble_fn connect_ble = nullptr;
    helix_bt_ble_write_fn ble_write = nullptr;
    helix_bt_ble_read_fn ble_read = nullptr;
    helix_bt_disconnect_fn disconnect = nullptr;
    helix_bt_last_error_fn last_error = nullptr;
    helix_bt_lzo_compress_fn lzo_compress = nullptr;

    /// Get a shared BT context, creating it on first call. Thread-safe: concurrent first
    /// callers share one init(), since a second one registers a conflicting D-Bus agent.
    helix_bt_context* get_or_create_context();

    BluetoothLoader(const BluetoothLoader&) = delete;
    BluetoothLoader& operator=(const BluetoothLoader&) = delete;

  private:
    friend class BluetoothLoaderTestAccess;

    BluetoothLoader();
    ~BluetoothLoader();

    bool try_load();
    static bool has_bt_hardware();

    void* dl_handle_ = nullptr;
    bool available_ = false;
    std::mutex ctx_mutex_;
    helix_bt_context* shared_ctx_ = nullptr; ///< guarded by ctx_mutex_
};

} // namespace helix::bluetooth
