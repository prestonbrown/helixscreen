// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_material_z_offset.h
 * @brief Settings > Printing > Z Offset by Material: the opt-in toggle, each
 *        common material's offset with its usual bed temperature and stored-mesh
 *        mark, and the one-time setup where the printer has no storage for it
 *        (prestonbrown/helixscreen#2142).
 */

#pragma once

#include "core/observer_guard.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <array>
#include <string>

namespace helix::settings {

class MaterialZOffsetOverlay : public OverlayBase {
  public:
    /// The rows: DEFAULT, then the common materials.
    static constexpr std::array<const char*, 8> kMaterials = {"DEFAULT", "PLA", "PETG", "ABS",
                                                              "ASA",     "TPU", "PA",   "PC"};

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Z Offset by Material";
    }
    bool destroy_on_close() const override {
        return true;
    }

    lv_obj_t* create(lv_obj_t* parent) override;

    /// Open the keypad for @p material's offset ("DEFAULT" for the fallback).
    void edit_offset(const std::string& material);

  protected:
    void before_show() override;
    void on_ui_destroyed() override;

  private:
    void refresh_values();

    SubjectManager subjects_;
    std::array<helix::Subject, kMaterials.size()> value_subjects_{};
    std::array<helix::Subject, kMaterials.size()> temp_subjects_{};
    std::array<helix::Subject, kMaterials.size()> mesh_subjects_{};
    char value_bufs_[kMaterials.size()][32]{};
    char temp_bufs_[kMaterials.size()][96]{};
    /// The setup's two lines of code, shown as written in every language.
    helix::Subject macro_line_subject_{};
    helix::Subject slicer_line_subject_{};
    char macro_line_buf_[96]{};
    char slicer_line_buf_[64]{};
    std::string editing_title_; ///< The open keypad's title
    ObserverGuard rev_observer_;
};

inline MaterialZOffsetOverlay& get_material_z_offset_overlay() {
    return lazy_global<MaterialZOffsetOverlay>("MaterialZOffsetOverlay");
}

} // namespace helix::settings
