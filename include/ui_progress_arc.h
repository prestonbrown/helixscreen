// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

namespace helix::ui {

/// Wire diameter-driven thickness + auto resize-to-square for a
/// `<helix_progress_arc>` (or any plain lv_arc whose stroke we want to scale
/// with its size).
///
/// On `LV_EVENT_SIZE_CHANGED` of `parent`:
///   1. The arc is squared to `min(parent content w, parent content h)`.
///   2. A thickness tier (0..4 → 4/6/8/10/12 px) is derived from that
///      diameter and published to `tier_subject`.
///   3. Five `lv_obj_bind_style()` registrations attach the `arc_w_*` styles
///      (defined in `helix_progress_arc.xml`) to the tier value on both
///      `LV_PART_MAIN` and `LV_PART_INDICATOR`, so the XML side stays
///      declarative.
///
/// Caller owns `tier_subject` lifetime. Initialize as int with value 2
/// (medium) before calling so the arc has a sane initial stroke until the
/// first size event fires.
///
/// `parent` defaults to `lv_obj_get_parent(arc)` if `nullptr`.
void attach_progress_arc(lv_obj_t* arc, lv_obj_t* parent, lv_subject_t* tier_subject);

/// Convenience: like `attach_progress_arc()` but the helper allocates and
/// owns the tier subject (freed on the arc's `LV_EVENT_DELETE`). Returns
/// a non-owning pointer to the subject — do not free; do not outlive the
/// arc. Returns nullptr, and keeps no subject, when the attach fails. Use
/// when the caller doesn't need the subject for anything else
/// (e.g., multiple dynamic instances in fan widgets).
lv_subject_t* attach_progress_arc_owned(lv_obj_t* arc, lv_obj_t* parent);

/// Map a diameter (in px) to its thickness tier. Exposed for callers that
/// want to compute or test tiers independently of the attach helper.
int progress_arc_thickness_tier_for(int diameter_px);

/// Re-run the resize+publish for an arc previously attached via
/// `attach_progress_arc()`. Use when an ancestor layout change won't
/// propagate as a SIZE_CHANGED on the arc's direct parent (e.g., the
/// outer panel grid relayouts but the immediate arc container's own
/// dimensions don't strictly change yet). No-op if the arc wasn't
/// attached.
void refresh_progress_arc(lv_obj_t* arc);

} // namespace helix::ui
