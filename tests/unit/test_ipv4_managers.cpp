// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ipv4_revert_retry.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "core/http_executor.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "ethernet_manager.h"
#include "ipv4_config_system.h"
#include "misc/lv_timer_private.h" // timer_cb: each tick is fired by hand
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
    p.snapshot = mock_ipv4_snapshot(Ipv4Link::Ethernet);
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

    std::optional<Ipv4Result::Status> applied;
    wm->apply_ipv4_change_async(was, static_config("192.168.1.50"),
                                std::make_shared<std::atomic<bool>>(false), guard.token(),
                                [&](Ipv4Result::Status st) { applied = st; });
    settle();
    REQUIRE(applied == Ipv4Result::Status::Ok);
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
        REQUIRE(restore_pending_ipv4(Ipv4Link::Wifi, /*include_own=*/true).kind ==
                Ipv4RestoreOutcome::Kind::Restored);
        CHECK(backend_config() == was);
        CHECK_FALSE(record_exists());
    }
    SECTION("the startup revert leaves this run's own record alone") {
        REQUIRE(restore_pending_ipv4(Ipv4Link::Wifi, /*include_own=*/false).kind ==
                Ipv4RestoreOutcome::Kind::Skipped);
        CHECK(record_exists());
    }
    SECTION("an Ethernet restore does not touch a Wi-Fi record") {
        REQUIRE(restore_pending_ipv4(Ipv4Link::Ethernet, true).kind ==
                Ipv4RestoreOutcome::Kind::Skipped);
        CHECK(record_exists());
    }
    SECTION("a failed restore keeps the record and says Retry") {
        setenv("HELIX_MOCK_IPV4_APPLY_FAIL", "1", 1);
        CHECK(restore_pending_ipv4(Ipv4Link::Wifi, true).kind == Ipv4RestoreOutcome::Kind::Retry);
        CHECK(record_exists());
    }
    SECTION("a second change cannot replace the way back, until it is given up") {
        std::optional<Ipv4Result::Status> again;
        wm->apply_ipv4_change_async(static_config("192.168.1.50"), static_config("192.168.1.60"),
                                    std::make_shared<std::atomic<bool>>(false), guard.token(),
                                    [&](Ipv4Result::Status st) { again = st; });
        settle();
        CHECK(again == Ipv4Result::Status::Refused);
        CHECK(backend_config() == static_config("192.168.1.50"));
        CHECK(load_pending_ipv4_revert()->previous == was);

        REQUIRE(mark_pending_ipv4_failed(Ipv4Link::Wifi, "edited by hand"));
        CHECK(restore_pending_ipv4(Ipv4Link::Wifi, true).kind == Ipv4RestoreOutcome::Kind::Skipped);
        wm->apply_ipv4_change_async(static_config("192.168.1.50"), static_config("192.168.1.60"),
                                    std::make_shared<std::atomic<bool>>(false), guard.token(),
                                    [&](Ipv4Result::Status st) { again = st; });
        settle();
        CHECK(again == Ipv4Result::Status::Ok);
    }
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a failed apply leaves the record for the revert",
                 "[ipv4][ipv4_manager]") {
    start(static_config("192.168.1.40"));
    setenv("HELIX_MOCK_IPV4_APPLY_FAIL", "1", 1);
    std::optional<Ipv4Result::Status> applied;
    wm->apply_ipv4_change_async(static_config("192.168.1.40"), Ipv4Config{},
                                std::make_shared<std::atomic<bool>>(false), guard.token(),
                                [&](Ipv4Result::Status st) { applied = st; });
    settle();
    REQUIRE(applied == Ipv4Result::Status::Failed);
    CHECK(backend_config() == static_config("192.168.1.40"));
    CHECK(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "an apply whose screen went away first never starts",
                 "[ipv4][ipv4_manager]") {
    start(static_config("192.168.1.40"));
    std::optional<Ipv4Result::Status> applied;
    wm->apply_ipv4_change_async(static_config("192.168.1.40"), static_config("192.168.1.50"),
                                std::make_shared<std::atomic<bool>>(true), guard.token(),
                                [&](Ipv4Result::Status st) { applied = st; });
    settle();
    CHECK(applied == Ipv4Result::Status::Refused);
    CHECK(backend_config() == static_config("192.168.1.40"));
    CHECK_FALSE(record_exists());
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a record that cannot be used is a failure, not nothing",
                 "[ipv4][ipv4_manager]") {
    REQUIRE(helix::text_io::write_file_atomic(pending_ipv4_revert_path(), "{\"link\":\"wifi\""));
    CHECK(restore_pending_ipv4(Ipv4Link::Wifi, true).kind == Ipv4RestoreOutcome::Kind::Terminal);

    SECTION("its bytes are kept beside the failed stub") {
        REQUIRE(mark_pending_ipv4_failed(Ipv4Link::Wifi, "undecodable"));
        CHECK(std::filesystem::exists(pending_ipv4_revert_path() + ".unreadable"));
    }
    SECTION("a dhcpcd record left by another build is given up, not restored") {
        REQUIRE(helix::text_io::write_file_atomic(
            pending_ipv4_revert_path(),
            R"({"link":"wifi","store":"dhcpcd","iface":"wlan0","original_b64":"aG9zdG5hbWUK"})"));
        CHECK(restore_pending_ipv4(Ipv4Link::Wifi, true).kind ==
              Ipv4RestoreOutcome::Kind::Terminal);
        CHECK(record_exists());
    }
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a given-up revert is announced once its record says so",
                 "[ipv4][ipv4_manager]") {
    PendingIpv4Revert p;
    p.link = Ipv4Link::Ethernet;
    p.snapshot = mock_ipv4_snapshot(Ipv4Link::Ethernet);
    REQUIRE(save_pending_ipv4_revert(p));
    std::optional<bool> failed_when_told;
    helix::settings::set_ipv4_revert_listener(
        [&](Ipv4Link link, helix::settings::Ipv4RevertEnd end) {
            CHECK(link == Ipv4Link::Ethernet);
            CHECK(end == helix::settings::Ipv4RevertEnd::GaveUp);
            const auto record = load_pending_ipv4_revert();
            failed_when_told = record && record->failed;
        });
    helix::settings::give_up_ipv4_revert(Ipv4Link::Ethernet, "unreachable");
    CHECK(wait_until([&] { return failed_when_told.has_value(); }));
    helix::settings::set_ipv4_revert_listener(nullptr);
    CHECK(failed_when_told.value_or(false));
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a record that fails to read is retried, never replaced",
                 "[ipv4][ipv4_manager]") {
    // A directory exists, opens, and fails every read.
    REQUIRE(std::filesystem::create_directory(pending_ipv4_revert_path()));
    CHECK(read_pending_ipv4_revert().kind == Ipv4RecordLoad::Kind::ReadError);
    CHECK(restore_pending_ipv4(Ipv4Link::Wifi, true).kind == Ipv4RestoreOutcome::Kind::Retry);
    CHECK_FALSE(mark_pending_ipv4_failed(Ipv4Link::Wifi, "read error"));
    CHECK(std::filesystem::is_directory(pending_ipv4_revert_path()));
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a give-up its record cannot hold says so and keeps refusing",
                 "[ipv4][ipv4_manager]") {
    using helix::settings::Ipv4RevertEnd;
    REQUIRE(std::filesystem::create_directory(pending_ipv4_revert_path()));
    std::optional<Ipv4RevertEnd> told;
    helix::settings::set_ipv4_revert_listener([&](Ipv4Link, Ipv4RevertEnd end) { told = end; });
    helix::settings::give_up_ipv4_revert(Ipv4Link::Wifi, "read error");
    REQUIRE(wait_until([&] { return told.has_value(); }));
    CHECK(told == Ipv4RevertEnd::GaveUpUnrecorded);
    CHECK(helix::settings::ipv4_revert_unrecorded(Ipv4Link::Wifi) == "read error");
    CHECK(std::filesystem::is_directory(pending_ipv4_revert_path()));

    // Once a give-up is recorded, the screen takes a change again.
    std::filesystem::remove(pending_ipv4_revert_path());
    PendingIpv4Revert p;
    p.snapshot = mock_ipv4_snapshot(Ipv4Link::Wifi);
    REQUIRE(save_pending_ipv4_revert(p));
    told.reset();
    helix::settings::give_up_ipv4_revert(Ipv4Link::Wifi, "unreachable");
    REQUIRE(wait_until([&] { return told.has_value(); }));
    helix::settings::set_ipv4_revert_listener(nullptr);
    CHECK(told == Ipv4RevertEnd::GaveUp);
    CHECK_FALSE(helix::settings::ipv4_revert_unrecorded(Ipv4Link::Wifi));
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "a startup record that fails to read is read again",
                 "[ipv4][ipv4_manager]") {
    // The test binary runs no periodic timer, so each tick is fired by hand.
    const auto fire = [] {
        lv_timer_t* t = helix::settings::ipv4_startup_read_timer_for_test();
        REQUIRE(t);
        t->timer_cb(t);
    };
    const Ipv4Config was = static_config("192.168.1.40");
    REQUIRE(std::filesystem::create_directory(pending_ipv4_revert_path()));
    helix::settings::start_ipv4_startup_revert();
    fire();

    SECTION("once it reads, another run's change is undone") {
        REQUIRE(mock_ipv4_store(Ipv4Link::Wifi).apply(was).ok());
        PendingIpv4Revert p;
        p.previous = was;
        p.owner = "another-run";
        p.snapshot = mock_ipv4_snapshot(Ipv4Link::Wifi);
        REQUIRE(mock_ipv4_store(Ipv4Link::Wifi).apply(static_config("192.168.1.50")).ok());
        std::filesystem::remove(pending_ipv4_revert_path());
        REQUIRE(save_pending_ipv4_revert(p));
        process_lvgl(static_cast<int>(kIpv4RetryEveryMs));
        fire();
        CHECK(helix::settings::ipv4_startup_read_timer_for_test() == nullptr);
        CHECK(wait_until([&] { return !record_exists(); }));
        CHECK(backend_config() == was);
    }
    SECTION("one that never reads stops in two minutes and is left as it is") {
        process_lvgl(static_cast<int>(kIpv4RetryForMs));
        fire();
        CHECK(helix::settings::ipv4_startup_read_timer_for_test() == nullptr);
        CHECK(std::filesystem::is_directory(pending_ipv4_revert_path()));
    }
}

TEST_CASE_METHOD(Ipv4ManagerFixture, "EthernetManager reads and applies through the mock",
                 "[ipv4][ipv4_manager]") {
    EthernetManager eth;
    REQUIRE(eth.supports_ipv4_config());

    std::optional<Ipv4Result::Status> applied;
    eth.apply_ipv4_change_async(Ipv4Config{}, static_config("192.168.1.60"),
                                std::make_shared<std::atomic<bool>>(false), guard.token(),
                                [&](Ipv4Result::Status st) { applied = st; });
    REQUIRE(wait_until([&] { return applied.has_value(); }, 5000));
    REQUIRE(*applied == Ipv4Result::Status::Ok);

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
