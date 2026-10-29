// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

/// The Moonraker API key, and the one printer it belongs to.
///
/// A Moonraker with `force_logins` refuses every request that carries no
/// credential, trusted address or not, and accepts its API key in an
/// `X-Api-Key` header. A wrong key is refused just as surely, even from a
/// trusted client, so the key goes only to the host and port it was configured
/// for: never to another printer, a webcam or any other server.
namespace helix::moonraker_auth {

inline constexpr const char* kApiKeyHeader = "X-Api-Key";

struct Credential {
    std::string host;
    int port = 0;
    std::string api_key; ///< Empty: this printer takes no key.
};

/// Records the active printer's credential, replacing the previous one. A new
/// credential has not been refused yet, so this also clears unauthorized().
void set_credential(Credential credential);

/// A key as Moonraker issues one: exactly 32 hexadecimal digits. Moonraker
/// refuses a wrong key outright, so anything else is never worth sending.
[[nodiscard]] bool is_plausible_api_key(std::string_view key);

/// Moonraker refusing a request for want of a login: JSON-RPC code 401, or its
/// "Unauthorized" message where the code is missing.
[[nodiscard]] bool is_unauthorized_error(int code, std::string_view message);

/// The connected Moonraker refused this client's login. Every request fails the
/// same way until set_credential() records the next attempt. True when this call
/// is the one that marked it, so the refusal is reported once.
bool mark_unauthorized();

/// Whether the connected Moonraker has refused this client's login.
[[nodiscard]] bool unauthorized();

/// The key to send to @p host:@p port: the recorded key when that is the
/// printer it belongs to, else empty. Hosts compare case-insensitively.
[[nodiscard]] std::string key_for(std::string_view host, int port);

/// key_for() of the host and port in @p url ("ws://host:port/websocket",
/// "http://[::1]:7125/server/info"). Empty for a URL with no explicit port.
[[nodiscard]] std::string key_for_url(std::string_view url);

/// What a request to @p host:@p port does with its `X-Api-Key` header.
enum class HeaderAction {
    Leave,  ///< Not ours to touch: no key here, or another service's key.
    Set,    ///< Send the recorded key.
    Remove, ///< Carries the recorded key to a host it does not belong to.
};

struct HeaderDecision {
    HeaderAction action = HeaderAction::Leave;
    std::string key; ///< The value to send when action is Set.
};

/// @p current is the header's present value, empty when the request has none.
/// Decided under one lock, so a credential replaced mid-request cannot pair Set
/// with an empty key.
[[nodiscard]] HeaderDecision header_action(std::string_view host, int port,
                                           std::string_view current);

} // namespace helix::moonraker_auth
