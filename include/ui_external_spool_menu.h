// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_context_menu.h"
#include "ui_bypass_toggle_controller.h"

#include "lvgl.h"

#include <functional>
#include <memory>

namespace helix::ui {

class AmsOperationSidebar;

/// What a surface contributes to the External Spool menu.
///
/// Deliberately NOT "what the surface does when an entry is tapped": the rules
/// that make a bypass op correct — engaging bypass before a load, above all —
/// belong to the menu, so that every surface showing it gets them. A surface
/// supplies only the parts that are genuinely its own.
struct ExternalSpoolMenuHooks {
    /// EDIT and SPOOLMAN. Unset, the menu raises the external-spool editor
    /// itself, which needs only a parent screen and the global API.
    std::function<void(bool open_on_picker)> on_edit;

    /// How this surface DISPATCHES a load, once bypass is engaged — not the
    /// whole Load action. Unset, the menu dispatches through the shared
    /// executor, which is right for a surface with no operation stepper.
    std::function<void()> on_load;

    /// Unload counterpart of on_load. Worth setting more often: a bypass unload
    /// plans onto tier 1, so a surface with a stepper has something to show.
    std::function<void()> on_unload;

    /// This surface's bypass controller. Drives the Enable/Disable Bypass entry
    /// AND the engage that must precede a load — with bypass disengaged,
    /// plan_load() refuses kExternalSpoolSlot outright
    /// (requires_slot_selection_for_load() is !is_bypass_active()), so a Load
    /// offered without it is a dead end.
    ///
    /// Null hides the toggle entry and dispatches Load with no engage, which is
    /// only correct where bypass cannot be driven at all.
    BypassToggleController* toggle = nullptr;
};

/// The hooks an AMS panel contributes: its operation sidebar dispatches Load and
/// Unload so the stepper is built, and lends its bypass controller. Written once
/// because AmsPanel and AmsOverviewPanel contribute exactly the same thing, and
/// two copies of that answer is how the two panels drift.
///
/// @param sidebar May be null; the hooks then fall back to the menu's own
///                dispatch, which is correct but shows no stepper.
[[nodiscard]] ExternalSpoolMenuHooks sidebar_external_spool_hooks(AmsOperationSidebar* sidebar);

/// Open the external-spool editor directly. Needs only a parent screen and the
/// global API.
void open_external_spool_editor(lv_obj_t* parent_screen, bool open_on_picker = false);

/// Show the bypass spool's menu. @p anchor_widget is the spool's own widget,
/// which the card goes around; with @p at_tap it is a canvas the spool is drawn
/// on, and the card hangs off the tap that opened it instead.
void show_external_spool_menu(lv_obj_t* parent_screen, lv_obj_t* anchor_widget,
                              std::unique_ptr<AmsContextMenu>& context_menu,
                              ExternalSpoolMenuHooks hooks, bool at_tap = false);

} // namespace helix::ui
