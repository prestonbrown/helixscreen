// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Firmware-brokered LAN client authorization.
//
// Some firmwares put a pairing broker in front of their own control channel: a
// slicer or phone app that wants in does not get in on network reach alone, it
// files a request, and the firmware asks the printer's OWN TOUCHSCREEN to
// approve it. The screen is the second factor. Approve and the firmware mints
// that client its credentials; deny and it tells the client so, immediately.
//
// HelixScreen replaces the stock touchscreen on such machines, which makes it
// the thing being asked. Answer nothing and pairing simply hangs: the firmware
// broadcast its request and is waiting on a screen that no longer implements
// its half of the handshake. The client sits on "requesting connection" until
// it times out, with nothing on the printer to show why.
//
// This module is the ONLY place that knows which firmwares broker this, what
// they name the notification, and what shape the answer takes. The router that
// drives the UI asks these functions and never names a vendor.
//
// Adding a firmware means adding one Provider to the table in
// lan_client_authorization.cpp - no call site changes.
//
// Deliberately NOT gated on a capability probe. The notification IS the probe:
// a firmware without a broker never sends one, so the subscription is inert
// and costs a map entry. That keeps discovery, PrinterState and the
// subscription builder free of any knowledge that this feature exists.

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

namespace helix::lan_auth {

/// One client asking to be let in, normalized across firmwares.
struct PendingRequest {
    /// Provider that raised it. Routes the answer back to the right RPC, and
    /// names the firmware in logs.
    std::string provider;
    /// Opaque id echoed back with the decision.
    std::string request_id;
    /// Stable per-client identity the firmware keys stored credentials on.
    /// Also the dedup key: a client that retries reuses it.
    std::string client_id;
    /// Per-attempt id, echoed back so the client can correlate the answer.
    std::string app_id;
    /// Product name of the asking app ("Snapmaker Orca"), or empty when the
    /// firmware gave nothing recognizable. A proper noun - the caller injects
    /// it into a translated sentence rather than translating it.
    std::string requester;
};

/// The RPC that answers a request.
struct Decision {
    std::string method;
    nlohmann::json params;
};

/// Every notification method that can carry an authorization request. The
/// router subscribes to all of them; only a firmware that brokers pairing
/// ever sends one.
const std::vector<std::string>& notification_methods();

/// Parse one notification frame arriving on @p method. nullopt when the frame
/// belongs to no provider, or is malformed - a request with no client_id
/// cannot be answered, so it is dropped rather than shown to the user.
std::optional<PendingRequest> parse_request(const std::string& method, const nlohmann::json& msg);

/// The RPC that grants (@p approve true) or refuses @p req. nullopt when the
/// request names no known provider.
///
/// Answering late is safe and worth doing: the firmware stores the minted
/// credentials, so a client that already gave up waiting picks them up on its
/// next connection attempt. That is why neither this module nor its router
/// expires a pending request.
std::optional<Decision> build_decision(const PendingRequest& req, bool approve);

/// How long a denial suppresses re-prompting for that client. A denied client
/// never enters the firmware's registry, so every reconnect files a fresh
/// request and the screen would re-prompt forever; the window bounds a
/// mistaken Deny to one minute of quiet instead of a restart's worth.
constexpr auto denial_suppression_window = std::chrono::seconds(60);

/// True when a request from @p client_id arrives inside the suppression window
/// for a client the user denied. Pure in @p now so tests can walk the window
/// edge to edge. A client with no entry — never denied, approved since, or
/// beyond the window — is never suppressed: only an explicit Deny records an
/// entry, so a dismissal answered nothing and leaves the gate open
/// (prestonbrown/helixscreen#1376). Per-client so denying one client never
/// re-arms another's prompts.
bool suppressed_by_denial(
    const std::string& client_id,
    const std::unordered_map<std::string, std::chrono::steady_clock::time_point>& denied_clients,
    std::chrono::steady_clock::time_point now);

} // namespace helix::lan_auth
