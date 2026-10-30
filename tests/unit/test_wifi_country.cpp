// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// WiFi regulatory country: the ISO table and parsers, the startup policy that
// applies a saved country without overriding one the system set, and the
// rescan a regulatory domain change triggers.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "http_executor.h"
#include "system_settings_manager.h"
#include "wifi_backend_mock.h"
#include "wifi_country.h"
#include "wifi_manager.h"

#include <atomic>
#include <chrono>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using helix::Config;
using helix::SystemSettingsManager;
using helix::WiFiManager;
using helix::WiFiManagerTestAccess;
using helix::http::HttpExecutor;
namespace country = helix::wifi::country;

// ============================================================================
// Pure: table, parsers, policy
// ============================================================================

TEST_CASE("country table is ISO 3166 alpha-2, unique and round-trips the dropdown",
          "[wifi][wifi_country]") {
    REQUIRE(country::table_size() == 249);
    std::set<std::string> codes;
    std::set<int> rows;
    for (size_t i = 0; i < country::table_size(); ++i) {
        const std::string code = country::table_entry(i).code;
        CHECK(code.size() == 2);
        CHECK(codes.insert(code).second);
        // Every country has exactly one row, after the not-set row at 0.
        const int row = country::dropdown_index(code);
        CHECK(row >= 1);
        CHECK(row <= static_cast<int>(country::table_size()));
        CHECK(rows.insert(row).second);
        CHECK(country::code_at_dropdown_index(row) == code);
    }
    CHECK(country::dropdown_index("") == 0);
    CHECK(country::dropdown_index("00") == 0);
    CHECK(country::code_at_dropdown_index(0).empty());
    CHECK(country::code_at_dropdown_index(-1).empty());
    CHECK(country::code_at_dropdown_index(static_cast<int>(country::table_size()) + 1).empty());

    const std::string options = country::dropdown_options("Not set");
    CHECK(options.rfind("Not set\n", 0) == 0);
    CHECK(options.find("\nUnited States (US)") != std::string::npos);
}

TEST_CASE("country dropdown lists the common countries first, once each", "[wifi][wifi_country]") {
    const char* const pinned[] = {"US", "GB", "DE", "CA", "AU", "FR"};
    REQUIRE(country::pinned_count() == std::size(pinned));
    for (size_t i = 0; i < std::size(pinned); ++i) {
        CAPTURE(pinned[i]);
        CHECK(country::dropdown_index(pinned[i]) == static_cast<int>(i) + 1);
        CHECK(country::code_at_dropdown_index(static_cast<int>(i) + 1) == pinned[i]);
    }
    CHECK(country::dropdown_index("us") == 1);

    // Then the rest alphabetically: Afghanistan leads, and the pinned ones are
    // not listed a second time.
    const int first_rest = static_cast<int>(country::pinned_count()) + 1;
    CHECK(country::code_at_dropdown_index(first_rest) == "AF");
    CHECK(country::dropdown_index("AT") < country::dropdown_index("BE"));
    CHECK(country::code_at_dropdown_index(static_cast<int>(country::table_size())) == "ZW");

    const std::string options = country::dropdown_options("Nicht festgelegt");
    CHECK(options.rfind("Nicht festgelegt\nUnited States (US)\nUnited Kingdom (GB)\n"
                        "Germany (DE)\nCanada (CA)\nAustralia (AU)\nFrance (FR)\n"
                        "Afghanistan (AF)\n",
                        0) == 0);
    size_t lines = 0;
    for (const char c : options) {
        lines += c == '\n';
    }
    CHECK(lines == country::table_size());
    CHECK(options.find("(US)") == options.rfind("(US)"));
}

TEST_CASE("country normalize accepts table codes in any case and nothing else",
          "[wifi][wifi_country]") {
    CHECK(country::normalize("us") == "US");
    CHECK(country::normalize("De") == "DE");
    CHECK(country::normalize("00").empty());
    CHECK(country::normalize("ZZ").empty());
    CHECK(country::normalize("USA").empty());
    CHECK(country::normalize("U").empty());
    CHECK(country::is_world(""));
    CHECK(country::is_world("00"));
    CHECK_FALSE(country::is_world("US"));
}

TEST_CASE("country parsers read wpa GET, iw reg get and REGDOM-CHANGE", "[wifi][wifi_country]") {
    CHECK(country::parse_wpa_get_country("US") == "US");
    CHECK(country::parse_wpa_get_country("de\n") == "DE");
    CHECK(country::parse_wpa_get_country("FAIL\n").empty());
    CHECK(country::parse_wpa_get_country("").empty());

    // The global section comes first; a self-managed phy's own line follows it.
    const std::string iw = "global\n"
                           "country US: DFS-FCC\n"
                           "\t(2400 - 2472 @ 40), (N/A, 30), (N/A)\n"
                           "\n"
                           "phy#0 (self-managed)\n"
                           "country DE: DFS-ETSI\n";
    CHECK(country::parse_iw_reg_get(iw) == "US");
    CHECK(country::parse_iw_reg_get("global\ncountry 00: DFS-UNSET\n").empty());
    CHECK(country::parse_iw_reg_get("").empty());

    CHECK(country::parse_regdom_change_alpha2(
              "<3>CTRL-EVENT-REGDOM-CHANGE init=USER type=COUNTRY alpha2=US") == "US");
    CHECK(country::parse_regdom_change_alpha2("<3>CTRL-EVENT-REGDOM-CHANGE init=CORE type=WORLD")
              .empty());
}

TEST_CASE("country plan applies a saved choice and never overrides the system's own",
          "[wifi][wifi_country]") {
    SECTION("saved, radio disagrees: apply it") {
        const auto p = country::plan("DE", "", "");
        CHECK(p.apply == "DE");
        CHECK(p.display == "DE");
    }
    SECTION("saved, radio already agrees: nothing to send") {
        const auto p = country::plan("de", "DE", "");
        CHECK(p.apply.empty());
        CHECK(p.display == "DE");
    }
    SECTION("saved beats remembered") {
        const auto p = country::plan("DE", "00", "US");
        CHECK(p.apply == "DE");
        CHECK(p.display == "DE");
    }
    SECTION("nothing saved, system set a country: show it, leave it") {
        const auto p = country::plan("", "GB", "");
        CHECK(p.apply.empty());
        CHECK(p.display == "GB");
    }
    SECTION("nothing saved, system set a country other than the remembered one: leave it") {
        const auto p = country::plan("", "GB", "US");
        CHECK(p.apply.empty());
        CHECK(p.display == "GB");
    }
    SECTION("nothing saved, world, a country remembered: send it back") {
        const auto p = country::plan("", "00", "us");
        CHECK(p.apply == "US");
        CHECK(p.display.empty()); // still world until the radio takes it
    }
    SECTION("nothing saved, world, nothing remembered: not set") {
        const auto p = country::plan("", "00", "");
        CHECK(p.apply.empty());
        CHECK(p.display.empty());
    }
    SECTION("nothing saved, world, junk remembered: not set") {
        const auto p = country::plan("", "", "XX");
        CHECK(p.apply.empty());
        CHECK(p.display.empty());
    }
}

TEST_CASE("country 5 GHz hint shows only for a dual-band radio in the world domain",
          "[wifi][wifi_country]") {
    CHECK(country::should_hint_5ghz(true, ""));
    CHECK(country::should_hint_5ghz(true, "00"));
    CHECK_FALSE(country::should_hint_5ghz(true, "US"));
    CHECK_FALSE(country::should_hint_5ghz(false, ""));
}

TEST_CASE("CapEff parsing finds CAP_NET_ADMIN, and only on the CapEff line",
          "[wifi][wifi_country]") {
    const std::string service = "Name:\thelix-screen\n"
                                "CapInh:\t0000000000000000\n"
                                "CapPrm:\t0000000000401000\n"
                                "CapEff:\t0000000000400000\n"
                                "CapBnd:\t000001ffffffffff\n";
    // CAP_SYS_BOOT (22) is effective; CAP_NET_ADMIN is permitted only.
    CHECK_FALSE(country::proc_status_has_capability(service, country::kCapNetAdmin));
    CHECK(country::proc_status_has_capability(service, 22));

    CHECK(
        country::proc_status_has_capability("CapEff:\t0000000000001000\n", country::kCapNetAdmin));
    CHECK(country::proc_status_has_capability("CapEff:\t000001ffffffffff", country::kCapNetAdmin));
    CHECK_FALSE(country::proc_status_has_capability("", country::kCapNetAdmin));
    CHECK_FALSE(country::proc_status_has_capability("CapEff:\tzz\n", country::kCapNetAdmin));
    CHECK_FALSE(
        country::proc_status_has_capability("XCapEff:\t0000000000001000\n", country::kCapNetAdmin));
    CHECK_FALSE(country::proc_status_has_capability("CapEff:\t0000000000001000\n", 64));
}

// ============================================================================
// WiFiManager: persist, apply at startup, rescan on a domain change
// ============================================================================

namespace {

/// Mock that counts scan triggers, so a test can see a rescan go out.
class CountingCountryBackend : public WifiBackendMock {
  public:
    WiFiError trigger_scan() override {
        triggers_++;
        return WiFiError(WiFiResult::SUCCESS);
    }
    int triggers() const {
        return triggers_.load();
    }
    /// Stand in for a kernel that refuses the change (no CAP_NET_ADMIN).
    std::atomic<bool> refuse_country{false};
    /// Hold set_country() this long, to outlast a user-op deadline.
    std::atomic<int> set_delay_ms{0};
    WiFiError set_country(const std::string& code) override {
        if (const int ms = set_delay_ms.load(); ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        if (refuse_country.load()) {
            return WiFiError(WiFiResult::PERMISSION_DENIED, "iw reg set exited 1");
        }
        return WifiBackendMock::set_country(code);
    }

  private:
    std::atomic<int> triggers_{0};
};

struct CountryFixture : LVGLTestFixture {
    CountingCountryBackend* backend = nullptr;
    std::shared_ptr<WiFiManager> wm;
    helix::AsyncLifetimeGuard guard;

    CountryFixture() {
        Config::get_instance();
        SystemSettingsManager::instance().set_wifi_country("");
        SystemSettingsManager::instance().set_wifi_country_last_seen("");
        HttpExecutor::fast().start();
    }

    /// Builds the manager; its READY handler runs the startup apply.
    void start(const std::string& system_country) {
        auto b = std::make_unique<CountingCountryBackend>();
        backend = b.get();
        backend->set_country_for_test(system_country);
        wm = std::make_shared<WiFiManager>(std::move(b));
        wm->init_self_reference(wm);
        REQUIRE(WiFiManagerTestAccess::settle(*wm));
    }

    ~CountryFixture() override {
        if (wm) {
            wm->stop_scan();
            WiFiManagerTestAccess::settle(*wm);
            wm.reset();
        }
        helix::ui::UpdateQueue::instance().drain();
        SystemSettingsManager::instance().set_wifi_country("");
        SystemSettingsManager::instance().set_wifi_country_last_seen("");
    }
};

std::string radio_country(WifiBackendMock& backend) {
    std::string code;
    REQUIRE(backend.get_country(code).success());
    return code;
}

} // namespace

TEST_CASE_METHOD(CountryFixture, "setting the country saves it and applies it to the radio",
                 "[wifi][wifi_country]") {
    start("");
    bool done = false;
    bool ok = false;
    wm->set_country_async("de", guard.token(), [&](bool success) {
        done = true;
        ok = success;
    });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(done);
    CHECK(ok);
    CHECK(SystemSettingsManager::instance().get_wifi_country() == "DE");
    CHECK(radio_country(*backend) == "DE");
    CHECK(backend->set_country_calls() == 1);
}

TEST_CASE_METHOD(CountryFixture, "clearing the country forgets the choice and leaves the radio",
                 "[wifi][wifi_country]") {
    start("");
    wm->set_country_async("FR", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(backend->set_country_calls() == 1);

    wm->set_country_async("", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(SystemSettingsManager::instance().get_wifi_country().empty());
    CHECK(radio_country(*backend) == "FR");
    CHECK(backend->set_country_calls() == 1);
}

TEST_CASE_METHOD(CountryFixture, "a saved country is applied at startup when the radio differs",
                 "[wifi][wifi_country]") {
    SystemSettingsManager::instance().set_wifi_country("JP");
    start("");
    CHECK(radio_country(*backend) == "JP");
    CHECK(backend->set_country_calls() == 1);
}

TEST_CASE_METHOD(CountryFixture, "a saved country the radio already has is not sent again",
                 "[wifi][wifi_country]") {
    SystemSettingsManager::instance().set_wifi_country("JP");
    start("JP");
    CHECK(backend->set_country_calls() == 0);
}

TEST_CASE_METHOD(CountryFixture, "with nothing saved, the system's country is shown and kept",
                 "[wifi][wifi_country]") {
    start("GB");
    CHECK(backend->set_country_calls() == 0);
    CHECK(radio_country(*backend) == "GB");

    std::optional<WiFiManager::CountryState> got;
    wm->get_country_async(guard.token(), [&](const WiFiManager::CountryState& s) { got = s; });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(got.has_value());
    CHECK(got->read_ok);
    CHECK(got->stored.empty());
    CHECK(got->system == "GB");
    CHECK(got->display == "GB");
}

TEST_CASE_METHOD(CountryFixture, "the radio's own country is remembered at startup",
                 "[wifi][wifi_country]") {
    start("GB");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(SystemSettingsManager::instance().get_wifi_country_last_seen() == "GB");
    CHECK(SystemSettingsManager::instance().get_wifi_country().empty());
    CHECK(backend->set_country_calls() == 0);
}

TEST_CASE_METHOD(CountryFixture, "the country state carries the remembered country, normalized",
                 "[wifi][wifi_country]") {
    start("");
    SystemSettingsManager::instance().set_wifi_country_last_seen("us");

    std::optional<WiFiManager::CountryState> got;
    wm->get_country_async(guard.token(), [&](const WiFiManager::CountryState& s) { got = s; });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    REQUIRE(got.has_value());
    REQUIRE(got->system.empty());
    CHECK(got->last_seen == "US");
}

TEST_CASE_METHOD(CountryFixture, "a radio wiped to world gets its remembered country back",
                 "[wifi][wifi_country]") {
    SystemSettingsManager::instance().set_wifi_country_last_seen("US");
    start("");
    CHECK(radio_country(*backend) == "US");
    CHECK(backend->set_country_calls() == 1);
    // A restore is not a choice: Not set still defers to the system.
    helix::ui::UpdateQueue::instance().drain();
    CHECK(SystemSettingsManager::instance().get_wifi_country().empty());
}

TEST_CASE_METHOD(CountryFixture, "a remembered country never overwrites one the radio reports",
                 "[wifi][wifi_country]") {
    SystemSettingsManager::instance().set_wifi_country_last_seen("US");
    start("CA");
    CHECK(radio_country(*backend) == "CA");
    CHECK(backend->set_country_calls() == 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(SystemSettingsManager::instance().get_wifi_country_last_seen() == "CA");
}

TEST_CASE_METHOD(CountryFixture, "a world radio with nothing saved or remembered is left alone",
                 "[wifi][wifi_country]") {
    start("");
    CHECK(radio_country(*backend).empty());
    CHECK(backend->set_country_calls() == 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(SystemSettingsManager::instance().get_wifi_country_last_seen().empty());
}

TEST_CASE_METHOD(CountryFixture, "a regulatory domain change remembers a real country only",
                 "[wifi][wifi_country]") {
    start("");
    WiFiManagerTestAccess::fire_regdom_change(
        *wm, "<3>CTRL-EVENT-REGDOM-CHANGE init=COUNTRY_IE type=COUNTRY alpha2=AU");
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(SystemSettingsManager::instance().get_wifi_country_last_seen() == "AU");

    WiFiManagerTestAccess::fire_regdom_change(
        *wm, "<3>CTRL-EVENT-REGDOM-CHANGE init=CORE type=WORLD alpha2=00");
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(SystemSettingsManager::instance().get_wifi_country_last_seen() == "AU");
}

TEST_CASE_METHOD(CountryFixture, "a regulatory domain change rescans an open scan session",
                 "[wifi][wifi_country]") {
    start("");
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    WiFiManagerTestAccess::fire_scan_complete(*wm);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    const int before = backend->triggers();
    REQUIRE(before >= 1);

    // The mock dispatches REGDOM_CHANGE from set_country, as wpa_supplicant does.
    wm->set_country_async("US", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(backend->triggers() == before + 1);
}

TEST_CASE_METHOD(CountryFixture, "a regulatory domain change with no scan session sends no scan",
                 "[wifi][wifi_country]") {
    start("");
    wm->set_country_async("US", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    CHECK(radio_country(*backend) == "US");
    CHECK(backend->triggers() == 0);
}

TEST_CASE_METHOD(CountryFixture, "a country the radio refuses is not saved",
                 "[wifi][wifi_country]") {
    SystemSettingsManager::instance().set_wifi_country("FR");
    start("FR");
    backend->refuse_country = true;
    bool ok = true;
    wm->set_country_async("DE", guard.token(), [&](bool success) { ok = success; });
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(ok);
    // The previous choice stands; DE would fail again at every start.
    CHECK(SystemSettingsManager::instance().get_wifi_country() == "FR");
    CHECK(radio_country(*backend) == "FR");
}

TEST_CASE_METHOD(CountryFixture,
                 "country observers wake on a domain change, not on connection changes",
                 "[wifi][wifi_country]") {
    start("");
    int country_wakes = 0;
    int state_wakes = 0;
    wm->add_country_observer(guard.token(), [&]() { ++country_wakes; });
    wm->add_state_observer(guard.token(), [&]() { ++state_wakes; });

    WiFiManagerTestAccess::fire_connected(*wm);
    WiFiManagerTestAccess::fire_disconnected(*wm);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(state_wakes >= 2);
    CHECK(country_wakes == 0);

    wm->set_country_async("US", guard.token(), nullptr);
    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(country_wakes >= 1);
}

TEST_CASE_METHOD(CountryFixture, "a country the radio takes after the deadline is still saved",
                 "[wifi][wifi_country]") {
    start("");
    WiFiManagerTestAccess::set_user_op_timeouts(*wm, 100, 100, 60000);
    backend->set_delay_ms = 400;
    std::optional<bool> ok;
    wm->set_country_async("DE", guard.token(), [&](bool success) { ok = success; });
    REQUIRE(wait_until([&] { return ok.has_value(); }, 2000));
    CHECK_FALSE(*ok); // the caller was told it failed at the deadline

    REQUIRE(WiFiManagerTestAccess::settle(*wm));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(radio_country(*backend) == "DE");
    CHECK(SystemSettingsManager::instance().get_wifi_country() == "DE");
}
