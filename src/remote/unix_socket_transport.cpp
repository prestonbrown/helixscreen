// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "unix_socket_transport.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <optional>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

static constexpr size_t MAX_CLIENT_BUFFER = 65536; // 64KB max per request line.

namespace helix {

namespace {

constexpr const char* INSTANCE_PREFIX = "helixscreen-control-";
constexpr const char* INSTANCE_SUFFIX = ".sock";

/// The token between prefix and suffix in an instance socket name, if the name is
/// one. Shared by discovery and the stale sweep so they can never disagree about
/// what counts as an instance socket. Returns nullopt for the bare well-known
/// `helixscreen-control.sock`, whose ownership is decided separately.
std::optional<std::string> instance_token(const std::string& name) {
    const std::string prefix(INSTANCE_PREFIX);
    const std::string suffix(INSTANCE_SUFFIX);

    // Strictly greater: at least one character has to sit between the two, which
    // is exactly what excludes the bare well-known socket.
    if (name.size() <= prefix.size() + suffix.size()) {
        return std::nullopt;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    return name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
}

/// Is @p token an all-digit pid belonging to a process that still exists?
///
/// Deliberately conservative: anything we cannot prove dead counts as alive, so a
/// non-numeric token, an out-of-range number, or a kill() that fails with EPERM
/// (process exists, different uid) all report true and the file is left alone.
bool token_names_live_process(const std::string& token) {
    if (token.empty() || !std::all_of(token.begin(), token.end(),
                                      [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return true; // Not a pid we wrote; not ours to remove.
    }

    long pid = 0;
    try {
        pid = std::stol(token);
    } catch (const std::exception&) {
        return true; // Overflows a long — treat as unknown, keep the file.
    }
    if (pid <= 0) {
        return true;
    }

    // ESRCH is the only answer that proves the process is gone. EPERM means it
    // exists under another uid, and any other errno leaves us guessing.
    return !(kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH);
}

} // namespace

UnixSocketTransport::UnixSocketTransport(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

bool UnixSocketTransport::path_is_live(const std::string& path) {
    if (path.empty() || access(path.c_str(), F_OK) != 0) {
        return false; // Nothing there at all.
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        // Can't tell. Report "live" so callers err toward not stealing.
        return true;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.length() >= sizeof(addr.sun_path)) {
        close(fd);
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    const bool live = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return live;
}

std::vector<std::string> UnixSocketTransport::discover_instances(const std::string& dir) {
    std::vector<std::string> found;

    DIR* d = opendir(dir.c_str());
    if (!d) {
        return found;
    }

    while (struct dirent* ent = readdir(d)) {
        if (!instance_token(ent->d_name).has_value()) {
            continue;
        }
        std::string full = dir + "/" + ent->d_name;
        if (path_is_live(full)) {
            found.push_back(std::move(full));
        }
    }
    closedir(d);

    std::sort(found.begin(), found.end());
    return found;
}

int UnixSocketTransport::sweep_stale_instances(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return 0;
    }

    int removed = 0;
    while (struct dirent* ent = readdir(d)) {
        auto token = instance_token(ent->d_name);
        if (!token.has_value() || token_names_live_process(*token)) {
            continue;
        }

        // The owning pid is gone, so nothing can be serving this path and no
        // starting instance can be mid-bind on it either. Deciding by pid rather
        // than by a connect() probe matters: a probe cannot tell a crashed
        // instance from one that has bound but not yet called listen(), and
        // unlinking that second one would strand it exactly the way the
        // unconditional unlink this branch removed used to.
        const std::string full = dir + "/" + ent->d_name;
        if (unlink(full.c_str()) == 0) {
            spdlog::debug("[RemoteControl] Removed stale socket from dead pid {}: {}", *token,
                          full);
            ++removed;
        }
    }
    closedir(d);
    return removed;
}

int UnixSocketTransport::create_listener() {
    // Never take a path a running instance is serving. Unlinking unconditionally
    // used to hand the path to whichever process started last, leaving the earlier
    // one running but unreachable — and every subsequent client talking to the
    // wrong app. Callers pick a different path (see resolve_socket_path) rather
    // than racing for this one.
    if (path_is_live(socket_path_)) {
        spdlog::error("[RemoteControl] {} is already served by a live instance", socket_path_);
        return -1;
    }

    // Only now is unlinking safe: nothing answered, so this is a leftover from an
    // unclean exit and would otherwise fail bind() with EADDRINUSE.
    unlink(socket_path_.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        spdlog::error("[RemoteControl] Failed to create socket: {}", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socket_path_.length() >= sizeof(addr.sun_path)) {
        spdlog::error("[RemoteControl] Socket path too long: {}", socket_path_);
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    // Create the socket file owner-only from the moment bind() makes it, closing
    // the TOCTOU window between bind() and the chmod below where another local
    // user could connect to the control channel. bind() applies mode & ~umask.
    mode_t old_umask = umask(0077);
    int bind_rc = bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    int bind_errno = errno;
    umask(old_umask);
    if (bind_rc < 0) {
        spdlog::error("[RemoteControl] Failed to bind socket at {}: {}", socket_path_,
                      strerror(bind_errno));
        close(fd);
        return -1;
    }

    // Belt-and-suspenders: enforce owner-only even if the umask above was a no-op
    // on this platform's AF_UNIX permission semantics.
    chmod(socket_path_.c_str(), 0600);

    if (listen(fd, 1) < 0) {
        spdlog::error("[RemoteControl] Failed to listen on socket: {}", strerror(errno));
        close(fd);
        unlink(socket_path_.c_str());
        return -1;
    }

    return fd;
}

void UnixSocketTransport::serve_client(int client_fd) {
    std::string buffer;
    char chunk[4096];

    // Read timeout so a stalled client can't pin the accept thread forever.
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (running_.load()) {
        ssize_t n = read(client_fd, chunk, sizeof(chunk) - 1);
        if (n <= 0) {
            break; // Client disconnected or error.
        }
        chunk[n] = '\0';
        buffer.append(chunk, static_cast<size_t>(n));

        if (buffer.size() > MAX_CLIENT_BUFFER) {
            spdlog::warn("[RemoteControl] Client buffer overflow (>64KB), disconnecting");
            return;
        }

        // Process each complete newline-delimited request.
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty()) {
                continue;
            }

            std::string response = handler_(line);
            response += '\n';
            if (!write_all(client_fd, response.c_str(), response.length())) {
                spdlog::warn("[RemoteControl] Failed to write response: {}", strerror(errno));
                return;
            }
        }
    }
}

void UnixSocketTransport::on_stopped() {
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
        spdlog::debug("[RemoteControl] Removed socket file: {}", socket_path_);
    }
}

} // namespace helix
