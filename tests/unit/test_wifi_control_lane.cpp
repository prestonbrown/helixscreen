// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// WiFiManager's control lane and breaker: a backend that stops answering must
// never hold the LVGL thread, must not be polled while it is wedged, and must
// not be asked twice for a scan it is already running.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "async_lifetime_guard.h"
#include "core/update_queue.h"
#include "http_executor.h"
#include "wifi_backend_mock.h"
#include "wifi_backend_wpa_supplicant.h"
#include "wifi_control_breaker.h"
#include "wifi_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::WiFiManager;
using helix::WiFiManagerTestAccess;
using helix::http::HttpExecutor;

namespace {

// Outlives any one backend, so a test can ask after the manager is gone.
std::atomic<int> g_forget_calls{0};

/// Every control call parks while `stall()` is in force, standing in for a
/// wpa_supplicant whose driver wedged: wpa_ctrl waits out its reply deadline.
/// The park is bounded so a call made on the test's own thread fails the
/// timing checks instead of hanging the run. get_status() can also be made
/// merely slow, which is what the breaker measures.
class StallingBackend : public WifiBackendMock {
  public:
    static constexpr auto kMaxStall = std::chrono::seconds(3);

    WiFiError get_scan_results(std::vector<WiFiNetwork>& networks) override {
        if (fail_results_.load()) {
            networks.clear();
            return WiFiError(WiFiResult::BACKEND_ERROR, "SCAN_RESULTS failed", "");
        }
        return WifiBackendMock::get_scan_results(networks);
    }
    WiFiError trigger_scan() override {
        enter(triggers_);
        if (fail_triggers_.load()) {
            return WiFiError(WiFiResult::BACKEND_ERROR, "FAIL-BUSY", "");
        }
        return WiFiError(WiFiResult::SUCCESS);
    }
    WiFiError connect_network(const std::string&, const std::string&, bool) override {
        enter(connects_);
        if (const int ms = connect_delay_ms_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        return WiFiError(WiFiResult::SUCCESS);
    }
    bool supports_wpa_supplicant_fallback() const override {
        return true;
    }
    WiFiError disconnect_network() override {
        enter(disconnects_);
        return WiFiError(WiFiResult::SUCCESS);
    }
    WiFiError forget_network(const std::string&) override {
        g_forget_calls++;
        enter(forgets_);
        return WiFiError(WiFiResult::SUCCESS);
    }
    WiFiError set_radio_enabled(bool on) override {
        enter(radios_);
        return WifiBackendMock::set_radio_enabled(on);
    }
    ConnectionStatus get_status() override {
        enter(statuses_);
        if (const int ms = status_delay_ms_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        return WifiBackendMock::get_status();
    }

    void stall() {
        std::lock_guard<std::mutex> lock(mutex_);
        stalled_ = true;
    }
    void release() {
        // Notify under the lock: a parked call returns as soon as stalled_
        // flips, and this backend dies with its manager.
        std::lock_guard<std::mutex> lock(mutex_);
        stalled_ = false;
        cv_.notify_all();
    }
    void set_status_delay_ms(int ms) {
        status_delay_ms_ = ms;
    }
    /// A join that takes this long, the way NetworkManager's does while it
    /// joins the previous nmcli worker.
    void set_connect_delay_ms(int ms) {
        connect_delay_ms_ = ms;
    }
    void fail_triggers() {
        fail_triggers_ = true;
    }
    void fail_results() {
        fail_results_ = true;
    }

    int triggers() const {
        return triggers_.load();
    }
    int connects() const {
        return connects_.load();
    }
    int disconnects() const {
        return disconnects_.load();
    }
    int forgets() const {
        return forgets_.load();
    }
    int radios() const {
        return radios_.load();
    }
    int statuses() const {
        return statuses_.load();
    }
    int calls() const {
        return triggers() + connects() + disconnects() + forgets() + radios() + statuses();
    }
    std::vector<std::thread::id> callers() {
        std::lock_guard<std::mutex> lock(mutex_);
        return callers_;
    }

  private:
    void enter(std::atomic<int>& counter) {
        std::unique_lock<std::mutex> lock(mutex_);
        callers_.push_back(std::this_thread::get_id());
        counter++;
        cv_.wait_for(lock, kMaxStall, [this] { return !stalled_; });
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stalled_ = false;
    std::vector<std::thread::id> callers_;
    std::atomic<int> triggers_{0}, connects_{0}, disconnects_{0}, forgets_{0}, radios_{0},
        statuses_{0};
    std::atomic<int> status_delay_ms_{0};
    std::atomic<int> connect_delay_ms_{0};
    std::atomic<bool> fail_triggers_{false};
    std::atomic<bool> fail_results_{false};
};

struct ControlLaneFixture : LVGLTestFixture {
    StallingBackend* backend = nullptr;
    std::shared_ptr<WiFiManager> wm;
    helix::AsyncLifetimeGuard guard;
    const std::thread::id main_thread = std::this_thread::get_id();

    ControlLaneFixture() {
        HttpExecutor::fast().start();
        auto b = std::make_unique<StallingBackend>();
        backend = b.get();
        wm = std::make_shared<WiFiManager>(std::move(b));
        wm->init_self_reference(wm);
        REQUIRE(WiFiManagerTestAccess::settle(*wm));
    }

    ~ControlLaneFixture() override {
        if (backend) {
            backend->release();
        }
        if (wm) {
            WiFiManagerTestAccess::settle(*wm);
            wm.reset();
        }
        helix::ui::UpdateQueue::instance().drain();
    }

    /// One status read, answered on this (the LVGL) thread.
    /// The answer is held by the callback, not this frame, so a REQUIRE that
    /// gives up leaves nothing for a late answer to write through.
    WifiBackend::ConnectionStatus read_status() {
        auto got = std::make_shared<std::optional<WifiBackend::ConnectionStatus>>();
        wm->get_status_async(guard.token(),
                             [got](const WifiBackend::ConnectionStatus& s) { *got = s; });
        REQUIRE(wait_until([&] { return got->has_value(); }));
        return **got;
    }

    /// Park a status read on the lane and wait until it has outlived the
    /// breaker's slow-call time, so the lane counts as stalled.
    void stall_lane_on_status_read(int slow_call_ms) {
        backend->stall();
        const int before = backend->statuses();
        wm->get_status_async(guard.token(), [](const WifiBackend::ConnectionStatus&) {});
        REQUIRE(wait_until([&] { return backend->statuses() == before + 1; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(slow_call_ms * 2));
    }
};

} // namespace

TEST_CASE_METHOD(ControlLaneFixture,
                 "WiFi UI entry points return at once while the backend is stalled",
                 "[wifi][wifi_control_lane]") {
    backend->stall();

    const auto started = std::chrono::steady_clock::now();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    wm->connect("Net", "password", [](bool, const std::string&, WiFiResult) {});
    wm->disconnect();
    wm->forget("Net", [](bool, const std::string&) {});
    wm->set_enabled_async(true, guard.token(), [](bool, bool) {});
    wm->get_status_async(guard.token(), [](const WifiBackend::ConnectionStatus&) {});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // One stalled call on this thread would cost StallingBackend::kMaxStall.
    CHECK(elapsed < std::chrono::milliseconds(500));

    // The calls happen, one at a time and in order, on a worker.
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));
    CHECK(backend->calls() == 1);
    backend->release();
    REQUIRE(wait_until([&] {
        return backend->connects() == 1 && backend->disconnects() == 1 && backend->forgets() == 1 &&
               backend->radios() == 1 && backend->statuses() == 1;
    }));
    for (const auto& id : backend->callers()) {
        CHECK(id != main_thread);
    }
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "a scan result reads the cached connection state, not the backend",
                 "[wifi][wifi_control_lane]") {
    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    CHECK(read_status().ssid == "Home");

    int delivered = 0;
    wm->start_scan([&](const std::vector<WiFiNetwork>&) { delivered++; });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    backend->clear_status_callers();

    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(wait_until([&] { return delivered == 1; }));

    const auto readers = backend->status_callers();
    CHECK(std::find(readers.begin(), readers.end(), main_thread) == readers.end());
}

TEST_CASE_METHOD(ControlLaneFixture, "a scan requested while one is in flight is not sent again",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));

    // The user reopens the list while the first trigger is still out.
    int second = 0;
    wm->start_scan([&](const std::vector<WiFiNetwork>&) { second++; });
    backend->release();
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->triggers() == 1);

    // The one trigger's results reach the callback that asked second.
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(wait_until([&] { return second == 1; }));
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "the control breaker trips on slow status reads, answers from cache, and "
                 "recovers on a probe",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    int notified = 0;
    WiFiManagerTestAccess::add_observer(*wm, guard.token(), [&] { notified++; });

    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    REQUIRE(read_status().ssid == "Home");

    // The driver wedges: every read now runs past the threshold.
    backend->set_status_delay_ms(80);
    for (int i = 0; i < 3; ++i) {
        read_status();
    }
    REQUIRE(WiFiManagerTestAccess::breaker_open(*wm));
    REQUIRE(wait_until([&] { return notified == 1; }));

    // While open, a poll is answered from the last good read without asking.
    const int asked = backend->statuses();
    const auto cached = read_status();
    CHECK(cached.unresponsive);
    CHECK(cached.connected);
    CHECK(cached.ssid == "Home");
    CHECK(backend->statuses() == asked);

    // So is a scan: nothing is sent, and the caller still hears back.
    bool scan_answered = false;
    wm->start_scan([&](const std::vector<WiFiNetwork>& n) { scan_answered = n.empty(); });
    REQUIRE(wait_until([&] { return scan_answered; }));
    CHECK(backend->triggers() == 0);

    // The driver recovers; once the backoff passes, one poll probes and closes it.
    backend->set_status_delay_ms(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto fresh = read_status();
    CHECK_FALSE(fresh.unresponsive);
    CHECK(backend->statuses() == asked + 1);
    CHECK_FALSE(WiFiManagerTestAccess::breaker_open(*wm));
    REQUIRE(wait_until([&] { return notified == 2; }));
}

TEST_CASE_METHOD(ControlLaneFixture, "a status answer for a caller that died is dropped",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    auto owner = std::make_unique<helix::AsyncLifetimeGuard>();
    bool answered = false;
    wm->get_status_async(owner->token(),
                         [&](const WifiBackend::ConnectionStatus&) { answered = true; });
    REQUIRE(wait_until([&] { return backend->statuses() == 1; }));

    owner.reset(); // the overlay closes while the read is parked
    backend->release();
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK_FALSE(answered);
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "destroying the manager waits for the call in progress and drops the queue",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));
    g_forget_calls = 0;
    bool forgot = false;
    wm->forget("Net", [&](bool, const std::string&) { forgot = true; });

    std::thread releaser([b = backend] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        b->release();
    });
    const auto started = std::chrono::steady_clock::now();
    wm.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    releaser.join();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(g_forget_calls == 0);
    CHECK_FALSE(forgot);
    backend = nullptr;
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "polls and scans behind a call that never returns are answered without it",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    // A send blocked on a daemon that stopped reading never finishes, so the
    // breaker never hears of it; the lane's own clock has to.
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    REQUIRE(read_status().ssid == "Home");

    backend->stall();
    bool first_answered = false;
    wm->get_status_async(guard.token(),
                         [&](const WifiBackend::ConnectionStatus&) { first_answered = true; });
    REQUIRE(wait_until([&] { return backend->statuses() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Still parked, and a poll arriving now is answered from cache at once.
    const auto cached = read_status();
    CHECK(cached.unresponsive);
    CHECK(cached.ssid == "Home");
    CHECK_FALSE(first_answered);

    bool scan_answered = false;
    wm->start_scan([&](const std::vector<WiFiNetwork>& n) { scan_answered = n.empty(); });
    REQUIRE(wait_until([&] { return scan_answered; }, 1000));
    CHECK(backend->triggers() == 0);

    backend->release();
    REQUIRE(wait_until([&] { return first_answered; }));
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "user actions queued behind a call that never returns fail by their deadline",
                 "[wifi][wifi_control_lane]") {
    // Short deadlines; the breaker's own slow-call time stays at its default, so
    // the lane is not yet stalled when these are queued.
    WiFiManagerTestAccess::set_user_op_timeouts(*wm, 300, 300, 60000);
    backend->stall();
    const int asked = backend->statuses();
    wm->get_status_async(guard.token(), [](const WifiBackend::ConnectionStatus&) {});
    REQUIRE(wait_until([&] { return backend->statuses() == asked + 1; }));

    std::optional<WiFiResult> connect_result;
    std::optional<bool> forgot;
    std::optional<bool> radio_ok;
    wm->connect("Net", "password",
                [&](bool, const std::string&, WiFiResult r) { connect_result = r; });
    wm->forget("Net", [&](bool ok, const std::string&) { forgot = ok; });
    wm->set_enabled_async(true, guard.token(), [&](bool ok, bool) { radio_ok = ok; });

    REQUIRE(wait_until([&] { return connect_result && forgot && radio_ok; }, 2000));
    CHECK(*connect_result == WiFiResult::TIMEOUT);
    CHECK_FALSE(*forgot);
    CHECK_FALSE(*radio_ok);
    CHECK_FALSE(WiFiManagerTestAccess::connecting(*wm));

    // Once the stuck call returns, what the user was told failed is not sent.
    backend->release();
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->connects() == 0);
    CHECK(backend->forgets() == 0);
    CHECK(backend->radios() == 0);
}

TEST_CASE_METHOD(ControlLaneFixture, "user actions fail at once while WiFi control is stalled",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    // An open WiFi link would hide the radio's toast; this case is about the wording.
    WiFiManagerTestAccess::set_os_link_probe([] { return false; });
    stall_lane_on_status_read(50);
    // Held by the hook, so a REQUIRE that gives up leaves nothing dangling.
    auto toasts = std::make_shared<std::vector<std::string>>();
    helix::ui::set_test_notification_event_hook(
        [toasts](const helix::ui::NotificationEvent& e) { toasts->push_back(e.message); });

    std::optional<WiFiResult> connect_result;
    std::optional<std::string> forget_error;
    std::optional<bool> radio_ok;
    wm->disconnect(); // first: a disconnect supersedes a connect made before it
    wm->connect("Net", "password",
                [&](bool, const std::string&, WiFiResult r) { connect_result = r; });
    wm->forget("Net", [&](bool, const std::string& e) { forget_error = e; });
    wm->set_enabled_async(true, guard.token(), [&](bool ok, bool) { radio_ok = ok; });

    // Answered on the next pass, not after the stuck call.
    REQUIRE(wait_until([&] { return connect_result && forget_error && radio_ok; }, 200));
    CHECK(*connect_result == WiFiResult::TIMEOUT);
    CHECK(*forget_error == "WiFi isn't responding");
    CHECK_FALSE(*radio_ok);

    // One plain message per action, not each action's failure toast with the
    // reason glued on.
    helix::ui::set_test_notification_event_hook(nullptr);
    WiFiManagerTestAccess::reset_os_link_probe();
    CHECK(toasts->size() == 4);
    for (const auto& t : *toasts) {
        CHECK(t == "WiFi isn't responding");
    }

    backend->release();
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->connects() == 0);
    CHECK(backend->forgets() == 0);
    CHECK(backend->radios() == 0);
    CHECK(backend->disconnects() == 0);
}

TEST_CASE_METHOD(ControlLaneFixture, "a slow connect does not mark WiFi unresponsive",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    backend->set_connect_delay_ms(400);

    std::optional<WiFiResult> first;
    wm->connect("Net", "password", [&](bool, const std::string&, WiFiResult r) { first = r; });
    REQUIRE(wait_until([&] { return backend->connects() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // A poll waits for the join instead of reporting WiFi as not responding,
    // and the user retrying the join is not refused.
    std::optional<WiFiResult> retry;
    wm->connect("Net", "password", [&](bool, const std::string&, WiFiResult r) { retry = r; });
    const auto status = read_status();
    CHECK_FALSE(status.unresponsive);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->connects() == 2);
    CHECK_FALSE(first.has_value()); // superseded by the retry
    CHECK_FALSE(retry.has_value()); // sent; waiting on the backend's events
    CHECK_FALSE(WiFiManagerTestAccess::breaker_open(*wm));
}

TEST_CASE_METHOD(ControlLaneFixture, "a refused scan keeps the networks already shown",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    std::vector<WiFiNetwork> shown;
    wm->start_scan([&](const std::vector<WiFiNetwork>& n) { shown = n; });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(wait_until([&] { return !shown.empty(); }));
    const auto first = shown;

    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    stall_lane_on_status_read(50);
    bool answered = false;
    wm->start_scan([&](const std::vector<WiFiNetwork>& n) {
        answered = true;
        shown = n;
    });
    REQUIRE(wait_until([&] { return answered; }, 1000));
    CHECK(shown.size() == first.size());
}

TEST_CASE_METHOD(ControlLaneFixture, "a refresh coalesced onto a trigger that fails is answered",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    backend->fail_triggers();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));

    bool answered = false;
    wm->start_scan([&](const std::vector<WiFiNetwork>&) { answered = true; });
    backend->release();
    REQUIRE(wait_until([&] { return answered; }, 2000));
    CHECK(backend->triggers() == 1);
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "a refresh coalesced onto a trigger that never returns is answered",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {200, 3, 400, 2000});
    backend->stall();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));

    bool answered = false;
    wm->start_scan([&](const std::vector<WiFiNetwork>&) { answered = true; });
    REQUIRE(wait_until([&] { return answered; }, 2000));
    CHECK(backend->triggers() == 1); // still parked: the answer did not wait for it
}

TEST_CASE_METHOD(ControlLaneFixture, "calls after the lane has closed are not run",
                 "[wifi][wifi_control_lane]") {
    SECTION("closed for shutdown") {
        // A call in progress, so a closed lane that still queued the forget
        // would answer it only once that call returned.
        backend->stall();
        const int asked = backend->statuses();
        wm->get_status_async(guard.token(), [](const WifiBackend::ConnectionStatus&) {});
        REQUIRE(wait_until([&] { return backend->statuses() == asked + 1; }));
        WiFiManagerTestAccess::shut_control_lane(*wm);
        std::optional<bool> forgot;
        wm->forget("Net", [&](bool ok, const std::string&) { forgot = ok; });
        REQUIRE(wait_until([&] { return forgot.has_value(); }, 500));
        CHECK_FALSE(*forgot);
        backend->release();
        REQUIRE(WiFiManagerTestAccess::settle(*wm));
        CHECK(backend->forgets() == 0);
    }
    SECTION("the worker pool stopped") {
        read_status(); // the pool has run this manager's calls
        const int asked = backend->statuses();
        HttpExecutor::fast().stop();
        backend->stall(); // a call run on this thread now would take kMaxStall
        const auto started = std::chrono::steady_clock::now();
        std::optional<bool> forgot;
        wm->forget("Net", [&](bool ok, const std::string&) { forgot = ok; });
        std::optional<WifiBackend::ConnectionStatus> status;
        wm->get_status_async(guard.token(),
                             [&](const WifiBackend::ConnectionStatus& s) { status = s; });
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
        REQUIRE(wait_until([&] { return forgot.has_value() && status.has_value(); }, 1000));
        CHECK_FALSE(*forgot);
        CHECK(status->unresponsive);
        CHECK(backend->forgets() == 0);
        CHECK(backend->statuses() == asked);
        HttpExecutor::fast().start();
    }
}

// The wpa_supplicant backend is Linux-only (wifi_backend_wpa_supplicant.h).
#ifndef __APPLE__
TEST_CASE_METHOD(ControlLaneFixture,
                 "the wpa_supplicant fallback does not hold the UI thread for the lane",
                 "[wifi][wifi_control_lane]") {
    backend->stall();
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));
    g_forget_calls = 0;
    std::optional<bool> forgot;
    wm->forget("Net", [&](bool ok, const std::string&) { forgot = ok; });

    WiFiManagerTestAccess::fire_init_failed(*wm, /*silent=*/true, "init failed");
    // The stalled call never returns until release() below, so a UI thread that waited
    // on it would hang, not overrun: the bound only has to be far above scheduling noise
    // on a loaded runner.
    constexpr auto kNeverWaited = std::chrono::milliseconds(1000);
    const auto started = std::chrono::steady_clock::now();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(std::chrono::steady_clock::now() - started < kNeverWaited);

    // The swap keeps retrying while the call is stuck; no retry may wait on it.
    auto worst = std::chrono::steady_clock::duration::zero();
    for (int i = 0; i < 4; ++i) {
        lv_tick_inc(150);
        const auto t0 = std::chrono::steady_clock::now();
        lv_timer_handler();
        worst = std::max(worst, std::chrono::steady_clock::now() - t0);
    }
    CHECK(worst < kNeverWaited);
    REQUIRE(dynamic_cast<WifiBackendWpaSupplicant*>(WiFiManagerTestAccess::backend(*wm)) ==
            nullptr);

    // The swap happens once the call in progress returns; what was queued for
    // the failed backend is answered, not sent.
    backend->release();
    REQUIRE(wait_until([&] {
        return dynamic_cast<WifiBackendWpaSupplicant*>(WiFiManagerTestAccess::backend(*wm)) !=
               nullptr;
    }));
    backend = nullptr; // freed by the swap
    REQUIRE(wait_until([&] { return forgot.has_value(); }));
    CHECK_FALSE(*forgot);
    CHECK(g_forget_calls == 0);
}
#endif

TEST_CASE_METHOD(ControlLaneFixture,
                 "a CONNECTED event does not leave another network's name in the cache",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    REQUIRE(read_status().ssid == "Home");

    // A join to another network: its DISCONNECTED is swallowed mid-connect.
    WiFiManagerTestAccess::begin_connect(*wm, [](bool, const std::string&, WiFiResult) {});
    WiFiManagerTestAccess::fire_disconnected(*wm);
    WiFiManagerTestAccess::fire_connected(*wm);

    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    stall_lane_on_status_read(50);
    const auto cached = read_status();
    REQUIRE(cached.unresponsive);
    CHECK(cached.connected);
    CHECK(cached.ssid != "Home");
    CHECK(cached.ip_address != "192.168.1.10");
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "a status read batched behind a failed probe is answered from cache",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {100, 3, 300, 5000});
    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    REQUIRE(read_status().ssid == "Home");

    backend->set_status_delay_ms(150);
    for (int i = 0; i < 3; ++i) {
        read_status();
    }
    REQUIRE(WiFiManagerTestAccess::breaker_open(*wm));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // The probe is admitted and still slow; a poll arriving during it joins the
    // next batch, which the open breaker must not send.
    const int asked = backend->statuses();
    bool probe_answered = false;
    wm->get_status_async(guard.token(),
                         [&](const WifiBackend::ConnectionStatus&) { probe_answered = true; });
    REQUIRE(wait_until([&] { return backend->statuses() == asked + 1; }, 1000, 1));
    const auto batched = read_status();
    CHECK(batched.unresponsive);
    CHECK(batched.ssid == "Home");
    CHECK(probe_answered);
    CHECK(backend->statuses() == asked + 1);
}

TEST_CASE_METHOD(ControlLaneFixture, "a periodic scan tick sends its trigger down the lane",
                 "[wifi][wifi_control_lane]") {
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(WiFiManagerTestAccess::scan_should_trigger(*wm));

    WiFiManagerTestAccess::fire_scan_timer(*wm);
    REQUIRE(wait_until([&] { return backend->triggers() == 2; }));
    const auto callers = backend->callers();
    REQUIRE_FALSE(callers.empty());
    CHECK(callers.back() != main_thread);
}

TEST_CASE_METHOD(ControlLaneFixture, "a scan whose results cannot be read keeps the list shown",
                 "[wifi][wifi_control_lane]") {
    std::vector<size_t> deliveries;
    wm->start_scan([&](const std::vector<WiFiNetwork>& n) { deliveries.push_back(n.size()); });
    REQUIRE(wait_until([&] { return backend->triggers() == 1; }));
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(wait_until([&] { return deliveries.size() == 1; }));
    const size_t shown = deliveries.front();
    REQUIRE(shown > 0);

    // The next scan completes, but SCAN_RESULTS fails: nothing new was learned.
    backend->fail_results();
    WiFiManagerTestAccess::fire_scan_timer(*wm);
    REQUIRE(wait_until([&] { return backend->triggers() == 2; }));
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(wait_until([&] { return deliveries.size() == 2; }));
    CHECK(deliveries.back() == shown);
}

TEST_CASE_METHOD(ControlLaneFixture,
                 "a disconnect clears the cached connection unresponsive polls answer with",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    backend->set_connected_state(true, "Home", "192.168.1.10", 80);
    REQUIRE(read_status().ssid == "Home");

    WiFiManagerTestAccess::fire_disconnected(*wm);
    stall_lane_on_status_read(50);

    const auto cached = read_status();
    REQUIRE(cached.unresponsive);
    CHECK_FALSE(cached.connected);
    CHECK(cached.ssid.empty());
}

// The wpa_supplicant backend is Linux-only (wifi_backend_wpa_supplicant.h).
#ifndef __APPLE__
TEST_CASE_METHOD(ControlLaneFixture, "the wpa_supplicant fallback starts with the breaker closed",
                 "[wifi][wifi_control_lane][wifi_breaker]") {
    WiFiManagerTestAccess::set_breaker_config(*wm, {50, 3, 400, 2000});
    backend->set_status_delay_ms(80);
    for (int i = 0; i < 3; ++i) {
        read_status();
    }
    REQUIRE(WiFiManagerTestAccess::breaker_open(*wm));
    backend->set_status_delay_ms(0);

    WiFiManagerTestAccess::fire_init_failed(*wm, /*silent=*/true, "init failed");
    REQUIRE(wait_until([&] {
        return dynamic_cast<WifiBackendWpaSupplicant*>(WiFiManagerTestAccess::backend(*wm)) !=
               nullptr;
    }));
    backend = nullptr; // freed by the swap
    CHECK_FALSE(WiFiManagerTestAccess::breaker_open(*wm));
}
#endif

TEST_CASE_METHOD(ControlLaneFixture,
                 "a user op that finishes after its deadline still refreshes the UI",
                 "[wifi][wifi_control_lane]") {
    WiFiManagerTestAccess::set_user_op_timeouts(*wm, 300, 300, 60000);
    int notified = 0;
    WiFiManagerTestAccess::add_observer(*wm, guard.token(), [&] { notified++; });

    // Radio off to start with; turning it on fires no backend event of its
    // own, so only the manager can tell the UI it moved.
    backend->set_radio_enabled(false);
    helix::ui::UpdateQueue::instance().drain();
    const int radios = backend->radios();

    backend->stall();
    std::optional<bool> radio_ok;
    wm->set_enabled_async(true, guard.token(), [&](bool ok, bool) { radio_ok = ok; });
    REQUIRE(wait_until([&] { return backend->radios() == radios + 1; }));
    REQUIRE(wait_until([&] { return radio_ok.has_value(); }, 2000));
    REQUIRE_FALSE(*radio_ok);
    const int before = notified;

    // The radio call returns after the user was told it failed: it did move.
    backend->release();
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->is_radio_enabled());
    CHECK(notified > before);
}
