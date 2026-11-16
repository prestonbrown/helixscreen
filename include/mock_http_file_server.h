// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_MOCKS

#include <memory>
#include <string>

namespace helix {

/**
 * @brief Loopback HTTP server standing in for Moonraker's file endpoints under --test
 *
 * The mock MoonrakerClient answers WebSocket RPC, but thumbnails do not arrive
 * that way: `MoonrakerFileTransferAPI::download_thumbnail()` issues a real
 * `requests::get()` against the configured HTTP base URL, on an HttpExecutor
 * worker. With nothing listening, `--test` never downloads, decodes or
 * pre-scales a thumbnail — so the entire cold-fetch pipeline (download →
 * stb_image decode → resize → `.bin` write → cache eviction) was unreachable
 * outside a live printer.
 *
 * That gap is why the #960 heap-corruption hunt could not be pursued under a
 * sanitizer: the instrumented app exercised the UI half and never touched the
 * half the crash bundle implicated. Stubbing the transfer API would not have
 * closed it either — the point is to keep the real HTTP client, the real worker
 * threads and the real decode running, and only replace the server.
 *
 * Binds 127.0.0.1 on an ephemeral port so parallel instances (worktrees, test
 * shards) never collide. `--test` only; nothing here is compiled into a
 * production build.
 */
class MockHttpFileServer {
  public:
    MockHttpFileServer();
    ~MockHttpFileServer();

    MockHttpFileServer(const MockHttpFileServer&) = delete;
    MockHttpFileServer& operator=(const MockHttpFileServer&) = delete;

    /**
     * @brief Bind and start serving
     * @return true on success; false leaves base_url() empty and the caller
     *         should fall back to the configured host, not abort: a mock
     *         convenience must never break boot.
     */
    bool start();

    void stop();

    /// "http://127.0.0.1:<port>", or empty when not running.
    const std::string& base_url() const {
        return base_url_;
    }

    bool running() const {
        return running_;
    }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string base_url_;
    bool running_ = false;
};

} // namespace helix

#endif // HELIX_ENABLE_MOCKS
