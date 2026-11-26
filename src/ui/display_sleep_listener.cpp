// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_sleep_listener.h"

#include <algorithm>
#include <vector>

namespace helix {

namespace {

std::vector<DisplaySleepListener*>& live_listeners() {
    static std::vector<DisplaySleepListener*> s_listeners;
    return s_listeners;
}

bool s_sleeping = false;

} // namespace

DisplaySleepListener::~DisplaySleepListener() {
    if (registered_) {
        auto& live = live_listeners();
        live.erase(std::remove(live.begin(), live.end(), this), live.end());
    }
}

void DisplaySleepListener::listen(std::function<void(bool sleeping)> on_change) {
    on_change_ = std::move(on_change);
    if (!registered_) {
        live_listeners().push_back(this);
        registered_ = true;
    }
}

bool DisplaySleepListener::display_sleeping() {
    return s_sleeping;
}

void DisplaySleepListener::notify(bool sleeping) {
    s_sleeping = sleeping;
    // A handler may destroy a listener, so each is looked up again before it is called.
    const std::vector<DisplaySleepListener*> snapshot = live_listeners();
    for (DisplaySleepListener* listener : snapshot) {
        const auto& live = live_listeners();
        if (std::find(live.begin(), live.end(), listener) != live.end() && listener->on_change_) {
            listener->on_change_(sleeping);
        }
    }
}

} // namespace helix
