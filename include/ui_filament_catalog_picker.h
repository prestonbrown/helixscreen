// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_catalog_selector.h"
#include "ui_modal.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/// Offline branded-filament picker. Thin Modal wrapper around the shared
/// FilamentCatalogSelector fragment; emits one EffectiveFilament by value.
/// Knows nothing about slots or presets — callers decide what to do with the
/// result.
class FilamentCatalogPickerModal : public Modal {
  public:
    using SelectCallback = std::function<void(const helix::printer::EffectiveFilament&)>;

    FilamentCatalogPickerModal() = default;
    ~FilamentCatalogPickerModal() override;
    FilamentCatalogPickerModal(const FilamentCatalogPickerModal&) = delete;
    FilamentCatalogPickerModal& operator=(const FilamentCatalogPickerModal&) = delete;

    /// seed_type: pre-select the Type dropdown (preset path); nullopt = free (AMS path).
    void show(lv_obj_t* parent, std::optional<std::string> seed_type, SelectCallback on_select);
    /// allowed_types: restrict the Type dropdown to this set (AMS backend whitelist).
    void show(lv_obj_t* parent, std::optional<std::string> seed_type,
              std::optional<std::vector<std::string>> allowed_types, SelectCallback on_select);

    /// Set before calling show() to reveal a "Reset to defaults" row (preset-editing
    /// context only).
    void set_reset_callback(std::function<void()> cb);

    [[nodiscard]] const char* get_name() const override {
        return "Filament Catalog Picker";
    }
    [[nodiscard]] const char* component_name() const override {
        return "filament_catalog_picker";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    void handle_select_button();

    friend struct FilamentPickerTestAccess;

    static void register_callbacks();
    static bool callbacks_registered_;
    static FilamentCatalogPickerModal* active_instance_;

    FilamentCatalogSelector selector_;
    std::optional<std::string> seed_type_;
    std::optional<std::vector<std::string>> allowed_types_;
    SelectCallback on_select_;
    std::function<void()> reset_callback_; // set -> show "Reset to defaults" row
};

} // namespace helix::ui
