// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "socket_server_base.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace helix {

bool SocketServerBase::write_all(int fd, const char* buf, size_t len) {
    while (len > 0) {
        // MSG_NOSIGNAL: a client that closed its read end mid-write must yield
        // EPIPE, never a SIGPIPE that would terminate the whole process. (Same
        // pattern as src/system/label_printer_utils.cpp.)
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool SocketServerBase::start(RequestHandler handler) {
    if (running_.load()) {
        spdlog::warn("[RemoteControl] Transport already running");
        return false;
    }

    handler_ = std::move(handler);

    // Self-pipe to wake the accept loop out of poll() on shutdown.
    if (pipe(shutdown_pipe_) < 0) {
        spdlog::error("[RemoteControl] Failed to create shutdown pipe: {}", strerror(errno));
        return false;
    }

    listener_fd_ = create_listener();
    if (listener_fd_ < 0) {
        close(shutdown_pipe_[0]);
        close(shutdown_pipe_[1]);
        shutdown_pipe_[0] = shutdown_pipe_[1] = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread(&SocketServerBase::accept_loop, this);
    spdlog::info("[RemoteControl] Transport started on {}", endpoint());
    return true;
}

SocketServerBase::~SocketServerBase() {
    // Only when a subclass destructor did not already stop(): a completed stop()
    // joins the thread, and re-running the teardown would shutdown() a client fd
    // number the process may have handed to something else by now.
    if (accept_thread_.joinable()) {
        halt_accept_thread();
    }
}

void SocketServerBase::halt_accept_thread() {
    running_.store(false);

    // Wake the accept loop.
    if (shutdown_pipe_[1] >= 0) {
        char c = 'x';
        (void)write(shutdown_pipe_[1], &c, 1);
    }

    // Unblock a read()/write() in serve_client() on the current connection so
    // the accept thread returns promptly instead of stalling on the receive
    // timeout. shutdown() (not close()) leaves accept_loop to close the fd it
    // owns; a stale/-1 fd just yields a harmless ENOTCONN/EBADF.
    int cf = client_fd_.load();
    if (cf >= 0) {
        shutdown(cf, SHUT_RDWR);
    }

    // Close the listener to unblock accept().
    int lf = listener_fd_.exchange(-1);
    if (lf >= 0) {
        close(lf);
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    if (shutdown_pipe_[0] >= 0) {
        close(shutdown_pipe_[0]);
        shutdown_pipe_[0] = -1;
    }
    if (shutdown_pipe_[1] >= 0) {
        close(shutdown_pipe_[1]);
        shutdown_pipe_[1] = -1;
    }
}

void SocketServerBase::stop() {
    if (!running_.load()) {
        return;
    }

    halt_accept_thread();
    on_stopped();
    spdlog::debug("[RemoteControl] Transport stopped ({})", endpoint());
}

void SocketServerBase::accept_loop() {
    spdlog::debug("[RemoteControl] Accept loop started ({})", endpoint());

    while (running_.load()) {
        struct pollfd fds[2];
        fds[0].fd = listener_fd_;
        fds[0].events = POLLIN;
        fds[1].fd = shutdown_pipe_[0];
        fds[1].events = POLLIN;

        int ret = poll(fds, 2, -1); // Block indefinitely.
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (running_.load()) {
                spdlog::error("[RemoteControl] poll() failed: {}", strerror(errno));
            }
            break;
        }

        // Shutdown signalled.
        if (fds[1].revents & POLLIN) {
            break;
        }

        if (fds[0].revents & POLLIN) {
            int client_fd = accept(listener_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                if (running_.load() && errno != EINVAL) {
                    spdlog::warn("[RemoteControl] accept() failed: {}", strerror(errno));
                }
                continue;
            }

            spdlog::debug("[RemoteControl] Client connected (fd={})", client_fd);
            client_fd_.store(client_fd);
            serve_client(client_fd);
            client_fd_.store(-1);
            close(client_fd);
            spdlog::debug("[RemoteControl] Client disconnected");
        }
    }

    spdlog::debug("[RemoteControl] Accept loop ended ({})", endpoint());
}

} // namespace helix
