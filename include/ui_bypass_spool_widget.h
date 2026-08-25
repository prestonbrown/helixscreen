// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <cstdint>

class AmsBackend;

namespace helix::ui {

/**
 * @brief Whether the bypass node belongs on the filament path at all.
 *
 * AFC publishes a virtual bypass sensor whether or not the user has a bypass
 * wired, so `supports_bypass` is effectively always true there and the node was
 * drawn permanently — then painted from the external-spool slot, which put the
 * currently-loaded lane's material and colour on it. The green "ASA / Bypass" in
 * #1229 was lane1's spool on a machine with no bypass at all.
 *
 * On AFC the node is therefore hidden while bypass is disengaged, unless the
 * user opts back in via the AMS setting. Other backends are unaffected: their
 * bypass is a real physical position, so it stays visible whenever supported.
 *
 * Pure by design. The render sites are LVGL-bound and previously carried this
 * condition inline in four places, where it could neither be tested nor kept
 * consistent — three of the four had already drifted apart.
 *
 * @param supports_bypass Backend reports a bypass position at all
 * @param bypass_active Bypass is currently engaged (firmware state, not a proxy)
 * @param is_afc Backend is AFC — the only one with a phantom virtual bypass
 * @param always_show User setting: keep it visible even when disengaged
 * @return true when the bypass node should be rendered
 */
[[nodiscard]] constexpr bool bypass_node_visible(bool supports_bypass, bool bypass_active,
                                                 bool is_afc, bool always_show) {
    if (!supports_bypass) {
        return false;
    }
    if (bypass_active) {
        return true;
    }
    if (!is_afc) {
        return true;
    }
    return always_show;
}

/// Gather bypass_node_visible()'s inputs from the live backend and settings.
/// Returns false when no backend is attached.
[[nodiscard]] bool bypass_node_visible_for(const AmsBackend* backend);

/// Bag of widget pointers that make up the bypass-spool overlay: a small card
/// containing the spool icon, with a material label above and "Bypass" label
/// below — matching how lane spools display their material on the AMS path
/// canvases. Shared by both the Multi-Filament panel (single-AMS) and the
/// Multi-Filament Overview panel (multi-AMS).
///
/// The widgets are positioned as FLOATING siblings of a path-canvas, with the
/// owning panel responsible for placing the box at the canvas-computed bypass
/// coordinate. The canvas itself only draws the connecting lines.
struct BypassSpoolWidgets {
    lv_obj_t* box = nullptr;            ///< Card container with click target
    lv_obj_t* spool_canvas = nullptr;   ///< The spool icon inside the box
    lv_obj_t* bypass_label = nullptr;   ///< "Bypass" text below the box
    lv_obj_t* material_label = nullptr; ///< Material name above the box (hidden if empty)

    // Cached state for change-detection (avoids spurious invalidates on every
    // panel refresh). Not part of the public API surface — read via setters.
    /// Paired with cached_color_rgb because black IS 0: without a separate
    /// "has it ever been painted" bit, a black spool reads as unchanged from
    /// the default and the canvas keeps its own creation-time 0xE0E0E0, so the
    /// bypass swatch renders white forever (K2 Plus, spool 86 "Black ASA").
    /// The same black-is-not-unset trap is already handled in
    /// AmsState::notify_external_spool_changed() and
    /// filament_slot_override_store's identity check; this widget was missed.
    bool color_painted = false;
    uint32_t cached_color_rgb = 0;
    bool cached_has_spool = false;
    /// Paired like color_painted: the ring's natural default is "off", so a
    /// plain cached_active could not tell "never applied" from "already off"
    /// and the first engage-at-startup would be skipped.
    bool active_applied = false;
    bool cached_active = false;
    char cached_material[32] = {};
    int32_t cached_bypass_label_w = 0; ///< "Bypass" is constant — measure once

    [[nodiscard]] bool valid() const {
        return box != nullptr;
    }
};

/// Create the bypass spool overlay widgets as floating children of `parent`.
/// Clicks dispatch to the caller-supplied `on_click` LVGL event handler with
/// `user_data` carried via `lv_event_get_user_data()` — keeps the helper out
/// of the user-data slot of the spool widget itself.
BypassSpoolWidgets bypass_spool_create(lv_obj_t* parent, lv_event_cb_t on_click, void* user_data);

/// Destroy all owned widgets and zero the struct. Main-thread synchronous —
/// callers from within queued/observer callbacks must marshal first.
void bypass_spool_destroy(BypassSpoolWidgets& w);

/// Update the spool icon color (RGB 0xRRGGBB). No-op when unchanged.
void bypass_spool_set_color(BypassSpoolWidgets& w, uint32_t color_rgb);

/// Show filled spool when true, hollow outline when false. No-op when unchanged.
void bypass_spool_set_has_spool(BypassSpoolWidgets& w, bool has_spool);

/// Set the material label text above the spool. Empty string hides the label.
/// No-op when text is unchanged.
void bypass_spool_set_material(BypassSpoolWidgets& w, const char* material);

/// Draw (or clear) the active ring around the bypass card - the same
/// border-plus-glow a lane slot wears when it is the active node, via
/// ams_draw::set_active_ring(). No-op when unchanged.
void bypass_spool_set_active(BypassSpoolWidgets& w, bool active);

/// Position the spool box so its center sits at (`cx`, `cy`) in parent-relative
/// coordinates. The material label is placed above, the "Bypass" label below.
void bypass_spool_set_position(BypassSpoolWidgets& w, int32_t cx, int32_t cy);

/// Show or hide the whole bypass overlay — card, spool and both labels. Used
/// when the bypass node does not belong on the path at all; see
/// bypass_node_visible().
void bypass_spool_set_visible(BypassSpoolWidgets& w, bool visible);

} // namespace helix::ui
