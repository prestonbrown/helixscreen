// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_update_notify_modal.h
 * @brief The "update available" dialog (update_notify_modal.xml)
 *
 * UpdateChecker decides when a release is worth announcing and fills the
 * release-notes subjects; it asks for this dialog with
 * UiRequestKind::ShowUpdateNotification. The dialog's buttons install,
 * ignore the version, or close it.
 */

namespace helix::ui {

/// Show the dialog unless it is already up. Main thread.
void show_update_notification();

/// Close the dialog if it is up. Main thread.
void hide_update_notification();

} // namespace helix::ui
