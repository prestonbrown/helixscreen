// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"
#include "ui_wizard.h"
#include "ui_wizard_connection.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"
#include "misc/lv_timer_private.h" // timer_cb — assert the cancel neutered it
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Test Fixture for Wizard Connection UI
// ============================================================================
// Extends LVGLUITestFixture which provides full XML component registration

class WizardConnectionUIFixture : public LVGLUITestFixture {
  public:
    WizardConnectionUIFixture() {
        // LVGLUITestFixture handles all LVGL, theme, widget, subject,
        // callback, and XML component initialization

        // Create the wizard container on the test screen
        wizard = ui_wizard_create(test_screen());
        if (!wizard) {
            spdlog::error("[WizardConnectionUIFixture] Failed to create wizard!");
            return;
        }

        // Check if XML infrastructure is available before navigating.
        // Navigation initializes step state (timers, callbacks) that can crash
        // if widgets don't exist, and makes cleanup unsafe.
        lv_obj_t* content = lv_obj_find_by_name(wizard, "wizard_content");
        if (!content) {
            spdlog::warn(
                "[WizardConnectionUIFixture] XML components not loaded, skipping navigation");
            return;
        }

        // Navigate to the Moonraker Connection screen
        ui_wizard_navigate_to_step(helix::wizard::StepId::Connection);

        // Verify that connection step loaded by checking for a key widget
        ready_ = (lv_obj_find_by_name(wizard, "ip_input") != nullptr);

        // Stop mDNS discovery and timers to prevent hangs during
        // UITest::wait_ms() timer processing. Widgets remain in the LVGL
        // tree - tests find them via lv_obj_find_by_name on the wizard.
        get_wizard_connection_step()->cleanup();

        // Initialize UI test system with test screen
        UITest::init(test_screen());

        // Skip LVGL processing in constructor - let individual tests process
        // NOTE: mDNS timer processing was causing test hangs
    }

    ~WizardConnectionUIFixture() {
        if (ready_) {
            UITest::cleanup();
        }
        // Clean up connection step (stops mDNS discovery, cancels timers)
        get_wizard_connection_step()->cleanup();
        // Do NOT call lv_obj_delete(wizard) - let lv_deinit() in
        // LVGLTestFixture handle widget tree cleanup.
        wizard = nullptr;
    }

    void require_ready() {
        if (!ready_) {
            SKIP("XML infrastructure not available (ui_integration test)");
        }
    }

    lv_obj_t* wizard = nullptr;
    bool ready_ = false;
};

// ============================================================================
// UI Widget Tests
// ============================================================================

// =============================================================================
// UI Integration Tests - Require XML component registration
// =============================================================================
// These tests are marked [.ui_integration] (hidden from the default run)
// because they need the ui_xml/ component tree readable from disk, which makes
// them dependent on the working directory rather than on the build alone.
// LVGLUITestFixture does register the components, so they pass when run from
// the repo root:
//   ./build/bin/helix-tests "[.ui_integration]"
//
// They assert the wizard connection step's XML structure (widget names, title
// text, flex layout). Nothing else in the suite covers that, and tests/ui/ has
// no wizard coverage — the wizard is pre-first-boot, so ctl cannot reach it.
// =============================================================================

TEST_CASE_METHOD(WizardConnectionUIFixture, "Connection UI: All widgets exist",
                 "[wizard][connection][ui][.ui_integration]") {
    require_ready();

    // Find the main connection screen widgets (search in wizard, not test_screen)
    lv_obj_t* ip_input = lv_obj_find_by_name(wizard, "ip_input");
    REQUIRE(ip_input != nullptr);

    lv_obj_t* port_input = lv_obj_find_by_name(wizard, "port_input");
    REQUIRE(port_input != nullptr);

    lv_obj_t* test_btn = lv_obj_find_by_name(wizard, "btn_test_connection");
    REQUIRE(test_btn != nullptr);

    // Note: connection_status_text is the actual widget name in XML
    lv_obj_t* status_label = lv_obj_find_by_name(wizard, "connection_status_text");
    REQUIRE(status_label != nullptr);
}

TEST_CASE_METHOD(WizardConnectionUIFixture, "Connection UI: Test button state",
                 "[wizard][connection][ui][.ui_integration]") {
    require_ready();
    lv_obj_t* test_btn = UITest::find_by_name(test_screen(), "btn_test_connection");
    REQUIRE(test_btn != nullptr);

    // Button should not have the CLICKABLE flag removed
    bool has_clickable = lv_obj_has_flag(test_btn, LV_OBJ_FLAG_CLICKABLE);
    REQUIRE(has_clickable == true);

    // Button should be visible
    REQUIRE(UITest::is_visible(test_btn) == true);
}

TEST_CASE_METHOD(WizardConnectionUIFixture, "Connection UI: Navigation buttons",
                 "[wizard][connection][ui][.ui_integration]") {
    require_ready();
    // Find navigation buttons (names from wizard_container.xml)
    lv_obj_t* back_btn = UITest::find_by_name(test_screen(), "btn_back");
    lv_obj_t* next_btn = UITest::find_by_name(test_screen(), "btn_next");

    // Both should exist
    REQUIRE(back_btn != nullptr);
    REQUIRE(next_btn != nullptr);

    // On step 3 (Connection), back button should be visible
    REQUIRE(UITest::is_visible(back_btn) == true);
}

TEST_CASE_METHOD(WizardConnectionUIFixture, "Connection UI: Title and progress",
                 "[wizard][connection][ui][.ui_integration]") {
    require_ready();
    // Find title label (from wizard_header_bar.xml)
    lv_obj_t* title = UITest::find_by_name(test_screen(), "wizard_title");
    REQUIRE(title != nullptr);

    // Check title text (set from step_title const in wizard_connection.xml)
    std::string title_text = UITest::get_text(title);
    REQUIRE(title_text == "Printer Setup: Connection");
}

// ============================================================================
// Mock Connection Tests
// ============================================================================

// Mock MoonrakerClient for testing
class MockMoonrakerClient {
  public:
    int connect(const char* url, std::function<void()> on_connected,
                std::function<void()> on_disconnected) {
        last_url = url;
        connected_callback = on_connected;
        disconnected_callback = on_disconnected;
        return 0;
    }

    void trigger_connected() {
        if (connected_callback) {
            connected_callback();
        }
    }

    void trigger_disconnected() {
        if (disconnected_callback) {
            disconnected_callback();
        }
    }

    void set_connection_timeout(int timeout_ms) {
        timeout = timeout_ms;
    }

    ConnectionState get_connection_state() const {
        return state;
    }

    void close() {
        state = ConnectionState::DISCONNECTED;
    }

    std::string last_url;
    std::function<void()> connected_callback;
    std::function<void()> disconnected_callback;
    int timeout = 0;
    ConnectionState state = ConnectionState::DISCONNECTED;
};

TEST_CASE("Connection UI: Mock connection flow", "[wizard][connection][mock]") {
    MockMoonrakerClient mock_client;

    SECTION("Successful connection") {
        bool connected = false;

        mock_client.connect(
            "ws://192.168.1.100:7125/websocket", [&connected]() { connected = true; }, []() {});

        // Verify URL was captured
        REQUIRE(mock_client.last_url == "ws://192.168.1.100:7125/websocket");

        // Trigger successful connection
        mock_client.trigger_connected();

        REQUIRE(connected == true);
    }

    SECTION("Failed connection") {
        bool disconnected = false;

        mock_client.connect(
            "ws://192.168.1.100:7125/websocket", []() {},
            [&disconnected]() { disconnected = true; });

        // Trigger disconnection/failure
        mock_client.trigger_disconnected();

        REQUIRE(disconnected == true);
    }

    SECTION("Timeout configuration") {
        mock_client.set_connection_timeout(5000);
        REQUIRE(mock_client.timeout == 5000);
    }
}

// ============================================================================
// Responsive Layout Tests
// ============================================================================

TEST_CASE_METHOD(WizardConnectionUIFixture, "Connection UI: Responsive layout",
                 "[wizard][connection][ui][responsive][.ui_integration]") {
    require_ready();

    // Get the wizard content area
    lv_obj_t* content = lv_obj_find_by_name(wizard, "wizard_content");
    REQUIRE(content != nullptr);

    // Connection screen root is the first child of wizard_content
    lv_obj_t* connection_root = lv_obj_get_child(content, 0);
    REQUIRE(connection_root != nullptr);

    // Verify connection root uses column flex layout
    lv_flex_flow_t flow = lv_obj_get_style_flex_flow(connection_root, LV_PART_MAIN);
    REQUIRE(flow == LV_FLEX_FLOW_COLUMN);

    // Verify key widgets exist and are structured correctly
    lv_obj_t* ip_input = lv_obj_find_by_name(wizard, "ip_input");
    REQUIRE(ip_input != nullptr);

    lv_obj_t* port_input = lv_obj_find_by_name(wizard, "port_input");
    REQUIRE(port_input != nullptr);

    // Verify the connection root has children (layout content exists)
    uint32_t child_count = lv_obj_get_child_count(connection_root);
    REQUIRE(child_count > 0);
}

// ============================================================================
// AsyncLifetimeGuard Integration Tests
// ============================================================================
// These test the lifetime-token-based callback safety that replaced the old
// cleanup_called_ + connection_generation_ + m_alive pattern (#827).

class WizardConnectionLifetimeFixture : public LVGLTestFixture {
  public:
    WizardConnectionLifetimeFixture() {
        step = get_wizard_connection_step();
        step->init_subjects();
    }
    ~WizardConnectionLifetimeFixture() {
        step->cleanup();
    }
    WizardConnectionStep* step = nullptr;
};

TEST_CASE_METHOD(WizardConnectionLifetimeFixture,
                 "Connection step: IP and port subjects carry the seeded defaults",
                 "[wizard][connection]") {
    // Regression: seeding the string subject from its own buffer aliased
    // snprintf's source and destination and left both fields blank.
    lv_subject_t* ip = lv_xml_get_subject(nullptr, "connection_ip");
    lv_subject_t* port = lv_xml_get_subject(nullptr, "connection_port");
    REQUIRE(ip != nullptr);
    REQUIRE(port != nullptr);
    CHECK(std::string(lv_subject_get_string(ip)).find_first_not_of(" ") != std::string::npos);
    CHECK(std::string(lv_subject_get_string(port)) == "7125");
}

TEST_CASE_METHOD(WizardConnectionLifetimeFixture,
                 "Connection step: cleanup expires lifetime tokens",
                 "[wizard][connection][lifetime]") {
    auto tok = step->lifetime_token_for_test();
    REQUIRE_FALSE(tok.expired());

    step->cleanup();
    REQUIRE(tok.expired());

    // tok.defer should be silently skipped (no crash, no side effects)
    bool callback_ran = false;
    tok.defer("test_after_cleanup", [&callback_ran]() { callback_ran = true; });

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(callback_ran);
}

TEST_CASE_METHOD(WizardConnectionLifetimeFixture,
                 "Connection step: retry invalidates previous attempt tokens",
                 "[wizard][connection][lifetime]") {
    // First "attempt"
    auto tok1 = step->lifetime_token_for_test();
    REQUIRE_FALSE(tok1.expired());

    // Simulate the invalidation that happens at the start of a new attempt
    step->cleanup();

    REQUIRE(tok1.expired());

    // Re-init for new attempt (as wizard framework does on re-navigation)
    step->init_subjects();
    auto tok2 = step->lifetime_token_for_test();
    REQUIRE_FALSE(tok2.expired());

    // Old token still expired, new token valid
    REQUIRE(tok1.expired());
    REQUIRE_FALSE(tok2.expired());
}

TEST_CASE_METHOD(WizardConnectionLifetimeFixture,
                 "Connection step: deferred callback runs when token valid",
                 "[wizard][connection][lifetime]") {
    auto tok = step->lifetime_token_for_test();

    bool callback_ran = false;
    tok.defer("test_valid_token", [&callback_ran]() { callback_ran = true; });

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(callback_ran);
}

// ============================================================================
// Klipper-down dead end (Moonraker up, Klippy in `error`)
// ============================================================================
// The connection step never skips, the Next/Finish buttons bind to
// connection_test_passed, and this step raises no Skip button — so gating the
// gate on hardware discovery made the single most common first-boot state
// (Moonraker running, Klipper down on a bad printer.cfg) an unbypassable
// full-screen wall: no Next, no Skip, Back hidden at the first visible step,
// and no way into the app's own Klipper error surface. Only --skip-wizard or
// hand-editing settings.json got the user out.

extern lv_subject_t connection_test_passed;

class WizardConnectionGateFixture : public LVGLTestFixture {
  public:
    WizardConnectionGateFixture() {
        ui_wizard_init_subjects(); // defines connection_test_passed (idempotent)
        step = get_wizard_connection_step();
        step->init_subjects();
    }
    ~WizardConnectionGateFixture() override {
        step->cleanup();
    }
    WizardConnectionStep* step = nullptr;
};

TEST_CASE_METHOD(WizardConnectionGateFixture, "Connection step: entering the step gates Next off",
                 "[wizard][connection][gate]") {
    REQUIRE(lv_subject_get_int(&connection_test_passed) == 0);
}

TEST_CASE_METHOD(WizardConnectionGateFixture,
                 "Connection step: Moonraker up + Klipper down still allows Next",
                 "[wizard][connection][gate][regression]") {
    REQUIRE(lv_subject_get_int(&connection_test_passed) == 0);

    // Reached only after the WebSocket connected, i.e. Moonraker answered on
    // the entered address; discovery then aborted because klippy_state was
    // "error"/"startup".
    step->allow_continue_without_klipper();

    REQUIRE(lv_subject_get_int(&connection_test_passed) == 1);

    // Still explicitly NOT a full validation — discovery never ran, so the
    // hardware lists are empty and nothing downstream should assume otherwise.
    REQUIRE_FALSE(step->is_validated());
}

TEST_CASE_METHOD(WizardConnectionGateFixture, "Connection step: re-entering the step re-gates Next",
                 "[wizard][connection][gate]") {
    step->allow_continue_without_klipper();
    REQUIRE(lv_subject_get_int(&connection_test_passed) == 1);

    // Navigating away and back must not inherit the previous pass (same
    // cleanup/re-init sequence the wizard framework runs on a revisit).
    step->cleanup();
    step->init_subjects();
    REQUIRE(lv_subject_get_int(&connection_test_passed) == 0);
}

// ============================================================================
// Silent-discovery dead end (#1161)
// ============================================================================
// The Klipper-down fix above only helps when discovery *reports* an error.
// MoonrakerDiscoverySequence drops its own RPC replies whenever the connection
// generation or the discovery sequence number has moved on (is_stale() /
// is_current_sequence()), and continue_discovery() nulls on_complete_discovery_
// on the error path — so there are live paths where NEITHER discover_printer()
// callback ever fires. connection_discovering_ then stays 1: spinner forever,
// Next disabled forever, on a step with no Skip and Back hidden. The timeout
// has to be owned here, in the step, for exactly that reason.

TEST_CASE_METHOD(WizardConnectionGateFixture,
                 "Connection step: silent discovery times out and unblocks Next",
                 "[wizard][connection][gate][watchdog][regression]") {
    REQUIRE(lv_subject_get_int(&connection_test_passed) == 0);

    // Real timer, real lv_timer_handler dispatch — only the window is shortened.
    step->set_discovery_watchdog_ms_for_test(20);
    step->start_discovery_watchdog();

    // Nothing happens while the window is open.
    REQUIRE(lv_subject_get_int(&connection_test_passed) == 0);

    process_lvgl(60);

    REQUIRE(lv_subject_get_int(&connection_test_passed) == 1);

    // Same contract as the Klipper-down path: the gate opens, but discovery
    // never produced hardware, so this is not a validated connection.
    REQUIRE_FALSE(step->is_validated());
}

TEST_CASE_METHOD(WizardConnectionGateFixture,
                 "Connection step: step teardown cancels the discovery watchdog",
                 "[wizard][connection][gate][watchdog]") {
    step->set_discovery_watchdog_ms_for_test(20);
    step->start_discovery_watchdog();

    // Navigating away mid-discovery. A watchdog that outlives the step would
    // fire into a torn-down screen and re-open the gate behind the user.
    step->cleanup();
    step->init_subjects();

    process_lvgl(60);

    REQUIRE(lv_subject_get_int(&connection_test_passed) == 0);
}

TEST_CASE_METHOD(WizardConnectionGateFixture,
                 "Connection step: watchdog expiry after discovery settled is a no-op",
                 "[wizard][connection][gate][watchdog]") {
    // A cancelled one-shot can still reach its handler in the same
    // lv_timer_handler pass that cancelled it; that must not touch the gate.
    step->discovery_watchdog_expired();
    REQUIRE(lv_subject_get_int(&connection_test_passed) == 0);
}

// ============================================================================
// Auto-probe timer must not outlive the step (#1173)
// ============================================================================
// cleanup() cancels the one-shot, so the normal navigation path is covered. A
// teardown that destroys the step WITHOUT calling cleanup() first left it armed
// on a freed `this` — and StaticPanelRegistry::destroy_all() runs before
// lv_deinit() (application.cpp), so at destructor time the timer really is still
// in LVGL's list. Same shape as the discovery watchdog's destructor cancel
// (#1161), and the same UAF that auto_probe_timer_cb's own comment documents.

TEST_CASE_METHOD(LVGLTestFixture, "Connection step: destructor cancels the auto-probe timer",
                 "[wizard][connection][timer][regression]") {
    auto step = std::make_unique<WizardConnectionStep>();
    step->arm_auto_probe_timer_for_test();

    lv_timer_t* timer = step->auto_probe_timer_for_test();
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);

    // Destroy without cleanup() — the path that used to leave it armed.
    step.reset();

    // Neutered, not deleted: lv_timer_cancel_safe() nulls the callback and lets
    // lv_timer_handler reap the timer on its next pass. Reading it here is safe
    // because the timer is LVGL-owned memory, not the step's.
    REQUIRE(timer->timer_cb == nullptr);

    // A still-armed one-shot would dispatch into the freed step here.
    process_lvgl(150);
}
