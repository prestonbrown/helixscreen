// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include "toolhead_preset_slots.h"

#include <functional>

namespace helix {

/// The Toolhead widget's preset picker, opened from its edit-mode gear. Three slot tabs;
/// the list under them offers the nine positions, Park, the Z steps, Macro and Empty for
/// the selected slot, and Macro switches it to the printer's macros. Every choice is
/// handed to the owner at once, which saves it.
class ToolheadPresetConfigModal : public Modal {
  public:
    using ChangeCallback = std::function<void(const ToolheadPresetSlots&)>;

    ToolheadPresetConfigModal(ToolheadPresetSlots slots, ChangeCallback on_change);

    const char* get_name() const override {
        return "Toolhead Presets";
    }
    const char* component_name() const override {
        return "toolhead_preset_config_modal";
    }
    HELIX_KIND_TAG(ToolheadPresetConfigModal)

    /// Edit slot @p index; the list goes back to the choices.
    void select_slot(std::size_t index);
    /// Put the choice at @p choice of the choice list into the selected slot. Macro opens
    /// the macro list instead.
    void pick_choice(std::size_t choice);
    /// Put macro @p name into the selected slot and go back to the choices.
    void pick_macro(const std::string& name);

    [[nodiscard]] const ToolheadPresetSlots& slots() const {
        return slots_;
    }
    [[nodiscard]] std::size_t selected_slot() const {
        return selected_;
    }
    [[nodiscard]] bool showing_macros() const {
        return showing_macros_;
    }

    /// The choices the list offers, in order: the nine positions, Park, Z up, Z down,
    /// Macro (its name left empty) and Empty.
    [[nodiscard]] static std::vector<ToolheadPresetSlot> choices();

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    void apply(const ToolheadPresetSlot& slot);
    void publish_slot_texts();
    void populate_list();

    ToolheadPresetSlots slots_;
    ChangeCallback on_change_;
    std::size_t selected_ = 0;
    bool showing_macros_ = false;
    lv_obj_t* list_ = nullptr;
};

/// The picker's subjects and XML callbacks; registered with the Toolhead widget.
void register_toolhead_preset_config();

} // namespace helix
