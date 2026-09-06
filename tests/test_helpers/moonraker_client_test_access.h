// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_client.h"

#include <shared_mutex>

namespace helix {

// Grants tests visibility into — and a handle on — MoonrakerClient's install-once
// WebSocket callback machinery: whether the trampolines are installed, how to install
// and drive them without a socket, and the shared lock they hold while a callback runs.
// Declared a friend of MoonrakerClient (see moonraker_client.h). Follows the existing
// TestAccess pattern (tests/test_helpers/, [L088]) rather than adding a production
// _for_testing() accessor.
class MoonrakerClientTestAccess {
  public:
    // True once install_ws_callbacks() has run for this client. The guard
    // ws_callbacks_installed_ is what ensures the inherited onopen/onmessage/onclose
    // std::functions are assigned exactly once and never reassigned per connect()
    // (the reassignment was the UAF in bundle UK9QCFY3).
    static bool callbacks_installed(const MoonrakerClient& c) {
        return c.ws_callbacks_installed_;
    }

    // Force connection_state_ without a socket, so tests can exercise gates that
    // read get_connection_state() (e.g. MoonrakerAPI::execute_gcode's klippy-halt
    // gate, which may only speak for a printer we are actually connected to).
    // Writes the atomic directly rather than calling set_connection_state(), which
    // would emit events and fire the state-change callback — side effects a gate
    // test does not want.
    static void force_connection_state(MoonrakerClient& c, ConnectionState state) {
        c.connection_state_.store(state);
    }

    // Install the three WebSocket trampolines without opening a socket, the way
    // the first connect() does. Lets a test drive onopen/onmessage/onclose directly
    // and observe what the trampolines themselves gate on, with no event loop and
    // no reachable host in the picture.
    static void install_ws_callbacks(MoonrakerClient& c) {
        c.install_ws_callbacks();
        c.ws_callbacks_installed_ = true;
    }

    // Take the shared lock a WebSocket trampoline holds for as long as its callback
    // runs. A test holding it stands in for a callback that has not returned, which
    // is what disconnect()'s bounded drain is waiting on. Acquire it from a thread
    // other than the one that will call disconnect(): std::shared_mutex is neither
    // recursive nor upgradeable.
    static std::shared_lock<std::shared_mutex> hold_callback_lock(MoonrakerClient& c) {
        return std::shared_lock<std::shared_mutex>(c.callback_lifecycle_mutex_);
    }

    // Invoke all persistent method callbacks registered for `method`, as the
    // WebSocket onmessage dispatch would (copy under lock, invoke outside it).
    // Lets tests simulate Moonraker notifications (notify_filelist_changed,
    // notify_klippy_ready, ...) without a live connection. Runs on the calling
    // thread — production callbacks must already be thread-agnostic (they
    // marshal member access to the main thread via token.defer()).
    static void fire_method_callbacks(MoonrakerClient& c, const std::string& method,
                                      const nlohmann::json& msg) {
        std::vector<std::function<void(const nlohmann::json&)>> cbs;
        {
            std::lock_guard<std::mutex> lock(c.callbacks_mutex_);
            auto it = c.method_callbacks_.find(method);
            if (it != c.method_callbacks_.end()) {
                for (auto& [name, cb] : it->second) {
                    cbs.push_back(cb);
                }
            }
        }
        for (auto& cb : cbs) {
            if (cb) {
                cb(msg);
            }
        }
    }
};

} // namespace helix
