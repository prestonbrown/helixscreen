// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notice.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

// Constant-initialized, so a subscriber installed by another translation unit's
// static initializer cannot be overwritten by this one's.
NoticeSubscriber s_subscriber = nullptr;

} // namespace

NoticeSubscriber set_notice_subscriber(NoticeSubscriber subscriber) {
    NoticeSubscriber previous = s_subscriber;
    s_subscriber = subscriber;
    return previous;
}

void publish_notice(const Notice& notice) {
    if (!s_subscriber) {
        spdlog::warn("[Notice] No subscriber; dropping: {}", notice.message);
        return;
    }
    s_subscriber(notice);
}

} // namespace helix
