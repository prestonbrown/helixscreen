// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_lan_client_authorization.cpp
 * @brief Tests for firmware-brokered LAN client authorization.
 *
 * Some firmwares ask the printer's own touchscreen to approve a slicer or
 * phone app before letting it in. helix::lan_auth owns which firmwares those
 * are, what they name the notification, and what the answer looks like; the
 * router asks those questions and never names a vendor. These tests exercise
 * the capability API and are the guard on that boundary.
 *
 * They also pin the two shapes that are quietly dangerous to get wrong: a
 * request with no client id must be dropped rather than answered (Moonraker
 * stringifies whatever it receives, so a null would authorize a client
 * literally named "None"), and the request id has to survive arriving as a
 * JSON number, which is how the stock screen sends it back.
 */

#include "../lvgl_test_fixture.h"
#include "lan_client_auth_router.h"
#include "lan_client_authorization.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::lan_auth::PendingRequest;
using nlohmann::json;

namespace {

constexpr const char* SNAPMAKER_NOTIFY = "notify_client_access";

/// A notification frame as Moonraker puts it on the wire: the component's
/// event payload wrapped in the params array.
json frame(const json& payload) {
    return json{
        {"jsonrpc", "2.0"}, {"method", SNAPMAKER_NOTIFY}, {"params", json::array({payload})}};
}

json snapmaker_payload(const std::string& client_id) {
    return json{{"id", "0"}, {"clientid", client_id}, {"app_id", "orca-1787643423061664"}};
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

// ============================================================================
// Subscription surface
// ============================================================================

TEST_CASE("lan auth: the notification is its own capability probe", "[lanauth]") {
    // No discovery gate exists by design — a firmware without a broker simply
    // never sends one. The router must still be told what to listen for.
    CHECK(contains(helix::lan_auth::notification_methods(), SNAPMAKER_NOTIFY));
}

TEST_CASE("lan auth: a notification from no provider is ignored", "[lanauth]") {
    // Every Moonraker notification reaches every websocket client; only the
    // provider's own method may be treated as an authorization request.
    CHECK_FALSE(helix::lan_auth::parse_request("notify_status_update",
                                               frame(snapmaker_payload("orca-abc"))));
}

// ============================================================================
// Parsing
// ============================================================================

TEST_CASE("lan auth: a well-formed Snapmaker request parses", "[lanauth]") {
    auto req =
        helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(snapmaker_payload("orca-abc")));

    REQUIRE(req);
    CHECK(req->provider == "Snapmaker");
    CHECK(req->client_id == "orca-abc");
    CHECK(req->app_id == "orca-1787643423061664");
    CHECK(req->request_id == "0");
}

TEST_CASE("lan auth: the request id survives arriving as a number", "[lanauth]") {
    // The stock screen formats it back with an unquoted "id":%s, so the field
    // round-trips as a JSON number even though the notification declares it a
    // string. Moonraker's get_str accepts either, so both are legal.
    json payload = snapmaker_payload("orca-abc");
    payload["id"] = 7;

    auto req = helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(payload));

    REQUIRE(req);
    CHECK(req->request_id == "7");
}

TEST_CASE("lan auth: a request with no client id is dropped, never answered", "[lanauth]") {
    // The trap this guards: Moonraker's get_str is str(val), so answering with
    // a null clientid authorizes a client literally named "None". Dropping is
    // the only safe response.
    json missing = snapmaker_payload("orca-abc");
    missing.erase("clientid");
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(missing)));

    json null_id = snapmaker_payload("orca-abc");
    null_id["clientid"] = nullptr;
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(null_id)));

    json empty_id = snapmaker_payload("orca-abc");
    empty_id["clientid"] = "";
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(empty_id)));
}

TEST_CASE("lan auth: malformed frames are dropped rather than crashing", "[lanauth]") {
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, json::object()));
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, json{{"params", json::array()}}));
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, json{{"params", "not-an-array"}}));
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY,
                                               json{{"params", json::array({"not-an-object"})}}));
    CHECK_FALSE(helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, json("not-an-object")));
}

TEST_CASE("lan auth: missing optional fields still yield an answerable request", "[lanauth]") {
    // Only the client id is load-bearing. A firmware that omits the rest still
    // gets an answer rather than a hang.
    auto req =
        helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(json{{"clientid", "orca-abc"}}));
    REQUIRE(req);
    CHECK(req->request_id.empty());
    CHECK(req->app_id.empty());
}

// ============================================================================
// Naming the requester
// ============================================================================

TEST_CASE("lan auth: the client id prefix names the asking product", "[lanauth]") {
    // Nothing else in the payload says who is asking, and the popup is asking
    // the user to trust it — so the prefix is what makes the prompt specific.
    auto orca = helix::lan_auth::parse_request(
        SNAPMAKER_NOTIFY, frame(snapmaker_payload("orca-9bbcbf74-a264-483e-bfff-22ffd07d6f70")));
    REQUIRE(orca);
    CHECK(orca->requester == "Snapmaker Orca");

    auto app = helix::lan_auth::parse_request(
        SNAPMAKER_NOTIFY, frame(snapmaker_payload("app-eb2b366a-464b-5a9f-97ab-dbf3e22437ca")));
    REQUIRE(app);
    CHECK(app->requester == "Snapmaker App");
}

TEST_CASE("lan auth: an unrecognized client is left unnamed, not guessed", "[lanauth]") {
    auto req =
        helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(snapmaker_payload("weird-1")));
    REQUIRE(req);
    CHECK(req->requester.empty());
}

// ============================================================================
// Building the answer
// ============================================================================

TEST_CASE("lan auth: approving mirrors the stock screen's own request", "[lanauth]") {
    auto req =
        helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(snapmaker_payload("orca-abc")));
    REQUIRE(req);

    auto decision = helix::lan_auth::build_decision(*req, true);

    REQUIRE(decision);
    CHECK(decision->method == "server.client_manager.approve");
    CHECK(decision->params["clientid"] == "orca-abc");
    CHECK(decision->params["app_id"] == "orca-1787643423061664");
    CHECK(decision->params["id"] == "0");
    CHECK(decision->params["approve"] == 1);
    // userid/username belong to the cloud-account path; sending them empty
    // would look like a request to bind an account named "".
    CHECK_FALSE(decision->params.contains("userid"));
    CHECK_FALSE(decision->params.contains("username"));
}

TEST_CASE("lan auth: denying is an explicit answer, not a silence", "[lanauth]") {
    // Denial is what turns the client's 70-second wait into an immediate
    // refusal, so it has to go on the wire like an approval does.
    auto req =
        helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(snapmaker_payload("orca-abc")));
    REQUIRE(req);

    auto decision = helix::lan_auth::build_decision(*req, false);

    REQUIRE(decision);
    CHECK(decision->method == "server.client_manager.approve");
    CHECK(decision->params["approve"] == 0);
    CHECK(decision->params["clientid"] == "orca-abc");
}

TEST_CASE("lan auth: an unknown id is omitted, not sent empty", "[lanauth]") {
    // The component runs the field through int(), which raises on "" and turns
    // the approval into {"state": "error"} — pairing would fail silently.
    // Omitted, it falls back to the component's own last_id.
    auto req =
        helix::lan_auth::parse_request(SNAPMAKER_NOTIFY, frame(json{{"clientid", "orca-abc"}}));
    REQUIRE(req);
    REQUIRE(req->request_id.empty());

    auto decision = helix::lan_auth::build_decision(*req, true);

    REQUIRE(decision);
    CHECK_FALSE(decision->params.contains("id"));
    CHECK(decision->params["clientid"] == "orca-abc");
    CHECK(decision->params["approve"] == 1);
}

TEST_CASE("lan auth: a request naming no provider cannot be answered", "[lanauth]") {
    PendingRequest orphan;
    orphan.client_id = "orca-abc";

    CHECK_FALSE(helix::lan_auth::build_decision(orphan, true));
}

// ============================================================================
// Prompt wording
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "lan auth: the prompt names the app when it can", "[lanauth]") {
    PendingRequest req;
    req.client_id = "orca-abc";
    req.requester = "Snapmaker Orca";

    std::string message = helix::LanClientAuthRouter::describe_request(req);

    CHECK(message.find("Snapmaker Orca") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "lan auth: an unnamed client gets a truthful prompt",
                 "[lanauth]") {
    // The user is being asked to trust this thing — the fallback must not
    // invent a product name, and must not render an empty placeholder.
    PendingRequest req;
    req.client_id = "weird-1";

    std::string message = helix::LanClientAuthRouter::describe_request(req);

    CHECK_FALSE(message.empty());
    CHECK(message.find("Snapmaker") == std::string::npos);
}

// ============================================================================
// Reading the firmware's answer
// ============================================================================

TEST_CASE("lan auth: only the firmware's own success counts as paired", "[lanauth]") {
    // The RPC completing means the component received the decision, not that
    // it acted on it — it answers {"state": "error"} for one it rejected, and
    // reporting that as a paired device is how a silent pairing failure looks
    // like a success on screen.
    using helix::LanClientAuthRouter;

    CHECK(LanClientAuthRouter::decision_succeeded(json{{"result", {{"state", "success"}}}}));

    CHECK_FALSE(LanClientAuthRouter::decision_succeeded(json{{"result", {{"state", "error"}}}}));
    // A reply that says nothing about the outcome is not a success.
    CHECK_FALSE(LanClientAuthRouter::decision_succeeded(json{{"result", json::object()}}));
    CHECK_FALSE(LanClientAuthRouter::decision_succeeded(json::object()));
    CHECK_FALSE(LanClientAuthRouter::decision_succeeded(json{{"result", "not-an-object"}}));
    // Non-string state must not be coerced into one.
    CHECK_FALSE(LanClientAuthRouter::decision_succeeded(json{{"result", {{"state", 1}}}}));
}

TEST_CASE("lan auth: the failure log can name the state the firmware sent", "[lanauth]") {
    // Split out from the predicate so a refused decision logs WHY rather than
    // just that it failed — that string is the whole diagnostic when pairing
    // silently does not happen.
    using helix::LanClientAuthRouter;

    CHECK(LanClientAuthRouter::decision_state(json{{"result", {{"state", "error"}}}}) == "error");
    CHECK(LanClientAuthRouter::decision_state(json::object()).empty());
}
