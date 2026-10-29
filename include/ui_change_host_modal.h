// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"
#include "ui_observer_guard.h"

#include "lan_sweep.h"
#include "mdns_discovery.h"
#include "utils/network_validation.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
 * @brief Show the same modal for adding a printer
 *
 * Starts from an empty host and writes nothing to the active printer's config: Save hands
 * the tested host and port to `on_add`, deferred past the modal's exit. Leaving any other way
 * reconnects the saved printer if Test Connection moved the client.
 */
void show_add_printer_modal(std::function<bool(const std::string& host, int port)> on_add);

/// A printer chooser closed on a selection: the prompt it was holding belongs to the
/// connection the user just moved away from, so it is dropped rather than shown.
void drop_held_connection_failed();

/**
 * @brief Prompt that the printer is unreachable, offering to fix the address
 *
 * Replaces an OK-only error modal for CONNECTION_FAILED: on a stale address,
 * acknowledging the error leaves the user exactly where they started, and the
 * setting itself is buried under Settings > Connection > Host.
 *
 * Safe to call from any thread — marshals itself to the main thread, which the
 * libhv event-loop thread relies on.
 *
 * @param title   Dialog title
 * @param message Body text; should name the host:port that could not be reached
 */
void show_connection_failed_modal(const std::string& title, const std::string& message);

/// How long the connection-failed prompt browses mDNS before it shows.
/// Under MdnsDiscovery's 3.5s query cycle, so the stop lands in its idle wait rather than
/// a socket read, and the first query's answers are all in.
constexpr uint32_t kRediscoveryWindowMs = 3000;

/**
 * @brief The one discovered Moonraker that is the saved printer at another address
 *
 * A printer is recognised by hostname, compared without case, ".local" or a trailing dot.
 * Empty when no printer carries any of @p identities, when more than one does, when one
 * still answers at the saved address (the address is not what is wrong), or when the saved
 * host is a name rather than an IP literal.
 */
std::optional<DiscoveredPrinter> find_moved_printer(const std::vector<DiscoveredPrinter>& found,
                                                    const std::string& saved_host, int saved_port,
                                                    const std::vector<std::string>& identities);

/// Where the connection-failed prompt gets its one-shot mDNS browse. Unset, it does not
/// browse at all; the app sets it at startup, outside --test.
void set_printer_rediscovery_source(std::function<std::unique_ptr<IMdnsDiscovery>()> source);

} // namespace helix::ui

class ChangeHostModal : public Modal {
  public:
    using CompletionCallback = std::function<void(bool changed)>;
    /// Returns whether it switched to a printer: a switch retargets the client itself.
    using AddCallback = std::function<bool(const std::string& host, int port)>;

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
    /// With `on_add` set the modal adds a printer instead of changing the active one's host.
    bool show_modal(lv_obj_t* parent, AddCallback on_add = nullptr);

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
    lv_subject_t host_ip_subject_{};
    lv_subject_t host_port_subject_{};
    /// The active printer's Moonraker API key; the field is masked until revealed.
    lv_subject_t api_key_subject_{};
    lv_subject_t testing_subject_{};
    lv_subject_t validated_subject_{};
    lv_subject_t adding_subject_{};
    /// Save is held disabled: changing a host needs a passed test, adding one does not.
    lv_subject_t save_locked_subject_{};
    /// 1 while printer discovery runs: shows the discovered-printers dropdown.
    lv_subject_t discovery_active_subject_{};

    char host_ip_buf_[256] = {0};
    char host_port_buf_[8] = {0};
    char api_key_buf_[80] = {0};
    bool subjects_initialized_ = false;

    /// Where the port field's value came from; moonraker_ports_to_try() decides from it.
    helix::PortOrigin port_origin_ = helix::PortOrigin::FilledIn;
    /// Set while the modal writes the port field itself, so the write is not an edit.
    bool port_set_by_app_ = false;
    /// Test Connection; never touches the live client.
    helix::MoonrakerConnectionTest connection_test_;

    // === Printer discovery (mDNS + LAN sweep) while the modal is open ===
    std::unique_ptr<helix::IMdnsDiscovery> discovery_;
    std::vector<helix::DiscoveredPrinter> discovered_;
    /// Separate from lifetime_, which every Test Connection invalidates.
    helix::AsyncLifetimeGuard discovery_lifetime_;

    // === Completion callback ===
    CompletionCallback completion_callback_;
    AddCallback add_callback_;

    // === Input change observers (reset validation on edit) ===
    ObserverGuard host_ip_observer_;
    ObserverGuard host_port_observer_;
    ObserverGuard api_key_observer_;

    // === Internal methods ===
    void init_subjects();
    void deinit_subjects();
    void handle_test_connection();
    void handle_save();
    void update_save_lock();
    void commit_add(const std::string& host, int port);
    void handle_cancel();
    void set_status(const char* icon_name, const char* color_token, const char* text);
    /// Shows the reason and returns false when the host or port cannot be used.
    bool input_valid(const char* ip, const std::string& port_clean);
    /// The API key field, trimmed; blank means the printer takes none.
    std::string entered_api_key();
    void on_test_success(const std::string& host,
                         const helix::MoonrakerConnectionTest::Result& result);
    void on_test_failure(const helix::MoonrakerConnectionTest::Result& result);
    void start_discovery();
    void stop_discovery();
    void show_discovered(const std::vector<helix::DiscoveredPrinter>& printers);
    void handle_printer_picked(uint32_t selected);
    static void on_input_changed_cb(lv_observer_t* observer, lv_subject_t* subject);

    // === Static callback registration ===
    static void register_callbacks();
    static bool callbacks_registered_;
    static ChangeHostModal* active_instance_;

    // === Static callbacks ===
    static void on_test_connection_cb(lv_event_t* e);
    static void on_save_cb(lv_event_t* e);
    static void on_cancel_cb(lv_event_t* e);
    static void on_printer_picked_cb(lv_event_t* e);
};
