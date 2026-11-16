// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"

#include <optional>

namespace helix::ui {

/**
 * @brief The "Disable Motors?" confirmation, shared by every panel that offers it
 *
 * One wording, one M84 send, three toasts: confirming releases the steppers,
 * a failed send reports the error, cancelling moves nothing. The created
 * dialog is assigned into `stored` and the handle is dropped on every close
 * path, so a second entry while the dialog is open is the caller's only
 * remaining guard.
 *
 * @param api The API M84 is sent through on confirm; nullptr still shows the
 *        dialog but sends nothing.
 * @param stored The caller's guard. It reads nullptr once the dialog is gone,
 *        whoever closed it; the callbacks capture its address, so it must
 *        outlive the dialog unless owner_token expires first.
 * @param owner_token Gates every callback; pass the panel's lifetime token so
 *        a dialog that outlives its panel fires nothing into freed memory.
 */
void show_motors_off_confirm(IMoonrakerAPI* api, WidgetRef& stored,
                             std::optional<LifetimeToken> owner_token = std::nullopt);

} // namespace helix::ui
