// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file http_transport.h
 * @brief HTTP/TCP transport for the remote-control server (opt-in).
 *
 * A minimal, self-contained HTTP/1.1 server (no libhv HttpServer dependency):
 * POST a single JSON-RPC request to /rpc and receive the JSON-RPC response as
 * the body. Reachable from curl, browsers, or any HTTP client, and the base for
 * the post-1.0 web config UI. Binds loopback by default; LAN exposure is opt-in
 * via the bind host. Dev/test-only — compiled out of release builds.
 */

#include "socket_server_base.h"

#include <string>

namespace helix {

class HttpTransport : public SocketServerBase {
  public:
    HttpTransport(std::string bind_host, int port, std::string token);

    /// Stops a still-running transport; closes the listener and joins the accept thread.
    ~HttpTransport() override {
        stop();
    }

    std::string endpoint() const override {
        return "http://" + bind_host_ + ":" + std::to_string(port_) + "/rpc";
    }

  protected:
    int create_listener() override;
    void serve_client(int client_fd) override;

  private:
    std::string bind_host_;
    int port_;
    std::string token_;
};

} // namespace helix
