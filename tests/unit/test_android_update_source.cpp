// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/android_update_source.h"

#include "../catch_amalgamated.hpp"

using helix::android::OfferedRelease;
using helix::android::update_url;

namespace {
constexpr const char* kPlay = "com.android.vending";
constexpr const char* kTagBase = "https://github.com/prestonbrown/helixscreen/releases/tag/";
constexpr const char* kLatest = "https://github.com/prestonbrown/helixscreen/releases/latest";

OfferedRelease version_only(const std::string& version) {
    return {"", version, true};
}
} // namespace

TEST_CASE("android update_url: a Play install goes to the Play listing", "[android_update]") {
    CHECK(update_url(kPlay, version_only("1.2.3")) == "market://details?id=org.helixscreen.app");
    CHECK(update_url(kPlay, version_only("")) == "market://details?id=org.helixscreen.app");
    CHECK(update_url(kPlay, {"v1.2.3", "1.2.3", false}) ==
          "market://details?id=org.helixscreen.app");
}

TEST_CASE("android update_url: a sideload goes to the offered GitHub release", "[android_update]") {
    SECTION("no installer reported") {
        CHECK(update_url("", version_only("1.2.3")) == std::string(kTagBase) + "v1.2.3");
    }
    SECTION("installed by adb, a browser or a file manager") {
        CHECK(update_url("com.google.android.packageinstaller", version_only("1.2.3")) ==
              std::string(kTagBase) + "v1.2.3");
        CHECK(update_url("com.android.chrome", version_only("1.1.0-beta.3")) ==
              std::string(kTagBase) + "v1.1.0-beta.3");
    }
    SECTION("a store that merely resembles Play is not Play") {
        CHECK(update_url("com.android.vending.fake", version_only("1.2.3")) ==
              std::string(kTagBase) + "v1.2.3");
        CHECK(update_url("org.fdroid.fdroid", version_only("1.2.3")) ==
              std::string(kTagBase) + "v1.2.3");
    }
    SECTION("build metadata characters are part of a tag") {
        CHECK(update_url("", version_only("1.2.3+build_1")) ==
              std::string(kTagBase) + "v1.2.3+build_1");
    }
}

TEST_CASE("android update_url: the release's own tag wins over the version", "[android_update]") {
    CHECK(update_url("", {"v1.2.3", "9.9.9", true}) == std::string(kTagBase) + "v1.2.3");
    // A tag spelled without the 'v' is used as published, not rewritten.
    CHECK(update_url("", {"1.2.3", "1.2.3", true}) == std::string(kTagBase) + "1.2.3");
    CHECK(update_url("", {"", "1.2.3", true}) == std::string(kTagBase) + "v1.2.3");
}

TEST_CASE("android update_url: a leading v is not doubled", "[android_update]") {
    CHECK(update_url("", version_only("v1.2.3")) == std::string(kTagBase) + "v1.2.3");
    CHECK(update_url("", version_only("V1.2.3")) == std::string(kTagBase) + "v1.2.3");
}

TEST_CASE("android update_url: a release not on GitHub goes to the latest release",
          "[android_update]") {
    CHECK(update_url("", {"v1.3.0-dev.42", "1.3.0-dev.42", false}) == kLatest);
    CHECK(update_url("com.android.chrome", {"", "1.3.0", false}) == kLatest);
}

TEST_CASE("android update_url: no usable version falls back to the latest release",
          "[android_update]") {
    CHECK(update_url("", version_only("")) == kLatest);
    CHECK(update_url("", version_only("v")) == kLatest);
    CHECK(update_url("com.android.chrome", version_only("")) == kLatest);
    // A version from a remote manifest never smuggles a path or query into the URL.
    CHECK(update_url("", version_only("1.2.3/../../evil")) == kLatest);
    CHECK(update_url("", version_only("1.2.3?x=1")) == kLatest);
    CHECK(update_url("", version_only("1.2 3")) == kLatest);
    CHECK(update_url("", {"v1.2.3/../x", "1.2.3", true}) == kLatest);
}
