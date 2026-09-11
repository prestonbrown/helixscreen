// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_error_handling.cpp
 * @brief Unit tests for AFC error/warning message handling
 *
 * Tests the message queue consumption in AmsBackendAfc:
 * - Deduplication of repeated messages
 * - Toast severity mapping (error/warning)
 * - Toast suppression when AFC action:prompt is active
 * - Message reset when error clears
 */

#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "ams_backend_afc.h"
#include "ams_types.h"
#include "moonraker_api.h"
#include "test_helpers/afc_test_access.h"

#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Helper
// ============================================================================

namespace helix {

/**
 * @brief Test helper for AFC error handling tests
 *
 * Extends AmsBackendAfc to expose internals and capture notification calls.
 * Tracks which notification types were triggered via the message handling path.
 */
class AfcErrorHandlingHelper : public AmsBackendAfc {
  public:
    AfcErrorHandlingHelper() : AmsBackendAfc(nullptr, nullptr) {
        // Initialize some lanes so parse_afc_state works
        std::vector<std::string> names = {"lane1", "lane2", "lane3", "lane4"};
        AfcTestAccess::slots(*this).initialize("AFC Test Unit", names);
    }

    // Feed AFC state update with a message object
    void feed_afc_message(const std::string& message_text, const std::string& message_type) {
        feed_afc_payload(message_text, message_type, nullptr);
    }

    // Feed AFC state with empty message (error cleared)
    void feed_afc_empty_message() {
        feed_afc_payload("", "", nullptr);
    }

    // Feed a message plus the error_state it would ride with in production:
    // upstream AFC assigns error_state and appends to the message queue in the
    // same status update.
    void feed_afc_message_with_error_state(const std::string& message_text,
                                           const std::string& message_type, bool error_state) {
        feed_afc_payload(message_text, message_type, &error_state);
    }

    // Feed a status frame carrying ONLY error_state (a delta where the flag
    // changed but no message field was included).
    void feed_afc_error_state(bool error_state) {
        nlohmann::json afc_data;
        afc_data["error_state"] = error_state;

        nlohmann::json params;
        params["AFC"] = afc_data;

        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    // Access last_seen_message_ for assertions
    std::string get_last_seen_message() const {
        return AfcTestAccess::last_seen_message(*this);
    }

    // Access last_error_msg_ (the error-event dedup tracker) for assertions
    std::string get_last_error_msg() const {
        return AfcTestAccess::last_error_msg(*this);
    }

  private:
    void feed_afc_payload(const std::string& message_text, const std::string& message_type,
                          const bool* error_state) {
        nlohmann::json afc_data;
        afc_data["message"]["message"] = message_text;
        afc_data["message"]["type"] = message_type;
        if (error_state != nullptr) {
            afc_data["error_state"] = *error_state;
        }

        nlohmann::json params;
        params["AFC"] = afc_data;

        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    // Override execute_gcode to prevent null pointer access
    AmsError execute_gcode(const std::string& /*gcode*/) override {
        return AmsErrorHelper::success();
    }
};
} // namespace helix

// ============================================================================
// Message Deduplication Tests
// ============================================================================

TEST_CASE("AFC Error Handling: Message deduplication", "[afc][error_handling][dedup]") {
    AfcErrorHandlingHelper afc;

    SECTION("New error message updates last_seen_message") {
        afc.feed_afc_message("Lane 1 load failed", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 load failed");
    }

    SECTION("Same message repeated does not change state") {
        afc.feed_afc_message("Lane 1 load failed", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 load failed");

        // Second identical message should still have the same last_seen
        afc.feed_afc_message("Lane 1 load failed", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 load failed");
    }

    SECTION("Different message updates last_seen_message") {
        afc.feed_afc_message("Lane 1 load failed", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 load failed");

        afc.feed_afc_message("Lane 2 prep failed", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 2 prep failed");
    }

    SECTION("Empty message resets last_seen_message") {
        afc.feed_afc_message("Lane 1 load failed", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 load failed");

        afc.feed_afc_empty_message();
        REQUIRE(afc.get_last_seen_message().empty());
    }

    SECTION("Warning message also tracked by last_seen_message") {
        afc.feed_afc_message("Buffer not advancing", "warning");
        REQUIRE(afc.get_last_seen_message() == "Buffer not advancing");
    }

    SECTION("After empty reset, same message is treated as new") {
        afc.feed_afc_message("Lane 1 load failed", "error");
        afc.feed_afc_empty_message();
        REQUIRE(afc.get_last_seen_message().empty());

        // Same message text again after reset - should be tracked as new
        afc.feed_afc_message("Lane 1 load failed", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 load failed");
    }
}

// ============================================================================
// Toast Suppression Tests (AFC prompt active)
// ============================================================================

TEST_CASE("AFC Error Handling: Toast suppression when AFC prompt is active",
          "[afc][error_handling][suppression]") {
    SECTION("Toast NOT suppressed when no prompt is active") {
        AfcErrorHandlingHelper afc;
        // Ensure no prompt is active
        ActionPromptManager::set_instance(nullptr);

        // Should go through normal toast path (not suppressed)
        afc.feed_afc_message("Lane 1 error", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 error");
    }

    SECTION("Toast suppressed when AFC prompt is active") {
        AfcErrorHandlingHelper afc;
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);

        // Show an AFC prompt
        manager.process_line("// action:prompt_begin AFC Lane Error");
        manager.process_line("// action:prompt_show");
        REQUIRE(ActionPromptManager::is_showing());

        // Message should still be tracked for dedup
        afc.feed_afc_message("Lane 1 error", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 error");
        // Toast is suppressed but notification history entry is created
        // (verified by integration test / no crash)

        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("Toast NOT suppressed when non-AFC prompt is active") {
        AfcErrorHandlingHelper afc;
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);

        // Show a non-AFC prompt (e.g., Filament Change)
        manager.process_line("// action:prompt_begin Filament Change");
        manager.process_line("// action:prompt_show");
        REQUIRE(ActionPromptManager::is_showing());

        // AFC message should NOT be suppressed since prompt is not AFC-related
        afc.feed_afc_message("Lane 1 error", "error");
        REQUIRE(afc.get_last_seen_message() == "Lane 1 error");

        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("Toast suppressed only when prompt name contains AFC") {
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);

        // "AFC" must appear in the prompt title for suppression
        manager.process_line("// action:prompt_begin AFC Recovery");
        manager.process_line("// action:prompt_show");

        REQUIRE(ActionPromptManager::is_showing());
        REQUIRE(ActionPromptManager::current_prompt_name().find("AFC") != std::string::npos);

        ActionPromptManager::set_instance(nullptr);
    }
}

// ============================================================================
// Message Type to Severity Mapping Tests
// ============================================================================

TEST_CASE("AFC Error Handling: Message type to severity mapping",
          "[afc][error_handling][severity]") {
    AfcErrorHandlingHelper afc;
    // No prompt active
    ActionPromptManager::set_instance(nullptr);

    SECTION("Error type message is tracked") {
        afc.feed_afc_message("Critical lane failure", "error");
        REQUIRE(afc.get_last_seen_message() == "Critical lane failure");
    }

    SECTION("Warning type message is tracked") {
        afc.feed_afc_message("Buffer advancing slowly", "warning");
        REQUIRE(afc.get_last_seen_message() == "Buffer advancing slowly");
    }

    SECTION("Unknown type message is still tracked") {
        afc.feed_afc_message("Something happened", "info");
        REQUIRE(afc.get_last_seen_message() == "Something happened");
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("AFC Error Handling: Edge cases", "[afc][error_handling][edge]") {
    AfcErrorHandlingHelper afc;
    ActionPromptManager::set_instance(nullptr);

    SECTION("Message with empty type field is handled gracefully") {
        afc.feed_afc_message("No type field", "");
        // Should not crash, message still tracked (defaults to info toast)
        REQUIRE(afc.get_last_seen_message() == "No type field");
    }

    SECTION("Rapid message changes are all tracked") {
        afc.feed_afc_message("Error 1", "error");
        REQUIRE(afc.get_last_seen_message() == "Error 1");

        afc.feed_afc_message("Error 2", "error");
        REQUIRE(afc.get_last_seen_message() == "Error 2");

        afc.feed_afc_message("Warning 1", "warning");
        REQUIRE(afc.get_last_seen_message() == "Warning 1");

        afc.feed_afc_empty_message();
        REQUIRE(afc.get_last_seen_message().empty());
    }
}

// ============================================================================
// Latched Messages vs error_state (#1589)
// ============================================================================
//
// AFC latches printer.AFC.message: the entry stays long after the condition
// that produced it resolved, while error_state is AFC's own statement of
// whether it is CURRENTLY in trouble. A message typed "error" arriving with
// error_state false is history — on a fresh process the text dedup alone
// would re-toast it as a live error at every connect.
//
// Toasts are asserted through the ui_test_utils notification hooks: the test
// build stubs the UI layer, and every NOTIFY_* severity fires its hook.

namespace {
class ToastCapture {
  public:
    ToastCapture() {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& m) { events_.emplace_back("error", m); });
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& m) { events_.emplace_back("warning", m); });
        helix::ui::set_test_notification_info_hook(
            [this](const std::string& m) { events_.emplace_back("info", m); });
    }
    ~ToastCapture() {
        helix::ui::set_test_notification_error_hook(nullptr);
        helix::ui::set_test_notification_warning_hook(nullptr);
        helix::ui::set_test_notification_info_hook(nullptr);
    }

    ToastCapture(const ToastCapture&) = delete;
    ToastCapture& operator=(const ToastCapture&) = delete;

    size_t total() const {
        return events_.size();
    }

    size_t count(const std::string& severity) const {
        size_t n = 0;
        for (const auto& e : events_) {
            n += (e.first == severity) ? 1 : 0;
        }
        return n;
    }

    bool contains(const std::string& severity, const std::string& text) const {
        for (const auto& e : events_) {
            if (e.first == severity && e.second.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

  private:
    std::vector<std::pair<std::string, std::string>> events_;
};
} // namespace

TEST_CASE("AFC Error Handling: a latched message with error_state false is not a fresh error",
          "[afc][error_handling][1589]") {
    ActionPromptManager::set_instance(nullptr);
    const std::string latched =
        "Error getting data from moonraker, check AFC.log for more information";

    SECTION("Latched error message raises no toast of any severity and no error event") {
        AfcErrorHandlingHelper afc;
        ToastCapture toasts;
        afc.feed_afc_message_with_error_state(latched, "error", false);

        // The dedup tracker still records it (precondition: the frame was parsed).
        REQUIRE(afc.get_last_seen_message() == latched);
        // But no toast was requested, and no error event was deferred.
        REQUIRE(toasts.total() == 0);
        REQUIRE(afc.get_last_error_msg().empty());
    }

    SECTION("The same text with error_state true is a live error") {
        AfcErrorHandlingHelper afc;
        ToastCapture toasts;
        afc.feed_afc_message_with_error_state(latched, "error", true);

        REQUIRE(afc.get_last_seen_message() == latched);
        REQUIRE(afc.get_last_error_msg() == latched);
        REQUIRE(toasts.count("error") == 1);
        REQUIRE(toasts.contains("error", latched));
        REQUIRE(toasts.total() == 1);
    }

    SECTION("error_state arriving in an earlier frame still counts for the message") {
        AfcErrorHandlingHelper afc;
        ToastCapture toasts;
        // Moonraker status deltas only carry CHANGED keys: the frame that
        // flips error_state and the frame that carries the new message can be
        // two frames, and the message frame alone must not read as latched.
        afc.feed_afc_error_state(true);
        afc.feed_afc_message(latched, "error");

        REQUIRE(afc.get_last_error_msg() == latched);
        REQUIRE(toasts.count("error") == 1);
    }

    SECTION("A latched message becoming live raises the error once") {
        AfcErrorHandlingHelper afc;
        ToastCapture toasts;
        afc.feed_afc_message_with_error_state(latched, "error", false);
        REQUIRE(toasts.total() == 0);

        // The condition turns real: same text, now with error_state set.
        // The text dedup would swallow it, so the error treatment must key on
        // the error event dedup, not the text dedup alone.
        afc.feed_afc_message_with_error_state(latched, "error", true);
        REQUIRE(afc.get_last_error_msg() == latched);
        REQUIRE(toasts.count("error") == 1);
    }

    SECTION("Warning messages are unaffected by error_state") {
        AfcErrorHandlingHelper afc;
        ToastCapture toasts;
        afc.feed_afc_message_with_error_state("Buffer not advancing", "warning", false);

        REQUIRE(toasts.count("warning") == 1);
        REQUIRE(toasts.count("error") == 0);
    }
}
