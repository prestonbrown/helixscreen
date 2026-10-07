// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: exercises the shipped ESP32 header unmodified --
//                 firmware/helixscreen-esp32/components/helixnet/transport_lifecycle.h. That
//                 is production firmware code; this gate only scans include/ and src/, so
//                 a firmware/ include reads to it as no include at all.

/**
 * @file test_esp32_transport_lifecycle.cpp
 * @brief The K-Touch WebSocket client's transport ordering, with fake transports.
 *
 * Jobs are held in a queue and run when the test says, which is how a request issued
 * before the worker reaches an earlier one looks on the device.
 */

#include "firmware/helixscreen-esp32/components/helixnet/transport_lifecycle.h"

#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct FakeTransport {
    std::string url;
    bool running = false;
};

class Harness {
  public:
    Harness()
        : lifecycle_({[this](const char*, std::function<void()> job) { jobs_.push_back(job); },
                      [this](const std::string& url) -> FakeTransport* {
                          if (fail_create) {
                              return nullptr;
                          }
                          ++creates;
                          auto* t = new FakeTransport{url};
                          live_.insert(t);
                          return t;
                      },
                      [this](FakeTransport* t) {
                          if (fail_begin) {
                              return false;
                          }
                          t->running = true;
                          return true;
                      },
                      [this](FakeTransport* t) {
                          t->running = false;
                          if (on_stop) {
                              on_stop();
                          }
                      },
                      [this](FakeTransport* t) {
                          live_.erase(t);
                          delete t;
                      },
                      [this] {
                          ++room_checks;
                          return room;
                      },
                      [this] { ++no_room_calls; }, [this] { ++start_failures; }}) {}

    ~Harness() {
        for (auto* t : live_) {
            delete t;
        }
    }

    void run_jobs() {
        while (!jobs_.empty()) {
            auto job = jobs_.front();
            jobs_.pop_front();
            job();
        }
    }

    [[nodiscard]] size_t live() const {
        return live_.size();
    }

    helix::net::TransportLifecycle<FakeTransport*>& lc() {
        return lifecycle_;
    }

    std::function<void()> on_stop;
    bool room = true;
    bool fail_create = false;
    bool fail_begin = false;
    int creates = 0;
    int room_checks = 0;
    int no_room_calls = 0;
    int start_failures = 0;

  private:
    std::deque<std::function<void()>> jobs_;
    std::set<FakeTransport*> live_;
    helix::net::TransportLifecycle<FakeTransport*> lifecycle_;
};

} // namespace

TEST_CASE("Transport: a second connect queued before the first runs leaks nothing",
          "[esp32][transport]") {
    Harness h;
    h.lc().connect("ws://a");
    h.lc().connect("ws://b");
    h.run_jobs();

    REQUIRE(h.lc().current() != nullptr);
    CHECK(h.lc().current()->url == "ws://b");
    CHECK(h.live() == 1);
    // The superseded connect never opened a transport only to have it stopped.
    CHECK(h.creates == 1);
}

TEST_CASE("Transport: Add's hand-back then switch, both queued, leave one transport",
          "[esp32][transport]") {
    Harness h;
    h.lc().connect("ws://test-host"); // the Add form's Test, already running
    h.run_jobs();
    REQUIRE(h.creates == 1);

    h.lc().connect("ws://saved"); // closing the form hands the client back
    h.lc().connect("ws://added"); // the switch to the added printer, before either runs
    h.run_jobs();

    REQUIRE(h.lc().current() != nullptr);
    CHECK(h.lc().current()->url == "ws://added");
    CHECK(h.live() == 1);
    CHECK(h.creates == 2); // the hand-back never opened a transport
}

TEST_CASE("Transport: a disconnect after a queued connect stays disconnected",
          "[esp32][transport]") {
    Harness h;
    h.lc().connect("ws://a");
    h.lc().disconnect();
    h.run_jobs();

    CHECK(h.lc().current() == nullptr);
    CHECK(h.live() == 0);
    CHECK(h.creates == 0);
}

TEST_CASE("Transport: a switch retires the old transport before starting the new one",
          "[esp32][transport]") {
    Harness h;
    h.lc().connect("ws://a");
    h.run_jobs();
    FakeTransport* a = h.lc().current();
    REQUIRE(a != nullptr);

    h.lc().disconnect();
    // Events from A after the disconnect request are stale even before its job runs.
    CHECK_FALSE(h.lc().accepts(a));
    h.lc().connect("ws://b");
    h.run_jobs();

    REQUIRE(h.lc().current() != nullptr);
    CHECK(h.lc().current()->url == "ws://b");
    CHECK(h.lc().accepts(h.lc().current()));
    CHECK(h.live() == 1);
}

TEST_CASE("Transport: the first connect does not ask for room, a connect after a retire does",
          "[esp32][transport]") {
    Harness h;
    h.room = false;
    h.lc().connect("ws://a");
    h.run_jobs();
    CHECK(h.room_checks == 0);
    REQUIRE(h.lc().current() != nullptr);

    h.lc().connect("ws://b");
    h.run_jobs();
    CHECK(h.room_checks == 1);
    CHECK(h.no_room_calls == 1);
    CHECK(h.lc().current() == nullptr);
}

TEST_CASE("Transport: a transport that cannot start is reported for a retry",
          "[esp32][transport]") {
    SECTION("create fails") {
        Harness h;
        h.fail_create = true;
        h.lc().connect("ws://a");
        h.run_jobs();
        CHECK(h.start_failures == 1);
        CHECK(h.lc().current() == nullptr);
    }
    SECTION("begin fails") {
        Harness h;
        h.fail_begin = true;
        h.lc().connect("ws://a");
        h.run_jobs();
        CHECK(h.start_failures == 1);
        CHECK(h.lc().current() == nullptr);
        CHECK(h.live() == 0);
    }
}

TEST_CASE("Transport: a reconnect after a failed start connects again", "[esp32][transport]") {
    Harness h;
    h.fail_begin = true;
    h.lc().connect("ws://a");
    h.run_jobs();
    REQUIRE(h.lc().current() == nullptr);

    h.fail_begin = false;
    h.lc().reconnect();
    h.run_jobs();

    REQUIRE(h.lc().current() != nullptr);
    CHECK(h.lc().current()->url == "ws://a");
}

TEST_CASE("Transport: a disconnect after a failed start stops the retry", "[esp32][transport]") {
    Harness h;
    h.fail_begin = true;
    h.lc().connect("ws://a");
    h.run_jobs();
    REQUIRE(h.start_failures == 1);
    const int creates_before = h.creates;

    h.fail_begin = false;
    h.lc().disconnect();
    h.lc().reconnect();
    h.run_jobs();

    CHECK(h.lc().current() == nullptr);
    CHECK(h.live() == 0);
    CHECK(h.creates == creates_before);
}

TEST_CASE("Transport: a stop of a transport that never connected is flagged while it runs",
          "[esp32][transport]") {
    Harness h;
    h.lc().connect("ws://a");
    h.run_jobs();
    FakeTransport* a = h.lc().current();

    bool flagged_during_stop = false;
    h.on_stop = [&] { flagged_during_stop = h.lc().retiring_unconnected(); };
    h.lc().disconnect();
    h.run_jobs();
    CHECK(flagged_during_stop);
    CHECK_FALSE(h.lc().retiring_unconnected());

    h.lc().connect("ws://b");
    h.run_jobs();
    h.lc().mark_connected(h.lc().current());
    h.lc().disconnect();
    h.run_jobs();
    CHECK_FALSE(flagged_during_stop);
    (void)a;
}

TEST_CASE("Transport: a reconnect of a transport that never connected is flagged while it stops",
          "[esp32][transport]") {
    Harness h;
    h.lc().connect("ws://a");
    h.run_jobs();

    bool flagged_during_stop = false;
    h.on_stop = [&] { flagged_during_stop = h.lc().retiring_unconnected(); };
    h.lc().reconnect();
    h.run_jobs();
    CHECK(flagged_during_stop);
    CHECK_FALSE(h.lc().retiring_unconnected());

    h.lc().mark_connected(h.lc().current());
    h.lc().reconnect();
    h.run_jobs();
    CHECK_FALSE(flagged_during_stop);
}

TEST_CASE("Transport: an in-place reconnect keeps accepting the same transport's events",
          "[esp32][transport]") {
    // A boot whose first connect attempt fails restarts the same transport in place; its
    // events must still be accepted, or replies and disconnects would be dropped as stale.
    Harness h;
    h.lc().connect("ws://a");
    h.run_jobs();
    FakeTransport* a = h.lc().current();
    REQUIRE(a != nullptr);

    h.lc().reconnect();
    CHECK_FALSE(h.lc().accepts(a)); // between the request and its job: stale
    h.run_jobs();

    CHECK(h.lc().current() == a);
    CHECK(h.lc().accepts(a));
    CHECK(h.creates == 1);
}
