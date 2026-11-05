// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The IPv4 half of WiFiManager and EthernetManager: reading and applying an
// interface's IPv4 config, and restoring one a change never kept
// (prestonbrown/helixscreen#1900). Member definitions live here so the
// firmware, whose backends never support a static address, can leave them out.

#include "async_lifetime_guard.h"
#include "core/http_executor.h"
#include "ethernet_manager.h"
#include "ipv4_config.h"
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

void WiFiManager::apply_ipv4_config_async(const helix::net::Ipv4Config& config,
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
    spdlog::info("[WiFiManager] Applying IPv4 config: {}", helix::net::ipv4_config_text(config));
    // Up to three nmcli calls: connect's window, not the shorter one for a
    // single control round trip.
    submit_user_op(
        "apply_ipv4_config", user_op_timeouts_.connect_ms,
        [this, config, answer]() -> helix::Callback<void()> {
            const helix::net::Ipv4Result result = backend_->apply_ipv4_config(config);
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

void WiFiManager::revert_pending_ipv4() {
    const auto pending = helix::net::load_pending_ipv4_revert();
    if (!helix::net::is_stale_pending_ipv4_revert(pending, helix::net::Ipv4Link::Wifi)) {
        return;
    }
    if (!supports_ipv4_config()) {
        spdlog::warn("[WiFiManager] A Wi-Fi IPv4 change was never kept, but this backend "
                     "cannot restore it");
        return;
    }
    spdlog::warn("[WiFiManager] A Wi-Fi IPv4 change was never kept: restoring {}",
                 helix::net::ipv4_config_text(pending->previous));
    const helix::net::Ipv4Config previous = pending->previous;
    submit_control("revert_ipv4", ControlKind::User, [this, previous]() {
        const helix::net::Ipv4Result result = backend_->apply_ipv4_config(previous);
        if (!result.ok()) {
            spdlog::error("[WiFiManager] Could not restore the previous IPv4 config: {}",
                          result.detail);
            return;
        }
        spdlog::info("[WiFiManager] Previous IPv4 config restored");
        helix::net::clear_pending_ipv4_revert();
        notify_state_observers();
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

void EthernetManager::apply_ipv4_config_async(const helix::net::Ipv4Config& config,
                                              helix::LifetimeToken token,
                                              helix::SharedCallback<void(bool ok)> on_done) {
    if (!supports_ipv4_config()) {
        if (on_done) {
            on_done(false);
        }
        return;
    }
    spdlog::info("[EthernetManager] Applying IPv4 config: {}",
                 helix::net::ipv4_config_text(config));
    helix::http::HttpExecutor::fast().start();
    std::shared_ptr<EthernetBackend> backend = backend_;
    helix::http::HttpExecutor::fast().submit([backend, config, token, on_done]() {
        const helix::net::Ipv4Result result = backend->apply_ipv4_config(config);
        if (!result.ok()) {
            spdlog::warn("[EthernetManager] Could not apply the IPv4 config: {}", result.detail);
        }
        token.defer("EthernetManager::apply_ipv4_config", [on_done, ok = result.ok()]() {
            if (on_done) {
                on_done(ok);
            }
        });
    });
}

void EthernetManager::revert_pending_ipv4_at_startup() {
    const auto pending = helix::net::load_pending_ipv4_revert();
    if (!helix::net::is_stale_pending_ipv4_revert(pending, helix::net::Ipv4Link::Ethernet)) {
        return;
    }
    auto backend = std::shared_ptr<EthernetBackend>(EthernetBackend::create());
    if (!backend || !backend->supports_ipv4_config()) {
        spdlog::warn("[EthernetManager] An Ethernet IPv4 change was never kept, but this "
                     "backend cannot restore it");
        return;
    }
    spdlog::warn("[EthernetManager] An Ethernet IPv4 change was never kept: restoring {}",
                 helix::net::ipv4_config_text(pending->previous));
    helix::http::HttpExecutor::fast().start();
    helix::http::HttpExecutor::fast().submit([backend, previous = pending->previous]() {
        const helix::net::Ipv4Result result = backend->apply_ipv4_config(previous);
        if (!result.ok()) {
            spdlog::error("[EthernetManager] Could not restore the previous IPv4 config: {}",
                          result.detail);
            return;
        }
        spdlog::info("[EthernetManager] Previous IPv4 config restored");
        helix::net::clear_pending_ipv4_revert();
    });
}
