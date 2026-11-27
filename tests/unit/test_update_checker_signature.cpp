// SPDX-License-Identifier: GPL-3.0-or-later

// The update checker trusts a channel manifest only once its minisign signature verifies
// (docs/devel/RELEASE_SIGNING.md). Fixtures: tests/fixtures/release_signing/README.md.

#include "../test_helpers/loopback_http_server.h"
#include "../test_helpers/scoped_update_urls.h"
#include "../test_helpers/update_checker_test_access.h"
#include "config.h"
#include "system/update_checker.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

namespace {

std::string fixture(const std::string& name) {
    std::string src = __FILE__;
    const auto pos = src.rfind("/tests/unit/");
    const std::string path = (pos != std::string::npos ? src.substr(0, pos) + "/" : "") +
                             "tests/fixtures/release_signing/" + name;
    std::ifstream f(path, std::ios::binary);
    INFO("unreadable: " << path);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string test_key(const std::string& pub_file = "test.pub") {
    const std::string pub = fixture(pub_file);
    const auto first = pub.find('\n');
    return pub.substr(first + 1, pub.find('\n', first + 1) - first - 1);
}

// The fixtures are signed for the dev channel by the test key, which stands in for the release
// key; no dev key.
helix::release_signature::ChannelKeys test_keys(const std::string& release) {
    return {release, ""};
}

const std::vector<std::string> kPi{"pi"};
constexpr const char* kPiZipSha =
    "0000000000000000000000000000000000000000000000000000000000000001";

} // namespace

TEST_CASE("signed manifest: a good signature yields a verified, installable release",
          "[update_checker][signature]") {
    UpdateChecker::ReleaseInfo info;
    std::string error;
    const std::string key = test_key();
    REQUIRE(helix::parse_signed_release_manifest(fixture("manifest.json"), 200,
                                                 fixture("good.minisig"), "dev", test_keys(key),
                                                 info, error, kPi));
    CHECK(info.version == "99.0.0");
    CHECK(info.sha256 == kPiZipSha);
    CHECK(info.signature_verified);
    CHECK(helix::release_is_verified(info));
}

TEST_CASE("signed manifest: unsigned or badly signed manifests are refused untouched",
          "[update_checker][signature]") {
    const std::string manifest = fixture("manifest.json");
    UpdateChecker::ReleaseInfo info;
    info.version = "sentinel";
    std::string error;
    const std::string key = test_key();
    const auto keys = test_keys(key);

    SECTION("no signature served") {
        CHECK_FALSE(helix::parse_signed_release_manifest(manifest, 404, "Not Found", "dev", keys,
                                                         info, error, kPi));
        CHECK(error.find("not signed") != std::string::npos);
    }
    SECTION("signature unreachable") {
        CHECK_FALSE(
            helix::parse_signed_release_manifest(manifest, 0, "", "dev", keys, info, error, kPi));
    }
    SECTION("manifest changed after signing") {
        std::string tampered = manifest;
        tampered.replace(tampered.find(kPiZipSha), 64, std::string(64, 'f'));
        CHECK_FALSE(helix::parse_signed_release_manifest(tampered, 200, fixture("good.minisig"),
                                                         "dev", keys, info, error, kPi));
        CHECK(error.find("signature refused") != std::string::npos);
    }
    SECTION("signed by another key") {
        CHECK_FALSE(helix::parse_signed_release_manifest(manifest, 200, fixture("other.minisig"),
                                                         "dev", keys, info, error, kPi));
    }
    SECTION("legacy signature") {
        CHECK_FALSE(helix::parse_signed_release_manifest(manifest, 200, fixture("legacy.minisig"),
                                                         "dev", keys, info, error, kPi));
    }
    SECTION("signed for another channel") {
        CHECK_FALSE(helix::parse_signed_release_manifest(manifest, 200, fixture("good.minisig"),
                                                         "stable", keys, info, error, kPi));
        CHECK(error.find("another channel") != std::string::npos);
    }
    CHECK(info.version == "sentinel");
    CHECK_FALSE(info.signature_verified);
}

TEST_CASE("signed manifest: a release is installable only when signed and hashed",
          "[update_checker][signature]") {
    // A GitHub-API release parses with no signature and no hash: it may announce, never install.
    UpdateChecker::ReleaseInfo github;
    std::string error;
    REQUIRE(helix::parse_github_release(
        R"({"tag_name":"v99.0.0","assets":[{"name":"helixscreen-pi.zip",)"
        R"("browser_download_url":"https://github.com/x/helixscreen-pi.zip"}]})",
        github, error, kPi));
    REQUIRE_FALSE(github.download_url.empty());
    CHECK_FALSE(helix::release_is_verified(github));

    // A signed manifest with no asset for this board carries no hash to check against.
    UpdateChecker::ReleaseInfo no_asset;
    const std::string key = test_key();
    REQUIRE(helix::parse_signed_release_manifest(fixture("manifest.json"), 200,
                                                 fixture("good.minisig"), "dev", test_keys(key),
                                                 no_asset, error, {"k2"}));
    REQUIRE(no_asset.signature_verified);
    CHECK_FALSE(helix::release_is_verified(no_asset));
}

TEST_CASE("update check verifies the served manifest's signature end to end",
          "[update_checker][signature][slow]") {
    std::string served_sig;
    helix::test::LoopbackHttpServer server;
    server.service().GET("/manifest.json", [](HttpRequest*, HttpResponse* resp) {
        return resp->String(fixture("manifest.json"));
    });
    server.service().GET("/manifest.json.minisig", [&served_sig](HttpRequest*, HttpResponse* resp) {
        if (served_sig.empty()) {
            resp->status_code = HTTP_STATUS_NOT_FOUND;
            return 404;
        }
        return resp->String(served_sig);
    });
    REQUIRE(server.start());

    auto* config = helix::Config::get_instance();
    config->set<bool>("/beta_features", true);
    config->set<int>("/update/channel", 2); // Dev: reads dev_url and skips the rate limiter
    helix::test::ScopedUpdateUrls urls("helix_sig_check", R"({"dev_url": ")" + server.url("/") +
                                                              R"(", "release_pubkey": ")" +
                                                              test_key() + R"("})");

    auto& checker = UpdateChecker::instance();
    checker.init();
    checker.clear_cache();

    auto run_check = [&checker]() {
        checker.check_for_updates(nullptr);
        for (int i = 0; i < 8000 && checker.get_status() == UpdateChecker::Status::Checking; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return checker.get_status();
    };

    SECTION("a good signature offers the update") {
        served_sig = fixture("good.minisig");
        REQUIRE(run_check() == UpdateChecker::Status::UpdateAvailable);
        const auto cached = checker.get_cached_update();
        REQUIRE(cached);
        CHECK(helix::release_is_verified(*cached));
    }
    SECTION("no signature is an error, not an update") {
        REQUIRE(run_check() == UpdateChecker::Status::Error);
        CHECK(checker.get_error_message().find("not signed") != std::string::npos);
        CHECK_FALSE(checker.get_cached_update());
    }
    SECTION("a signature by another key is an error") {
        served_sig = fixture("other.minisig");
        REQUIRE(run_check() == UpdateChecker::Status::Error);
        CHECK(checker.get_error_message().find("signature refused") != std::string::npos);
        CHECK_FALSE(checker.get_cached_update());
    }
    checker.shutdown();
}

TEST_CASE("update check holds each published channel to its own signed channel",
          "[update_checker][signature][slow]") {
    // R2 serves a manifest signed for dev on every channel path; stable and beta must refuse it
    // as made for another channel, which only holds when each fetch names its own channel.
    helix::test::LoopbackHttpServer server;
    for (const char* channel : {"stable", "beta"}) {
        server.service().GET((std::string("/") + channel + "/manifest.json").c_str(),
                             [](HttpRequest*, HttpResponse* resp) {
                                 return resp->String(fixture("manifest.json"));
                             });
        server.service().GET(
            (std::string("/") + channel + "/manifest.json.minisig").c_str(),
            [](HttpRequest*, HttpResponse* resp) { return resp->String(fixture("good.minisig")); });
    }
    REQUIRE(server.start());

    helix::test::ScopedUpdateUrls urls("helix_sig_channel", R"({"r2_url": ")" + server.url("/") +
                                                                R"(", "release_pubkey": ")" +
                                                                test_key() + R"("})");
    auto* config = helix::Config::get_instance();
    auto& checker = UpdateChecker::instance();
    checker.init();

    auto run_check = [&checker]() {
        checker.clear_cache();
        UpdateCheckerTestAccess::reset_rate_limit(checker);
        checker.check_for_updates(nullptr);
        for (int i = 0; i < 8000 && checker.get_status() == UpdateChecker::Status::Checking; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return checker.get_status();
    };

    SECTION("stable") {
        config->set<int>("/update/channel", 0);
        REQUIRE(run_check() == UpdateChecker::Status::Error);
        CHECK(checker.get_error_message().find("another channel") != std::string::npos);
        CHECK_FALSE(checker.get_cached_update());
    }
    SECTION("beta") {
        config->set<int>("/update/channel", 1);
        REQUIRE(run_check() == UpdateChecker::Status::Error);
        CHECK(checker.get_error_message().find("another channel") != std::string::npos);
        CHECK_FALSE(checker.get_cached_update());
    }
    checker.shutdown();
}

TEST_CASE("a Beta check offers the stable release when it is newer than the beta manifest",
          "[update_checker][signature][slow][2205]") {
    // Beta serves 1.0.0, older than this build; stable serves 99.0.0. Read alone, the beta
    // manifest offers a downgrade; read with stable, the check offers 99.0.0 as an update.
    helix::test::LoopbackHttpServer server;
    for (const std::string channel : {"stable", "beta"}) {
        server.service().GET(("/" + channel + "/manifest.json").c_str(),
                             [channel](HttpRequest*, HttpResponse* resp) {
                                 return resp->String(fixture(channel + "_manifest.json"));
                             });
        server.service().GET(("/" + channel + "/manifest.json.minisig").c_str(),
                             [channel](HttpRequest*, HttpResponse* resp) {
                                 return resp->String(fixture(channel + "_manifest.minisig"));
                             });
    }
    REQUIRE(server.start());

    helix::test::ScopedUpdateUrls urls("helix_beta_reads_stable",
                                       R"({"r2_url": ")" + server.url("/") +
                                           R"(", "release_pubkey": ")" + test_key("channels.pub") +
                                           R"("})");
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
    CHECK(helix::release_is_verified(*cached));
    checker.shutdown();
}
