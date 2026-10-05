// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"
#include "ui_split_button.h"

#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <functional>

/**
 * @file ui_shutdown_modal.h
 * @brief Scope-aware confirmation dialog for shutdown/reboot.
 *
 * Single subject drives the XML (multiple bind_flag bindings on one element
 * race — last observer wins — so we collapse scope×pending into one value):
 *
 *   shutdown_view_state:
 *     0 = single-scope confirm  (same host: one row of buttons)
 *     1 = dual-scope confirm    (separate hosts: Both / Printer / Screen rows)
 *     2 = screen shutting down  (progress spinner)
 *     3 = screen rebooting      (progress spinner)
 *     4 = printer shutting down (progress spinner)
 *     5 = printer rebooting     (progress spinner)
 *     6 = both shutting down    (progress spinner)
 *     7 = both rebooting        (progress spinner)
 *
 * In single-scope mode, both buttons invoke on_printer_*_cb_ — the host
 * running helixscreen IS the printer host, so Moonraker's machine.shutdown
 * brings the screen down with it.
 *
 * In dual-scope mode the widget composes the "both" callbacks itself
 * (printer Moonraker + local SystemPower), so the modal doesn't need to
 * know — it just fires the callback that was wired to each button.
 */
class ShutdownModal : public Modal {
  public:
    using ActionCallback = std::function<void()>;

    ShutdownModal() {
        init_subjects();
    }

    const char* get_name() const override {
        return "Shutdown";
    }
    const char* component_name() const override {
        return "shutdown_modal";
    }

    /// Single-scope (same host) — uses printer callbacks for both buttons.
    void set_single_callbacks(ActionCallback on_shutdown, ActionCallback on_reboot) {
        mode_ = Mode::Single;
        on_printer_shutdown_cb_ = std::move(on_shutdown);
        on_printer_reboot_cb_ = std::move(on_reboot);
        on_screen_shutdown_cb_ = nullptr;
        on_screen_reboot_cb_ = nullptr;
        on_both_shutdown_cb_ = nullptr;
        on_both_reboot_cb_ = nullptr;
        lv_subject_set_int(&view_state_subject_, 0);
    }

    /// Dual-scope (separate hosts) — six distinct callbacks: Both, Printer, Screen.
    void set_dual_callbacks(ActionCallback both_shutdown, ActionCallback both_reboot,
                            ActionCallback printer_shutdown, ActionCallback printer_reboot,
                            ActionCallback screen_shutdown, ActionCallback screen_reboot) {
        mode_ = Mode::Dual;
        on_both_shutdown_cb_ = std::move(both_shutdown);
        on_both_reboot_cb_ = std::move(both_reboot);
        on_printer_shutdown_cb_ = std::move(printer_shutdown);
        on_printer_reboot_cb_ = std::move(printer_reboot);
        on_screen_shutdown_cb_ = std::move(screen_shutdown);
        on_screen_reboot_cb_ = std::move(screen_reboot);
        lv_subject_set_int(&view_state_subject_, 1);
    }

    // Invoked from XML event callback free functions (registered in
    // shutdown_widget.cpp). Public so those free functions can reach them
    // after looking the modal up via the view root's user_data.
    void fire_screen_shutdown() {
        lv_subject_set_int(&view_state_subject_, 2);
        if (on_screen_shutdown_cb_)
            on_screen_shutdown_cb_();
    }
    void fire_screen_reboot() {
        lv_subject_set_int(&view_state_subject_, 3);
        if (on_screen_reboot_cb_)
            on_screen_reboot_cb_();
    }
    void fire_printer_shutdown() {
        lv_subject_set_int(&view_state_subject_, 4);
        if (on_printer_shutdown_cb_)
            on_printer_shutdown_cb_();
    }
    void fire_printer_reboot() {
        lv_subject_set_int(&view_state_subject_, 5);
        if (on_printer_reboot_cb_)
            on_printer_reboot_cb_();
    }
    void fire_both_shutdown() {
        lv_subject_set_int(&view_state_subject_, 6);
        if (on_both_shutdown_cb_)
            on_both_shutdown_cb_();
    }
    void fire_both_reboot() {
        lv_subject_set_int(&view_state_subject_, 7);
        if (on_both_reboot_cb_)
            on_both_reboot_cb_();
    }

  protected:
    void on_show() override {
        // Reset to confirm state for current mode; setter was called before show().
        lv_subject_set_int(&view_state_subject_, mode_ == Mode::Single ? 0 : 1);
        wire_cancel_button("btn_close");
        // Stamp this on the view root so XML event callbacks can walk up
        // from a clicked button and recover the owning modal instance.
        if (dialog_) {
            lv_obj_set_user_data(dialog_, this);
            // Reset split-button selections to "Both" each time the dual-scope
            // modal opens — XML's selected="0" only applies at parse time, so
            // a leftover "Printer" selection from a previous open would persist
            // otherwise.
            if (mode_ == Mode::Dual) {
                if (lv_obj_t* sb = lv_obj_find_by_name(dialog_, "split_restart")) {
                    ui_split_button_set_selected(sb, 0);
                }
                if (lv_obj_t* sb = lv_obj_find_by_name(dialog_, "split_shutdown")) {
                    ui_split_button_set_selected(sb, 0);
                }
            }
        }
    }

  private:
    enum class Mode { Single, Dual };
    Mode mode_ = Mode::Single;

    ActionCallback on_screen_shutdown_cb_;
    ActionCallback on_screen_reboot_cb_;
    ActionCallback on_printer_shutdown_cb_;
    ActionCallback on_printer_reboot_cb_;
    ActionCallback on_both_shutdown_cb_;
    ActionCallback on_both_reboot_cb_;

    static inline lv_subject_t view_state_subject_{};
    static inline bool subjects_initialized_ = false;

    static void init_subjects() {
        if (subjects_initialized_)
            return;
        subjects_initialized_ = true;

        lv_subject_init_int(&view_state_subject_, 0);

        auto* scope = lv_xml_component_get_scope("shutdown_modal");
        if (scope) {
            lv_xml_register_subject(scope, "shutdown_view_state", &view_state_subject_);
        } else {
            spdlog::warn("[ShutdownModal] Component scope not found — "
                         "ensure shutdown_modal.xml is registered first");
        }

        StaticSubjectRegistry::instance().register_deinit("ShutdownModal", []() {
            if (!subjects_initialized_)
                return;
            if (auto* scope = lv_xml_component_get_scope("shutdown_modal")) {
                lv_xml_unregister_subject(scope, "shutdown_view_state");
            }
            lv_subject_deinit(&view_state_subject_);
            subjects_initialized_ = false;
        });
    }
};
