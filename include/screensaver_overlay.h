// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "ui_widget_ref.h"

#include <lvgl.h>

namespace helix::ui {

/**
 * @brief The black, touch-absorbing full-screen overlay a screensaver shows on lv_layer_top()
 *
 * Opaque until an opaque canvas covers it: top-layer children are never cover-culled, so an
 * opaque background under a full-screen canvas would be filled every frame.
 */
class SaverOverlay {
  public:
    SaverOverlay() = default;
    SaverOverlay(const SaverOverlay&) = delete;
    SaverOverlay& operator=(const SaverOverlay&) = delete;

    /// Creates the opaque black overlay. Does nothing while one exists.
    void create();

    /// Stops filling the background, for when an opaque canvas covers the whole overlay.
    void make_transparent();

    /// Hides the overlay and deletes it with its children on a later timer pass.
    void destroy();

    lv_obj_t* obj() const {
        return obj_;
    }

  private:
    helix::ui::WidgetRef obj_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
