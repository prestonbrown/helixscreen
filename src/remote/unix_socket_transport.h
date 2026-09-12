// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file unix_socket_transport.h
 * @brief Local AF_UNIX transport for the remote-control server (default).
 *
 * Newline-delimited JSON-RPC over a Unix domain socket, restricted to the
 * owner (0600). This is the safe local default — no network exposure.
 */

#include "socket_server_base.h"

#include <string>
#include <vector>

namespace helix {

class UnixSocketTransport : public SocketServerBase {
  public:
    explicit UnixSocketTransport(std::string socket_path);

    /// Stops a still-running transport; also unlinks the socket file through on_stopped().
    ~UnixSocketTransport() override {
        stop();
    }

    std::string endpoint() const override {
        return socket_path_;
    }

    /**
     * @brief Is a live server currently listening on this AF_UNIX path?
     *
     * Distinguishes a socket file owned by a running instance from one left
     * behind by an unclean exit, by attempting a connect(). Success means a live
     * owner; ECONNREFUSED means the file is a leftover and safe to unlink.
     *
     * Needed because the two are indistinguishable by stat(): both are just a
     * socket file on disk. Unlinking without this check silently steals the path
     * from a running instance, which then keeps its listener open but becomes
     * unreachable — every later client connects to the newest process instead.
     *
     * @param path AF_UNIX socket path
     * @return true if something accepted a connection
     */
    [[nodiscard]] static bool path_is_live(const std::string& path);

    /**
     * @brief Live pid-suffixed instance sockets in @p dir, sorted.
     *
     * Matches `helixscreen-control-<something>.sock`, deliberately excluding the
     * bare well-known `helixscreen-control.sock`. A second instance parks itself on
     * such a path rather than stealing the well-known one, so this is how a client
     * finds the running app when the well-known path is absent or dead.
     *
     * Only paths that answer a connect() are returned — a crashed instance leaves
     * its file behind and would otherwise look like a live candidate.
     *
     * @param dir Directory to scan (typically $XDG_RUNTIME_DIR, else /tmp)
     * @return Sorted absolute paths; empty when none are live or dir is unreadable
     */
    [[nodiscard]] static std::vector<std::string> discover_instances(const std::string& dir);

    /**
     * @brief Unlink instance sockets whose owning process no longer exists.
     *
     * A SIGTERM fast-exit skips teardown by design (see
     * graceful_quit_signal_handler), so pid-suffixed socket files outlive their
     * process and accumulate. Nothing malfunctions — discover_instances() probes
     * before reporting — but the litter is unbounded, so each server start sweeps.
     *
     * Liveness is decided from the pid embedded in the filename, not a connect()
     * probe. A probe cannot distinguish a crashed instance from one that has
     * called bind() but not yet listen(), and unlinking the latter would hand its
     * path away — the very failure this class exists to prevent. Anything not
     * provably dead (non-numeric token, EPERM from another uid) is left alone.
     *
     * @param dir Directory to sweep (typically $XDG_RUNTIME_DIR, else /tmp)
     * @return Number of files removed
     */
    static int sweep_stale_instances(const std::string& dir);

  protected:
    int create_listener() override;
    void serve_client(int client_fd) override;
    void on_stopped() override;

  private:
    std::string socket_path_;
};

/**
 * @brief Directory that holds the control socket
 *
 * In preference order: systemd's `$RUNTIME_DIRECTORY`, then `$XDG_RUNTIME_DIR`,
 * then `/tmp`.
 *
 * `RuntimeDirectory=helixscreen` in `config/helixscreen.service` has to win.
 * That unit sets `ProtectSystem=strict`, which leaves `/tmp` read-only, so a
 * bind there fails on every systemd install while the launcher has already
 * logged that remote control is enabled (prestonbrown/helixscreen#1602). The
 * variable may name several directories, colon separated; the first is ours.
 *
 * Client and server both resolve through this, or `ctl` looks for the socket
 * somewhere the app never created it.
 */
std::string control_socket_dir();

/// The well-known control socket path, inside control_socket_dir().
std::string well_known_socket_path();

} // namespace helix
