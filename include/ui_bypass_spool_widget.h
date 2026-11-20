// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "lvgl.h"

#include <cstdint>
#include <functional>

namespace helix {
class AmsBackend;
struct AmsSystemInfo;
struct SlotInfo;
} // namespace helix

namespace helix::ui {

/**
 * @brief Whether the bypass node belongs on the filament path at all.
 *
 * AFC publishes a virtual bypass sensor whether or not the user has a bypass
 * wired, so `supports_bypass` is effectively always true there. Drawn anyway,
 * the node is painted from the external-spool slot, which carries the
 * currently-loaded lane's material and colour, on a machine that may have no
 * bypass at all (#1229).
 *
 * On AFC the node is therefore hidden while bypass is disengaged, unless the
 * user opts back in via the AMS setting. Other backends are unaffected: their
 * bypass is a real physical position, so it stays visible whenever supported.
 *
 * Pure, so the LVGL-bound render sites share one tested rule instead of each
 * carrying the condition inline.
 *
 * @param supports_bypass Backend reports a bypass position at all
 * @param bypass_active Bypass is currently engaged (firmware state, not a proxy)
 * @param bypass_is_virtual The reported bypass is virtual, not a physical
 *        position (AmsBackend::bypass_is_virtual())
 * @param always_show User setting: keep it visible even when disengaged
 * @return true when the bypass node should be rendered
 */
[[nodiscard]] constexpr bool bypass_node_visible(bool supports_bypass, bool bypass_active,
                                                 bool bypass_is_virtual, bool always_show) {
    if (!supports_bypass) {
        return false;
    }
    if (bypass_active) {
        return true;
    }
    if (!bypass_is_virtual) {
        return true;
    }
    return always_show;
}

/// Gather bypass_node_visible()'s inputs from the live backend and settings.
/// Returns false when no backend is attached.
[[nodiscard]] bool bypass_node_visible_for(const AmsBackend* backend);

/// The same answer from a system reading the caller already holds, which saves the copy of
/// the whole system the one-argument form takes. @p info must be @p backend's own reading.
[[nodiscard]] bool bypass_node_visible_for(const AmsBackend* backend, const AmsSystemInfo& info);

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
    WeakWidget box;            ///< Card container with click target
    WeakWidget spool_canvas;   ///< The spool icon inside the box
    WeakWidget bypass_label;   ///< "Bypass" text below the box
    WeakWidget material_label; ///< Material name above the box (hidden if empty)
    int32_t spool_size = 0;    ///< The spool icon's edge
    int32_t box_size = 0;      ///< Box edge: the spool plus padding

    // Cached state for change-detection (avoids spurious invalidates on every
    // panel refresh). Not part of the public API surface — read via setters.
    /// Paired with cached_color_rgb because black IS 0: without a separate
    /// "has it ever been painted" bit, a black spool reads as unchanged from
    /// the default and the canvas keeps its own creation-time 0xE0E0E0, so the
    /// bypass swatch renders white forever. The same black-is-not-unset rule
    /// applies in AmsState::notify_external_spool_changed() and
    /// filament_slot_override_store's identity check.
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

/// Edge of the bypass card in px: a lane-sized spool plus the card's padding.
/// The path canvases keep the spool's centre this far from what it must clear.
[[nodiscard]] int32_t bypass_spool_box_size();

/// Edge of the bypass card around a spool @p spool_size px across.
[[nodiscard]] int32_t bypass_spool_box_size(int32_t spool_size);

/// Where the bypass spool stands beside a drawn unit, and how large it is drawn.
struct BypassSpoolSpot {
    int32_t spool = 0;       ///< The spool icon's edge
    int32_t box = 0;         ///< The card's edge (bypass_spool_box_size(spool))
    int32_t cx = 0;          ///< The card's centre, same coordinates as the inputs
    int32_t top = 0;         ///< The card's top, where side_entry
    bool side_entry = false; ///< Below the unit: the tube runs into the card's side
};

/// The room a unit view gives the bypass spool's card, in one set of coordinates.
struct BypassSpoolRoom {
    int32_t home_cx = 0;    ///< The card's centre where nothing moves it
    int32_t right = 0;      ///< The card's right edge stands no further right
    int32_t box_bottom = 0; ///< Just under the card (bypass_spool_card_bottom_on())
    int32_t top = 0;        ///< The card's material label rises no higher
    int32_t gap = 0;        ///< Kept between the card and the unit
};

/// The bypass spool beside a drawn unit is drawn as large as the unit's own spools
/// (@p lane_spool), never smaller than @p min_spool, the size it has everywhere else. It grows
/// only while its card, with its material label above it, fits @p room right of the unit:
/// @p unit_right(top, bottom) is the rightmost x the unit covers in rows @p top to @p bottom
/// (INT32_MIN where it covers none). Its centre stays at the room's home unless the card
/// must move right to clear the unit; when no larger card fits, it keeps @p min_spool at home.
[[nodiscard]] BypassSpoolSpot
bypass_spool_spot(int32_t lane_spool, int32_t min_spool, const BypassSpoolRoom& room,
                  const std::function<int32_t(int32_t top, int32_t bottom)>& unit_right);

/// The room right of the hub and below a drawn unit, in one set of coordinates.
struct BypassSpoolBelowRoom {
    int32_t left = 0;    ///< The card's left edge stands no further left (clear of the hub)
    int32_t right = 0;   ///< The card's right edge stands no further right
    int32_t bottom = 0;  ///< The card's bottom, with its "Bypass" label under it, no lower
    int32_t merge_y = 0; ///< The merge line, which runs into the card's side
    int32_t gap = 0;     ///< Kept between the card's material label and the unit
};

/// The bypass spool on the merge line right of the hub, below the unit: as large as the unit's
/// spools (@p lane_spool) as far as its card, with its material label above it, fits under the
/// unit and over @p room's bottom, standing across the merge line so the tube runs into its
/// side. @p unit_bottom(x) is the lowest row the unit covers at or right of x (INT32_MIN where
/// it covers none). The card stands at the room's right. With no card larger than
/// @p min_spool, the spot is @p min_spool and not side_entry: there is no lower spot.
[[nodiscard]] BypassSpoolSpot
bypass_spool_spot_below(int32_t lane_spool, int32_t min_spool, const BypassSpoolBelowRoom& room,
                        const std::function<int32_t(int32_t x)>& unit_bottom);

/// The spot the bypass spool takes: @p beside while it reaches @p lane_spool there, else
/// whichever of @p beside and @p below draws the larger spool (beside on a tie).
[[nodiscard]] BypassSpoolSpot bypass_spool_better(const BypassSpoolSpot& beside,
                                                  const BypassSpoolSpot& below, int32_t lane_spool);

/// Stand the card with its top at @p top, centred on @p cx, its "Bypass" label under it and
/// its material label above it.
void bypass_spool_set_card_position(BypassSpoolWidgets& w, int32_t cx, int32_t top);

/// The room the "Bypass" label takes under a card standing below the unit.
int32_t bypass_spool_label_below_room(const BypassSpoolWidgets& w);

/// Draw the spool @p spool_size px across, its card sized to hold it. No-op when unchanged.
void bypass_spool_set_size(BypassSpoolWidgets& w, int32_t spool_size);

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

/// Paint the bypass spool with a slot's filament: colour, multi-colour split and finish.
void bypass_spool_set_filament(BypassSpoolWidgets& w, const SlotInfo& slot);

/// Show filled spool when true, hollow outline when false. No-op when unchanged.
void bypass_spool_set_has_spool(BypassSpoolWidgets& w, bool has_spool);

/// Set the material label text above the spool. Empty string hides the label.
/// No-op when text is unchanged.
void bypass_spool_set_material(BypassSpoolWidgets& w, const char* material);

/// Draw (or clear) the active ring around the bypass card - the same
/// border-plus-glow a lane slot wears when it is the active node, via
/// ams_draw::set_active_ring(). No-op when unchanged.
void bypass_spool_set_active(BypassSpoolWidgets& w, bool active);

/// How far above the bypass merge line the spool's bottom edge stands: its tube leaves
/// the spool there and turns along the merge line, between the spool and its label.
int32_t bypass_spool_bottom_above_merge();

/// The room the material label takes above the spool's card.
int32_t bypass_spool_label_room();

/// Just under the card of a spool standing on the merge line at @p merge_y: its bottom edge.
int32_t bypass_spool_card_bottom_on(int32_t merge_y);

/// What the overlay among @p parent's children covers on screen: its spool box, and its material
/// label where that shows. False where @p parent holds no overlay or its box is hidden.
[[nodiscard]] bool bypass_spool_drawn_area(lv_obj_t* parent, lv_area_t* out);

/// Place the overlay on the bypass merge line at (`cx`, `cy`), parent-relative: the spool
/// box stands above it (bypass_spool_bottom_above_merge()), the material label above the
/// box, and the "Bypass" label below the line, clear of the tube.
void bypass_spool_set_position(BypassSpoolWidgets& w, int32_t cx, int32_t cy);

/// Bring the whole overlay in line with AmsState: shown only where the bypass node
/// belongs on the path, wearing the external spool's color, fill and material, and the
/// active ring while bypass is engaged. The caller repositions it afterwards, since the
/// material label's visibility changes what sits above the box.
void bypass_spool_sync_from_state(BypassSpoolWidgets& w);

/// The same, for a caller that has already decided whether the node belongs on the path
/// (bypass_node_visible_for) and so avoids a second read of the system info.
void bypass_spool_sync_from_state(BypassSpoolWidgets& w, bool node_visible);

/// Show or hide the whole bypass overlay — card, spool and both labels. Used
/// when the bypass node does not belong on the path at all; see
/// bypass_node_visible().
void bypass_spool_set_visible(BypassSpoolWidgets& w, bool visible);

} // namespace helix::ui
