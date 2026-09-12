// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// Live thread count for this process, and the identities behind it.
///
/// A test that spawns a background thread and does not join it before returning
/// leaks it; the loop later fires a callback on freed state and crashes a
/// *different* test. The cross-test isolation listener uses this to name the
/// leaking test, and individual tests use it to assert that a specific
/// operation is thread-neutral so a regression re-fires at the source rather
/// than as a nondeterministic crash somewhere downstream.
///
/// The count names the test the thread appeared under; it does not name the
/// thread. live_thread_identities() supplies that from /proc/self/task, so a
/// report can say which thread arrived rather than only how many.
///
/// macOS has no /proc, so proc_pidinfo/PROC_PIDTASKINFO is the equivalent for
/// the count: pti_threadnum is the live Mach thread count for the task
/// (prestonbrown/helixscreen#1146). There is no equivalent for the identities,
/// so callers there get an empty snapshot and fall back to the count alone.
///
/// live_thread_count() returns -1 if the count could not be determined.

#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#else
#include <cctype>
#include <dirent.h>
#include <fstream>
#endif

namespace helix::test {

inline int live_thread_count() {
#if defined(__APPLE__)
    struct proc_taskinfo ti;
    const int rc = proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &ti, sizeof(ti));
    if (rc == static_cast<int>(sizeof(ti))) {
        return static_cast<int>(ti.pti_threadnum);
    }
    return -1;
#else
    std::ifstream st("/proc/self/status");
    std::string line;
    while (std::getline(st, line)) {
        if (line.rfind("Threads:", 0) == 0) {
            return std::atoi(line.c_str() + 8);
        }
    }
    return -1;
#endif
}

/// One live thread: its kernel id, plus the two facts /proc will tell you
/// about it from the outside.
struct ThreadIdentity {
    long tid = 0;
    /// /proc/self/task/<tid>/comm. The kernel caps it at 15 characters, and a
    /// thread inherits its creator's until it calls pthread_setname_np — so in
    /// a binary that never does, every thread reads as the process name.
    std::string name;
    /// The kernel symbol the thread is parked in (/proc/self/task/<tid>/wchan):
    /// "ep_poll" for an event loop, "futex_wait" for a pool worker idling on a
    /// condition variable. Empty while the thread is running, and where the
    /// kernel withholds it. It is the only discriminator left when every comm
    /// in the process is the same string.
    std::string wchan;
};

/// Snapshot of every live thread in this process.
///
/// Empty where /proc is unavailable, which is the signal to fall back to the
/// count. Reading it costs an opendir plus one small file per thread, so it is
/// for the moment a report is being written, not for a per-test baseline.
inline std::vector<ThreadIdentity> live_thread_identities() {
    std::vector<ThreadIdentity> threads;
#if !defined(__APPLE__)
    DIR* dir = opendir("/proc/self/task");
    if (!dir) {
        return threads;
    }
    while (const dirent* entry = readdir(dir)) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) {
            continue; // "." and ".."
        }
        ThreadIdentity thread;
        thread.tid = std::atol(entry->d_name);
        const std::string dir_path = std::string("/proc/self/task/") + entry->d_name;
        // A thread that exits between readdir and here leaves no comm to read;
        // it is not one that outlived the test, so drop it.
        std::ifstream comm(dir_path + "/comm");
        if (!std::getline(comm, thread.name)) {
            continue;
        }
        std::ifstream wchan(dir_path + "/wchan");
        if (std::getline(wchan, thread.wchan) && thread.wchan == "0") {
            thread.wchan.clear(); // running, not parked anywhere nameable
        }
        threads.push_back(std::move(thread));
    }
    closedir(dir);
#endif
    return threads;
}

} // namespace helix::test
