// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_sleep_listener.h"

#include "display_manager.h"

namespace helix {

void DisplaySleepListener::listen(std::function<void(bool sleeping)> on_change) {
    if (handler_) {
        *handler_ = std::move(on_change);
        return;
    }
    auto* dm = DisplayManager::instance();
    if (!dm) {
        return;
    }
    handler_ = std::make_shared<std::function<void(bool)>>(std::move(on_change));
    std::weak_ptr<std::function<void(bool)>> handler = handler_;
    dm->register_sleep_callback([handler](bool sleeping) {
        if (auto fn = handler.lock(); fn && *fn) {
            (*fn)(sleeping);
        }
    });
}

bool DisplaySleepListener::display_sleeping() {
    const auto* dm = DisplayManager::instance();
    return dm != nullptr && dm->is_display_sleeping();
}

} // namespace helix
