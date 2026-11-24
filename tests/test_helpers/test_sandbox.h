// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// POSIX only: test_main.cpp calls these from a priority-101 constructor, before
// the C++ runtime is warmed up.

#include <cerrno>
#include <cstdio>
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

/// True when @p name is a pid no live process has.
inline bool names_dead_pid(const char* name) {
    if (name[0] == '\0') {
        return false;
    }
    long pid = 0;
    for (const char* c = name; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9' || pid > 100000000L) {
            return false;
        }
        pid = pid * 10 + (*c - '0');
    }
    if (pid <= 0) {
        return false;
    }
    return kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

/// Remove every run sandbox under @p parent whose run is gone: killed by the
/// shard timeout, a signal, or anything else that skipped its exit handlers.
/// Returns how many it removed.
inline int sweep_dead_sandboxes(const char* parent) {
    DIR* dir = opendir(parent);
    if (dir == nullptr) {
        return 0;
    }
    int removed = 0;
    char path[512];
    while (struct dirent* entry = readdir(dir)) {
        if (!names_dead_pid(entry->d_name)) {
            continue;
        }
        if (std::snprintf(path, sizeof(path), "%s/%s", parent, entry->d_name) <
            static_cast<int>(sizeof(path))) {
            remove_sandbox_tree(path);
            ++removed;
        }
    }
    closedir(dir);
    return removed;
}

} // namespace helix::test
