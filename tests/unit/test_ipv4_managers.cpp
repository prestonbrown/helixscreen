// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "ethernet_manager.h"
#include "ipv4_config_system.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using helix::WiFiManager;
using helix::http::HttpExecutor;
using namespace helix::net;

namespace {

Ipv4Config static_config(const char* address) {
    Ipv4Config c;
    c.dhcp = false;
    c.address = address;
    c.prefix = 24;
    c.gateway = "192.168.1.1";
    c.dns = {"1.1.1.1"};
    return c;
}

/// Points the pending-revert record at a private directory for the test.
struct Ipv4ManagerFixture : LVGLTestFixture {
    // Every EthernetManager here must build the mock: the real backend would
    // rewrite this machine's own network profile.
    ScopedRuntimeConfig scoped_config;
    std::string dir;
    std::optional<std::string> saved_config_dir;
    WifiBackendMock* backend = nullptr;
    std::shared_ptr<WiFiManager> wm;
    helix::AsyncLifetimeGuard guard;

    Ipv4ManagerFixture() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_ethernet = false;
        helix::Config::get_instance();
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir = prev;
        }
        char tmpl[] = "/tmp/helix_ipv4_test_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != nullptr);
        dir = tmpl;
        setenv("HELIX_CONFIG_DIR", dir.c_str(), 1);
        unsetenv("HELIX_MOCK_IPV4_APPLY_FAIL");
        HttpExecutor::fast().start();
    }

    /// Builds the manager over a mock already holding @p initial.
    void start(const Ipv4Config& initial = {}) {
        REQUIRE(mock_ipv4_store(Ipv4Link::Wifi).apply(initial).ok());
        auto b = std::make_unique<WifiBackendMock>();
        backend = b.get();
        wm = std::make_shared<WiFiManager>(std::move(b));
        wm->init_self_reference(wm);
        settle();
    }

    /// Waits for everything queued on the manager's control lane: the lane runs
    /// in order, so a read submitted now answers after it.
    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        bool answered = false;
        wm->get_ipv4_config_async(guard.token(), [&](bool, const Ipv4Config&) { answered = true; });
        REQUIRE(wait_until([&] { return answered; }));
    }

    Ipv4Config backend_config() {
        Ipv4Config c;
        REQUIRE(mock_ipv4_store(Ipv4Link::Wifi).get(c).ok());
        return c;
    }

    bool record_exists() const {
        return std::filesystem::exists(pending_ipv4_revert_path());
    }

    ~Ipv4ManagerFixture() override {
        unsetenv("HELIX_MOCK_IPV4_APPLY_FAIL");
        mock_ipv4_store(Ipv4Link::Ethernet).apply(Ipv4Config{});
        mock_ipv4_store(Ipv4Link::Wifi).apply(Ipv4Config{});
        wm.reset();
        helix::ui::UpdateQueue::instance().drain();
        unsetenv("HELIX_MOCK_IPV4_APPLY_FAIL");
        if (saved_config_dir) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir->c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE_METHOD(Ipv4ManagerFixture, "pending revert record saves, loads and clears",
                 "[ipv4][ipv4_manager]") {
    REQUIRE_FALSE(load_pending_ipv4_revert());
    REQUIRE(clear_pending_ipv4_revert());

    PendingIpv4Revert p;
    p.link = Ipv4Link::Ethernet;
    p.previous = static_config("192.168.1.40");
    REQUIRE(save_pending_ipv4_revert(p));
    REQUIRE(pending_ipv4_revert_path().rfind(dir, 0) == 0);
    const auto back = load_pending_ipv4_revert();
    REQUIRE(back);
    CHECK(back->link == Ipv4Link::Ethernet);
    CHECK(back->previous == p.previous);

    REQUIRE(clear_pending_ipv4_revert());
    CHECK_FALSE(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "WiFiManager applies a change and the record restores it",
                 "[ipv4][ipv4_manager]") {
    const Ipv4Config was = static_config("192.168.1.40");
    start(was);
    REQUIRE(wm->supports_ipv4_config());

    std::optional<bool> applied;
    wm->apply_ipv4_change_async(was, static_config("192.168.1.50"),
                                std::make_shared<std::atomic<bool>>(false), guard.token(),
                                [&](bool ok) { applied = ok; });
    settle();
    REQUIRE(applied == true);
    CHECK(backend_config() == static_config("192.168.1.50"));
    const auto record = load_pending_ipv4_revert();
    REQUIRE(record);
    CHECK(record->snapshot.store == "mock");

    std::optional<Ipv4Config> read;
    wm->get_ipv4_config_async(guard.token(), [&](bool ok, const Ipv4Config& c) {
        REQUIRE(ok);
        read = c;
    });
    settle();
    CHECK(read == static_config("192.168.1.50"));

    SECTION("restoring the record puts the snapshot back and removes it") {
        REQUIRE(restore_pending_ipv4(Ipv4Link::Wifi, /*stale_only=*/false).ok());
        CHECK(backend_config() == was);
        CHECK_FALSE(record_exists());
    }
    SECTION("the startup revert leaves this run's own record alone") {
        REQUIRE(restore_pending_ipv4(Ipv4Link::Wifi, /*stale_only=*/true).ok());
        CHECK(record_exists());
    }
    SECTION("an Ethernet restore does not touch a Wi-Fi record") {
        REQUIRE(restore_pending_ipv4(Ipv4Link::Ethernet, /*stale_only=*/false).ok());
        CHECK(record_exists());
    }
    SECTION("a failed restore keeps the record for the next start") {
        setenv("HELIX_MOCK_IPV4_APPLY_FAIL", "1", 1);
        CHECK_FALSE(restore_pending_ipv4(Ipv4Link::Wifi, /*stale_only=*/false).ok());
        CHECK(record_exists());
    }
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a refused apply answers false and leaves the record",
                 "[ipv4][ipv4_manager]") {
    start(static_config("192.168.1.40"));
    setenv("HELIX_MOCK_IPV4_APPLY_FAIL", "1", 1);
    std::optional<bool> applied;
    wm->apply_ipv4_change_async(static_config("192.168.1.40"), Ipv4Config{},
                                std::make_shared<std::atomic<bool>>(false), guard.token(),
                                [&](bool ok) { applied = ok; });
    settle();
    REQUIRE(applied == false);
    CHECK(backend_config() == static_config("192.168.1.40"));
    CHECK(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "an apply whose screen went away restores itself",
                 "[ipv4][ipv4_manager]") {
    start(static_config("192.168.1.40"));
    std::optional<bool> applied;
    wm->apply_ipv4_change_async(static_config("192.168.1.40"), static_config("192.168.1.50"),
                                std::make_shared<std::atomic<bool>>(true), guard.token(),
                                [&](bool ok) { applied = ok; });
    settle();
    CHECK(backend_config() == static_config("192.168.1.40"));
    CHECK_FALSE(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "EthernetManager reads and applies through the mock",
                 "[ipv4][ipv4_manager]") {
    EthernetManager eth;
    REQUIRE(eth.supports_ipv4_config());

    std::optional<bool> applied;
    eth.apply_ipv4_change_async(Ipv4Config{}, static_config("192.168.1.60"),
                                std::make_shared<std::atomic<bool>>(false), guard.token(),
                                [&](bool ok) { applied = ok; });
    REQUIRE(wait_until([&] { return applied.has_value(); }, 5000));
    REQUIRE(*applied);

    std::optional<Ipv4Config> read;
    eth.get_ipv4_config_async(guard.token(), [&](bool ok, const Ipv4Config& c) {
        REQUIRE(ok);
        read = c;
    });
    REQUIRE(wait_until([&] { return read.has_value(); }, 5000));
    CHECK(*read == static_config("192.168.1.60"));
    CHECK(eth.get_info().ip_address == "192.168.1.60");
    CHECK(load_pending_ipv4_revert()->link == Ipv4Link::Ethernet);
}
