// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// POSIX only: test_main.cpp calls these from a priority-101 constructor, before
// the C++ runtime is warmed up.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <ftw.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace helix::test {

/// Parent of every helix-tests run's sandbox, one directory per run named by its
/// pid. One parent keeps the startup sweep a scan of this directory, never of
/// /tmp, and keeps a dead run's leftovers to one place the next run can find.
constexpr const char* kTestSandboxParent = "/tmp/helix-test-sandboxes";

namespace detail {
inline int remove_sandbox_entry(const char* path, const struct stat*, int, struct FTW*) {
    std::remove(path);
    return 0;
}
} // namespace detail

/// Remove @p path and everything under it, without following symlinks.
inline void remove_sandbox_tree(const char* path) {
    nftw(path, detail::remove_sandbox_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/// A sandbox whose owner looks dead is still left alone this long after its last
/// change. No harness today shares /tmp or /dev/shm across pid namespaces, but a
/// run in a container with a bind-mounted /tmp would see the host's live runs
/// as dead pids; an hour of litter is cheaper than deleting a live run.
constexpr int kSandboxSweepMinAgeSeconds = 3600;

/// The pid in @p name after @p prefix, or 0 when the rest is not all digits.
inline long sandbox_owner_pid(const char* name, const char* prefix) {
    while (*prefix != '\0') {
        if (*name++ != *prefix++) {
            return 0;
        }
    }
    if (*name == '\0') {
        return 0;
    }
    long pid = 0;
    for (const char* c = name; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9' || pid > 100000000L) {
            return 0;
        }
        pid = pid * 10 + (*c - '0');
    }
    return pid;
}

/// Remove every `<prefix><pid>` sandbox under @p parent whose run is gone
/// (killed by the shard timeout, a signal, or anything else that skipped its
/// exit handlers) and that has not changed for @p min_age_seconds.
/// Returns how many it removed.
inline int sweep_dead_sandboxes(const char* parent, const char* prefix = "",
                                int min_age_seconds = kSandboxSweepMinAgeSeconds) {
    DIR* dir = opendir(parent);
    if (dir == nullptr) {
        return 0;
    }
    const time_t now = time(nullptr);
    int removed = 0;
    char path[512];
    while (struct dirent* entry = readdir(dir)) {
        const long pid = sandbox_owner_pid(entry->d_name, prefix);
        if (pid <= 0 || kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) {
            continue;
        }
        if (std::snprintf(path, sizeof(path), "%s/%s", parent, entry->d_name) >=
            static_cast<int>(sizeof(path))) {
            continue;
        }
        struct stat st {};
        if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || now - st.st_mtime < min_age_seconds) {
            continue;
        }
        remove_sandbox_tree(path);
        ++removed;
    }
    closedir(dir);
    return removed;
}

/// Where HelixTestFixture's config sandbox (`helix-test-config-<pid>`) lives:
/// tmpfs when the host has one, else the temp directory.
inline const char* config_sandbox_base() {
    struct stat st {};
    if (stat("/dev/shm", &st) == 0 && S_ISDIR(st.st_mode)) {
        return "/dev/shm";
    }
    const char* tmp = getenv("TMPDIR");
    return tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp";
}

constexpr const char* kConfigSandboxPrefix = "helix-test-config-";

} // namespace helix::test
