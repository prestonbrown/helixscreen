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

/// Where "Install Update" sends an Android user.
///
/// Android never runs the tarball updater; an update arrives through whatever
/// installed the app. A Play install goes to the Play listing. Anything else
/// (an APK sideloaded through adb, a browser or a file manager, which often
/// reports no installer at all) goes to the GitHub release page for
/// `offered_version`, where the per-ABI APKs live, or to the latest release when
/// the version is empty or not something a tag can be built from.
///
/// `offered_version` may carry a leading 'v' or not ("1.2.3" and "v1.2.3" are the
/// same release). Pure: no JNI, compiled on every platform.
std::string update_url(const std::string& installer_package, const std::string& offered_version);

#ifdef __ANDROID__
/// Package name of whatever installed this app, per PackageManager, or "" when
/// Android reports none or the JNI bridge is unavailable.
std::string installer_package();
#endif

} // namespace helix::android
