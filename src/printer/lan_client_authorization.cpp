// SPDX-License-Identifier: GPL-3.0-or-later

#include "lan_client_authorization.h"

#include <spdlog/spdlog.h>

#include <iterator>

namespace helix::lan_auth {
namespace {

/// One firmware that brokers LAN client authorization.
struct Provider {
    const char* name;
    /// Moonraker notification the firmware broadcasts to the screen.
    const char* notification;
    /// JSON-RPC method that carries the screen's answer back.
    const char* decision_method;
    /// Pull a request out of the notification's params payload.
    std::optional<PendingRequest> (*parse)(const nlohmann::json& params);
    /// Build the decision params for a request this provider raised.
    nlohmann::json (*decision_params)(const PendingRequest& req, bool approve);
};

/// The notification's payload object, or nullptr.
///
/// Moonraker wraps every notification's event args in an array, so the object
/// is params[0] and not params itself.
const nlohmann::json* notification_payload(const nlohmann::json& msg) {
    if (!msg.is_object()) {
        return nullptr;
    }
    auto params = msg.find("params");
    if (params == msg.end() || !params->is_array() || params->empty()) {
        return nullptr;
    }
    const nlohmann::json& payload = (*params)[0];
    return payload.is_object() ? &payload : nullptr;
}

/// Read a string member, tolerating the number a firmware may send instead.
///
/// Snapmaker's own screen formats the request id back into the answer with
/// `"id":%s` - unquoted - so the field makes a round trip as a JSON number
/// even though the notification declares it a string. Moonraker's `get_str`
/// is `str(val)` and accepts either, so both shapes are legal on the wire and
/// this has to read both.
std::string string_member(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<long long>());
    }
    return {};
}

// --- Snapmaker (U1 and siblings running Moonraker's client_manager) ---------
//
// Snapmaker ships a `client_manager` Moonraker component that owns the whole
// pairing protocol: the client talks JSON-RPC to it over MQTT, and the only
// step it delegates to the screen is the human approval. It hands the screen a
// `notify_client_access` and waits for `server.client_manager.approve`; on
// approval it mints the client an mTLS certificate and publishes it back over
// MQTT, which is what actually admits Snapmaker Orca and the Snapmaker App.
//
// The client id doubles as the product tag: the component records clients as
// `orca-<uuid>` or `app-<uuid>`, and nothing else in the payload says who is
// asking. Matching the prefix is how the popup can name the app.
std::string snapmaker_requester(const std::string& client_id) {
    if (client_id.rfind("orca-", 0) == 0) {
        return "Snapmaker Orca"; // i18n: do not translate - product name
    }
    if (client_id.rfind("app-", 0) == 0) {
        return "Snapmaker App"; // i18n: do not translate - product name
    }
    return {};
}

std::optional<PendingRequest> parse_snapmaker(const nlohmann::json& payload) {
    PendingRequest req;
    req.client_id = string_member(payload, "clientid");
    if (req.client_id.empty()) {
        // Without it the answer cannot name a client, and approving with a
        // missing id is worse than dropping: Moonraker's get_str stringifies
        // whatever it receives, so a null would authorize the literal client
        // "None".
        return std::nullopt;
    }
    req.request_id = string_member(payload, "id");
    req.app_id = string_member(payload, "app_id");
    req.requester = snapmaker_requester(req.client_id);
    return req;
}

nlohmann::json snapmaker_decision_params(const PendingRequest& req, bool approve) {
    // Mirrors the stock screen's own request, minus the fields it only sends
    // on the cloud-account path (userid/username), which the component treats
    // as absent rather than empty.
    nlohmann::json params{
        {"clientid", req.client_id},
        {"app_id", req.app_id},
        {"approve", approve ? 1 : 0},
    };
    // OMIT an unknown id rather than sending "". The component runs the field
    // through int(), which raises on an empty string and turns the whole
    // approval into {"state": "error"} — a silent refusal to pair. Left out,
    // it falls back to the component's own last_id.
    if (!req.request_id.empty()) {
        params["id"] = req.request_id;
    }
    return params;
}

const Provider PROVIDERS[] = {
    {"Snapmaker", "notify_client_access", "server.client_manager.approve", parse_snapmaker,
     snapmaker_decision_params},
};

const Provider* provider_for_notification(const std::string& method) {
    for (const Provider& p : PROVIDERS) {
        if (method == p.notification) {
            return &p;
        }
    }
    return nullptr;
}

const Provider* provider_by_name(const std::string& name) {
    for (const Provider& p : PROVIDERS) {
        if (name == p.name) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

const std::vector<std::string>& notification_methods() {
    static const std::vector<std::string> methods = [] {
        std::vector<std::string> m;
        m.reserve(std::size(PROVIDERS));
        for (const Provider& p : PROVIDERS) {
            m.emplace_back(p.notification);
        }
        return m;
    }();
    return methods;
}

std::optional<PendingRequest> parse_request(const std::string& method,
                                            const nlohmann::json& msg) {
    const Provider* provider = provider_for_notification(method);
    if (!provider) {
        return std::nullopt;
    }
    const nlohmann::json* payload = notification_payload(msg);
    if (!payload) {
        spdlog::warn("[LanAuth] {} notification carried no payload object", provider->name);
        return std::nullopt;
    }
    std::optional<PendingRequest> req = provider->parse(*payload);
    if (!req) {
        spdlog::warn("[LanAuth] {} authorization request was malformed, ignoring",
                     provider->name);
        return std::nullopt;
    }
    req->provider = provider->name;
    return req;
}

std::optional<Decision> build_decision(const PendingRequest& req, bool approve) {
    const Provider* provider = provider_by_name(req.provider);
    if (!provider) {
        return std::nullopt;
    }
    return Decision{provider->decision_method, provider->decision_params(req, approve)};
}

} // namespace helix::lan_auth
