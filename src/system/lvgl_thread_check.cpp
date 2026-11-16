// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/lvgl_thread_check.h"

#include "core/update_queue.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <unordered_set>

// Stack trace and thread names: glibc Linux / macOS only. Android NDK and musl
// (Creality K1) have neither execinfo.h nor pthread_getname_np.
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__))
#include <execinfo.h>
#define HELIX_THREAD_CHECK_HAS_BACKTRACE 1
#endif

namespace helix::lvgl_thread_check {

namespace {

std::atomic<Mode> g_mode{Mode::Off};

std::mutex g_sites_mutex;
std::size_t g_reports = 0; // guarded by g_sites_mutex
std::unordered_set<std::uint64_t>& reported_sites() {
    static auto* sites = new std::unordered_set<std::uint64_t>(); // outlives static teardown
    return *sites;
}

// Frames of the stack that identify a call site: the LVGL entry point and the
// callers above it. Several frames, not just the return address, because most
// subject writes go through a shared helper, and one key per helper would
// collapse every site behind it into one report.
constexpr int kSiteFrames = 5;
constexpr int kMaxFrames = 32;
// helix_lvgl_thread_check itself.
constexpr int kSkipFrames = 1;

std::uint64_t mix(std::uint64_t h, std::uintptr_t v) {
    h ^= static_cast<std::uint64_t>(v);
    return h * 0x100000001b3ULL;
}

void print_header(const char* entry, const void* caller, const char* verdict) {
    char when[32] = "";
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_buf);

    char thread_name[32] = "?";
#ifdef HELIX_THREAD_CHECK_HAS_BACKTRACE
    pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
#endif
    std::fprintf(stderr,
                 "\n[%s] [LvglThreadCheck] %s: %s called off the LVGL thread "
                 "(thread '%s', caller %p)\n",
                 when, verdict, entry, thread_name, caller);
    spdlog::error("[LvglThreadCheck] {}: {} called off the LVGL thread (thread '{}', caller {})",
                  verdict, entry, thread_name, caller);
}

} // namespace

Action decide(Mode mode, bool on_lvgl_thread, bool site_reported) noexcept {
    if (on_lvgl_thread) {
        return Action::None;
    }
    switch (mode) {
    case Mode::Off:
        return Action::None;
    case Mode::Report:
        return site_reported ? Action::None : Action::Report;
    case Mode::Abort:
        return Action::Abort;
    }
    return Action::None;
}

Mode parse_mode(const char* value, Mode fallback) noexcept {
    if (!value) {
        return fallback;
    }
    if (std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0) {
        return Mode::Off;
    }
    if (std::strcmp(value, "report") == 0) {
        return Mode::Report;
    }
    if (std::strcmp(value, "abort") == 0) {
        return Mode::Abort;
    }
    return fallback;
}

Mode mode_from_env(Mode fallback) noexcept {
    return parse_mode(std::getenv("HELIX_LVGL_THREAD_CHECK"), fallback);
}

void set_mode(Mode mode) noexcept {
    g_mode.store(mode, std::memory_order_release);
}

Mode current_mode() noexcept {
    return g_mode.load(std::memory_order_acquire);
}

std::size_t reported_site_count() noexcept {
    std::lock_guard<std::mutex> lock(g_sites_mutex);
    return g_reports;
}

void reset_reported_sites() noexcept {
    std::lock_guard<std::mutex> lock(g_sites_mutex);
    reported_sites().clear();
    g_reports = 0;
}

} // namespace helix::lvgl_thread_check

// NAMESPACE_OK: C linkage; the patched LVGL entry points call this symbol by name
extern "C" void helix_lvgl_thread_check(const char* entry, const void* caller) {
    using namespace helix::lvgl_thread_check;

    const Mode current = current_mode();
    if (current == Mode::Off || helix::ui::UpdateQueue::is_main_thread()) {
        return;
    }

    std::uint64_t key = mix(0xcbf29ce484222325ULL, reinterpret_cast<std::uintptr_t>(caller));
#ifdef HELIX_THREAD_CHECK_HAS_BACKTRACE
    void* frames[kMaxFrames];
    const int depth = ::backtrace(frames, kMaxFrames);
    for (int i = kSkipFrames; i < depth && i < kSkipFrames + kSiteFrames; ++i) {
        key = mix(key, reinterpret_cast<std::uintptr_t>(frames[i]));
    }
#endif

    bool site_reported = false;
    {
        std::lock_guard<std::mutex> lock(g_sites_mutex);
        site_reported = !reported_sites().insert(key).second;
    }

    const Action action = decide(current, false, site_reported);
    if (action == Action::None) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_sites_mutex);
        ++g_reports;
    }

    // One report at a time, and the header flushed before the frames: stderr may
    // be buffered, and backtrace_symbols_fd() writes the descriptor directly.
    static std::mutex print_mutex;
    std::lock_guard<std::mutex> print_lock(print_mutex);
    print_header(entry, caller, action == Action::Abort ? "ABORT" : "REPORT");
    std::fflush(stderr);
#ifdef HELIX_THREAD_CHECK_HAS_BACKTRACE
    if (depth > kSkipFrames) {
        ::backtrace_symbols_fd(frames + kSkipFrames, depth - kSkipFrames, STDERR_FILENO);
    }
#endif
    std::fflush(stderr);

    if (action == Action::Abort) {
        std::abort();
    }
}
