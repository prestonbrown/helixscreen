// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __ANDROID__

#include "system/http_android.h"

#include "system/android_jni.h"

#include <spdlog/spdlog.h>

#include <SDL.h>
#include <jni.h>

namespace helix::android {

namespace {

/// Copy a Java string into a std::string, tolerating an allocation failure.
/// GetStringUTFChars returns null (with OutOfMemoryError pending) under memory
/// pressure, and constructing a std::string from that dereferences null.
bool jstring_to_string(JNIEnv* env, jstring value, std::string& out) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) {
        env->ExceptionClear();
        return false;
    }
    out.assign(chars);
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

} // namespace

std::pair<int, std::string> https_get(const std::string& url, const std::string& user_agent,
                                      const std::string& accept, int timeout_sec) {
    // SDL_AndroidGetJNIEnv attaches the calling thread to the JavaVM if needed,
    // so this is safe from the UpdateChecker worker thread.
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) {
        spdlog::error("[http_android] Failed to get JNI env");
        return {0, "JNI env unavailable"};
    }

    jclass cls = helix_activity_class(env);
    if (!cls) {
        return {0, "HelixActivity class not found"};
    }

    jmethodID method = env->GetStaticMethodID(
        cls, "httpsGet",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    if (!method) {
        spdlog::error("[http_android] Failed to find httpsGet method");
        env->ExceptionClear();
        return {0, "httpsGet method not found"};
    }

    jstring j_url = env->NewStringUTF(url.c_str());
    jstring j_ua = env->NewStringUTF(user_agent.c_str());
    jstring j_accept = env->NewStringUTF(accept.c_str());

    if (!j_url || !j_ua || !j_accept) {
        if (j_url)
            env->DeleteLocalRef(j_url);
        if (j_ua)
            env->DeleteLocalRef(j_ua);
        if (j_accept)
            env->DeleteLocalRef(j_accept);
        env->ExceptionClear();
        return {0, "JNI string allocation failed"};
    }

    auto j_result = static_cast<jstring>(env->CallStaticObjectMethod(
        cls, method, j_url, j_ua, j_accept, static_cast<jint>(timeout_sec)));

    env->DeleteLocalRef(j_url);
    env->DeleteLocalRef(j_ua);
    env->DeleteLocalRef(j_accept);

    if (!j_result || env->ExceptionCheck()) {
        env->ExceptionClear();
        return {0, "JNI call failed"};
    }

    std::string result;
    bool converted = jstring_to_string(env, j_result, result);
    env->DeleteLocalRef(j_result);
    if (!converted) {
        spdlog::error("[http_android] Could not read httpsGet result string");
        return {0, "JNI string conversion failed"};
    }

    // Java side returns "STATUS\nBODY" or "0\nERROR".
    auto newline = result.find('\n');
    if (newline == std::string::npos) {
        return {0, result};
    }
    int status = 0;
    try {
        status = std::stoi(result.substr(0, newline));
    } catch (...) {
        status = 0;
    }
    return {status, result.substr(newline + 1)};
}

std::pair<int, std::string>
https_post_binary(const std::string& url, const std::vector<unsigned char>& body,
                  const std::string& content_type, const std::string& content_encoding,
                  const std::string& user_agent, const std::string& api_key, int timeout_sec) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) {
        spdlog::error("[http_android] Failed to get JNI env");
        return {0, "JNI env unavailable"};
    }

    jclass cls = helix_activity_class(env);
    if (!cls) {
        return {0, "HelixActivity class not found"};
    }

    jmethodID method =
        env->GetStaticMethodID(cls, "httpsPostBinary",
                               "(Ljava/lang/String;[BLjava/lang/String;Ljava/lang/String;"
                               "Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    if (!method) {
        spdlog::error("[http_android] Failed to find httpsPostBinary method");
        env->ExceptionClear();
        return {0, "httpsPostBinary method not found"};
    }

    jstring j_url = env->NewStringUTF(url.c_str());
    jbyteArray j_body = env->NewByteArray(static_cast<jsize>(body.size()));
    jstring j_ct = env->NewStringUTF(content_type.c_str());
    jstring j_ce = env->NewStringUTF(content_encoding.c_str());
    jstring j_ua = env->NewStringUTF(user_agent.c_str());
    jstring j_key = env->NewStringUTF(api_key.c_str());

    if (!j_url || !j_body || !j_ct || !j_ce || !j_ua || !j_key) {
        spdlog::error("[http_android] JNI allocation failed");
        if (j_url)
            env->DeleteLocalRef(j_url);
        if (j_body)
            env->DeleteLocalRef(j_body);
        if (j_ct)
            env->DeleteLocalRef(j_ct);
        if (j_ce)
            env->DeleteLocalRef(j_ce);
        if (j_ua)
            env->DeleteLocalRef(j_ua);
        if (j_key)
            env->DeleteLocalRef(j_key);
        env->ExceptionClear();
        return {0, "JNI allocation failed"};
    }

    if (!body.empty())
        env->SetByteArrayRegion(j_body, 0, static_cast<jsize>(body.size()),
                                reinterpret_cast<const jbyte*>(body.data()));

    auto j_result = static_cast<jstring>(env->CallStaticObjectMethod(
        cls, method, j_url, j_body, j_ct, j_ce, j_ua, j_key, static_cast<jint>(timeout_sec)));

    env->DeleteLocalRef(j_url);
    env->DeleteLocalRef(j_body);
    env->DeleteLocalRef(j_ct);
    env->DeleteLocalRef(j_ce);
    env->DeleteLocalRef(j_ua);
    env->DeleteLocalRef(j_key);

    if (!j_result || env->ExceptionCheck()) {
        env->ExceptionClear();
        return {0, "JNI call failed"};
    }

    std::string result;
    bool converted = jstring_to_string(env, j_result, result);
    env->DeleteLocalRef(j_result);
    if (!converted) {
        spdlog::error("[http_android] Could not read httpsPostBinary result string");
        return {0, "JNI string conversion failed"};
    }

    auto newline = result.find('\n');
    if (newline == std::string::npos)
        return {0, result};
    int status = 0;
    try {
        status = std::stoi(result.substr(0, newline));
    } catch (...) {
        status = 0;
    }
    return {status, result.substr(newline + 1)};
}

} // namespace helix::android

#endif // __ANDROID__
