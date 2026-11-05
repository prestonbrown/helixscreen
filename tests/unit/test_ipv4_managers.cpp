// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "ethernet_backend_mock.h"
#include "ethernet_manager.h"
#include "ipv4_config.h"
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

    /// Builds the manager over a mock already holding @p initial, with applies
    /// failing from then on when @p fail_applies. The mock fires READY inside
    /// the constructor, so its startup revert is queued by the time this drains.
    void start(const Ipv4Config& initial = {}, bool fail_applies = false) {
        auto b = std::make_unique<WifiBackendMock>();
        REQUIRE(b->apply_ipv4_config(initial).ok());
        if (fail_applies) {
            setenv("HELIX_MOCK_IPV4_APPLY_FAIL", "1", 1);
        }
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
        REQUIRE(backend->get_ipv4_config(c).ok());
        return c;
    }

    bool record_exists() const {
        return std::filesystem::exists(pending_ipv4_revert_path());
    }

    ~Ipv4ManagerFixture() override {
        unsetenv("HELIX_MOCK_IPV4_APPLY_FAIL");
        EthernetBackendMock().apply_ipv4_config(Ipv4Config{});
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

TEST_CASE_METHOD(Ipv4ManagerFixture, "WiFiManager reads and applies the backend's IPv4 config",
                 "[ipv4][ipv4_manager]") {
    start();
    REQUIRE(wm->supports_ipv4_config());

    std::optional<bool> applied;
    wm->apply_ipv4_config_async(static_config("192.168.1.50"), guard.token(),
                                [&](bool ok) { applied = ok; });
    settle();
    REQUIRE(applied == true);
    CHECK(backend_config() == static_config("192.168.1.50"));

    std::optional<Ipv4Config> read;
    wm->get_ipv4_config_async(guard.token(), [&](bool ok, const Ipv4Config& c) {
        REQUIRE(ok);
        read = c;
    });
    settle();
    REQUIRE(read);
    CHECK(*read == static_config("192.168.1.50"));

    SECTION("a refused apply answers false and changes nothing") {
        setenv("HELIX_MOCK_IPV4_APPLY_FAIL", "1", 1);
        std::optional<bool> again;
        wm->apply_ipv4_config_async(Ipv4Config{}, guard.token(), [&](bool ok) { again = ok; });
        settle();
        REQUIRE(again == false);
        CHECK(backend_config() == static_config("192.168.1.50"));
    }
}

TEST_CASE_METHOD(Ipv4ManagerFixture,
                 "a Wi-Fi change never kept is reverted when the backend is ready",
                 "[ipv4][ipv4_manager]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Wifi;
    p.previous.dns = {"9.9.9.9"}; // DHCP with a DNS override
    REQUIRE(save_pending_ipv4_revert(p));

    start(static_config("192.168.1.50"));
    CHECK(backend_config() == p.previous);
    CHECK_FALSE(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a revert that fails keeps the record for the next start",
                 "[ipv4][ipv4_manager]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Wifi;
    REQUIRE(save_pending_ipv4_revert(p));
    start(static_config("192.168.1.50"), /*fail_applies=*/true);
    CHECK(backend_config() == static_config("192.168.1.50"));
    CHECK(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a record this process wrote belongs to its countdown",
                 "[ipv4][ipv4_manager]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Wifi;
    p.owner = ipv4_process_owner();
    REQUIRE(save_pending_ipv4_revert(p));

    start(static_config("192.168.1.50"));
    CHECK(backend_config() == static_config("192.168.1.50"));
    CHECK(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "WiFiManager leaves an Ethernet record alone",
                 "[ipv4][ipv4_manager]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Ethernet;
    REQUIRE(save_pending_ipv4_revert(p));

    start(static_config("192.168.1.50"));
    CHECK(backend_config() == static_config("192.168.1.50"));
    CHECK(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "an Ethernet change never kept is reverted at startup",
                 "[ipv4][ipv4_manager]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Ethernet;
    p.previous = static_config("192.168.1.40");
    REQUIRE(save_pending_ipv4_revert(p));

    EthernetManager::revert_pending_ipv4_at_startup();
    REQUIRE(wait_until([&] { return !record_exists(); }, 5000));
    Ipv4Config restored;
    REQUIRE(EthernetBackendMock().get_ipv4_config(restored).ok());
    CHECK(restored == p.previous);

    SECTION("a Wi-Fi record is not Ethernet's to revert") {
        PendingIpv4Revert w;
        w.link = Ipv4Link::Wifi;
        REQUIRE(save_pending_ipv4_revert(w));
        EthernetManager::revert_pending_ipv4_at_startup();
        process_lvgl(50);
        CHECK(record_exists());
    }
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "EthernetManager reads and applies through the mock",
                 "[ipv4][ipv4_manager]") {
    EthernetManager eth;
    REQUIRE(eth.supports_ipv4_config());

    std::optional<bool> applied;
    eth.apply_ipv4_config_async(static_config("192.168.1.60"), guard.token(),
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

    SECTION("every manager sees the one mock port") {
        EthernetManager other;
        CHECK(other.get_info().ip_address == "192.168.1.60");
    }
}
