// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The Beta channel reads the beta and stable manifests and offers the newer, so a
// graduated stable release reaches the beta fleet even while the beta manifest
// still names the last beta (prestonbrown/helixscreen#2205).

#include "../test_helpers/loopback_http_server.h"
#include "../test_helpers/scoped_update_urls.h"
#include "../test_helpers/update_checker_test_access.h"
#include "config.h"
#include "system/update_checker.h"

#include <chrono>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using helix::ChannelRead;
using helix::pick_channel_release;

namespace {

ChannelRead served(const std::string& version) {
    ChannelRead read;
    read.ok = true;
    read.info.version = version;
    read.info.tag_name = "v" + version;
    return read;
}

ChannelRead failed(const std::string& error) {
    ChannelRead read;
    read.error = error;
    return read;
}

} // namespace

TEST_CASE("Beta offers a stable release newer than the beta manifest",
          "[update_checker][channel][2205]") {
    // The stuck fleet: beta manifest at 1.1.0, stable at 1.1.1, device on 1.1.0.
    const auto picked = pick_channel_release("1.1.0", {served("1.1.0"), served("1.1.1")});
    REQUIRE(picked.ok);
    CHECK(picked.info.version == "1.1.1");

    const auto outcome = helix::decide_check_outcome("1.1.0", picked.info, {});
    CHECK(outcome.status == UpdateChecker::Status::UpdateAvailable);
    CHECK_FALSE(outcome.is_downgrade);

    // A graduated release outranks every beta of its own line.
    CHECK(pick_channel_release("1.1.0-beta.5", {served("1.1.0-beta.5"), served("1.1.0")})
              .info.version == "1.1.0");
}

TEST_CASE("Beta offers its own manifest when it is ahead of stable",
          "[update_checker][channel][2205]") {
    const auto picked = pick_channel_release("1.1.0", {served("1.2.0-beta.1"), served("1.1.1")});
    REQUIRE(picked.ok);
    CHECK(picked.info.version == "1.2.0-beta.1");

    // Both behind the install (a device switched over from Dev): the newer of the two
    // is still the offer, and the outcome marks it a downgrade, never auto-notified.
    const auto behind =
        pick_channel_release("1.2.0-alpha.3", {served("1.1.0-beta.5"), served("1.1.1")});
    REQUIRE(behind.ok);
    CHECK(behind.info.version == "1.1.1");
    CHECK(helix::decide_check_outcome("1.2.0-alpha.3", behind.info, {}).is_downgrade);
}

TEST_CASE("Beta falls back to the manifest that answered when the other fails",
          "[update_checker][channel][2205]") {
    // Stable unreachable: the beta manifest's forward step is still offered.
    auto picked = pick_channel_release("1.1.0", {served("1.2.0-beta.1"), failed("stable down")});
    REQUIRE(picked.ok);
    CHECK(picked.info.version == "1.2.0-beta.1");

    // Beta refused or unreachable: stable's forward step is offered.
    picked = pick_channel_release("1.1.0", {failed("beta down"), served("1.1.1")});
    REQUIRE(picked.ok);
    CHECK(picked.info.version == "1.1.1");
}

TEST_CASE("A half-read Beta check never offers a downgrade, and both failing reports beta's error",
          "[update_checker][channel][2205]") {
    // On 1.1.1 with stable unreachable, the stale beta manifest alone would read as a
    // downgrade the full answer (stable 1.1.1, Same) never offers. The failure stands.
    auto picked = pick_channel_release("1.1.1", {served("1.1.0-beta.5"), failed("stable down")});
    CHECK_FALSE(picked.ok);
    CHECK(picked.error == "stable down");

    // Up to date by the one answer is not up to date by both: still the failure.
    picked = pick_channel_release("1.1.1", {failed("beta down"), served("1.1.1")});
    CHECK_FALSE(picked.ok);
    CHECK(picked.error == "beta down");

    picked = pick_channel_release("1.1.0", {failed("beta down"), failed("stable down")});
    CHECK_FALSE(picked.ok);
    CHECK(picked.error == "beta down");
}

TEST_CASE("a Beta check offers the stable release when it is newer than the beta manifest",
          "[update_checker][slow][2205]") {
    // Beta serves 1.0.0, older than this build; stable serves 99.0.0. Read alone, the beta
    // manifest offers a downgrade; read with stable, the check offers 99.0.0 as an update.
    helix::test::LoopbackHttpServer server;
    for (const std::string channel : {"stable", "beta"}) {
        const std::string version = channel == "stable" ? "99.0.0" : "1.0.0";
        const std::string body = R"({"version": ")" + version + R"(", "tag": "v)" + version +
                                 R"(", "notes": "n", "assets": {"pi": {"zip_url": "https://r/)" +
                                 channel + R"(/pi.zip", "zip_sha256": "aa"}}})";
        server.service().GET(
            ("/" + channel + "/manifest.json").c_str(),
            [body](HttpRequest*, HttpResponse* resp) { return resp->String(body); });
    }
    REQUIRE(server.start());

    helix::test::ScopedUpdateUrls urls("helix_beta_reads_stable",
                                       R"({"r2_url": ")" + server.url("/") + R"("})");
    auto* config = helix::Config::get_instance();
    config->set<int>("/update/channel", 1);
    auto& checker = UpdateChecker::instance();
    checker.init();
    checker.clear_cache();
    UpdateCheckerTestAccess::reset_rate_limit(checker);
    checker.check_for_updates(nullptr);
    for (int i = 0; i < 8000 && checker.get_status() == UpdateChecker::Status::Checking; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    INFO(checker.get_error_message());
    REQUIRE(checker.get_status() == UpdateChecker::Status::UpdateAvailable);
    const auto cached = checker.get_cached_update();
    REQUIRE(cached);
    CHECK(cached->version == "99.0.0");
    CHECK_FALSE(cached->is_downgrade);
    checker.shutdown();
}

TEST_CASE("On a version tie Beta offers the manifest's release, not GitHub's announcement",
          "[update_checker][channel][2205]") {
    auto github = served("1.1.1"); // the beta line's GitHub fallback: announce only
    auto manifest = served("1.1.1");
    manifest.info.from_manifest = true;

    CHECK(pick_channel_release("1.1.0", {github, manifest}).info.from_manifest);
    CHECK(pick_channel_release("1.1.0", {manifest, github}).info.from_manifest);
}

TEST_CASE("Beta's GitHub fallback offers a beta-line release, never a devel-line alpha",
          "[update_checker][channel][2205]") {
    CHECK(helix::is_beta_line_version("1.2.0-beta.1"));
    CHECK(helix::is_beta_line_version("1.1.0-rc.2"));
    CHECK(helix::is_beta_line_version("1.1.1"));
    CHECK_FALSE(helix::is_beta_line_version("1.2.0-alpha.1"));
    CHECK_FALSE(helix::is_beta_line_version("not-a-version"));

    auto release = [](const char* tag, bool prerelease) {
        return std::string(R"({"tag_name": ")") + tag + R"(", "draft": false, "prerelease": )" +
               (prerelease ? "true" : "false") + R"(, "body": "", "assets": []})";
    };
    UpdateChecker::ReleaseInfo info;
    std::string error;

    // Newest first, as GitHub lists them: the alpha is skipped for the beta.
    REQUIRE(helix::parse_github_beta_release("[" + release("v1.2.0-alpha.1", true) + "," +
                                                 release("v1.1.0-beta.3", true) + "," +
                                                 release("v1.1.1", false) + "]",
                                             info, error, {}));
    CHECK(info.version == "1.1.0-beta.3");

    // No beta-line prerelease: the newest release, not the alpha.
    REQUIRE(helix::parse_github_beta_release("[" + release("v1.2.0-alpha.1", true) + "," +
                                                 release("v1.1.1", false) + "]",
                                             info, error, {}));
    CHECK(info.version == "1.1.1");

    // The stuck fleet with R2 beta unreachable: the fallback's 1.1.1 ties stable's manifest
    // 1.1.1, and the manifest's is offered.
    ChannelRead fallback;
    fallback.ok = true;
    fallback.info = info;
    auto stable = served("1.1.1");
    stable.info.from_manifest = true;
    const auto picked = pick_channel_release("1.1.0", {fallback, stable});
    REQUIRE(picked.ok);
    CHECK(picked.info.from_manifest);

    CHECK_FALSE(helix::parse_github_beta_release("{}", info, error, {}));
}
