// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_update_check.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::fwupdate;

namespace {

// A vendor answer shaped like the ones the research found: the release under a
// "data" wrapper, numbers where the vendor sends numbers.
constexpr const char* kFixture = R"({
  "code": 0,
  "data": {
    "version": "1.2.3.4",
    "url": "https://cdn.example.invalid/fw/U1_1.2.3.4.bin",
    "size": 268435456,
    "digest": {"sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"}
  }
})";

constexpr CheckSource kSource = {"https://updates.example.invalid/latest",
                                 "/data/version",
                                 "/data/url",
                                 "/data/size",
                                 "/data/digest/sha256",
                                 nullptr};

} // namespace

TEST_CASE("Firmware check: a vendor answer is read through the row's field paths",
          "[fwupdate][fwupdate_check]") {
    const auto fw = parse_check_response(kSource, kFixture);
    REQUIRE(fw.has_value());
    CHECK(fw->version == "1.2.3.4");
    CHECK(fw->download_url == "https://cdn.example.invalid/fw/U1_1.2.3.4.bin");
    CHECK(fw->size == 268435456u);
    CHECK(fw->sha256 == "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");

    const CheckSource in_array = {"u", "/releases/1/v", nullptr, nullptr, nullptr, nullptr};
    const auto second = parse_check_response(in_array, R"({"releases":[{"v":"a"},{"v":"b"}]})");
    REQUIRE(second.has_value());
    CHECK(second->version == "b");
    CHECK(second->size == 0u);
    CHECK_FALSE(parse_check_response(in_array, R"({"releases":[{"v":"a"}]})").has_value());

    CHECK_FALSE(parse_check_response(kSource, "not json").has_value());
    CHECK_FALSE(parse_check_response(kSource, R"({"data":{"url":"x"}})").has_value());
}

TEST_CASE("Firmware check: only a known, different version is an update",
          "[fwupdate][fwupdate_check]") {
    const RemoteFirmware offered{"1.2.3.4", "", 0, ""};
    CHECK(offers_update(offered, "1.2.3.3"));
    CHECK_FALSE(offers_update(offered, "1.2.3.4"));
    CHECK_FALSE(offers_update(offered, ""));
}

TEST_CASE("Firmware check: failures back off, a job or suppressed checks skip",
          "[fwupdate][fwupdate_check]") {
    CHECK(backoff_ticks(0) == 0);
    CHECK(backoff_ticks(1) == 0);
    CHECK(backoff_ticks(2) == 1);
    CHECK(backoff_ticks(3) == 3);
    CHECK(backoff_ticks(4) == 6);
    CHECK(backoff_ticks(40) == 6);

    CheckState s;
    CHECK_FALSE(should_check(s, /*suppressed=*/true, false, true));
    CHECK_FALSE(should_check(s, false, false, /*has_source=*/false));
    CHECK(should_check(s, false, false, true));
    CHECK_FALSE(should_check(s, false, /*job=*/true, true));

    record_check(s, false);
    record_check(s, false);
    REQUIRE(s.consecutive_failures == 2);
    CHECK_FALSE(should_check(s, false, false, true)); // one tick of backoff
    CHECK(should_check(s, false, false, true));
    record_check(s, true);
    CHECK(s.consecutive_failures == 0);
    CHECK(should_check(s, false, false, true));
}

TEST_CASE("Firmware check: a check fetches the row's URL as is and compares versions",
          "[fwupdate][fwupdate_check]") {
    std::vector<std::string> urls;
    auto answer = [&](int status, std::string body) {
        return [&urls, status, body](const std::string& url) {
            urls.push_back(url);
            return std::make_pair(status, body);
        };
    };

    auto newer = run_firmware_check(kSource, "1.2.3.3", answer(200, kFixture));
    CHECK(newer.succeeded);
    REQUIRE(newer.update.has_value());
    CHECK(newer.update->version == "1.2.3.4");
    REQUIRE(urls.size() == 1);
    CHECK(urls[0] == "https://updates.example.invalid/latest");

    auto same = run_firmware_check(kSource, "1.2.3.4", answer(200, kFixture));
    CHECK(same.succeeded);
    CHECK_FALSE(same.update.has_value());

    CHECK_FALSE(run_firmware_check(kSource, "1", answer(503, kFixture)).succeeded);
    CHECK_FALSE(run_firmware_check(kSource, "1", answer(0, "")).succeeded);
    CHECK_FALSE(run_firmware_check(kSource, "1", answer(200, "<html>")).succeeded);

    const CheckSource unconfigured = {nullptr, "/v", nullptr, nullptr, nullptr, nullptr};
    urls.clear();
    CHECK_FALSE(run_firmware_check(unconfigured, "1", answer(200, R"({"v":"2"})")).succeeded);
    CHECK(urls.empty());
}
