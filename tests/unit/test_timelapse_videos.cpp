// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include "../catch_amalgamated.hpp"

using namespace helix::timelapse;

TEST_CASE("Phone URL: a LAN-facing Moonraker URL is used as configured",
          "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://192.168.1.20:7125", "benchy.mp4", "10.0.0.9") ==
          "http://192.168.1.20:7125/server/files/timelapse/benchy.mp4");
    CHECK(phone_video_url("http://printer.local:7125/", "benchy.mp4", "") ==
          "http://printer.local:7125/server/files/timelapse/benchy.mp4");
}

TEST_CASE("Phone URL: a loopback Moonraker is reached on the LAN address",
          "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://127.0.0.1:7125", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50:7125/server/files/timelapse/a.mp4");
    CHECK(phone_video_url("http://localhost:7125", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50:7125/server/files/timelapse/a.mp4");
    CHECK(phone_video_url("http://[::1]:7125", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50:7125/server/files/timelapse/a.mp4");
    CHECK(phone_video_url("http://localhost", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50/server/files/timelapse/a.mp4");
}

TEST_CASE("Phone URL: no address a phone could reach gives no URL", "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://127.0.0.1:7125", "a.mp4", "").empty());
    CHECK(phone_video_url("", "a.mp4", "192.168.1.50").empty());
    CHECK(phone_video_url("not a url", "a.mp4", "192.168.1.50").empty());
}

TEST_CASE("Phone URL: the file name is percent-encoded", "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://192.168.1.20:7125", "my print #2 & co.mp4", "") ==
          "http://192.168.1.20:7125/server/files/timelapse/my%20print%20%232%20%26%20co.mp4");
}
