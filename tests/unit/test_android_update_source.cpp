// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/android_update_source.h"

#include "../catch_amalgamated.hpp"

using helix::android::update_url;

namespace {
constexpr const char* kPlay = "com.android.vending";
constexpr const char* kTagBase = "https://github.com/prestonbrown/helixscreen/releases/tag/";
constexpr const char* kLatest = "https://github.com/prestonbrown/helixscreen/releases/latest";
} // namespace

TEST_CASE("android update_url: a Play install goes to the Play listing", "[android_update]") {
    CHECK(update_url(kPlay, "1.2.3") == "market://details?id=org.helixscreen.app");
    CHECK(update_url(kPlay, "") == "market://details?id=org.helixscreen.app");
}

TEST_CASE("android update_url: a sideload goes to the offered GitHub release", "[android_update]") {
    SECTION("no installer reported") {
        CHECK(update_url("", "1.2.3") == std::string(kTagBase) + "v1.2.3");
    }
    SECTION("installed by adb, a browser or a file manager") {
        CHECK(update_url("com.google.android.packageinstaller", "1.2.3") ==
              std::string(kTagBase) + "v1.2.3");
        CHECK(update_url("com.android.chrome", "1.1.0-beta.3") ==
              std::string(kTagBase) + "v1.1.0-beta.3");
    }
    SECTION("a store that merely resembles Play is not Play") {
        CHECK(update_url("com.android.vending.fake", "1.2.3") == std::string(kTagBase) + "v1.2.3");
        CHECK(update_url("org.fdroid.fdroid", "1.2.3") == std::string(kTagBase) + "v1.2.3");
    }
}

TEST_CASE("android update_url: a leading v is not doubled", "[android_update]") {
    CHECK(update_url("", "v1.2.3") == std::string(kTagBase) + "v1.2.3");
    CHECK(update_url("", "V1.2.3") == std::string(kTagBase) + "v1.2.3");
}

TEST_CASE("android update_url: no usable version falls back to the latest release",
          "[android_update]") {
    CHECK(update_url("", "") == kLatest);
    CHECK(update_url("", "v") == kLatest);
    CHECK(update_url("com.android.chrome", "") == kLatest);
    // A version from a remote manifest never smuggles a path or query into the URL.
    CHECK(update_url("", "1.2.3/../../evil") == kLatest);
    CHECK(update_url("", "1.2.3?x=1") == kLatest);
    CHECK(update_url("", "1.2 3") == kLatest);
}
