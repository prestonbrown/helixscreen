// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "main_loop_wake.h"

#include <fcntl.h>
#include <unistd.h>

namespace helix::main_loop_wake {

namespace {

struct WakePipe {
    int read_fd = -1;
    int write_fd = -1;

    WakePipe() {
        int fds[2];
        if (pipe(fds) != 0) {
            return;
        }
        for (int f : fds) {
            fcntl(f, F_SETFL, fcntl(f, F_GETFL) | O_NONBLOCK);
            fcntl(f, F_SETFD, FD_CLOEXEC);
        }
        read_fd = fds[0];
        write_fd = fds[1];
    }
};

/// Made on first use from any thread; function-local static initialization is serialized.
WakePipe& wake_pipe() {
    static WakePipe p;
    return p;
}

} // namespace

void notify() noexcept {
    const int w = wake_pipe().write_fd;
    if (w >= 0) {
        const char byte = 1;
        // A full pipe already wakes the reader, so a failed write loses nothing.
        [[maybe_unused]] const ssize_t n = write(w, &byte, 1);
    }
}

int fd() noexcept {
    return wake_pipe().read_fd;
}

void drain() noexcept {
    const int r = wake_pipe().read_fd;
    if (r < 0) {
        return;
    }
    char buf[64];
    while (read(r, buf, sizeof(buf)) > 0) {
    }
}

} // namespace helix::main_loop_wake
