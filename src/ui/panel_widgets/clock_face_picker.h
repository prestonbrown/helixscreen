// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "ui_context_menu.h"

#include "src/ui/panel_widgets/clock_face.h"
#include "src/ui/panel_widgets/clock_face_view.h"

#include <array>
#include <functional>

namespace helix {

/// Register the picker's row callback, from the clock widget's registration.
void register_clock_face_picker();

/**
 * @brief The clock tile's Face picker: Text, then every painted face with a still preview
 *
 * The LightTargetPicker pattern: a context menu below the tile, the current face highlighted,
 * a row tap hides the picker and hands the face to the caller. Each painted face's row holds a
 * small ClockFaceView of that face at the current time, freed when the picker closes.
 */
class ClockFacePicker : public helix::ui::ContextMenu {
    HELIX_CONTEXT_MENU_KIND(ClockFacePicker)

  public:
    using OnPick = std::function<void(ClockFace face)>;

    ~ClockFacePicker() override;

    /// Opens below `anchor` with `current` highlighted. A row tap hides the picker, then hands
    /// its face to `on_pick`.
    void show_for(lv_obj_t* parent, lv_obj_t* anchor, ClockFace current, OnPick on_pick);

    static void row_cb(lv_event_t* e);

    /// The preview of painted face `face`, for tests; nullptr for Text.
    const ClockFaceView* preview(ClockFace face) const;

  protected:
    const char* xml_component_name() const override {
        return "clock_face_picker";
    }
    CardWidth card_width() const override {
        return {40, 220, 340};
    }
    void on_created(lv_obj_t* menu) override;
    void on_backdrop_clicked() override;
    void on_close_clicked() override;

  private:
    void close();
    void free_previews();

    std::array<ClockFaceView, kClockFaceCount - 1> previews_;
    OnPick on_pick_;
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
