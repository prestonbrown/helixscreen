// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/bluetooth_loader_test_access.h"
#include "bluetooth_loader.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::bluetooth;

TEST_CASE("BluetoothLoader - no crash when plugin missing", "[bluetooth]") {
    auto& loader = BluetoothLoader::instance();
    // On dev machines without the .so, is_available() should not crash
    REQUIRE_NOTHROW(loader.is_available());
}

TEST_CASE("BluetoothLoader - function pointers null when unavailable", "[bluetooth]") {
    auto& loader = BluetoothLoader::instance();
    if (!loader.is_available()) {
        REQUIRE(loader.init == nullptr);
        REQUIRE(loader.deinit == nullptr);
        REQUIRE(loader.discover == nullptr);
        REQUIRE(loader.connect_rfcomm == nullptr);
        REQUIRE(loader.connect_ble == nullptr);
    }
    // If available, function pointers should be non-null (tested on BT-capable machines)
}

TEST_CASE("bluetooth_enabled - production loads unless HELIX_BLUETOOTH=0", "[bluetooth]") {
    CHECK(bluetooth_enabled(nullptr, false));
    CHECK(bluetooth_enabled("1", false));
    CHECK(bluetooth_enabled("", false));
    CHECK_FALSE(bluetooth_enabled("0", false));
}

TEST_CASE("bluetooth_enabled - a --test run loads only with HELIX_BLUETOOTH=1", "[bluetooth]") {
    CHECK_FALSE(bluetooth_enabled(nullptr, true));
    CHECK_FALSE(bluetooth_enabled("", true));
    CHECK_FALSE(bluetooth_enabled("0", true));
    CHECK(bluetooth_enabled("1", true));
}

namespace {

int g_fake_ctx_storage = 0;
helix_bt_context* const kFakeCtx = reinterpret_cast<helix_bt_context*>(&g_fake_ctx_storage);
std::atomic<int> g_inits{0};

// Slow on purpose: a real init() makes D-Bus round trips, and the window it holds
// open is where a second caller would start its own context.
extern "C" helix_bt_context* slow_counting_init() {
    g_inits.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return kFakeCtx;
}

} // namespace

TEST_CASE("BluetoothLoader creates one shared context when first called from many threads",
          "[bluetooth][threading]") {
    auto& loader = BluetoothLoader::instance();
    BluetoothLoaderTestAccess::FakeLoaded fake(loader, &slow_counting_init);
    g_inits.store(0);

    constexpr int kThreads = 8;
    std::atomic<bool> go{false};
    std::vector<helix_bt_context*> got(kThreads, nullptr);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load())
                std::this_thread::yield();
            got[static_cast<size_t>(i)] = loader.get_or_create_context();
        });
    }
    go.store(true);
    for (auto& t : threads)
        t.join();

    CHECK(g_inits.load() == 1);
    for (auto* ctx : got)
        CHECK(ctx == kFakeCtx);
}
