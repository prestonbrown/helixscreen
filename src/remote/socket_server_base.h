// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file socket_server_base.h
 * @brief Shared accept-loop machinery for stream-socket transports.
 *
 * Both remote-control transports (AF_UNIX and AF_INET/HTTP) are stream servers
 * that differ only in how the listener is created and how a connection is
 * framed. This base owns the listener fd, a self-pipe for clean shutdown, and a
 * single accept thread; subclasses supply the listener and the per-connection
 * protocol.
 */

#include "remote_transport.h"

#include <atomic>
#include <thread>

namespace helix {

class SocketServerBase : public IRemoteTransport {
  public:
    /**
     * @brief Last-resort teardown for a transport destroyed while running
     *
     * Subclasses call stop() from their own destructor, which is the complete
     * teardown: it still reaches on_stopped() and endpoint() through virtual
     * dispatch. By the time this runs the derived object is gone and those are
     * unreachable, so all that is left to salvage is the accept thread - and it
     * has to be, because std::thread's destructor terminates the process when
     * it holds a joinable thread.
     */
    ~SocketServerBase() override;

    bool start(RequestHandler handler) override;
    void stop() override;

  protected:
    /**
     * @brief Create, bind, and listen; return the listener fd, or -1 on failure.
     * Called once from start() on the calling thread.
     */
    virtual int create_listener() = 0;

    /**
     * @brief Serve one accepted connection until the client disconnects.
     * Reads requests, feeds each to handler_, writes the responses.
     */
    virtual void serve_client(int client_fd) = 0;

    /**
     * @brief Optional cleanup after the listener has closed (e.g. unlink a
     * socket file). Called once from stop() after the accept thread joins.
     */
    virtual void on_stopped() {}

    // Blocking write of the full buffer; retries on EINTR. Shared by subclasses.
    static bool write_all(int fd, const char* buf, size_t len);

    RequestHandler handler_;
    std::atomic<bool> running_{false};

  private:
    void accept_loop();

    /// Teardown that touches no virtual member, so a destructor can run it.
    void halt_accept_thread();

    // Atomic: written by stop() on the caller thread, read by accept_loop().
    std::atomic<int> listener_fd_{-1};
    // The connection currently being served (or -1). stop() shuts it down to
    // unblock a serve_client() read() that would otherwise pin the accept thread
    // for the full receive timeout during teardown.
    std::atomic<int> client_fd_{-1};
    int shutdown_pipe_[2] = {-1, -1};
    std::thread accept_thread_;
};

} // namespace helix
