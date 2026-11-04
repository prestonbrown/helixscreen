// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fan_arc_resize.h"

#include "ui_progress_arc.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Thin wrappers around the shared helix_progress_arc helper. fan_arc_core.xml
// now uses <helix_progress_arc> directly, so the common arc styling + 5-tier
// thickness binding lives in helix_progress_arc.xml; the C++ here just finds
// the arc inside a fan card root and wires up auto-resize with an owned
// per-card tier subject.

void fan_arc_resize_to_fit(lv_obj_t* card_root) {
    if (!card_root)
        return;
    lv_obj_t* arc = lv_obj_find_by_name(card_root, "dial_arc");
    if (!arc)
        return;
    helix::ui::refresh_progress_arc(arc);
}

void fan_arc_attach_auto_resize(lv_obj_t* card_root) {
    if (!card_root)
        return;
    lv_obj_t* arc = lv_obj_find_by_name(card_root, "dial_arc");
    if (!arc) {
        spdlog::warn("[FanArcResize] dial_arc not found under card_root");
        return;
    }
    // dial_container, if present, is the size-driving container. Otherwise
    // fall back to the arc's direct parent — same logic as the old impl.
    lv_obj_t* container = lv_obj_find_by_name(card_root, "dial_container");
    if (!container)
        container = lv_obj_get_parent(arc);

    // Per-card owned subject; freed automatically on arc deletion.
    helix::ui::attach_progress_arc_owned(arc, container);
}

} // namespace helix::ui
