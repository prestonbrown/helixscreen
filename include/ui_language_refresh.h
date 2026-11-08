// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix::ui {

/// Re-render the text the printer layer formats once when it discovers the
/// hardware or syncs a backend: tool labels ("Tool 2"), extruder and fan names,
/// the current-tool label and the AMS status texts.
void refresh_display_texts();

/// Run refresh_display_texts() on every language switch and every change of the
/// tool number style setting. Call once, after subjects exist.
void init_language_refresh();

} // namespace helix::ui
