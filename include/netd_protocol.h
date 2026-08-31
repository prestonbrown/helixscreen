// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::netd {

/**
 * @brief Pure parser/encoder for the Forge-X netd wire protocol.
 *
 * netd is the WiFi/ethernet daemon on Forge-X firmware. It speaks a
 * newline-framed line protocol over an AF_UNIX SOCK_STREAM socket (default
 * /run/netd.sock). This module knows the protocol and nothing else: no LVGL,
 * no subjects, no threads, no long-lived sockets. Backends (wifi/ethernet)
 * are built on top of it in later phases.
 *
 * Daemon -> client line dispatch order (mirror of the first-party Python
 * client's parse_message()):
 *   1. parse_ack()      — "OK", "OK <text>", "ERR <reason>"
 *   2. parse_scan_row() — lines starting with "FREQUENCY=" containing
 *                         " NETWORK="
 *   3. parse_snapshot_line() — MODE=/STATE=/SSID=/SIGNAL=/IP=/REASON=/
 *                         PROGRESS=/ATTEMPT= (case-insensitive key)
 *   4. anything else    — silently ignored (forward compat: a newer daemon
 *                         may add fields and line shapes)
 */

/// One row of a SCAN reply.
struct ScanRow {
    std::string ssid;     ///< Decoded UTF-8 SSID.
    int frequency_mhz{};  ///< Center frequency in MHz (e.g. 2437, 5180).
    int signal_dbm{};     ///< Negative signal strength in dBm (e.g. -52).
    bool secured{};       ///< True when the SECURITY value contains "PSK".
    std::string security; ///< Raw SECURITY value (e.g. "WPA2-PSK", "NONE").
    bool saved{};         ///< True when SAVED=1 (known network).
};

/// Result of merging snapshot field lines. Raw strings, exactly as the
/// daemon sent them (SSID base64-decoded); empty means "absent".
struct NetdSnapshot {
    std::string mode, state, ssid, signal, ip, reason, progress, attempt;

    /// The two states in which the interface has an address and traffic flows.
    bool connected_state() const {
        return state == "CONNECTED" || state == "ONLINE";
    }
};

/// An OK/ERR acknowledgement line.
struct Ack {
    enum class Kind { None, Ok, Err };
    Kind kind = Kind::None;
    std::string text; ///< Text after "OK " / reason after "ERR ".

    bool ok() const {
        return kind == Kind::Ok;
    }
    bool err() const {
        return kind == Kind::Err;
    }
};

/**
 * @brief Classify an OK/ERR line.
 *
 * "OK" exactly -> Kind::Ok with empty text. "OK <text>" -> Kind::Ok with
 * text. "ERR <reason>" -> Kind::Err with the reason. Anything else ->
 * Kind::None.
 */
Ack parse_ack(const std::string& line);

/**
 * @brief Parse one daemon snapshot field line into @p out.
 *
 * Keys are matched case-insensitively; the FIRST '=' splits key from value;
 * the value is whitespace-trimmed; an SSID value is base64-decoded to UTF-8
 * (invalid base64 yields an empty string, never an error). Returns true iff
 * the line carried a KNOWN field, in which case ONLY that field of @p out is
 * updated — merge semantics: fields the line does not carry keep their
 * previous value. Unknown keys and other line shapes return false and leave
 * @p out untouched (forward compat).
 */
bool parse_snapshot_line(const std::string& line, NetdSnapshot& out);

/**
 * @brief Parse one SCAN reply row, or nullopt if the line is not a scan row.
 *
 * A scan row starts with "FREQUENCY=" and contains " NETWORK=". Tokens
 * before the marker are space-separated KEY=VALUE: FREQUENCY and SIGNAL must
 * both be present and parse as integers (otherwise the line is not a scan
 * row); SECURITY is kept as a string (secured() when it contains "PSK");
 * SAVED=1 means saved. The SSID is the base64 of everything after
 * " NETWORK=" — an empty decoded SSID means the line is not a scan row.
 * Unknown tokens are ignored.
 */
std::optional<ScanRow> parse_scan_row(const std::string& line);

/**
 * @brief Accumulates chunks from a stream socket and yields complete lines.
 *
 * Lines are '\n'-framed; a trailing '\r' (CRLF peers) is stripped. The
 * partial tail is carried across feed() calls. A partial exceeding 256 KiB
 * is discarded rather than hoarded (no legitimate netd line is that large,
 * and the first 8 KiB read assumption bounds a peer that never sends a
 * newline). Empty lines are skipped.
 */
class LineAssembler {
  public:
    /// Append @p chunk; returns the complete lines it completed (possibly none).
    std::vector<std::string> feed(std::string_view chunk);

    /// Drop any carried partial.
    void reset();

  private:
    std::string partial_;
};

// --- client -> daemon commands. Each returns the command WITHOUT a trailing
// newline; callers append it. -------------------------------------------------

std::string encode_get();
std::string encode_subscribe();
std::string encode_scan();
std::string encode_cancel();

/**
 * @brief True for ERR reasons that mean a credentials rejection.
 *
 * The one place that knows the daemon's auth-failure vocabulary; both the
 * sync error mapping and the async AUTH_FAILED event path classify through
 * this, so a renamed reason is fixed once.
 */
bool is_auth_failure_reason(const std::string& reason);

/**
 * @brief Encode a join request. Both parameters are base64-encoded so any
 * UTF-8 SSID or passphrase survives the line framing.
 *
 * For an open network pass an empty @p psk: the " psk=<b64>" token is then
 * omitted ENTIRELY (no trailing space), which is how netd distinguishes
 * open from secured joins.
 */
std::string encode_connect_wifi(const std::string& ssid, const std::string& psk);

// --- deployment probing. ------------------------------------------------------

/// netd control socket path: $HELIX_NETD_SOCKET if set and non-empty, else
/// "/run/netd.sock". Read from the environment PER CALL, never cached.
std::string socket_path();

/// netd binary path: $HELIX_NETD_BIN if set and non-empty, else
/// "/opt/config/mod/.bin/exec/netd". Read PER CALL, never cached.
std::string binary_path();

/**
 * @brief Cheap filesystem probe: is netd present on this machine?
 *
 * True iff socket_path() stats as a socket (S_IFSOCK) OR binary_path() is
 * executable (access X_OK). Never reads any version file — version strings
 * are untrustworthy on this platform.
 */
bool available();

// --- one-shot query. ----------------------------------------------------------

/**
 * @brief Connect to the daemon's socket with a bounded, non-blocking wait.
 *
 * A daemon wedged with a full accept backlog would park a BLOCKING connect
 * indefinitely — hanging the subscriber path's event loop, or pinning a
 * shared worker on the one-shot query path. Returns a CONNECTED,
 * still-nonblocking fd (the CALLER decides its final blocking mode), or -1
 * with @p error_out (when non-null) carrying the reason.
 */
int connect_unix(const std::string& path, int timeout_ms, std::string* error_out = nullptr);

struct QueryResult {
    bool reached{};        ///< False on any connect/write/read failure or timeout with no reply.
    bool saw_mode{};       ///< True when a reply line carried MODE=: only that answer is
                           ///< authoritative about which transport owns the link. A daemon
                           ///< that replied without it (an ERR verdict) said nothing
                           ///< authoritative, and its empty snapshot must not blank a row.
    NetdSnapshot snapshot; ///< Every snapshot line the daemon sent, merged.
};

/**
 * @brief Blocking one-shot snapshot fetch: connect to socket_path(), send
 * "GET\n", read until an ack, a short quiet period, or @p timeout_ms
 * elapses, then close.
 *
 * NEVER call this from the LVGL thread — it blocks for up to @p timeout_ms.
 * The ethernet adapter is the intended caller and runs it on a worker
 * thread. reached=true only when the daemon actually sent a response.
 */
QueryResult query_snapshot(int timeout_ms = 500);

} // namespace helix::netd
