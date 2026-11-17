// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {

/// Register the picker's row callback. Every widget that opens a
/// LightTargetPicker calls it from its own registration.
void register_light_target_picker();

/// One row per light_picker_devices() entry, then All
/// lights. Every home surface that switches a chosen set of lights (a light
/// button, the printer image's light chip) picks its `led` key here; the key
/// resolves through LedController::light_targets().
class LightTargetPicker : public helix::ui::ContextMenu {
    HELIX_CONTEXT_MENU_KIND(LightTargetPicker)

  public:
    using OnPick = std::function<void(const std::string& key)>;

    /// Opens below @p anchor under @p title (translated by the caller), with
    /// the device @p key resolves to highlighted, or the All lights row for
    /// kLightButtonAll. A row tap hides the picker and hands its key (a device
    /// id or kLightButtonAll) to @p on_pick.
    void show_for(lv_obj_t* parent, lv_obj_t* anchor, const std::string& key, const char* title,
                  OnPick on_pick);

    static void row_cb(lv_event_t* e);

  protected:
    const char* xml_component_name() const override {
        return "led_picker";
    }
    /// Same clamp as the fan picker: readable on a 480px panel, not sprawling on 1024px.
    CardWidth card_width() const override {
        return {30, 160, 240};
    }
    void on_created(lv_obj_t* menu) override;

  private:
    /// The device behind each row as the picker drew it, so a tap picks what the
    /// user saw even if the device list changed since.
    std::vector<std::string> row_ids_;
    OnPick on_pick_;
};

} // namespace helix
