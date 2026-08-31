// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file fake_moonraker_client.h
 * @brief Recording stand-in for helix::IMoonrakerClient.
 *
 * The interface is 39 pure virtuals, so anything that wants to test a consumer
 * of the transport has had to hand-write the whole no-op set inline (see the
 * ReconnectCountingClient in tests/unit/test_connection_failed_change_address.cpp,
 * which predates this file). This is that set, once, with the three things a
 * consumer test actually needs to observe kept as public records:
 *
 *   - notification subscriptions, so a test can FIRE one (fire_notification)
 *   - unregistrations, so a test can prove a destructor cleaned up
 *   - JSON-RPC sends, with both continuations kept so a test can invoke them
 *
 * Every override is virtual, so a test that needs one method to do something
 * real subclasses this and overrides just that one.
 *
 * Nothing here touches a thread: a fired notification runs on the caller's
 * thread. Consumers that wrap their handler in AsyncLifetimeGuard::bg_cb will
 * therefore only enqueue work — the test still has to pump the UpdateQueue
 * before asserting.
 */

#include "i_moonraker_client.h"
#include "moonraker_error.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::test {

class FakeMoonrakerClient : public helix::IMoonrakerClient {
  public:
    /// One live notification subscription.
    struct MethodCallback {
        std::string handler_name;
        std::function<void(const nlohmann::json&)> callback;
    };

    /// One unregister_method_callback() call.
    struct Unregistration {
        std::string method;
        std::string handler_name;
    };

    /// One send_jsonrpc() call. The continuations are empty for the overloads
    /// that take none; the four-argument form keeps both so a test can drive
    /// the reply path.
    struct RpcCall {
        std::string method;
        nlohmann::json params;
        std::function<void(const nlohmann::json&)> success_cb;
        std::function<void(const MoonrakerError&)> error_cb;
    };

    /// Live subscriptions keyed by notification method, mirroring the real
    /// client: several handlers may share a method and unregistering drops
    /// only the matching one.
    std::map<std::string, std::vector<MethodCallback>> method_callbacks;

    /// Every unregister_method_callback() call, in order, whether or not it
    /// matched anything.
    std::vector<Unregistration> unregistrations;

    /// Every send_jsonrpc() call, in order.
    std::vector<RpcCall> rpc_calls;

    /// What get_connection_state() reports. Settable so a subclass-free test
    /// can present a client that is up or down.
    helix::ConnectionState connection_state = helix::ConnectionState::CONNECTED;

    /// Deliver @p msg to every handler registered for @p method.
    /// @return false when nothing is subscribed — which is itself the
    ///         assertion for "the consumer never registered".
    bool fire_notification(const std::string& method, const nlohmann::json& msg) {
        auto it = method_callbacks.find(method);
        if (it == method_callbacks.end() || it->second.empty()) {
            return false;
        }
        // Copy first: a handler is allowed to register or unregister from
        // inside its own delivery, which would invalidate the vector.
        std::vector<MethodCallback> handlers = it->second;
        for (const MethodCallback& handler : handlers) {
            if (handler.callback) {
                handler.callback(msg);
            }
        }
        return true;
    }

    /// The most recent recorded RPC, or nullptr if nothing was sent.
    const RpcCall* last_rpc() const {
        return rpc_calls.empty() ? nullptr : &rpc_calls.back();
    }

    /// Number of recorded sends of @p method.
    size_t rpc_count(const std::string& method) const {
        return static_cast<size_t>(
            std::count_if(rpc_calls.begin(), rpc_calls.end(),
                          [&method](const RpcCall& call) { return call.method == method; }));
    }

    /// True if @p handler_name was unregistered from @p method.
    bool was_unregistered(const std::string& method, const std::string& handler_name) const {
        return std::any_of(unregistrations.begin(), unregistrations.end(),
                           [&](const Unregistration& u) {
                               return u.method == method && u.handler_name == handler_name;
                           });
    }

    // ========================================================================
    // Subscriptions & method callbacks — recorded
    // ========================================================================

    void register_method_callback(const std::string& method, const std::string& handler_name,
                                  std::function<void(const nlohmann::json&)> cb) override {
        method_callbacks[method].push_back({handler_name, std::move(cb)});
    }

    bool unregister_method_callback(const std::string& method,
                                    const std::string& handler_name) override {
        unregistrations.push_back({method, handler_name});
        auto it = method_callbacks.find(method);
        if (it == method_callbacks.end()) {
            return false;
        }
        std::vector<MethodCallback>& handlers = it->second;
        const size_t before = handlers.size();
        handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                      [&handler_name](const MethodCallback& h) {
                                          return h.handler_name == handler_name;
                                      }),
                       handlers.end());
        return handlers.size() != before;
    }

    // ========================================================================
    // JSON-RPC — recorded
    // ========================================================================

    int send_jsonrpc(const std::string& method) override {
        rpc_calls.push_back({method, nlohmann::json::object(), {}, {}});
        return 0;
    }

    int send_jsonrpc(const std::string& method, const nlohmann::json& params) override {
        rpc_calls.push_back({method, params, {}, {}});
        return 0;
    }

    helix::RequestId send_jsonrpc(const std::string& method, const nlohmann::json& params,
                                  std::function<void(const nlohmann::json&)> cb) override {
        rpc_calls.push_back({method, params, std::move(cb), {}});
        return ++next_request_id_;
    }

    helix::RequestId
    send_jsonrpc(const std::string& method, const nlohmann::json& params,
                 std::function<void(const nlohmann::json&)> success_cb,
                 std::function<void(const MoonrakerError&)> error_cb, uint32_t /*timeout_ms*/,
                 bool /*silent*/,
                 std::optional<helix::rpc_error_policy::CallerIntent> /*intent*/) override {
        rpc_calls.push_back({method, params, std::move(success_cb), std::move(error_cb)});
        return ++next_request_id_;
    }

    // ========================================================================
    // Everything else — inert
    // ========================================================================

    int connect(const char* url, std::function<void()>, std::function<void()>) override {
        last_url_ = url ? url : "";
        return 0;
    }
    void disconnect() override {}
    const std::string& get_last_url() const override {
        return last_url_;
    }
    void set_auto_reconnect(bool) override {}
    int gcode_script(const std::string&) override {
        return 0;
    }
    void get_gcode_store(int, std::function<void(const std::vector<GcodeStoreEntry>&)>,
                         std::function<void(const MoonrakerError&)>) override {}
    void get_temperature_store(std::function<void(const TemperatureStore&)>,
                               std::function<void(const MoonrakerError&)>) override {}
    void discover_printer(std::function<void()>, std::function<void(const std::string&)>) override {
    }
    helix::PrinterDiscovery hardware() const override {
        return {};
    }
    void parse_objects(const nlohmann::json&) override {}
    void clear_discovery_cache() override {}
    void set_on_hardware_discovered(std::function<void(const helix::PrinterDiscovery&)>) override {}
    void set_on_discovery_complete(
        std::function<void(const helix::PrinterDiscovery&, const nlohmann::json&)>) override {}
    void set_bed_mesh_callback(std::function<void(const nlohmann::json&)>) override {}
    helix::SubscriptionId
    register_notify_update(std::function<void(const nlohmann::json&)>) override {
        return helix::INVALID_SUBSCRIPTION_ID;
    }
    bool unsubscribe_notify_update(helix::SubscriptionId) override {
        return false;
    }
    void dispatch_status_update(const nlohmann::json&, bool) override {}
    helix::ConnectionState get_connection_state() const override {
        return connection_state;
    }
    void add_connected_observer(const std::string&, std::function<void()>) override {}
    bool remove_connected_observer(const std::string&) override {
        return false;
    }
    void force_reconnect() override {}
    void register_event_handler(helix::MoonrakerEventCallback) override {}
    void suppress_disconnect_modal(uint32_t) override {}
    bool is_disconnect_modal_suppressed() const override {
        return false;
    }
    bool cancel_request(helix::RequestId) override {
        return false;
    }
    void set_state_change_callback(
        std::function<void(helix::ConnectionState, helix::ConnectionState)>) override {}
    void set_connection_timeout(uint32_t) override {}
    void set_default_request_timeout(uint32_t) override {}
    void configure_timeouts(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void process_timeouts() override {}
    void toggle_filament_runout_simulation() override {}
    std::weak_ptr<bool> lifetime_weak() const override {
        return alive_;
    }

  protected:
    std::string last_url_;
    helix::RequestId next_request_id_ = 0;
    /// Held, not a temporary: a weak_ptr to a just-destroyed shared_ptr is
    /// born expired, which silently disarms every SubscriptionGuard built
    /// from it.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace helix::test
