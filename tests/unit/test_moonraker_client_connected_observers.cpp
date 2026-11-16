// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_client_connected_observers.cpp
 * @brief Tests for MoonrakerClient::add_connected_observer / remove_connected_observer.
 *
 * Context: PerformanceState::set_source() runs at app init, BEFORE the WS is
 * connected. send_jsonrpc is gated by ready_to_send (see send_gate tests), so
 * MoonrakerPerformanceSource's initial discovery RPC is silently dropped on
 * cold-connect without the connected-observer registry: components that
 * initialise early register a callback that fires once the WS is up.
 */

#include "../../include/moonraker_client.h"
#include "hv/EventLoopThread.h"

#include <atomic>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct UnconnectedClient {
    UnconnectedClient() : loop_(std::make_shared<hv::EventLoopThread>()) {
        loop_->start();
        client_ = std::make_unique<MoonrakerClient>(loop_->loop());
    }
    ~UnconnectedClient() {
        // JOIN the loop thread BEFORE destroying the client. The client was built
        // with an external loop, so is_loop_owner is false and ~WebSocketClient's
        // stop() will NOT join for us — destroying first frees the client's
        // std::function members under a live loop thread (#1146).
        loop_->stop();
        loop_->join();
        client_.reset();
    }

    std::shared_ptr<hv::EventLoopThread> loop_;
    std::unique_ptr<MoonrakerClient> client_;
};

} // namespace

TEST_CASE("add_connected_observer does not fire immediately when disconnected",
          "[moonraker][client][connected_observer][eventloop][slow]") {
    UnconnectedClient u;

    auto fired = std::make_shared<std::atomic<int>>(0);
    u.client_->add_connected_observer("test_handler", [fired]() { fired->fetch_add(1); });

    REQUIRE(fired->load() == 0);
}

TEST_CASE("remove_connected_observer returns true for known handler, false otherwise",
          "[moonraker][client][connected_observer][eventloop][slow]") {
    UnconnectedClient u;

    u.client_->add_connected_observer("test_handler", []() {});
    REQUIRE(u.client_->remove_connected_observer("test_handler") == true);
    REQUIRE(u.client_->remove_connected_observer("test_handler") == false);
    REQUIRE(u.client_->remove_connected_observer("never_registered") == false);
}

TEST_CASE("add_connected_observer replaces previous callback for the same handler name",
          "[moonraker][client][connected_observer][eventloop][slow]") {
    UnconnectedClient u;

    auto first_fired = std::make_shared<std::atomic<int>>(0);
    auto second_fired = std::make_shared<std::atomic<int>>(0);

    u.client_->add_connected_observer("test_handler",
                                      [first_fired]() { first_fired->fetch_add(1); });
    u.client_->add_connected_observer("test_handler",
                                      [second_fired]() { second_fired->fetch_add(1); });

    // Map upsert — only one entry remains. We can't directly verify which one
    // fires without driving the WS to CONNECTED, but the contract is that the
    // second add replaces the first.
    REQUIRE(u.client_->remove_connected_observer("test_handler") == true);
    REQUIRE(u.client_->remove_connected_observer("test_handler") == false);
}
