// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::android {

/// Installer package Android reports for an app that Google Play installed.
inline constexpr const char* kPlayStoreInstaller = "com.android.vending";

/// Play listing in the Play app, and the web listing for when no app handles market://.
inline constexpr const char* kPlayStoreMarketUrl = "market://details?id=org.helixscreen.app";
inline constexpr const char* kPlayStoreWebUrl =
    "https://play.google.com/store/apps/details?id=org.helixscreen.app";

/// The release an update check is offering, as far as choosing a page needs it.
struct OfferedRelease {
    std::string tag_name; ///< Release tag ("v1.2.3"); may be empty
    std::string version;  ///< Version with or without a leading 'v'; used when tag_name is empty
    /// False for a channel published somewhere other than GitHub releases (the dev
    /// channel), whose tags have no release page there.
    bool on_github = true;
};

/// Where "Install Update" sends an Android user.
///
/// Android never runs the tarball updater; an update arrives through whatever
/// installed the app. A Play install goes to the Play listing. Anything else
/// (an APK sideloaded through adb, a browser or a file manager, which often
/// reports no installer at all) goes to the GitHub release page for the offered
/// release, where the per-ABI APKs live. It goes to the latest release instead
/// when the release is not on GitHub, or has no tag or version a URL can be
/// built from.
///
/// Pure: no JNI, compiled on every platform.
std::string update_url(const std::string& installer_package, const OfferedRelease& offered);

#ifdef __ANDROID__
/// Package name of whatever installed this app, per PackageManager, or "" when
/// Android reports none or the JNI bridge is unavailable.
std::string installer_package();
#endif

} // namespace helix::android
