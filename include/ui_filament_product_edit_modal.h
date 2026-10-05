// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "filament_product_form.h"

#include <functional>
#include <string>

namespace helix::ui {

/**
 * @file ui_filament_product_edit_modal.h
 * @brief Modal for adding or editing a user filament "product".
 *
 * Writes the authored (sparse) product to config/user_filaments.json via
 * FilamentCatalog::save_user_products — the first production caller of that
 * write path. Add mode starts from an empty form; edit mode pre-fills from the
 * resolved catalog entry and, on save, writes an overlay entry that overrides
 * that id (whether the source was a built-in or a prior user entry).
 *
 * Main-thread only: everything runs from UI event handlers, so there is no
 * AsyncLifetimeGuard / background-thread work here.
 *
 * ## Usage:
 * @code
 * FilamentProductEditModal modal;
 * modal.set_on_saved([](const std::string& saved_id) { refresh_list(); });
 * modal.show_for_add(parent);            // add
 * modal.show_for_edit(parent, "poly-x"); // edit / override by id
 * @endcode
 */
class FilamentProductEditModal : public Modal {
  public:
    /// Fired after a successful save or delete/restore. @p saved_id is the id
    /// that was written (empty after a delete/restore-defaults). Lets the host
    /// reload the catalog and focus the change.
    using SavedCallback = std::function<void(const std::string& saved_id)>;

    FilamentProductEditModal();
    ~FilamentProductEditModal() override;

    FilamentProductEditModal(const FilamentProductEditModal&) = delete;
    FilamentProductEditModal& operator=(const FilamentProductEditModal&) = delete;

    void set_on_saved(SavedCallback cb);

    /// Empty form; id defaults from brand+name when left blank, type defaults
    /// to the first material.
    bool show_for_add(lv_obj_t* parent);
    /// Pre-fill from the resolved catalog entry for @p product_id.
    bool show_for_edit(lv_obj_t* parent, const std::string& product_id);

    [[nodiscard]] const char* get_name() const override {
        return "Filament Product Edit Modal";
    }
    [[nodiscard]] const char* component_name() const override {
        return "filament_product_edit_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    enum class Mode { Add, Edit };

    // === State ===
    Mode mode_ = Mode::Add;
    std::string edit_id_;      ///< id being edited (edit mode)
    bool is_builtin_ = false;  ///< edit target resolves in the shipped catalog
    bool has_overlay_ = false; ///< an overlay entry already exists for edit_id_
    SavedCallback on_saved_;

    bool subjects_initialized_ = false;
    SubjectManager subjects_;

    // Secondary (destructive) button label: "Delete" (user entry) or
    // "Restore Defaults" (built-in with an override). Hidden in Add mode.
    lv_subject_t secondary_text_subject_{};
    char secondary_text_buf_[40]{};

    // === Internal Methods ===
    void init_subjects();
    void populate_type_dropdown(const std::string& selected_type);
    void populate_fields();
    void register_keyboards();
    FilamentFormValues read_form() const;
    void configure_secondary_button();
    void handle_save();
    void handle_secondary();
    void handle_close();

    // === Static Callback Registration ===
    static void register_callbacks();
    static bool callbacks_registered_;
    static FilamentProductEditModal* active_instance_;

    static void on_save_cb(lv_event_t* e);
    static void on_secondary_cb(lv_event_t* e);
    static void on_close_cb(lv_event_t* e);
};

} // namespace helix::ui
