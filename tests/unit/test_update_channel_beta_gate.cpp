// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_update_channel_beta_gate.cpp
 * @brief Dev is the only update channel that needs beta features; Beta is not.
 *
 * Stable and Beta are both offered on a stock install (about_settings_overlay.xml),
 * so the effective channel must honour a stored Beta whatever /beta_features says.
 * Dev fetches from the arbitrary /update/dev_url in config and is offered only with
 * beta unlocked, so a stored Dev on a locked install has to fall back — to Stable,
 * not to Beta, since Beta is a neighbouring choice the user never made.
 *
 * /update/channel and /beta_features persist independently. The fallback is at READ
 * time, not a rewrite of the stored value — these tests pin that, since a rewrite
 * would silently discard the user's pick across a lock/unlock round trip.
 */

#include "../helix_test_fixture.h"
#include "config.h"
#include "system/update_checker.h"

#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

namespace {

/// Sets a config key for the duration of a test, then puts back what was there.
/// Config has no key-erase, so an absent key restores to `fallback` — pass the
/// same default the production reader uses and the state is equivalent.
template <typename T> class ScopedConfigValue {
  public:
    ScopedConfigValue(std::string key, T value, T fallback) : key_(std::move(key)) {
        auto* cfg = helix::Config::get_instance();
        prev_ = cfg->get<T>(key_, fallback);
        cfg->set<T>(key_, value);
    }
    ~ScopedConfigValue() {
        helix::Config::get_instance()->set<T>(key_, prev_);
    }

  private:
    std::string key_;
    T prev_;
};

// Config's own defaults, mirrored so the restore above is a true restore.
constexpr int CHANNEL_DEFAULT = 0;
constexpr bool BETA_DEFAULT = false;

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "Only the Dev update channel needs beta features",
                 "[update_checker][channel][beta]") {
    REQUIRE(helix::Config::get_instance() != nullptr);
    auto& checker = UpdateChecker::instance();

    SECTION("Dev channel with beta locked reports Stable") {
        ScopedConfigValue<bool> beta("/beta_features", false, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 2, CHANNEL_DEFAULT);

        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Stable);
        CHECK(std::string(UpdateChecker::channel_name(checker.get_channel())) == "stable");
    }

    SECTION("Dev channel with beta locked does not degrade to its Beta neighbour") {
        // Falling back one step would move the install onto a channel it was
        // never opted into, and the dropdown that appears on a locked install
        // would then read Beta for a value the user set to Dev.
        ScopedConfigValue<bool> beta("/beta_features", false, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 2, CHANNEL_DEFAULT);

        CHECK_FALSE(checker.get_channel() == UpdateChecker::UpdateChannel::Beta);
    }

    SECTION("Beta channel is honoured with beta locked") {
        // Stable/Beta is the picker every install gets, so a stored Beta has to
        // reach the checker without the beta-features unlock.
        ScopedConfigValue<bool> beta("/beta_features", false, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 1, CHANNEL_DEFAULT);

        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Beta);
        CHECK(std::string(UpdateChecker::channel_name(checker.get_channel())) == "beta");
    }

    SECTION("Stable channel with beta locked is unaffected") {
        ScopedConfigValue<bool> beta("/beta_features", false, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 0, CHANNEL_DEFAULT);

        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Stable);
    }

    SECTION("Unlocking beta again restores the channel the user picked") {
        // The whole point of clamping at read time: the stored pick survives a
        // lock/unlock round trip. A rewrite-on-lock fix would fail this.
        ScopedConfigValue<int> chan("/update/channel", 2, CHANNEL_DEFAULT);

        {
            ScopedConfigValue<bool> locked("/beta_features", false, BETA_DEFAULT);
            REQUIRE(checker.get_channel() == UpdateChecker::UpdateChannel::Stable);
            // Clamping must not touch the persisted value.
            CHECK(helix::Config::get_instance()->get<int>("/update/channel", 0) == 2);
        }

        ScopedConfigValue<bool> unlocked("/beta_features", true, BETA_DEFAULT);
        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Dev);
        CHECK(std::string(UpdateChecker::channel_name(checker.get_channel())) == "dev");
    }

    SECTION("Beta unlocked passes the Beta channel through") {
        ScopedConfigValue<bool> beta("/beta_features", true, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 1, CHANNEL_DEFAULT);

        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Beta);
        CHECK(std::string(UpdateChecker::channel_name(checker.get_channel())) == "beta");
    }

    SECTION("Out-of-range channel still degrades to Stable with beta unlocked") {
        ScopedConfigValue<bool> beta("/beta_features", true, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 99, CHANNEL_DEFAULT);

        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Stable);
    }
}
