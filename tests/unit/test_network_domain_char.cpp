// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_domain_char.cpp
 * @brief Characterization tests for PrinterState network/connection domain
 *
 * These tests capture the CURRENT behavior of network and connection-related
 * subjects in PrinterState before extraction to a dedicated component.
 *
 * Network subjects (5 subjects + 1 flag):
 * - printer_connection_state_ (int) - ConnectionState enum: 0=disconnected,
 *   1=connecting, 2=connected, 3=reconnecting, 4=failed
 * - printer_connection_message_ (string, 128-byte buffer) - status message
 * - network_status_ (int) - NetworkStatus enum: 0=disconnected, 1=connecting, 2=connected
 * - klippy_state_ (int) - KlippyState enum: 0=ready, 1=startup, 2=shutdown, 3=error
 * - nav_buttons_enabled_ (int, derived) - 1 when connected AND klippy ready, else 0
 * - was_ever_connected_ (bool flag, not subject) - tracks if ever successfully connected
 *
 * Default values:
 * - printer_connection_state_: 0 (disconnected)
 * - printer_connection_message_: "Disconnected"
 * - network_status_: 2 (connected - mock mode default)
 * - klippy_state_: 2 (shutdown - default until confirmed ready)
 * - nav_buttons_enabled_: 0 (starts disabled)
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_client.h" // For ConnectionState enum
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Initial State Tests - Document default initialization behavior
// ============================================================================

TEST_CASE("Network characterization: initial values after init",
          "[characterization][network][init]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("printer_connection_message initializes to 'Disconnected'") {
        const char* msg = lv_subject_get_string(state.get_printer_connection_message_subject());
        REQUIRE(msg != nullptr);
        REQUIRE(std::string(msg) == "Disconnected");
    }

    SECTION("network_status initializes to 2 (connected - mock mode default)") {
        REQUIRE(lv_subject_get_int(state.get_network_status_subject()) == 2);
    }

    // Note: was_ever_connected is NOT tested here because it persists across the
    // process lifetime. It is only false when the PrinterState singleton is first
    // constructed, and reset_for_testing() does NOT reset it.
    // See "was_ever_connected flag behavior" test case for characterization.
}

// ============================================================================
// Connection State Tests - Verify set_printer_connection_state_internal behavior
// ============================================================================

TEST_CASE("Network characterization: set_printer_connection_state_internal updates both subjects",
          "[characterization][network][connection]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("disconnected state (0)") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::DISCONNECTED),
                                                    "Not connected");

        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 0);
        REQUIRE(std::string(lv_subject_get_string(
                    state.get_printer_connection_message_subject())) == "Not connected");
    }

    SECTION("connecting state (1)") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTING),
                                                    "Connecting...");

        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 1);
        REQUIRE(std::string(lv_subject_get_string(
                    state.get_printer_connection_message_subject())) == "Connecting...");
    }

    SECTION("connected state (2)") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");

        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 2);
        REQUIRE(std::string(lv_subject_get_string(
                    state.get_printer_connection_message_subject())) == "Ready");
    }

    SECTION("reconnecting state (3)") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::RECONNECTING),
                                                    "Reconnecting...");

        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 3);
        REQUIRE(std::string(lv_subject_get_string(
                    state.get_printer_connection_message_subject())) == "Reconnecting...");
    }

    SECTION("failed state (4)") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::FAILED),
                                                    "Connection failed");

        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 4);
        REQUIRE(std::string(lv_subject_get_string(
                    state.get_printer_connection_message_subject())) == "Connection failed");
    }
}

// ============================================================================
// Network Status Tests - Verify set_network_status behavior
// ============================================================================

TEST_CASE("Network characterization: set_network_status updates subject",
          "[characterization][network][status]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("disconnected (0)") {
        state.set_network_status(static_cast<int>(NetworkStatus::DISCONNECTED));
        REQUIRE(lv_subject_get_int(state.get_network_status_subject()) == 0);
    }

    SECTION("connecting (1)") {
        state.set_network_status(static_cast<int>(NetworkStatus::CONNECTING));
        REQUIRE(lv_subject_get_int(state.get_network_status_subject()) == 1);
    }

    SECTION("connected (2)") {
        state.set_network_status(static_cast<int>(NetworkStatus::CONNECTED));
        REQUIRE(lv_subject_get_int(state.get_network_status_subject()) == 2);
    }
}

// ============================================================================
// Klippy State Tests - Verify set_klippy_state_sync behavior
// ============================================================================

TEST_CASE("Network characterization: set_klippy_state_sync updates subject",
          "[characterization][network][klippy]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("READY state (0)") {
        state.set_klippy_state_sync(KlippyState::READY);
        REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 0);
    }

    SECTION("STARTUP state (1)") {
        state.set_klippy_state_sync(KlippyState::STARTUP);
        REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 1);
    }

    SECTION("SHUTDOWN state (2)") {
        state.set_klippy_state_sync(KlippyState::SHUTDOWN);
        REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 2);
    }

    SECTION("ERROR state (3)") {
        state.set_klippy_state_sync(KlippyState::ERROR);
        REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 3);
    }
}

// ============================================================================
// nav_buttons_enabled Derivation Tests - Key behavior
// ============================================================================

TEST_CASE("Network characterization: nav_buttons_enabled derivation",
          "[characterization][network][nav_buttons]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("connected AND klippy ready -> nav_buttons_enabled = 1") {
        // Set connected state
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");
        // Set klippy ready
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 1);
    }

    SECTION("disconnected AND klippy ready -> nav_buttons_enabled = 0") {
        // Set disconnected state
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::DISCONNECTED),
                                                    "Disconnected");
        // Set klippy ready
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connected AND klippy error -> nav_buttons_enabled = 0") {
        // Set connected state
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");
        // Set klippy error
        state.set_klippy_state_sync(KlippyState::ERROR);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("disconnected AND klippy error -> nav_buttons_enabled = 0") {
        // Set disconnected state
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::DISCONNECTED),
                                                    "Disconnected");
        // Set klippy error
        state.set_klippy_state_sync(KlippyState::ERROR);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connecting state -> nav_buttons_enabled = 0") {
        // Set connecting state (not fully connected)
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTING),
                                                    "Connecting...");
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("reconnecting state -> nav_buttons_enabled = 0") {
        // Set reconnecting state (not fully connected)
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::RECONNECTING),
                                                    "Reconnecting...");
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connected AND klippy startup -> nav_buttons_enabled = 0") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");
        state.set_klippy_state_sync(KlippyState::STARTUP);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connected AND klippy shutdown -> nav_buttons_enabled = 0") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");
        state.set_klippy_state_sync(KlippyState::SHUTDOWN);

        REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    }
}

// ============================================================================
// was_ever_connected Flag Tests
// ============================================================================

TEST_CASE("Network characterization: was_ever_connected flag behavior",
          "[characterization][network][was_connected]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Note: was_ever_connected_ is NOT reset by reset_for_testing().
    // It persists across the process lifetime (application session tracking).
    // Tests that run after a successful connection will see this as true.
    // The flag is only initialized to false when PrinterState is first constructed.

    SECTION("becomes true when connection state becomes CONNECTED") {
        // Capture current state before we connect
        bool was_connected_before = state.was_ever_connected();

        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");

        // After connecting, should always be true
        REQUIRE(state.was_ever_connected() == true);

        // If it was false before, it changed; if true before, it stayed true
        if (!was_connected_before) {
            REQUIRE(state.was_ever_connected() != was_connected_before);
        }
    }

    SECTION("stays true even after disconnection") {
        // First connect
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");
        REQUIRE(state.was_ever_connected() == true);

        // Then disconnect
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::DISCONNECTED),
                                                    "Disconnected");
        REQUIRE(state.was_ever_connected() == true);
    }

    SECTION("stays true through reconnection cycle") {
        // Connect
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");
        REQUIRE(state.was_ever_connected() == true);

        // Reconnecting
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::RECONNECTING),
                                                    "Reconnecting...");
        REQUIRE(state.was_ever_connected() == true);

        // Disconnect (failed)
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::FAILED),
                                                    "Connection failed");
        REQUIRE(state.was_ever_connected() == true);
    }

    SECTION("CONNECTING state alone does not set the flag") {
        // Note: If was_ever_connected is already true from a prior test/connection,
        // it will stay true. We test that CONNECTING alone doesn't set it.
        bool before = state.was_ever_connected();

        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTING),
                                                    "Connecting...");

        // The flag should not have changed from CONNECTING alone
        REQUIRE(state.was_ever_connected() == before);
    }
}

// ============================================================================
// Observer Notification Tests - Verify observers fire on state changes
// ============================================================================

TEST_CASE("Network characterization: observer fires when printer_connection_state changes",
          "[characterization][network][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer = lv_subject_add_observer(state.get_printer_connection_state_subject(),
                                                      observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Initial value is 0 (disconnected)

    // Update connection state
    state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                "Ready");

    REQUIRE(user_data[0] == 2);
    REQUIRE(user_data[1] == 2);

    lv_observer_remove(observer);
}

TEST_CASE("Network characterization: observer fires when klippy_state changes",
          "[characterization][network][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_klippy_state_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 2); // Initial value is SHUTDOWN (2) until confirmed ready

    // Update klippy state
    state.set_klippy_state_sync(KlippyState::ERROR);

    REQUIRE(user_data[0] == 2);
    REQUIRE(user_data[1] == 3);

    lv_observer_remove(observer);
}

TEST_CASE("Network characterization: observer fires when nav_buttons_enabled changes",
          "[characterization][network][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_nav_buttons_enabled_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Starts disabled

    // Enable nav buttons by connecting with klippy ready
    state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                "Ready");
    state.set_klippy_state_sync(KlippyState::READY);

    REQUIRE(user_data[0] == 2);
    REQUIRE(user_data[1] == 1);

    // Disable by setting klippy to error
    state.set_klippy_state_sync(KlippyState::ERROR);

    REQUIRE(user_data[0] == 3);
    REQUIRE(user_data[1] == 0);

    lv_observer_remove(observer);
}

// ============================================================================
// Reset Cycle Tests - Verify subjects survive reset_for_testing cycle
// ============================================================================

TEST_CASE("Network characterization: subjects survive reset_for_testing cycle",
          "[characterization][network][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set some network values
    state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                "Ready");
    state.set_network_status(static_cast<int>(NetworkStatus::CONNECTED));
    state.set_klippy_state_sync(KlippyState::READY);

    // Verify values were set
    REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 2);
    REQUIRE(lv_subject_get_int(state.get_network_status_subject()) == 2);
    REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 1);
    REQUIRE(state.was_ever_connected() == true);

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // After reset, subject values should be back to defaults
    REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 0);
    REQUIRE(std::string(lv_subject_get_string(state.get_printer_connection_message_subject())) ==
            "Disconnected");
    REQUIRE(lv_subject_get_int(state.get_network_status_subject()) == 2); // Mock mode default
    REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 2);   // SHUTDOWN default
    REQUIRE(lv_subject_get_int(state.get_nav_buttons_enabled_subject()) == 0);
    // was_ever_connected_ IS cleared. It is session-scoped in PRODUCTION — it
    // survives a reconnect on purpose — but PrinterStateTestAccess::reset()
    // simulates a fresh session on the shared PrinterState singleton, so the flag
    // resets with it. Left latched, the first test in the binary to connect makes
    // every later test believe it had already been connected once.
    REQUIRE_FALSE(state.was_ever_connected());

    // Subjects should still be functional after reset
    state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTING),
                                                "Connecting...");

    REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 1);
}

TEST_CASE("Network characterization: subject pointers remain valid after reset",
          "[characterization][network][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Capture subject pointers
    lv_subject_t* connection_state_before = state.get_printer_connection_state_subject();
    lv_subject_t* klippy_state_before = state.get_klippy_state_subject();
    lv_subject_t* nav_buttons_before = state.get_nav_buttons_enabled_subject();

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Pointers should be the same (singleton subjects are reused)
    lv_subject_t* connection_state_after = state.get_printer_connection_state_subject();
    lv_subject_t* klippy_state_after = state.get_klippy_state_subject();
    lv_subject_t* nav_buttons_after = state.get_nav_buttons_enabled_subject();

    REQUIRE(connection_state_before == connection_state_after);
    REQUIRE(klippy_state_before == klippy_state_after);
    REQUIRE(nav_buttons_before == nav_buttons_after);
}

// ============================================================================
// Independence Tests - Verify network subjects are independent
// ============================================================================

TEST_CASE("Network characterization: connection and klippy subjects are independent",
          "[characterization][network][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("changing connection state does not affect klippy state") {
        // Set initial klippy state
        state.set_klippy_state_sync(KlippyState::STARTUP);
        REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 1);

        // Change connection state
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");

        // Klippy state should be unchanged
        REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 1);
    }

    SECTION("changing klippy state does not affect connection state") {
        // Set initial connection state
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTING),
                                                    "Connecting...");
        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 1);

        // Change klippy state
        state.set_klippy_state_sync(KlippyState::ERROR);

        // Connection state should be unchanged
        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 1);
    }

    SECTION("changing network status does not affect connection or klippy") {
        // Set initial states
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Ready");
        state.set_klippy_state_sync(KlippyState::READY);

        // Change network status
        state.set_network_status(static_cast<int>(NetworkStatus::DISCONNECTED));

        // Connection and klippy should be unchanged
        REQUIRE(lv_subject_get_int(state.get_printer_connection_state_subject()) == 2);
        REQUIRE(lv_subject_get_int(state.get_klippy_state_subject()) == 0);
    }
}

// ============================================================================
// Observer Independence Tests - Verify observer isolation
// ============================================================================

TEST_CASE("Network characterization: observers on different subjects are independent",
          "[characterization][network][observer][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    int connection_count = 0;
    int klippy_count = 0;

    auto connection_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    auto klippy_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* connection_observer = lv_subject_add_observer(
        state.get_printer_connection_state_subject(), connection_cb, &connection_count);
    lv_observer_t* klippy_observer =
        lv_subject_add_observer(state.get_klippy_state_subject(), klippy_cb, &klippy_count);

    // Both observers fire on initial add
    REQUIRE(connection_count == 1);
    REQUIRE(klippy_count == 1);

    // Update only connection state
    state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                "Ready");

    // Only connection observer should fire
    REQUIRE(connection_count == 2);
    REQUIRE(klippy_count == 1);

    // Update only klippy state
    state.set_klippy_state_sync(KlippyState::STARTUP);

    // Only klippy observer should fire
    REQUIRE(connection_count == 2);
    REQUIRE(klippy_count == 2);

    lv_observer_remove(connection_observer);
    lv_observer_remove(klippy_observer);
}

TEST_CASE("Network characterization: multiple observers on same subject all fire",
          "[characterization][network][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    int count1 = 0, count2 = 0, count3 = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* observer1 =
        lv_subject_add_observer(state.get_printer_connection_state_subject(), observer_cb, &count1);
    lv_observer_t* observer2 =
        lv_subject_add_observer(state.get_printer_connection_state_subject(), observer_cb, &count2);
    lv_observer_t* observer3 =
        lv_subject_add_observer(state.get_printer_connection_state_subject(), observer_cb, &count3);

    // All observers fire on initial add
    REQUIRE(count1 == 1);
    REQUIRE(count2 == 1);
    REQUIRE(count3 == 1);

    // Single update should fire all three
    state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                "Ready");

    REQUIRE(count1 == 2);
    REQUIRE(count2 == 2);
    REQUIRE(count3 == 2);

    lv_observer_remove(observer1);
    lv_observer_remove(observer2);
    lv_observer_remove(observer3);
}

// ============================================================================
// Connection Message String Buffer Tests
// ============================================================================

TEST_CASE("Network characterization: connection message buffer behavior",
          "[characterization][network][buffer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("message updates with state changes") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTING),
                                                    "Attempting connection...");
        REQUIRE(std::string(lv_subject_get_string(
                    state.get_printer_connection_message_subject())) == "Attempting connection...");

        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "Connected to printer");
        REQUIRE(std::string(lv_subject_get_string(
                    state.get_printer_connection_message_subject())) == "Connected to printer");
    }

    SECTION("empty message is handled") {
        state.set_printer_connection_state_internal(static_cast<int>(ConnectionState::CONNECTED),
                                                    "");
        REQUIRE(std::string(
                    lv_subject_get_string(state.get_printer_connection_message_subject())) == "");
    }
}
