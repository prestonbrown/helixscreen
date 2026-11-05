// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The IPv4 half of WiFiManager and EthernetManager: reading an interface's
// IPv4 config and applying a change (prestonbrown/helixscreen#1900). Member definitions live here
// so the firmware, whose backends never support a static address, can leave them out.

#include "core/http_executor.h"
#include "ethernet_manager.h"
#include "ipv4_config_system.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

// ============================================================================
// WiFiManager
// ============================================================================

namespace helix {

bool WiFiManager::supports_ipv4_config() {
    return backend_ && backend_->supports_ipv4_config();
}

void WiFiManager::get_ipv4_config_async(
    helix::LifetimeToken token,
    helix::SharedCallback<void(bool ok, const helix::net::Ipv4Config& config)> on_done) {
    if (!on_done) {
        return;
    }
    if (!supports_ipv4_config()) {
        on_done(false, helix::net::Ipv4Config{});
        return;
    }
    submit_user_op(
        "get_ipv4_config", user_op_timeouts_.other_ms,
        [this, token, on_done]() -> helix::Callback<void()> {
            helix::net::Ipv4Config config;
            const helix::net::Ipv4Result result = backend_->get_ipv4_config(config);
            return [token, on_done, result, config]() {
                if (!result.ok()) {
                    spdlog::debug("[WiFiManager] Could not read the IPv4 config: {}",
                                  result.detail);
                }
                if (!token.expired()) {
                    on_done(result.ok(), config);
                }
            };
        },
        [token, on_done]() {
            if (!token.expired()) {
                on_done(false, helix::net::Ipv4Config{});
            }
        });
}

void WiFiManager::apply_ipv4_change_async(const helix::net::Ipv4Config& previous,
                                          const helix::net::Ipv4Config& next,
                                          std::shared_ptr<std::atomic<bool>> abandoned,
                                          helix::LifetimeToken token,
                                          helix::SharedCallback<void(bool ok)> on_done) {
    auto answer = [token, on_done](bool ok) {
        if (on_done && !token.expired()) {
            on_done(ok);
        }
    };
    if (!supports_ipv4_config()) {
        answer(false);
        return;
    }
    spdlog::info("[WiFiManager] Applying IPv4 config: {}", helix::net::ipv4_config_text(next));
    submit_user_op(
        "apply_ipv4_change", user_op_timeouts_.ipv4_ms,
        [this, previous, next, abandoned, answer]() -> helix::Callback<void()> {
            const helix::net::Ipv4Result result = helix::net::apply_ipv4_change(
                *backend_, helix::net::Ipv4Link::Wifi, previous, next, abandoned.get());
            // The address in the status rows changes with it.
            notify_state_observers();
            return [result, answer]() {
                if (!result.ok()) {
                    spdlog::warn("[WiFiManager] Could not apply the IPv4 config: {}",
                                 result.detail);
                }
                answer(result.ok());
            };
        },
        [answer]() {
            notify_unresponsive();
            answer(false);
        });
}

} // namespace helix

// ============================================================================
// EthernetManager
// ============================================================================

bool EthernetManager::supports_ipv4_config() {
    return backend_ && backend_->supports_ipv4_config();
}

void EthernetManager::get_ipv4_config_async(
    helix::LifetimeToken token,
    helix::SharedCallback<void(bool ok, const helix::net::Ipv4Config& config)> on_done) {
    if (!on_done) {
        return;
    }
    if (!supports_ipv4_config()) {
        on_done(false, helix::net::Ipv4Config{});
        return;
    }
    helix::http::HttpExecutor::fast().start();
    // The backend is captured by value so it outlives this manager mid-call.
    std::shared_ptr<EthernetBackend> backend = backend_;
    helix::http::HttpExecutor::fast().submit([backend, token, on_done]() {
        helix::net::Ipv4Config config;
        const helix::net::Ipv4Result result = backend->get_ipv4_config(config);
        if (!result.ok()) {
            spdlog::debug("[EthernetManager] Could not read the IPv4 config: {}", result.detail);
        }
        token.defer("EthernetManager::get_ipv4_config",
                    [on_done, ok = result.ok(), config]() { on_done(ok, config); });
    });
}

void EthernetManager::apply_ipv4_change_async(const helix::net::Ipv4Config& previous,
                                              const helix::net::Ipv4Config& next,
                                              std::shared_ptr<std::atomic<bool>> abandoned,
                                              helix::LifetimeToken token,
                                              helix::SharedCallback<void(bool ok)> on_done) {
    if (!supports_ipv4_config()) {
        if (on_done) {
            on_done(false);
        }
        return;
    }
    spdlog::info("[EthernetManager] Applying IPv4 config: {}", helix::net::ipv4_config_text(next));
    helix::http::HttpExecutor::fast().start();
    std::shared_ptr<EthernetBackend> backend = backend_;
    helix::http::HttpExecutor::fast().submit([backend, previous, next, abandoned, token,
                                              on_done]() {
        const helix::net::Ipv4Result result = helix::net::apply_ipv4_change(
            *backend, helix::net::Ipv4Link::Ethernet, previous, next, abandoned.get());
        if (!result.ok()) {
            spdlog::warn("[EthernetManager] Could not apply the IPv4 config: {}", result.detail);
        }
        token.defer("EthernetManager::apply_ipv4_change", [on_done, ok = result.ok()]() {
            if (on_done) {
                on_done(ok);
            }
        });
    });
}
