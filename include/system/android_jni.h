// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __ANDROID__

#include <spdlog/spdlog.h>

#include <SDL_system.h>
#include <jni.h>
#include <mutex>

namespace helix::android {

/// Resolve org/helixscreen/app/HelixActivity as a process-lifetime global ref.
///
/// FindClass() is not usable from a natively created thread. SDL attaches those
/// with AttachCurrentThread(vm, &env, NULL) — no class loader, no Java frames on
/// the stack — so ART resolves FindClass through the *system* class loader,
/// which has no visibility into the APK's dex. An app class is simply not found,
/// and the failure is silent: the caller gets a null jclass and its JNI call
/// degrades to a no-op (or, for the HTTP bridge, an HTTP 0).
///
/// Deriving the class from the live Activity object sidesteps class loading
/// entirely: SDL_AndroidGetActivity() calls a static method on a jclass SDL
/// cached during JNI_OnLoad, and GetObjectClass() on the result needs no loader.
/// HelixActivity extends SDLActivity, so the Context SDL hands back *is* our
/// activity. The result is cached as a global ref because a local one is only
/// valid for the frame that created it.
///
/// The returned reference is owned here and lives for the process — callers must
/// NOT DeleteLocalRef() it. Returns nullptr if the class cannot be resolved at
/// all, which every caller must treat as "bridge unavailable".
///
/// Defined inline rather than in its own translation unit so that the Android
/// build's CMake source glob (android/app/jni/CMakeLists.txt) never has to be
/// re-run to pick up a new file; C++ gives the function-local statics below a
/// single instance across all translation units regardless.
inline jclass helix_activity_class(JNIEnv* env) {
    static std::mutex mutex;
    static jclass cached = nullptr;

    std::lock_guard<std::mutex> lock(mutex);
    if (cached) {
        return cached;
    }

    if (jobject activity = static_cast<jobject>(SDL_AndroidGetActivity())) {
        if (jclass local = env->GetObjectClass(activity)) {
            cached = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
        }
        env->DeleteLocalRef(activity);
    }

    if (!cached) {
        // No Activity yet (or SDL not initialized). FindClass still works from
        // the SDL main thread, which does have Java frames, so it is worth one
        // attempt before giving up.
        if (jclass local = env->FindClass("org/helixscreen/app/HelixActivity")) {
            cached = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
        } else {
            env->ExceptionClear();
        }
    }

    if (!cached) {
        spdlog::error("[android_jni] Failed to resolve HelixActivity class");
    }
    return cached;
}

} // namespace helix::android

#endif // __ANDROID__
