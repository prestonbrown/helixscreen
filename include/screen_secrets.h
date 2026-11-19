// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file screen_secrets.h
 * @brief Which widgets show a secret, and whether one is on the display now
 *
 * The remote screen pairing code proves that whoever pairs a browser can see the
 * printer's own display. A frame served to a browser while the code is on that display
 * would hand it to anyone viewing the stream, so the screen server serves a placeholder
 * for every refresh drawn while screen_secret_state() is not Absent.
 *
 * Every widget that renders a secret is marked once, right after it is created. A
 * marked widget stays registered until it is deleted.
 *
 * @threading Main thread only.
 */

#pragma once

#include "lvgl.h"
#include "remote_screen_sink.h"

namespace helix::ui {

/// Mark @p obj as showing a secret for as long as it exists.
void mark_screen_secret(lv_obj_t* obj);

/// Shown while any marked widget is visible on the active screen or a display layer
/// (`lv_obj_is_visible`: not hidden, no hidden ancestor, not clipped away by its
/// parents), Absent otherwise. Unknown when there is no display to judge against.
ScreenSecret screen_secret_state();

} // namespace helix::ui
