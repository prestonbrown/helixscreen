// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include <lvgl.h>
#include <string>
#include <unordered_map>

class NavigationManagerTestAccess; // NAMESPACE_OK: test accessor, global scope

namespace helix::ui {

/**
 * @brief What sits behind the overlay stack: the dismiss backdrop and its bookkeeping
 *
 * The primary backdrop is a darkened snapshot of the screen taken when the first
 * overlay opens (or a translucent dim layer where a snapshot is too costly). It is
 * a child of the screen, so any path that deletes the screen frees it without
 * going through the overlay stack; the handle clears itself when that happens.
 * Deletions go through safe_delete_deferred, since they run inside UpdateQueue
 * drains. Nested overlays can carry a backdrop of their own, tracked per overlay.
 */
class OverlayBackdrop {
    friend class ::NavigationManagerTestAccess;

  public:
    /// Take the snapshot of @p screen as the primary backdrop and wire @p click_cb to
    /// its press and click. The live E-stop and the @p arriving overlay stay out of
    /// the image: both sit above the backdrop. No-op replacement if the snapshot
    /// cannot be created.
    void adopt(lv_obj_t* screen, lv_obj_t* arriving, lv_event_cb_t click_cb);

    /// Re-take the snapshot from the live widget tree, for a change outside the
    /// overlay that lands behind it (the navbar). Hides every screen child except
    /// @p app_layout and shows @p base_panel while the picture is taken, so the new
    /// snapshot matches the original rather than the overlays stacked on top.
    /// @p base_panel is nullptr or live. No-op without a live snapshot.
    void refresh(lv_obj_t* app_layout, lv_obj_t* base_panel);

    /// The primary backdrop, or nullptr.
    [[nodiscard]] lv_obj_t* primary() const {
        return primary_.get();
    }

    /// A dim layer rather than a snapshot: translucent over the live screen.
    [[nodiscard]] bool primary_is_dim_layer() const;

    /// Whether the theme palette changed since the snapshot was taken.
    [[nodiscard]] bool palette_changed() const;

    /// Delete the primary backdrop, deferred.
    void release_primary();

    /// Delete the primary backdrop now, for teardown outside any queued callback.
    void delete_primary_now();

    /// Delete @p overlay's own backdrop, deferred, if it has one.
    void retire(lv_obj_t* overlay);

    /// Delete every overlay's own backdrop, deferred.
    void retire_all();

    /// Forget the backdrop tracked for a deleted overlay; LVGL owns the memory.
    void scrub(lv_obj_t* overlay);

    /// Move the backdrop tracked for @p old_overlay to @p new_overlay.
    void rekey(lv_obj_t* old_overlay, lv_obj_t* new_overlay);

    /// Record the on-screen keyboard's visibility at the backdrop's press. LVGL's
    /// click-focus DEFOCUS hides the keyboard between PRESSED and CLICKED, so it is
    /// captured here.
    void note_press(bool keyboard_visible) {
        press_keyboard_visible_ = keyboard_visible;
    }

    /// True once if the tap that just clicked hid the keyboard and must not also
    /// dismiss the overlay. Cleared on read.
    bool take_keyboard_dismiss();

    /// Forget every tracked backdrop and delete the primary now. Nothing is left
    /// valid after lv_deinit().
    void reset();

  private:
    WidgetRef primary_;
    std::string palette_key_;
    bool press_keyboard_visible_ = false;
    std::unordered_map<lv_obj_t*, WeakWidget> nested_;
};

} // namespace helix::ui
