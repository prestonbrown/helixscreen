// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"
#include "lvgl/lvgl.h"
#include "quick_action_slots.h"
#include "standard_macros.h"
#include "subject_managed_panel.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace helix {

class LedWidget;
class PrinterState;

/// The four Quick Actions slots on the Controls panel: each shows a standard
/// macro, the light toggle, or nothing, and publishes the subjects the panel's
/// XML binds (`macro_N_visible`, `macro_N_available`, `macro_N_name`,
/// `macro_N_light`, `macro_header_visible`).
///
/// Which kind a slot shows comes from quick_action_slots.h; this owns the
/// subjects, the observers that re-resolve them, and what a tap does.
class QuickActionButtons {
  public:
    // Out of line: the light cells are unique_ptr<LedWidget> to an incomplete type.
    QuickActionButtons();
    ~QuickActionButtons();

    QuickActionButtons(const QuickActionButtons&) = delete;
    QuickActionButtons& operator=(const QuickActionButtons&) = delete;

    /// Register the slot subjects. The subject storage lives here, so @p subjects
    /// must outlive this object's use of them (deinit before destruction).
    void init_subjects(SubjectManager& subjects);

    /// Build the light cells inside @p panel and start following StandardMacros
    /// and the LED controller.
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen, PrinterState& printer_state,
               IMoonrakerAPI* api);

    /// Detach the light cells while LVGL is still valid.
    void release_widgets();

    /// Re-read the stored slots and re-resolve every button.
    void reload();

    /// Run slot @p index (0-3). Raises the run confirmation when the Safety
    /// setting asks for one; @p owner scopes that dialog's callbacks.
    void execute(size_t index, IMoonrakerAPI* api, const LifetimeToken& owner);

  private:
    void refresh();
    void load_config();
    void update_button(StandardMacros& macros, const std::optional<StandardMacroSlot>& slot,
                       size_t index);
    void run(size_t index, IMoonrakerAPI* api, const std::map<std::string, std::string>& params);

    std::array<std::optional<StandardMacroSlot>, 4> slots_;

    // Two independent gates, because a macro button has three states, not two:
    //   visible   0 = nothing is assigned to this slot, do not render it
    //   available 0 = rendered but not usable: the slot resolves to a macro the
    //                 connected printer does not define
    // A slot the user configured against a macro this printer lacks stays
    // visible and goes disabled, so the button that stopped working is still
    // where they left it instead of silently vanishing.
    std::array<lv_subject_t, 4> visible_{};
    std::array<lv_subject_t, 4> available_{};
    std::array<lv_subject_t, 4> name_{};
    std::array<std::array<char, 64>, 4> name_buf_{};
    /// 1 while a slot shows the light toggle instead of a macro
    std::array<lv_subject_t, 4> light_{};
    lv_subject_t header_visible_{};

    /// The slots as stored; a never-written slot may take the light by default
    /// (see resolve_quick_slots).
    StoredQuickSlots stored_;

    /// Each slot's light cell reuses LedWidget, the class behind the home light
    /// tile, so the bulb shows on/off, brightness and colour, and a tap toggles
    /// the chamber light.
    std::array<std::unique_ptr<LedWidget>, 4> led_widgets_;

    ObserverGuard macros_version_observer_;
    ObserverGuard led_controllable_observer_;
    ui::WidgetRef run_confirmation_dialog_;
};

} // namespace helix
