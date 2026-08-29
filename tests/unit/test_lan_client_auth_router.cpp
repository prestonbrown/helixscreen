// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_lan_client_auth_router.cpp
 * @brief State-machine tests for LanClientAuthRouter.
 *
 * The pure layer (helix::lan_auth) is covered in
 * test_lan_client_authorization.cpp: what the notification looks like, what the
 * answer looks like, what is malformed. None of that says whether the SCREEN
 * behaves. This file drives the router itself — a fake transport in, a real
 * modal out — and pins the four things that only exist at this level:
 *
 *   - one prompt at a time, with a re-filing client refreshing it in place
 *   - a second client waiting rather than stacking a dialog on top
 *   - both answers reaching the wire, denial included
 *   - a prompt that goes away WITHOUT an answer releasing the gate
 *
 * That last one is the failure that has no symptom until much later: a
 * pending_ left set behind a dismissed dialog makes the router refuse every
 * subsequent request as "one is already awaiting an answer", and pairing on
 * that printer is dead until a restart.
 *
 * Threading note that every test here depends on: the router wraps its
 * notification handler in AsyncLifetimeGuard::bg_cb, so firing a notification
 * only ENQUEUES the handler. settle() is what actually runs it — assert before
 * that and you are asserting on an empty screen no matter what the router does.
 *
 * Everything here asserts through what the outside world can see — the modal
 * stack, the RPC the fake transport recorded, the fake's subscription book —
 * and drives the router through production entry points only. No test reaches
 * for a private member or pins which LVGL event carried the news, so the
 * router stays free to change how it hears about a dismissal.
 */

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../test_fixtures.h"
#include "lan_client_auth_router.h"
#include "lan_client_authorization.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::LanClientAuthRouter;
using helix::test::FakeMoonrakerClient;
using helix::ui::UpdateQueue;
using nlohmann::json;

namespace {

constexpr const char* SNAPMAKER_NOTIFY = "notify_client_access";
constexpr const char* APPROVE_METHOD = "server.client_manager.approve";
/// The handler name the router registers under, from its own .cpp.
constexpr const char* HANDLER_NAME = "lan_client_auth";

/// A notification frame as Moonraker puts it on the wire: the component's
/// event payload wrapped in the params array.
json access_frame(const json& payload) {
    return json{
        {"jsonrpc", "2.0"}, {"method", SNAPMAKER_NOTIFY}, {"params", json::array({payload})}};
}

json request_frame(const std::string& client_id, const std::string& app_id) {
    return access_frame(json{{"id", "0"}, {"clientid", client_id}, {"app_id", app_id}});
}

class LanAuthRouterFixture : public XMLTestFixture {
  public:
    LanAuthRouterFixture() {
        // modal_configure() silently no-ops without these, leaving the button
        // captions at their defaults — the app does this at startup.
        helix::ui::modal_init_subjects();
        REQUIRE(register_component("modal_dialog"));
    }

    ~LanAuthRouterFixture() override {
        while (lv_obj_t* top = ModalStack::instance().top_dialog()) {
            Modal::hide(top);
            settle();
        }
        settle();
    }

    /// Drain until the queue stops producing work. A drained callback can
    /// enqueue more, so a single drain leaves work in flight.
    void settle() {
        for (int i = 0; i < 16 && UpdateQueue::instance().pending_count() > 0; i++) {
            UpdateQueue::instance().drain();
        }
        UpdateQueue::instance().drain();
    }

    /// Deliver a notification the way the WebSocket thread would, then let the
    /// router's deferred handler actually run.
    /// @return whether anything was subscribed to receive it.
    bool deliver(FakeMoonrakerClient& client, const json& msg) {
        const bool fired = client.fire_notification(SNAPMAKER_NOTIFY, msg);
        settle();
        return fired;
    }

    /// Press a modal button by name and let the resulting work land.
    void press_button(lv_obj_t* dialog, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        settle();
    }
};

} // namespace

// ============================================================================
// Raising the prompt
// ============================================================================

TEST_CASE_METHOD(LanAuthRouterFixture, "lan auth router: a well-formed request raises the prompt",
                 "[lanauth][router]") {
    FakeMoonrakerClient client;
    LanClientAuthRouter router(&client);

    // A false here means the router never subscribed at all — the whole
    // "the prompt never appeared" failure mode starts there.
    REQUIRE(deliver(client, request_frame("orca-abc", "orca-1787643423061664")));

    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);

    // It has to be the router's prompt, not just any dialog: it names the
    // asking app and offers both answers.
    lv_obj_t* message = lv_obj_find_by_name(dialog, "dialog_message");
    REQUIRE(message != nullptr);
    CHECK(std::string(lv_label_get_text(message)).find("Snapmaker Orca") != std::string::npos);
    CHECK(lv_obj_find_by_name(dialog, "btn_primary") != nullptr);
    CHECK(lv_obj_find_by_name(dialog, "btn_secondary") != nullptr);
}

TEST_CASE_METHOD(LanAuthRouterFixture, "lan auth router: a malformed request raises nothing",
                 "[lanauth][router]") {
    FakeMoonrakerClient client;
    LanClientAuthRouter router(&client);

    // No clientid: unanswerable, so there is nothing to ask the user about.
    // Putting an un-actionable "Connection Request" on the printer's screen
    // would be worse than dropping it.
    REQUIRE(deliver(client, access_frame(json{{"id", "0"}, {"app_id", "orca-1"}})));

    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

// ============================================================================
// One request at a time
// ============================================================================

TEST_CASE_METHOD(LanAuthRouterFixture,
                 "lan auth router: a client re-filing refreshes its prompt in place",
                 "[lanauth][router]") {
    FakeMoonrakerClient client;
    LanClientAuthRouter router(&client);

    REQUIRE(deliver(client, request_frame("orca-abc", "orca-first")));
    lv_obj_t* prompt = ModalStack::instance().top_dialog();
    REQUIRE(prompt != nullptr);

    // Orca re-files roughly every time it reconnects, with a fresh app_id per
    // attempt. Repeats are the norm here, not an error.
    REQUIRE(deliver(client, request_frame("orca-abc", "orca-second")));
    CHECK(ModalStack::instance().top_dialog() == prompt);

    press_button(prompt, "btn_primary");

    // Nothing was stacked behind the prompt — one answer emptied the screen.
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    // And the answer carries the NEWEST attempt id, which is the one still
    // listening for it.
    REQUIRE(client.rpc_count(APPROVE_METHOD) == 1);
    const auto* rpc = client.last_rpc();
    REQUIRE(rpc != nullptr);
    CHECK(rpc->params["app_id"] == "orca-second");
}

TEST_CASE_METHOD(LanAuthRouterFixture,
                 "lan auth router: a second client waits rather than stacking a dialog",
                 "[lanauth][router]") {
    FakeMoonrakerClient client;
    LanClientAuthRouter router(&client);

    REQUIRE(deliver(client, request_frame("orca-abc", "orca-1")));
    lv_obj_t* prompt = ModalStack::instance().top_dialog();
    REQUIRE(prompt != nullptr);

    // A different client while one prompt is up. Two dialogs would need two
    // people standing at one printer to make sense of.
    REQUIRE(deliver(client, request_frame("app-phone", "app-1")));
    CHECK(ModalStack::instance().top_dialog() == prompt);

    press_button(prompt, "btn_primary");

    CHECK(ModalStack::instance().top_dialog() == nullptr);
    // The one answer belongs to the client that was actually being asked
    // about — approving the wrong one would admit a device nobody looked at.
    REQUIRE(client.rpc_count(APPROVE_METHOD) == 1);
    const auto* rpc = client.last_rpc();
    REQUIRE(rpc != nullptr);
    CHECK(rpc->params["clientid"] == "orca-abc");
}

// ============================================================================
// Answering
// ============================================================================

TEST_CASE_METHOD(LanAuthRouterFixture, "lan auth router: Allow approves the asking client",
                 "[lanauth][router]") {
    FakeMoonrakerClient client;
    LanClientAuthRouter router(&client);

    REQUIRE(deliver(client, request_frame("orca-abc", "orca-1787643423061664")));
    lv_obj_t* prompt = ModalStack::instance().top_dialog();
    REQUIRE(prompt != nullptr);

    press_button(prompt, "btn_primary");

    REQUIRE(client.rpc_count(APPROVE_METHOD) == 1);
    const auto* rpc = client.last_rpc();
    REQUIRE(rpc != nullptr);
    CHECK(rpc->method == APPROVE_METHOD);
    CHECK(rpc->params["approve"] == 1);
    CHECK(rpc->params["clientid"] == "orca-abc");
    CHECK(rpc->params["app_id"] == "orca-1787643423061664");
    // The prompt got out of the way; it has nothing left to ask.
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

TEST_CASE_METHOD(LanAuthRouterFixture, "lan auth router: Deny puts the refusal on the wire",
                 "[lanauth][router]") {
    FakeMoonrakerClient client;
    LanClientAuthRouter router(&client);

    REQUIRE(deliver(client, request_frame("orca-abc", "orca-1")));
    lv_obj_t* prompt = ModalStack::instance().top_dialog();
    REQUIRE(prompt != nullptr);

    press_button(prompt, "btn_secondary");

    // Silence is not a denial: saying nothing leaves the client sitting on
    // "requesting connection" until it times out. The refusal has to be sent.
    REQUIRE(client.rpc_count(APPROVE_METHOD) == 1);
    const auto* rpc = client.last_rpc();
    REQUIRE(rpc != nullptr);
    CHECK(rpc->method == APPROVE_METHOD);
    CHECK(rpc->params["approve"] == 0);
    CHECK(rpc->params["clientid"] == "orca-abc");
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

// ============================================================================
// The gate cannot wedge shut
// ============================================================================

TEST_CASE_METHOD(LanAuthRouterFixture,
                 "lan auth router: a prompt dismissed without an answer releases the gate",
                 "[lanauth][router]") {
    FakeMoonrakerClient client;
    LanClientAuthRouter router(&client);

    REQUIRE(deliver(client, request_frame("orca-abc", "orca-1")));
    lv_obj_t* prompt = ModalStack::instance().top_dialog();
    REQUIRE(prompt != nullptr);

    // The production dismissal: Modal::rebuild_top() is what takes these
    // prompts down on a breakpoint or theme change, with neither button
    // pressed. Drive that rather than deleting the widget by hand — HOW the
    // router notices is its own business, and a test that pins the mechanism
    // breaks on a refactor that keeps the behaviour.
    Modal::rebuild_top();

    // Teardown is not immediate: the dialog goes out through an exit animation
    // and a deferred delete, so the virtual clock has to advance and the queue
    // has to drain before the router can have heard about it. Asserting here
    // without the pump would pass against a router that never noticed at all.
    REQUIRE(wait_until([&] { return ModalStack::instance().stack_empty(); }, 2000));
    process_lvgl(50);
    settle();

    // Nothing was answered; the client is still waiting and will re-file.
    CHECK(client.rpc_count(APPROVE_METHOD) == 0);

    // The regression this guards: with the pending request left set behind the
    // dismissed dialog, every later request is refused as "one is already
    // awaiting an answer" and pairing is dead until a restart.
    REQUIRE(deliver(client, request_frame("app-phone", "app-1")));
    lv_obj_t* second = ModalStack::instance().top_dialog();
    REQUIRE(second != nullptr);

    press_button(second, "btn_primary");
    REQUIRE(client.rpc_count(APPROVE_METHOD) == 1);
    const auto* rpc = client.last_rpc();
    REQUIRE(rpc != nullptr);
    CHECK(rpc->params["clientid"] == "app-phone");
}

// ============================================================================
// Teardown
// ============================================================================

TEST_CASE_METHOD(LanAuthRouterFixture, "lan auth router: the destructor stops listening",
                 "[lanauth][router]") {
    // Application replaces the router on a printer switch. A subscription left
    // behind points at a dead object, and the transport does not block an
    // in-flight delivery for us.
    FakeMoonrakerClient client;
    {
        LanClientAuthRouter router(&client);
        for (const std::string& method : helix::lan_auth::notification_methods()) {
            REQUIRE(client.method_callbacks.count(method) == 1);
            const auto& handlers = client.method_callbacks.at(method);
            REQUIRE(handlers.size() == 1);
            CHECK(handlers[0].handler_name == HANDLER_NAME);
        }
    }
    settle();

    for (const std::string& method : helix::lan_auth::notification_methods()) {
        CHECK(client.was_unregistered(method, HANDLER_NAME));
    }
    // The record is only half of it — the subscription itself has to be gone,
    // so a notification arriving afterwards reaches nothing.
    CHECK_FALSE(client.fire_notification(SNAPMAKER_NOTIFY, request_frame("orca-abc", "orca-1")));
    settle();
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}
