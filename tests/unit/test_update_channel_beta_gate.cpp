// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_update_channel_beta_gate.cpp
 * @brief The effective update channel must fall back to Stable when beta is locked.
 *
 * /update/channel and /beta_features persist independently, but the dropdown that
 * writes the channel is gated on show_beta_features (about_settings_overlay.xml).
 * Unlock beta with the 7-tap easter egg, pick Dev, re-lock: the dropdown vanishes
 * while the app keeps fetching from /update/dev_url, with no UI route back.
 *
 * The clamp is deliberately at READ time, not a rewrite of the stored value —
 * these tests pin that, since a rewrite would silently discard the user's pick.
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

TEST_CASE_METHOD(HelixTestFixture, "Update channel falls back to Stable when beta is locked",
                 "[update_checker][channel][beta]") {
    REQUIRE(helix::Config::get_instance() != nullptr);
    auto& checker = UpdateChecker::instance();

    SECTION("Dev channel with beta locked reports Stable") {
        ScopedConfigValue<bool> beta("/beta_features", false, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 2, CHANNEL_DEFAULT);

        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Stable);
        CHECK(std::string(UpdateChecker::channel_name(checker.get_channel())) == "stable");
    }

    SECTION("Beta channel with beta locked reports Stable") {
        ScopedConfigValue<bool> beta("/beta_features", false, BETA_DEFAULT);
        ScopedConfigValue<int> chan("/update/channel", 1, CHANNEL_DEFAULT);

        CHECK(checker.get_channel() == UpdateChecker::UpdateChannel::Stable);
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
