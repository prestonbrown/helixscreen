// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_color_picker.h"
#include "ui_modal.h"

#include "spoolman_types.h"
#include "subject_managed_panel.h"

#include <functional>
#include <string>

class IMoonrakerAPI;

namespace helix::ui {

/**
 * @file ui_spoolman_edit_modal.h
 * @brief Modal dialog for editing Spoolman spool properties
 *
 * Displays a form with editable spool fields (remaining weight, spool weight,
 * price, lot number, notes) alongside read-only info (vendor, material, color)
 * and a 3D spool preview that updates live.
 *
 * Uses PATCH via Moonraker proxy to save changes to Spoolman.
 *
 * ## Usage:
 * @code
 * helix::ui::SpoolEditModal modal;
 * modal.set_completion_callback([](bool saved) {
 *     if (saved) { refresh_spool_list(); }
 * });
 * modal.show_for_spool(parent, spool_info, api);
 * @endcode
 */
class SpoolEditModal : public Modal {
  public:
    using CompletionCallback = std::function<void(bool saved)>;

    SpoolEditModal();
    ~SpoolEditModal() override;

    // Non-copyable
    SpoolEditModal(const SpoolEditModal&) = delete;
    SpoolEditModal& operator=(const SpoolEditModal&) = delete;

    /**
     * @brief Show modal for editing a spool
     * @param parent Parent screen
     * @param spool Spool info to edit
     * @param api IMoonrakerAPI for PATCH calls
     * @return true if modal was created successfully
     */
    bool show_for_spool(lv_obj_t* parent, const SpoolInfo& spool, IMoonrakerAPI* api);

    /**
     * @brief Set callback for when editing completes
     */
    void set_completion_callback(CompletionCallback callback);

    // Modal interface
    [[nodiscard]] const char* get_name() const override {
        return "Spool Edit Modal";
    }
    [[nodiscard]] const char* component_name() const override {
        return "spoolman_edit_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    // === State ===
    SpoolInfo original_spool_;
    SpoolInfo working_spool_;
    IMoonrakerAPI* api_ = nullptr;
    CompletionCallback completion_callback_;
    ColorPicker color_picker_;

    bool subjects_initialized_ = false;
    bool populating_ = false;
    void init_subjects();
    void deinit_subjects();

    // === Subjects ===
    lv_subject_t save_button_text_subject_{};
    char save_button_text_buf_[16]{};
    char save_button_text_prev_buf_[16]{};

    // === Internal Methods ===
    void populate_fields();
    void read_fields_into(SpoolInfo& spool);
    void register_textareas();
    void update_spool_preview();
    [[nodiscard]] bool is_dirty() const;
    bool validate_fields();
    void update_save_button_text();

    // === Event Handlers ===
    void handle_close();
    void handle_field_changed();
    void handle_reset();
    void handle_save();
    void handle_color_clicked();
#if HELIX_HAS_LABEL_PRINTER
    void handle_print_label();
#endif

    // === Static Callback Registration ===
    static void register_callbacks();
    static bool callbacks_registered_;
    static SpoolEditModal* active_instance_;

    // === Static Callbacks ===
    static SpoolEditModal* get_instance_from_event(lv_event_t* e);
    static void on_close_cb(lv_event_t* e);
    static void on_field_changed_cb(lv_event_t* e);
    static void on_reset_cb(lv_event_t* e);
    static void on_save_cb(lv_event_t* e);
    static void on_color_clicked_cb(lv_event_t* e);
#if HELIX_HAS_LABEL_PRINTER
    static void on_print_label_cb(lv_event_t* e);
#endif
};

} // namespace helix::ui
