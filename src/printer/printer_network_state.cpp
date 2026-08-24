// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_network_state.cpp
 * @brief Network and connection state management extracted from PrinterState
 *
 * Manages WebSocket connection state, network connectivity, and Klipper firmware
 * state. Maintains a derived nav_buttons_enabled subject for UI gating.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_network_state.h"

#include "connection_state.h" // For ConnectionState enum
#include "printer_state.h"    // For KlippyState enum
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix {

PrinterNetworkState::PrinterNetworkState() {
    // Initialize string buffer
    std::memset(printer_connection_message_buf_, 0, sizeof(printer_connection_message_buf_));
    std::strcpy(printer_connection_message_buf_, "Disconnected");
}

void PrinterNetworkState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterNetworkState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterNetworkState] Initializing subjects (register_xml={})", register_xml);

    // Printer connection state subjects (Moonraker WebSocket)
    INIT_SUBJECT_INT(printer_connection_state, 0, subjects_, register_xml); // 0 = disconnected
    INIT_SUBJECT_STRING(printer_connection_message, "Disconnected", subjects_, register_xml);

    // Network connectivity subject (WiFi/Ethernet)
    // Default to connected for mock mode
    INIT_SUBJECT_INT(network_status, 2, subjects_,
                     register_xml); // 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED

    // Klipper firmware state subject (default to SHUTDOWN until confirmed ready)
    INIT_SUBJECT_INT(klippy_state, static_cast<int>(KlippyState::SHUTDOWN), subjects_,
                     register_xml);

    // Combined nav button enabled subject (connected AND klippy ready)
    // Starts disabled (0) - will be updated when connection/klippy state changes
    INIT_SUBJECT_INT(nav_buttons_enabled, 0, subjects_, register_xml);

    // Moonraker WebSocket connection state (independent of Klipper)
    // Used by power widgets that only need Moonraker, not Klipper
    INIT_SUBJECT_INT(moonraker_connection_state, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterNetworkState] Subjects initialized successfully");
}

void PrinterNetworkState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterNetworkState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterNetworkState::set_printer_connection_state_internal(int state, const char* message) {
    // Called from main thread via ui_queue_update()
    // Log "Connected" at info level, transitional states at debug to reduce noise
    if (state == static_cast<int>(ConnectionState::CONNECTED)) {
        spdlog::info("[PrinterNetworkState] Printer connection state: Connected");
    } else {
        spdlog::debug("[PrinterNetworkState] Printer connection state changed: {} - {}", state,
                      message);
    }

    // Track if we've ever successfully connected
    if (state == static_cast<int>(ConnectionState::CONNECTED) && !was_ever_connected_) {
        was_ever_connected_ = true;
        spdlog::debug(
            "[PrinterNetworkState] First successful connection - was_ever_connected_ = true");
    }

    spdlog::trace(
        "[PrinterNetworkState] Setting printer_connection_state_ subject (at {}) to value {}",
        (void*)&printer_connection_state_, state);
    lv_subject_set_int(&printer_connection_state_, state);
    spdlog::trace("[PrinterNetworkState] Subject value now: {}",
                  lv_subject_get_int(&printer_connection_state_));
    lv_subject_copy_string(&printer_connection_message_, message);
    update_nav_buttons_enabled();
    spdlog::trace("[PrinterNetworkState] Printer connection state update complete, observers "
                  "should be notified");
}

void PrinterNetworkState::set_network_status(int status) {
    if (lv_subject_get_int(&network_status_) == status)
        return;
    spdlog::debug("[PrinterNetworkState] Network status changed: {}", status);
    lv_subject_set_int(&network_status_, status);
}

bool PrinterNetworkState::set_klippy_state_internal(KlippyState state) {
    const char* state_names[] = {"READY", "STARTUP", "SHUTDOWN", "ERROR"};
    int state_int = static_cast<int>(state);
    if (lv_subject_get_int(&klippy_state_) == state_int)
        return false;
    spdlog::debug("[PrinterNetworkState] Klippy state changed: {} ({})", state_names[state_int],
                  state_int);
    lv_subject_set_int(&klippy_state_, state_int);
    update_nav_buttons_enabled();

    // Clear state message when returning to READY (error is resolved)
    if (state == KlippyState::READY) {
        klippy_state_message_.clear();
    }
    return true;
}

void PrinterNetworkState::set_klippy_state_message(const std::string& message) {
    klippy_state_message_ = message;
    if (!message.empty()) {
        spdlog::info("[PrinterNetworkState] Klippy state message: {}", message);
    }
}

void PrinterNetworkState::update_nav_buttons_enabled() {
    // Compute combined state: enabled when connected AND klippy ready
    int connection = lv_subject_get_int(&printer_connection_state_);
    int klippy = lv_subject_get_int(&klippy_state_);
    bool connected = (connection == static_cast<int>(ConnectionState::CONNECTED));
    bool klippy_ready = (klippy == static_cast<int>(KlippyState::READY));
    int enabled = (connected && klippy_ready) ? 1 : 0;

    // Only update if changed to avoid unnecessary observer notifications
    if (lv_subject_get_int(&nav_buttons_enabled_) != enabled) {
        spdlog::debug(
            "[PrinterNetworkState] nav_buttons_enabled: {} (connected={}, klippy_ready={})",
            enabled, connected, klippy_ready);
        lv_subject_set_int(&nav_buttons_enabled_, enabled);
    }

    // Moonraker connection state: WebSocket connected, independent of Klipper
    int moonraker_connected = connected ? 1 : 0;
    if (lv_subject_get_int(&moonraker_connection_state_) != moonraker_connected) {
        spdlog::debug("[PrinterNetworkState] moonraker_connection_state: {}", moonraker_connected);
        lv_subject_set_int(&moonraker_connection_state_, moonraker_connected);
    }
}

} // namespace helix
