// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/android_update_source.h"

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <spdlog/spdlog.h>

#include <SDL.h>
#include <jni.h>
#endif

namespace helix::android {

namespace {

constexpr const char* kReleasesUrl = "https://github.com/prestonbrown/helixscreen/releases";

/// The version reaches us from a remote manifest and lands in a URL path, so
/// only the characters a release tag uses are accepted.
bool is_tag_safe(const std::string& version) {
    if (version.empty()) {
        return false;
    }
    for (char c : version) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '+' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string update_url(const std::string& installer_package, const std::string& offered_version) {
    if (installer_package == kPlayStoreInstaller) {
        return kPlayStoreMarketUrl;
    }

    std::string version = offered_version;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
        version.erase(0, 1);
    }
    if (!is_tag_safe(version)) {
        return std::string(kReleasesUrl) + "/latest";
    }
    return std::string(kReleasesUrl) + "/tag/v" + version;
}

#ifdef __ANDROID__
std::string installer_package() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) {
        return {};
    }

    // Cached global ref owned by helix_activity_class(), never released here.
    jclass cls = helix_activity_class(env);
    if (!cls) {
        return {};
    }

    jmethodID method = env->GetStaticMethodID(cls, "getInstallerPackage", "()Ljava/lang/String;");
    if (!method) {
        spdlog::error("[android_update_source] Failed to find getInstallerPackage method");
        env->ExceptionClear();
        return {};
    }

    auto j_result = static_cast<jstring>(env->CallStaticObjectMethod(cls, method));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (j_result) {
            env->DeleteLocalRef(j_result);
        }
        return {};
    }
    if (!j_result) {
        return {};
    }

    std::string result;
    if (const char* chars = env->GetStringUTFChars(j_result, nullptr)) {
        result.assign(chars);
        env->ReleaseStringUTFChars(j_result, chars);
    } else {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(j_result);
    return result;
}
#endif

} // namespace helix::android
