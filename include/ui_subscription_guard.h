// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "i_moonraker_client.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <memory>
#include <utility>

class IMoonrakerAPI;

/**
 * @brief RAII wrapper for Moonraker subscriptions - auto-unsubscribes on destruction
 *
 * Similar to ObserverGuard but for notification subscriptions.
 * Ensures subscriptions are properly cleaned up when the owning object is destroyed.
 *
 * Captures the client's lifetime guard (weak_ptr) so that reset() safely skips
 * unsubscription if the client has already been destroyed. This prevents crashes
 * from shutdown ordering issues without requiring manual release() calls.
 *
 * Supports construction from either helix::IMoonrakerClient or IMoonrakerAPI:
 * @code
 *   // Via helix::IMoonrakerClient (legacy)
 *   subscription_ = SubscriptionGuard(client, client->register_notify_update(...));
 *   // Via IMoonrakerAPI (preferred)
 *   subscription_ = SubscriptionGuard(api, api->subscribe_notifications(...));
 * @endcode
 */
class SubscriptionGuard {
  public:
    SubscriptionGuard() = default;

    /**
     * @brief Construct guard from client and subscription ID
     *
     * @param client Moonraker client that owns the subscription
     * @param id Subscription ID from register_notify_update()
     */
    SubscriptionGuard(helix::IMoonrakerClient* client, helix::SubscriptionId id)
        : subscription_id_(id), lifetime_(client ? client->lifetime_weak() : std::weak_ptr<bool>{}),
          unsubscribe_fn_(
              client
                  ? [client](helix::SubscriptionId sid) { client->unsubscribe_notify_update(sid); }
                  : std::function<void(helix::SubscriptionId)>{}) {}

    /**
     * @brief Construct guard from IMoonrakerAPI and subscription ID
     *
     * @param api IMoonrakerAPI that owns the subscription
     * @param id Subscription ID from subscribe_notifications()
     */
    SubscriptionGuard(IMoonrakerAPI* api, helix::SubscriptionId id);

    ~SubscriptionGuard() {
        reset();
    }

    SubscriptionGuard(SubscriptionGuard&& other) noexcept
        : subscription_id_(std::exchange(other.subscription_id_, helix::INVALID_SUBSCRIPTION_ID)),
          lifetime_(std::exchange(other.lifetime_, {})),
          unsubscribe_fn_(std::exchange(other.unsubscribe_fn_, {})) {}

    SubscriptionGuard& operator=(SubscriptionGuard&& other) noexcept {
        if (this != &other) {
            reset();
            subscription_id_ =
                std::exchange(other.subscription_id_, helix::INVALID_SUBSCRIPTION_ID);
            lifetime_ = std::exchange(other.lifetime_, {});
            unsubscribe_fn_ = std::exchange(other.unsubscribe_fn_, {});
        }
        return *this;
    }

    SubscriptionGuard(const SubscriptionGuard&) = delete;
    SubscriptionGuard& operator=(const SubscriptionGuard&) = delete;

    /**
     * @brief Unsubscribe and release the subscription
     *
     * If the client has been destroyed (lifetime guard expired), the unsubscription
     * is skipped with a warning log. This prevents crashes from shutdown ordering.
     */
    void reset() {
        if (unsubscribe_fn_ && subscription_id_ != helix::INVALID_SUBSCRIPTION_ID) {
            if (lifetime_.expired()) {
                spdlog::warn("[SubscriptionGuard] Client destroyed before unsubscribe (id={}), "
                             "releasing",
                             subscription_id_);
            } else {
                unsubscribe_fn_(subscription_id_);
            }
            subscription_id_ = helix::INVALID_SUBSCRIPTION_ID;
        }
        unsubscribe_fn_ = {};
        lifetime_.reset();
    }

    /**
     * @brief Release ownership without unsubscribing
     *
     * Use during shutdown when the client may already be destroyed.
     * The subscription will not be removed (it may already be gone).
     */
    void release() {
        unsubscribe_fn_ = {};
        lifetime_.reset();
        subscription_id_ = helix::INVALID_SUBSCRIPTION_ID;
    }

    /**
     * @brief Check if guard holds a valid subscription
     */
    explicit operator bool() const {
        return unsubscribe_fn_ && subscription_id_ != helix::INVALID_SUBSCRIPTION_ID;
    }

    /**
     * @brief Get the raw subscription ID
     */
    helix::SubscriptionId get() const {
        return subscription_id_;
    }

  private:
    helix::SubscriptionId subscription_id_ = helix::INVALID_SUBSCRIPTION_ID;
    std::weak_ptr<bool> lifetime_; ///< Tracks client lifetime — expired = client destroyed
    std::function<void(helix::SubscriptionId)> unsubscribe_fn_;
};
