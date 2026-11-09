// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Counting how often a background thread wakes, from /proc. A thread that polls
// on a timeout shows tens of voluntary context switches in a few hundred
// milliseconds; one that blocks until it has work shows none.

#ifdef __linux__

#include "core/text_io.h"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace helix::test {

inline std::set<std::string> thread_ids() {
    std::set<std::string> ids;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task")) {
        ids.insert(e.path().filename().string());
    }
    return ids;
}

/// Threads of this process that `spawn` started and that are still running.
inline std::vector<std::string> threads_started_by(const std::function<void()>& spawn) {
    const auto before = thread_ids();
    spawn();
    std::vector<std::string> fresh;
    for (const auto& id : thread_ids()) {
        if (!before.count(id)) {
            fresh.push_back(id);
        }
    }
    return fresh;
}

/// voluntary_ctxt_switches of one thread of this process, or -1.
inline long thread_voluntary_switches(const std::string& tid) {
    helix::text_io::LineReader r("/proc/self/task/" + tid + "/status");
    std::string line;
    constexpr std::string_view kKey = "voluntary_ctxt_switches:";
    while (r.next(line)) {
        if (line.rfind(kKey, 0) == 0) {
            return std::strtol(line.c_str() + kKey.size(), nullptr, 10);
        }
    }
    return -1;
}

} // namespace helix::test

#endif
