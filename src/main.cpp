// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file main.cpp
 * @brief Application entry point
 *
 * This file is intentionally minimal. All application logic is implemented
 * in the Application class (src/application/application.cpp).
 *
 * @see Application
 */

#include "app_globals.h"
#include "application.h"
#include "async_lifetime_guard.h"
#include "data_root_resolver.h"
#include "helix_version.h"
#include "system/crash_handler.h"
#ifdef HELIX_ENABLE_REMOTE_CONTROL
#include "remote_client.h"
#endif

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <unistd.h>

// SDL2 redefines main → SDL_main via this header.
// On Android, the SDL Java activity loads libmain.so and calls SDL_main().
// Without this include, the symbol is missing and the app crashes on launch.
#ifdef HELIX_PLATFORM_ANDROID
#include <SDL.h>
#endif

// Log to stderr using only async-signal-safe-ish functions.
// spdlog may not be initialized yet or may be in a broken state.
static void log_fatal(const char* msg) {
    fprintf(stderr, "[FATAL] %s\n", msg);
    fflush(stderr);
}

// SDL's video and audio subsystems are independent: SDL_VIDEODRIVER=dummy
// selects the no-op video driver, but SDL still opens the real audio device
// (PulseAudio/PipeWire/ALSA) and audibly beeps through the desktop speakers.
// That is pure spam for a headless run driven by `helix-screen ctl` — the
// whole point of going headless is to NOT interact with the user. When the
// caller asked for the dummy video driver, also steer SDL's audio toward the
// dummy driver so headless stays headless. Mirrors the
// ForceDummyAudioDriver static initializer in tests/helix_test_fixture.cpp.
// overwrite=0 lets a developer opt back into a real audio driver by
// exporting SDL_AUDIODRIVER themselves. Must run before SoundManager
// initializes — SDL picks its audio driver when SDL_InitSubSystem(AUDIO)
// first runs (inside SDLSoundBackend::initialize()), which is well after
// main() starts.
static void silence_audio_if_headless() {
    const char* video = std::getenv("SDL_VIDEODRIVER");
    if (video && std::strcmp(video, "dummy") == 0) {
        ::setenv("SDL_AUDIODRIVER", "dummy", /*overwrite=*/0);
    }
}

// Called by std::terminate() — covers uncaught exceptions, joinable thread
// destruction, and other fatal C++ runtime errors. Logs what we can before
// the default terminate handler calls abort() (which triggers crash_handler).
static void terminate_handler() {
    // Guard against re-entrance (e.g. exception::what() throws). The reason — if
    // already determined — was stashed via crash_handler::set_terminate_context()
    // below, so this bare abort() still surfaces it as `terminate_msg:` in the
    // SIGABRT record instead of producing a blank crash (issue #987).
    static bool entered = false;
    if (entered) {
        abort();
    }
    entered = true;

    // Stash a placeholder reason up front so even a fault while inspecting the
    // exception (rethrow / what()) leaves a trace — refined with the real reason
    // once it's known below.
    crash_handler::set_terminate_context("std::terminate (reason capture in progress)");

    // Check if there's a current exception we can inspect
    const char* what = nullptr;
    if (auto eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            what = e.what();
            fprintf(stderr, "[FATAL] Uncaught exception: %s\n", what);
            fflush(stderr);
        } catch (...) {
            log_fatal("Uncaught non-std::exception");
            what = "non-std::exception";
        }
    } else {
        log_fatal("std::terminate() called without active exception "
                  "(joinable thread destroyed? noexcept violation?)");
        what = "std::terminate without active exception";
    }

    // Refine the stashed reason now that the real exception text is known, so a
    // re-fault inside write_exception_record() still reports it (issue #987).
    crash_handler::set_terminate_context(what);

    // Write crash file BEFORE abort — abort triggers the signal handler which
    // would overwrite it without the exception message.
    crash_handler::write_exception_record(what);

    // Encode signal death via POSIX 128+signum convention so the watchdog's
    // exit-code translation (helix_watchdog.cpp: "exited with code N (signal N
    // via crash handler)") classifies this as a crash and shows the recovery
    // dialog. _exit(1) used to be here but the watchdog treated it as a clean
    // non-zero exit ("not a crash") and restarted silently — crash.txt was
    // written but the user never saw the dialog.
    _exit(128 + SIGABRT);
}

int main(int argc, char** argv) {
#ifdef HELIX_ENABLE_REMOTE_CONTROL
    // Client subcommands dispatch before any app/display init: `helix-screen ctl
    // <cmd>` and `helix-screen repl` run the folded helixctl client and exit.
    // argv is forwarded with the subcommand as argv[0] (e.g. {"ctl", ...}).
    if (argc >= 2 && (strcmp(argv[1], "ctl") == 0 || strcmp(argv[1], "repl") == 0)) {
        return helix::remote_client_main(argc - 1, argv + 1);
    }
#endif

    // Before any subsystem that can touch SDL audio. With overwrite=0, this
    // only fires when the caller didn't pick an audio driver themselves.
    silence_audio_if_headless();

    // Record the main thread id before any thread that uses LifetimeToken
    // can spawn. The bg-thread expired() detector compares against this.
    helix::internal::set_main_thread_id();

    std::set_terminate(terminate_handler);

    int rc = 1;
    try {
        Application app;
        rc = app.run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "[FATAL] Unhandled exception in Application: %s\n", e.what());
        fflush(stderr);
        crash_handler::write_exception_record(e.what());
        _exit(128 + SIGABRT);
    } catch (...) {
        log_fatal("Unhandled non-std::exception in Application");
        crash_handler::write_exception_record("non-std::exception");
        _exit(128 + SIGABRT);
    }

    // In-place restart: if the UI asked for a restart, replace this process
    // image with a fresh instance.  Cleanup ran on the way out of app.run(),
    // so the lockfile is released and LVGL/display state is torn down — the
    // new instance comes up clean.  See app_globals.cpp app_request_restart().
    if (app_restart_after_quit_requested()) {
        char** new_argv = app_get_stored_argv();
        const char* exe = app_get_executable_path();
        if (exe && new_argv) {
            fprintf(stderr, "[App] In-place restart: execv(%s)\n", exe);
            fflush(stderr);
            execv(exe, new_argv);
            fprintf(stderr, "[App] execv failed: %s\n", strerror(errno));
            fflush(stderr);
            return 1;
        }
        fprintf(stderr, "[App] Restart requested but argv/exe unavailable\n");
        fflush(stderr);
    }
    return rc;
}
