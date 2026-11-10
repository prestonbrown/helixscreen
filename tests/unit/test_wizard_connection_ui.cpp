// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard.h"
#include "ui_wizard_connection.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_ui_test_fixture.h"
#include "../mocks/mock_mdns_discovery.h"
#include "../test_helpers/fake_connection_test.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "config.h"
#include "core/update_queue.h"
#include "lan_sweep.h"
#include "lvgl/lvgl.h"
#include "misc/lv_timer_private.h" // timer_cb — assert the cancel neutered it
#include "moonraker_auth.h"
#include "moonraker_client.h"
#include "platform_info.h"
#include "printer_discovery_list.h"
#include "theme_manager.h"
#include "utils/network_validation.h"
#include "wizard_config_paths.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

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

    helix::ui::UpdateQueue::instance().drain();
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

    helix::ui::UpdateQueue::instance().drain();
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

// ============================================================================
// mDNS discovery behind a hidden section (#1217)
// ============================================================================

// ============================================================================
// Failed connection test names what to check
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: a failed test shows the hint for the failure kind",
                 "[wizard][connection][connect_failure]") {
    const auto kind = GENERATE(ConnectFailureKind::Timeout, ConnectFailureKind::Refused,
                               ConnectFailureKind::Unknown);

    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    helix::test::ScopedFakeConnectionTest net;
    net.failure = kind;

    WizardConnectionStep* step = get_wizard_connection_step();
    step->init_subjects();
    step->register_callbacks();
    step->set_mdns_discovery(std::make_unique<MockMdnsDiscovery>());
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);

    lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_ip"), "10.9.9.9");
    lv_obj_t* btn = lv_obj_find_by_name(root, "btn_test_connection");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // The untouched default also tries port 80 before giving up, and a failed test never
    // reaches the live client.
    CHECK(net.asked == std::vector<std::string>{"10.9.9.9:7125", "10.9.9.9:80"});
    CHECK(client.connect_urls.empty());

    lv_obj_t* status = lv_obj_find_by_name(root, "connection_status_text");
    REQUIRE(status != nullptr);
    const std::string text = lv_label_get_text(status);
    CHECK(text == connect_failure_status(kind, {7125, 80}));
    CHECK(text.find(lv_tr(connect_failure_hint(kind, is_android_platform()))) == 0);
    CHECK(text.find("80") != std::string::npos);

    // One icon shows, in the danger colour.
    lv_obj_t* icons = lv_obj_find_by_name(root, "connection_status_icon");
    REQUIRE(icons != nullptr);
    std::vector<lv_obj_t*> shown;
    for (uint32_t i = 0; i < lv_obj_get_child_count(icons); ++i) {
        if (!lv_obj_has_flag(lv_obj_get_child(icons, i), LV_OBJ_FLAG_HIDDEN)) {
            shown.push_back(lv_obj_get_child(icons, i));
        }
    }
    REQUIRE(shown.size() == 1);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(shown[0], LV_PART_MAIN),
                      theme_manager_get_color("danger")));
    step->cleanup();
}

// ============================================================================
// Moonraker behind a web server answers only on port 80
// ============================================================================

namespace {

/// Saves the active printer's host and port and puts them back, since a passed test
/// persists the address it reached.
struct SavedMoonrakerAddress {
    Config* cfg = Config::get_instance();
    std::string host_key = cfg->df() + helix::wizard::MOONRAKER_HOST;
    std::string port_key = cfg->df() + helix::wizard::MOONRAKER_PORT;
    std::string host = cfg->get<std::string>(host_key, "");
    int port = cfg->get<int>(port_key, 7125);
    ~SavedMoonrakerAddress() {
        cfg->set<std::string>(host_key, host);
        cfg->set<int>(port_key, port);
    }
};

/// The connection step is a process-wide singleton: put its port field back the way a
/// fresh step shows it.
struct RestoreDefaultPortField {
    WizardConnectionStep* step;
    ~RestoreDefaultPortField() {
        step->cleanup();
        lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_port"), "7125");
    }
};

lv_obj_t* create_step_and_test(WizardConnectionStep* step, lv_obj_t* parent) {
    step->init_subjects();
    step->register_callbacks();
    step->set_mdns_discovery(std::make_unique<MockMdnsDiscovery>());
    lv_obj_t* root = step->create(parent);
    REQUIRE(root != nullptr);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_ip"), "10.9.9.9");
    return root;
}

void click_test(lv_obj_t* root) {
    lv_obj_t* btn = lv_obj_find_by_name(root, "btn_test_connection");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: nothing on the default port, Moonraker found on port 80",
                 "[wizard][connection][port_fallback]") {
    SavedMoonrakerAddress saved;
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};

    helix::test::ScopedFakeConnectionTest net;
    net.answer("10.9.9.9:80");
    lv_obj_t* root = create_step_and_test(step, test_screen());
    click_test(root);
    CHECK(net.asked == std::vector<std::string>{"10.9.9.9:7125", "10.9.9.9:80"});

    // The live client goes straight to the port that answered, for discovery.
    REQUIRE(client.connect_urls == std::vector<std::string>{"ws://10.9.9.9:80/websocket"});
    REQUIRE(client.fire_connected());
    helix::ui::UpdateQueue::instance().drain();

    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "connection_port"))) ==
          "80");
    lv_obj_t* port_input = lv_obj_find_by_name(root, "port_input");
    REQUIRE(port_input != nullptr);
    CHECK(std::string(lv_textarea_get_text(port_input)) == "80");
    CHECK(saved.cfg->get<int>(saved.port_key, 0) == 80);
    CHECK(saved.cfg->get<std::string>(saved.host_key, "") == "10.9.9.9");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Connection step: a typed port is the only port tried",
                 "[wizard][connection][port_fallback]") {
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    helix::test::ScopedFakeConnectionTest net;
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};

    lv_obj_t* root = create_step_and_test(step, test_screen());
    lv_obj_t* port_input = lv_obj_find_by_name(root, "port_input");
    REQUIRE(port_input != nullptr);
    lv_textarea_set_text(port_input, "7125");
    click_test(root);

    CHECK(net.asked == std::vector<std::string>{"10.9.9.9:7125"});
    lv_obj_t* status = lv_obj_find_by_name(root, "connection_status_text");
    REQUIRE(status != nullptr);
    CHECK(std::string(lv_label_get_text(status)) == connect_failure_status(net.failure, {7125}));
}

// ============================================================================
// Printer discovery (mDNS and the LAN sweep)
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: with no discovery source the section stays hidden",
                 "[wizard][connection][mdns][lan_sweep]") {
    helix::set_printer_discovery_source(nullptr);
    WizardConnectionStep* step = get_wizard_connection_step();
    step->init_subjects();
    step->set_mdns_discovery(nullptr);

    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* section = lv_obj_find_by_name(root, "discovery_section");
    REQUIRE(section != nullptr);
    CHECK(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));
    step->cleanup();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: discovery runs on open and a pick fills host and port",
                 "[wizard][connection][mdns][lan_sweep]") {
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    step->init_subjects();
    step->register_callbacks();
    auto mock = std::make_unique<MockMdnsDiscovery>();
    MockMdnsDiscovery* mdns = mock.get();
    // The source the app sets at startup; the step asks it when it opens.
    helix::set_printer_discovery_source(
        [&mock]() -> std::unique_ptr<IMdnsDiscovery> { return std::move(mock); });
    step->set_mdns_discovery(nullptr);

    lv_obj_t* root = step->create(test_screen());
    helix::set_printer_discovery_source(nullptr);
    REQUIRE(root != nullptr);
    lv_obj_t* section = lv_obj_find_by_name(root, "discovery_section");
    REQUIRE(section != nullptr);
    CHECK_FALSE(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(mdns->is_discovering());

    // A connection test retires its own callbacks, not discovery's.
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    helix::test::ScopedFakeConnectionTest net;
    click_test(root);
    REQUIRE_FALSE(net.asked.empty());

    mdns->add_fake_printer("", "", "192.168.1.52", 80);
    mdns->simulate_discovery();
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "printer_dropdown");
    REQUIRE(dropdown != nullptr);
    REQUIRE(lv_dropdown_get_option_count(dropdown) == 2);

    lv_dropdown_set_selected(dropdown, 1);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);

    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "connection_ip"))) ==
          "192.168.1.52");
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "connection_port"))) ==
          "80");
    lv_obj_t* port_input = lv_obj_find_by_name(root, "port_input");
    REQUIRE(port_input != nullptr);
    CHECK(std::string(lv_textarea_get_text(port_input)) == "80");
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_ip"), "127.0.0.1");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: leaving the step forgets that the port was typed",
                 "[wizard][connection][port_fallback]") {
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};

    lv_obj_t* root = create_step_and_test(step, test_screen());
    lv_obj_t* port_input = lv_obj_find_by_name(root, "port_input");
    REQUIRE(port_input != nullptr);
    lv_textarea_set_text(port_input, "7125");

    // Away and back: the field still says 7125, now as the app's own default.
    step->cleanup();
    root = create_step_and_test(step, test_screen());
    helix::test::ScopedFakeConnectionTest net;
    click_test(root);
    CHECK(net.asked == std::vector<std::string>{"10.9.9.9:7125", "10.9.9.9:80"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: a discovered printer on 7125 is tried on 7125 alone",
                 "[wizard][connection][port_fallback][lan_sweep]") {
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    step->init_subjects();
    step->register_callbacks();
    auto mock = std::make_unique<MockMdnsDiscovery>();
    MockMdnsDiscovery* mdns = mock.get();
    step->set_mdns_discovery(std::move(mock));
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);
    mdns->add_fake_printer("voron", "voron.local", "192.168.1.112", 7125);
    mdns->simulate_discovery();

    lv_obj_t* dropdown = lv_obj_find_by_name(root, "printer_dropdown");
    REQUIRE(dropdown != nullptr);
    lv_dropdown_set_selected(dropdown, 1);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::test::ScopedFakeConnectionTest net;
    click_test(root);

    CHECK(net.asked == std::vector<std::string>{"192.168.1.112:7125"});
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_ip"), "127.0.0.1");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: picking another printer clears the key entered for this one",
                 "[wizard][connection][api-key][lan_sweep]") {
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    step->init_subjects();
    step->register_callbacks();
    auto mock = std::make_unique<MockMdnsDiscovery>();
    MockMdnsDiscovery* mdns = mock.get();
    step->set_mdns_discovery(std::move(mock));
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);

    lv_subject_t* key = lv_xml_get_subject(nullptr, "connection_api_key");
    lv_subject_t* login = lv_xml_get_subject(nullptr, "connection_login_required");
    REQUIRE(key != nullptr);
    REQUIRE(login != nullptr);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_ip"), "192.168.1.20");
    lv_subject_copy_string(key, "0123456789abcdef0123456789abcdef");
    lv_subject_set_int(login, 1);

    mdns->add_fake_printer("voron", "voron.local", "192.168.1.112", 7125);
    mdns->simulate_discovery();
    lv_obj_t* dropdown = lv_obj_find_by_name(root, "printer_dropdown");
    REQUIRE(dropdown != nullptr);
    lv_dropdown_set_selected(dropdown, 1);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);

    CHECK(std::string(lv_subject_get_string(key)).empty());
    CHECK(lv_subject_get_int(login) == 0);
    lv_subject_copy_string(key, "");
    lv_subject_set_int(login, 0);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_ip"), "127.0.0.1");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: editing the address drops a test still running",
                 "[wizard][connection][port_fallback]") {
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    // A worker that has not run yet when the user types.
    std::function<void()> pending;
    helix::set_connection_test_seams_for_test(
        [](const std::string&, uint16_t, const std::string&) { return MoonrakerProbe(true); },
        [&pending](size_t, const LanSweepLimits&, std::function<void()> body) {
            pending = std::move(body);
            return LanSweepWorkers{nullptr, 1};
        });

    lv_obj_t* root = create_step_and_test(step, test_screen());
    click_test(root);
    REQUIRE(pending);
    lv_obj_t* ip_input = lv_obj_find_by_name(root, "ip_input");
    REQUIRE(ip_input != nullptr);
    lv_textarea_set_text(ip_input, "10.9.9.8");

    pending();
    helix::ui::UpdateQueue::instance().drain();
    helix::set_connection_test_seams_for_test(nullptr, nullptr);

    // The answer was for the old address: no live connect, Next stays shut.
    CHECK(client.connect_urls.empty());
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "connection_testing")) == 0);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_ip"), "127.0.0.1");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: the auto-probe finds Moonraker on 80 and connects there",
                 "[wizard][connection][port_fallback]") {
    SavedMoonrakerAddress saved;
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    helix::test::ScopedFakeConnectionTest net;
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    lv_obj_t* root = create_step_and_test(step, test_screen());
    net.answer("10.9.9.9:80");

    step->arm_auto_probe_timer_for_test();
    process_lvgl(150);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(net.asked == std::vector<std::string>{"10.9.9.9:7125", "10.9.9.9:80"});
    REQUIRE(client.connect_urls == std::vector<std::string>{"ws://10.9.9.9:80/websocket"});

    REQUIRE(client.fire_connected());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "connection_port"))) ==
          "80");
    lv_obj_t* port_input = lv_obj_find_by_name(root, "port_input");
    REQUIRE(port_input != nullptr);
    CHECK(std::string(lv_textarea_get_text(port_input)) == "80");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: a test that finds nothing leaves the live client connected",
                 "[wizard][connection][connection_test]") {
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    helix::test::ScopedFakeConnectionTest net;
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    lv_obj_t* root = create_step_and_test(step, test_screen());

    SECTION("Test Connection") {
        click_test(root);
    }
    SECTION("the auto-probe") {
        step->arm_auto_probe_timer_for_test();
        process_lvgl(150);
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE_FALSE(net.asked.empty());
    CHECK(client.disconnect_calls == 0);
    CHECK(client.connect_urls.empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Connection step: leaving the step drops a test still running",
                 "[wizard][connection][connection_test]") {
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    std::function<void()> pending;
    helix::set_connection_test_seams_for_test(
        [](const std::string&, uint16_t, const std::string&) { return MoonrakerProbe(true); },
        [&pending](size_t, const LanSweepLimits&, std::function<void()> body) {
            pending = std::move(body);
            return LanSweepWorkers{nullptr, 1};
        });

    lv_obj_t* root = create_step_and_test(step, test_screen());
    click_test(root);
    REQUIRE(pending);
    step->cleanup();

    pending();
    helix::ui::UpdateQueue::instance().drain();
    helix::set_connection_test_seams_for_test(nullptr, nullptr);

    CHECK(client.connect_urls.empty());
}

// ============================================================================
// Moonraker behind its login (prestonbrown/helixscreen#1929)
// ============================================================================

namespace {

constexpr const char* kWizardKey = "0123456789abcdef0123456789abcdef";

/// Puts the saved key and the step's key field back the way a fresh install has them.
struct RestoreApiKey {
    Config* cfg = Config::get_instance();
    std::string key_path = cfg->df() + "moonraker_api_key";
    nlohmann::json saved =
        cfg->try_get_json(key_path) ? *cfg->try_get_json(key_path) : nlohmann::json(false);
    ~RestoreApiKey() {
        cfg->set(key_path, saved);
        lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_api_key"), "");
        lv_subject_set_int(lv_xml_get_subject(nullptr, "connection_login_required"), 0);
        helix::moonraker_auth::set_credential({});
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Connection step: a printer that requires a login asks for its API key",
                 "[wizard][connection][api-key]") {
    SavedMoonrakerAddress saved;
    helix::test::FakeMoonrakerClient client;
    ScopedMoonrakerClient installed(&client);
    WizardConnectionStep* step = get_wizard_connection_step();
    RestoreDefaultPortField restore{step};
    RestoreApiKey restore_key;
    step->register_callbacks();

    helix::test::ScopedFakeConnectionTest net;
    net.require_key("10.9.9.9:7125", kWizardKey);
    lv_obj_t* root = create_step_and_test(step, test_screen());
    lv_obj_t* section = lv_obj_find_by_name(root, "api_key_section");
    REQUIRE(section != nullptr);
    CHECK(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));

    click_test(root);

    // Refused: the field appears, the live client stays put, Next stays shut.
    CHECK_FALSE(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* status = lv_obj_find_by_name(root, "connection_status_text");
    REQUIRE(status != nullptr);
    CHECK(std::string(lv_label_get_text(status)).find("requires an API key") != std::string::npos);
    CHECK(client.connect_urls.empty());
    CHECK_FALSE(step->is_validated());

    SECTION("a key that is not 32 hex digits is refused before any test") {
        lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_api_key"), "secret");
        click_test(root);
        CHECK(net.asked.size() == 1);
        CHECK(std::string(lv_label_get_text(status)) == helix::api_key_problem("secret"));
    }
    SECTION("the right key connects with it and is saved with the printer") {
        lv_subject_copy_string(lv_xml_get_subject(nullptr, "connection_api_key"), kWizardKey);
        click_test(root);
        CHECK(net.keys_sent.back() == kWizardKey);
        REQUIRE(client.connect_urls == std::vector<std::string>{"ws://10.9.9.9:7125/websocket"});
        // The WebSocket upgrade to this printer carries it.
        CHECK(helix::moonraker_auth::key_for("10.9.9.9", 7125) == kWizardKey);

        REQUIRE(client.fire_connected());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(restore_key.cfg->get<std::string>(restore_key.key_path, "") == kWizardKey);
    }
}
