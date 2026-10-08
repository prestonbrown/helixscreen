// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"
#include "ui_observer_guard.h"

#include <functional>
#include <string>

/**
 * @file ui_change_host_modal.h
 * @brief Modal dialog for changing the Moonraker host connection
 *
 * Allows users to enter a new IP/hostname and port, test the connection,
 * and save the new configuration. Reconnection is handled by the caller
 * via the completion callback.
 *
 * ## Usage:
 * @code
 * auto modal = std::make_unique<ChangeHostModal>();
 * modal->set_completion_callback([](bool changed) {
 *     if (changed) { // reconnect to new host }
 * });
 * modal->show_modal(lv_screen_active());
 * @endcode
 */
namespace helix::ui {

/**
 * @brief Show the Change Host modal from anywhere, with the standard reconnect
 *
 * ChangeHostModal keeps a static active_instance_, so exactly one owner may
 * exist; this holds it. The reconnect sequence (suppress the recovery dialog,
 * disconnect, rebuild ws:// and http:// URLs, reconnect through the discovery
 * pipeline) lives here rather than at each call site so the settings row and
 * the connection-failed prompt cannot drift apart.
 *
 * @param extra_on_complete Optional caller-specific work, run on save only
 *                          (e.g. the settings panel refreshing its host label)
 */
void show_change_host_modal(std::function<void(bool changed)> extra_on_complete = nullptr);

/**
 * @brief Prompt that the printer is unreachable, offering to fix the address
 *
 * Replaces an OK-only error modal for CONNECTION_FAILED: on a stale address,
 * acknowledging the error leaves the user exactly where they started, and the
 * setting itself is buried under Settings > System > Printer Host.
 *
 * Safe to call from any thread — marshals itself to the main thread, which the
 * libhv event-loop thread relies on.
 *
 * @param title   Dialog title
 * @param message Body text; should name the host:port that could not be reached
 */
void show_connection_failed_modal(const std::string& title, const std::string& message);

} // namespace helix::ui

class ChangeHostModal : public Modal {
  public:
    using CompletionCallback = std::function<void(bool changed)>;

    ChangeHostModal();
    ~ChangeHostModal() override;

    // Non-copyable
    ChangeHostModal(const ChangeHostModal&) = delete;
    ChangeHostModal& operator=(const ChangeHostModal&) = delete;

    /**
     * @brief Show the change host modal
     * @param parent Parent screen for the modal
     * @return true if modal was created successfully
     */
    bool show_modal(lv_obj_t* parent);

    /**
     * @brief Set callback for when modal closes
     * @param callback Function called with true if host was changed, false if cancelled
     */
    void set_completion_callback(CompletionCallback callback);

    // Modal interface
    const char* get_name() const override {
        return "Change Host";
    }
    const char* component_name() const override {
        return "change_host_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    // === Subjects for XML binding ===
    SubjectManager subjects_;
    lv_subject_t host_ip_subject_;
    lv_subject_t host_port_subject_;
    lv_subject_t testing_subject_;
    lv_subject_t validated_subject_;

    char host_ip_buf_[256] = {0};
    char host_port_buf_[8] = {0};
    bool subjects_initialized_ = false;

    /// Test Connection moved the live client to the typed host.
    bool client_borrowed_ = false;

    // === Completion callback ===
    CompletionCallback completion_callback_;

    // === Input change observers (reset validation on edit) ===
    ObserverGuard host_ip_observer_;
    ObserverGuard host_port_observer_;

    // === Internal methods ===
    void init_subjects();
    void deinit_subjects();
    void handle_test_connection();
    void handle_save();
    void handle_cancel();
    void set_status(const char* icon_name, const char* color_token, const char* text);
    void on_test_success();
    void on_test_failure();
    static void on_input_changed_cb(lv_observer_t* observer, lv_subject_t* subject);

    // === Static callback registration ===
    static void register_callbacks();
    static bool callbacks_registered_;
    static ChangeHostModal* active_instance_;

    // === Static callbacks ===
    static void on_test_connection_cb(lv_event_t* e);
    static void on_save_cb(lv_event_t* e);
    static void on_cancel_cb(lv_event_t* e);
};
