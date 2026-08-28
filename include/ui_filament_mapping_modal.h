// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_slot_picker.h"
#include "ui_modal.h"

#include "filament_mapper.h"

#include <functional>
#include <string>
#include <vector>

class FilamentMappingModalTestAccess;

namespace helix::ui {

/**
 * @brief Modal dialog showing all tool-to-slot filament mappings
 *
 * Displays per-tool rows with color swatches, slot info, and mismatch
 * warnings. Each row is tappable to open an inline slot picker context
 * menu for reassignment. Changes are applied immediately to the internal
 * mappings vector and reported via callback when the user clicks "Done".
 */
class FilamentMappingModal : public Modal {
  public:
    const char* get_name() const override {
        return "Filament Mapping";
    }
    const char* component_name() const override {
        return "filament_mapping_modal";
    }

    /// Set G-code tool info (colors, materials)
    void set_tool_info(const std::vector<helix::GcodeToolInfo>& tools);

    /// Set available AMS/toolchanger slots
    void set_available_slots(const std::vector<helix::AvailableSlot>& slots);

    /// Set current mappings (copied in)
    void set_mappings(std::vector<helix::ToolMapping> mappings);

    /// Callback when user clicks "Done" — receives updated mappings
    using MappingsUpdatedCallback = std::function<void(std::vector<helix::ToolMapping>)>;
    void set_on_mappings_updated(MappingsUpdatedCallback cb);

  protected:
    void on_show() override;
    void on_ok() override;
    void on_cancel() override;

  private:
    friend class ::FilamentMappingModalTestAccess;

    void rebuild_rows();
    lv_obj_t* create_tool_row(int tool_index);
    void on_row_tapped(int tool_index);
    void on_slot_selected(int tool_index, const FilamentSlotPicker::Selection& sel);
    std::string get_slot_display_text(const helix::ToolMapping& mapping) const;

    /// Find the AvailableSlot matching a mapping's (slot_index, backend_index).
    /// Returns nullptr if the mapping is auto or the slot is not found.
    const helix::AvailableSlot* find_mapped_slot(const helix::ToolMapping& mapping) const;

    void create_toggle_row();
    void on_toggle_changed(bool auto_color);
    void recalculate_mappings();

    // State
    std::vector<helix::GcodeToolInfo> tool_info_;
    std::vector<helix::AvailableSlot> available_slots_;
    std::vector<helix::ToolMapping> mappings_;
    std::vector<helix::ToolMapping> original_mappings_;
    MappingsUpdatedCallback on_updated_cb_;
    bool auto_color_map_ = false;
    /// The persisted preference as it stood when the modal opened.
    ///
    /// on_toggle_changed() writes SettingsManager straight through, because the
    /// rows have to rebuild in the new mode immediately. That makes the write a
    /// side effect of LOOKING, so Cancel has to undo it — the snapshot is taken
    /// in on_show() alongside original_mappings_, and the two are restored
    /// together.
    bool original_auto_color_map_ = false;

    // UI
    lv_obj_t* tool_list_ = nullptr;
    lv_obj_t* toggle_switch_ = nullptr;
    FilamentSlotPicker slot_picker_;

    // Per-row dropdown trigger widgets (indexed by tool_index)
    std::vector<lv_obj_t*> trigger_widgets_;
};

} // namespace helix::ui
