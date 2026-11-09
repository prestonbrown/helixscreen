// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <cstring>
#include <sys/types.h>

namespace helix {

/// True when /proc/<pid>/comm names exactly @p expected. Guards a signal against
/// PID recycling: the kernel may have handed a reaped PID to an unrelated
/// process. Any uncertainty (bad PID, no /proc entry, unreadable file) is false.
/// The kernel truncates comm to 15 characters, so @p expected must fit in that.
inline bool proc_comm_is(pid_t pid, const char* expected) {
    if (pid <= 0) {
        return false;
    }
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", static_cast<int>(pid));
    FILE* f = fopen(comm_path, "r");
    if (!f) {
        return false;
    }
    char comm[64] = {0};
    const bool got_line = (fgets(comm, sizeof(comm), f) != nullptr);
    fclose(f);
    if (!got_line) {
        return false;
    }
    const size_t n = strlen(comm);
    if (n > 0 && comm[n - 1] == '\n') {
        comm[n - 1] = '\0';
    }
    return strcmp(comm, expected) == 0;
}

} // namespace helix
