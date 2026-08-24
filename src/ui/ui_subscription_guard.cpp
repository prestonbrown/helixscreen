// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_subscription_guard.h"

#include "i_moonraker_api.h"

using namespace helix;

SubscriptionGuard::SubscriptionGuard(IMoonrakerAPI* api, SubscriptionId id)
    : subscription_id_(id), lifetime_(api ? api->client_lifetime_weak() : std::weak_ptr<bool>{}),
      unsubscribe_fn_(api ? [api](SubscriptionId sid) { api->unsubscribe_notifications(sid); }
                          : std::function<void(SubscriptionId)>{}) {}
