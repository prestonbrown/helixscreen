// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/async_lifetime_guard.h"
#include "../../include/http_executor.h"
#include "../../include/ui_update_queue.h"
#include "../../include/wifi_backend.h"
#include "../../include/wifi_backend_mock.h"
#include "../../include/wifi_manager.h"
#include "../../include/wifi_radio_toggle.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Backends that make the blocking window observable
// ============================================================================

namespace {

/// Mock backend whose set_radio_enabled() parks until the test releases it,
/// standing in for wpa_ctrl's 5s send retry + 10s reply deadline (twice per
/// direction). Lets a test prove the call site returned while the radio work
/// was still in flight.
class LatchedRadioBackend : public WifiBackendMock {
  public:
    WiFiError set_radio_enabled(bool on) override {
        entered_.store(true);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return released_; });
        }
        if (fail_) {
            // A failure that leaves the radio where it was — the shape of an
            // rfkill write denied by permissions.
            return WiFiError(WiFiResult::RF_KILL_BLOCKED, "latched failure",
                             "Could not turn WiFi radio off");
        }
        return WifiBackendMock::set_radio_enabled(on);
    }

    bool entered() const {
        return entered_.load();
    }

    void fail_next() {
        fail_ = true;
    }

    void release() {
        // Notify under the lock: the parked set_radio_enabled() returns as soon
        // as released_ flips, and this backend is destroyed with the manager, so
        // notifying after the unlock races cv_'s destructor.
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

  private:
    std::atomic<bool> entered_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;
    bool fail_ = false;
};

/// Spins the UpdateQueue on the calling (main) thread until `pred` holds.
/// set_enabled_async delivers through LifetimeToken::defer(), which lands in
/// the queue — see tests/CLAUDE.md "Deferred work needs an explicit drain".
bool drain_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        helix::ui::UpdateQueue::instance().drain();
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    helix::ui::UpdateQueue::instance().drain();
    return pred();
}

/// Constructing a WiFiManager over the mock backend fires READY synchronously,
/// which queues WiFiManager::reassert_stored_radio_state through the
/// UpdateQueue. Run it now, while the radio is still in its default state, so
/// it cannot land in the middle of a later assertion and re-drive the radio.
void settle_construction() {
    helix::ui::UpdateQueue::instance().drain();
}

bool spin_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

} // namespace

// ============================================================================
// WiFiManager::set_enabled_async — the toggle must not block the LVGL thread
// ============================================================================

TEST_CASE("set_enabled_async returns before the backend radio call completes",
          "[wifi][manager][radio][async]") {
    helix::http::HttpExecutor::fast().start();

    auto backend = std::make_unique<LatchedRadioBackend>();
    LatchedRadioBackend* raw = backend.get();
    WiFiManager manager(std::move(backend));
    settle_construction();

    helix::AsyncLifetimeGuard lifetime;
    std::atomic<bool> completed{false};

    auto call_start = std::chrono::steady_clock::now();
    manager.set_enabled_async(false, lifetime.token(),
                              [&completed](bool, bool) { completed.store(true); });
    auto call_elapsed = std::chrono::steady_clock::now() - call_start;

    // The dispatch itself must be effectively instant. The synchronous
    // set_enabled() this replaced sat inside the backend for the whole window.
    CHECK(call_elapsed < std::chrono::milliseconds(250));

    // The worker really is parked inside the backend...
    REQUIRE(spin_until([raw] { return raw->entered(); }));
    // ...and the caller has already been handed control back, with no result.
    CHECK_FALSE(completed.load());

    raw->release();
    REQUIRE(drain_until([&completed] { return completed.load(); }));

    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("set_enabled_async reports the radio state the backend actually reached",
          "[wifi][manager][radio][async]") {
    helix::http::HttpExecutor::fast().start();

    auto backend = std::make_unique<LatchedRadioBackend>();
    LatchedRadioBackend* raw = backend.get();
    raw->release(); // no need to observe the blocking window here
    WiFiManager manager(std::move(backend));
    settle_construction();

    helix::AsyncLifetimeGuard lifetime;
    std::atomic<bool> done{false};
    bool cb_success = true;
    bool cb_actual = true;

    manager.set_enabled_async(false, lifetime.token(), [&](bool success, bool actual) {
        cb_success = success;
        cb_actual = actual;
        done.store(true);
    });

    REQUIRE(drain_until([&done] { return done.load(); }));
    CHECK(cb_success);
    CHECK_FALSE(cb_actual);
    CHECK_FALSE(raw->is_radio_enabled());
    // The backend must stay up so the radio can be switched back on.
    CHECK(raw->is_running());

    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("set_enabled_async reports failure and the unchanged radio state",
          "[wifi][manager][radio][async]") {
    helix::http::HttpExecutor::fast().start();

    auto backend = std::make_unique<LatchedRadioBackend>();
    LatchedRadioBackend* raw = backend.get();
    raw->fail_next();
    raw->release();
    WiFiManager manager(std::move(backend));
    settle_construction();

    helix::AsyncLifetimeGuard lifetime;
    std::atomic<bool> done{false};
    bool cb_success = true;
    bool cb_actual = false;

    manager.set_enabled_async(false, lifetime.token(), [&](bool success, bool actual) {
        cb_success = success;
        cb_actual = actual;
        done.store(true);
    });

    REQUIRE(drain_until([&done] { return done.load(); }));
    // The radio never went off, and the caller is told both halves: the
    // request failed AND the radio is still on, which is what drives the
    // optimistic switch back.
    CHECK_FALSE(cb_success);
    CHECK(cb_actual);
    CHECK(raw->is_radio_enabled());

    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("set_enabled_async drops its result when the caller's token expired",
          "[wifi][manager][radio][async]") {
    helix::http::HttpExecutor::fast().start();

    auto backend = std::make_unique<LatchedRadioBackend>();
    LatchedRadioBackend* raw = backend.get();
    WiFiManager manager(std::move(backend));
    settle_construction();

    std::atomic<int> fires{0};
    {
        helix::AsyncLifetimeGuard lifetime;
        manager.set_enabled_async(false, lifetime.token(),
                                  [&fires](bool, bool) { fires.fetch_add(1); });
        REQUIRE(spin_until([raw] { return raw->entered(); }));
        // Owner dies while the radio call is still in flight.
    }
    raw->release();

    // Give the worker time to finish and the queue time to run whatever it
    // posted; the callback must never fire against the dead owner.
    drain_until([] { return false; }, 400);
    CHECK(fires.load() == 0);

    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("~WiFiManager waits for an in-flight radio op instead of freeing the backend under it",
          "[wifi][manager][radio][async]") {
    helix::http::HttpExecutor::fast().start();

    auto backend = std::make_unique<LatchedRadioBackend>();
    LatchedRadioBackend* raw = backend.get();
    auto manager = std::make_unique<WiFiManager>(std::move(backend));
    settle_construction();

    helix::AsyncLifetimeGuard lifetime;
    manager->set_enabled_async(false, lifetime.token(), [](bool, bool) {});
    REQUIRE(spin_until([raw] { return raw->entered(); }));

    // Release from another thread a beat after the destructor starts, so the
    // destructor genuinely has to block. If it did not, the worker would be
    // inside a freed backend — ASAN catches it; without ASAN the timing check
    // below still proves the wait happened.
    std::thread releaser([raw] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        raw->release();
    });

    auto start = std::chrono::steady_clock::now();
    manager.reset();
    auto elapsed = std::chrono::steady_clock::now() - start;
    releaser.join();

    CHECK(elapsed >= std::chrono::milliseconds(100));

    helix::ui::UpdateQueue::instance().drain();
}

// ============================================================================
// Reconcile rule — how the optimistic flip folds reality back in
// ============================================================================

TEST_CASE("reconcile_radio_toggle keeps an optimistic flip that reality agreed with",
          "[wifi][radio][reconcile]") {
    auto off = helix::wifi::reconcile_radio_toggle(/*requested=*/false, /*success=*/true,
                                                   /*actual=*/false);
    CHECK_FALSE(off.enabled);
    CHECK_FALSE(off.reverted);
    CHECK_FALSE(off.silent_revert);

    auto on = helix::wifi::reconcile_radio_toggle(true, true, true);
    CHECK(on.enabled);
    CHECK_FALSE(on.reverted);
    CHECK_FALSE(on.silent_revert);
}

TEST_CASE("reconcile_radio_toggle reverts the optimistic flip when the radio refused",
          "[wifi][radio][reconcile]") {
    // User asked for off, the backend failed, the radio is still on.
    auto failed_off = helix::wifi::reconcile_radio_toggle(false, false, true);
    CHECK(failed_off.enabled);
    CHECK(failed_off.reverted);
    // WiFiManager already surfaced the backend error, so the caller must not
    // double-report.
    CHECK_FALSE(failed_off.silent_revert);

    // User asked for on, the backend failed, the radio is still off.
    auto failed_on = helix::wifi::reconcile_radio_toggle(true, false, false);
    CHECK_FALSE(failed_on.enabled);
    CHECK(failed_on.reverted);
    CHECK_FALSE(failed_on.silent_revert);
}

TEST_CASE("reconcile_radio_toggle flags a revert the backend never reported",
          "[wifi][radio][reconcile]") {
    // The NetworkManager/macOS shape: set_radio_enabled() claimed success but
    // the radio reads back the other way. Nothing has told the user anything,
    // so the caller has to.
    auto lying_off = helix::wifi::reconcile_radio_toggle(false, true, true);
    CHECK(lying_off.enabled);
    CHECK(lying_off.reverted);
    CHECK(lying_off.silent_revert);

    auto lying_on = helix::wifi::reconcile_radio_toggle(true, true, false);
    CHECK_FALSE(lying_on.enabled);
    CHECK(lying_on.reverted);
    CHECK(lying_on.silent_revert);
}

TEST_CASE("reconcile_radio_toggle never persists the request over reality",
          "[wifi][radio][reconcile]") {
    // Exhaustive: `enabled` tracks `actual` in every combination, because a
    // persisted "off" that never happened feeds the startup reassert a lie.
    for (bool requested : {false, true}) {
        for (bool success : {false, true}) {
            for (bool actual : {false, true}) {
                auto out = helix::wifi::reconcile_radio_toggle(requested, success, actual);
                CHECK(out.enabled == actual);
            }
        }
    }
}

// ============================================================================
// nmcli radio-state parsing (drives WifiBackendNetworkManager::is_radio_enabled)
// ============================================================================

TEST_CASE("wifi_parse_nm_radio_state reads nmcli's enabled/disabled answer", "[wifi][nm][radio]") {
    CHECK(wifi_parse_nm_radio_state("enabled\n") == std::optional<bool>(true));
    CHECK(wifi_parse_nm_radio_state("disabled\n") == std::optional<bool>(false));
    // Non-terse nmcli pads the value.
    CHECK(wifi_parse_nm_radio_state("  enabled  \n") == std::optional<bool>(true));
    CHECK(wifi_parse_nm_radio_state("DISABLED") == std::optional<bool>(false));
}

TEST_CASE("wifi_parse_nm_radio_state refuses to guess at an unparseable answer",
          "[wifi][nm][radio]") {
    // An absent/broken nmcli must read as "unknown", never as "enabled" —
    // guessing "enabled" is exactly what made the switch snap back on.
    CHECK_FALSE(wifi_parse_nm_radio_state("").has_value());
    CHECK_FALSE(wifi_parse_nm_radio_state("   \n\t ").has_value());
    CHECK_FALSE(wifi_parse_nm_radio_state("missing").has_value());
    CHECK_FALSE(wifi_parse_nm_radio_state("Error: NetworkManager is not running.").has_value());
}
