// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_notification_history.h"

/// Drive the history-version observer body directly. In production this only
/// ever runs from a queued UpdateQueue apply (observe_int_sync defers it), so
/// a test cannot pin the same-version dedup guard without either the queue's
/// exact publish/subscribe timing or this accessor.
class NotificationHistoryPanelTestAccess {
  public:
    static void handle_history_version_change(NotificationHistoryPanel& panel, int32_t version) {
        panel.handle_history_version_change(version);
    }
};
