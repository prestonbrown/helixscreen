// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Routing decisions for Moonraker events (#1219). The decision is pure — no
// LVGL, no clock, no globals — which is the whole point: it used to live inline
// in a lambda that ran on the libhv event-loop thread and called lv_tr() there.
// Pulling it out is what lets the caller apply lv_tr() on the main thread.
//
// These cases pin the routing table, including the two orderings that matter:
// recovery events must NOT be suppressible by the wizard, and
// deferred-discovery must be suppressed before the connection-failed check.

#include "moonraker_event_routing.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::decide_moonraker_event;
using helix::MoonrakerEventRoute;
using helix::MoonrakerEventSuppression;

namespace {
constexpr bool IS_ERROR = true;
constexpr bool NOT_ERROR = false;
constexpr bool WIZARD_UP = true;
constexpr bool NO_WIZARD = false;
constexpr bool MODAL_UP = true;
constexpr bool NO_MODAL = false;
} // namespace

TEST_CASE("Recovery events route to the unified dialog", "[moonraker][routing][1219]") {
    auto d = decide_moonraker_event(MoonrakerEventType::KLIPPY_DISCONNECTED, IS_ERROR, NO_WIZARD);
    REQUIRE(d.route == MoonrakerEventRoute::RecoveryDisconnected);

    d = decide_moonraker_event(MoonrakerEventType::KLIPPY_SHUTDOWN, IS_ERROR, NO_WIZARD);
    REQUIRE(d.route == MoonrakerEventRoute::RecoveryShutdown);
}

TEST_CASE("Recovery events survive the wizard", "[moonraker][routing][1219]") {
    // A disconnected or shut-down Klippy is not startup noise. If the wizard
    // check were hoisted above the recovery branch, the dialog would silently
    // not appear behind the setup wizard.
    for (auto type :
         {MoonrakerEventType::KLIPPY_DISCONNECTED, MoonrakerEventType::KLIPPY_SHUTDOWN}) {
        for (bool err : {IS_ERROR, NOT_ERROR}) {
            auto d = decide_moonraker_event(type, err, WIZARD_UP);
            INFO("type=" << static_cast<int>(type) << " is_error=" << err);
            REQUIRE(d.route != MoonrakerEventRoute::Ignore);
        }
    }
}

TEST_CASE("Connection failure gets the Change-Address prompt, not a toast",
          "[moonraker][routing][1219]") {
    auto d = decide_moonraker_event(MoonrakerEventType::CONNECTION_FAILED, IS_ERROR, NO_WIZARD);
    REQUIRE(d.route == MoonrakerEventRoute::ConnectionFailedModal);
    REQUIRE(std::string(d.title_tag) == "Connection Failed");
}

TEST_CASE("Connection failure degrades to a toast while a modal is open", "[moonraker][routing]") {
    // AD5X bundle 865DXBQ7: the latched CONNECTION_FAILED fires ~60 s after
    // startup, which on an unreachable printer is exactly when the user is in
    // Settings > Network typing a WiFi password to fix it. The prompt was pushed
    // at modal stack depth 2, over that keyboard, and the password had to be
    // retyped from scratch. A toast carries the same information without taking
    // the screen away.
    auto d = decide_moonraker_event(MoonrakerEventType::CONNECTION_FAILED, IS_ERROR, NO_WIZARD,
                                    MODAL_UP);
    REQUIRE(d.route == MoonrakerEventRoute::ErrorToast);
    REQUIRE(std::string(d.title_tag) == "Connection Failed");

    SECTION("and still gets the full prompt when nothing is open") {
        auto clear = decide_moonraker_event(MoonrakerEventType::CONNECTION_FAILED, IS_ERROR,
                                            NO_WIZARD, NO_MODAL);
        REQUIRE(clear.route == MoonrakerEventRoute::ConnectionFailedModal);
    }

    SECTION("an open modal does not suppress the event entirely") {
        // Degrade, never drop: the connection state has to reach the user
        // somehow, and this event fires once per session.
        REQUIRE(d.route != MoonrakerEventRoute::Ignore);
    }

    SECTION("an open modal does not reroute the recovery dialogs") {
        // Those are not "notifications" — a shut-down Klippy needs its dialog
        // whatever else is on screen.
        REQUIRE(decide_moonraker_event(MoonrakerEventType::KLIPPY_SHUTDOWN, IS_ERROR, NO_WIZARD,
                                       MODAL_UP)
                    .route == MoonrakerEventRoute::RecoveryShutdown);
        REQUIRE(decide_moonraker_event(MoonrakerEventType::KLIPPY_DISCONNECTED, IS_ERROR, NO_WIZARD,
                                       MODAL_UP)
                    .route == MoonrakerEventRoute::RecoveryDisconnected);
    }
}

TEST_CASE("Connection failure is suppressed entirely during the setup wizard",
          "[moonraker][routing][regression]") {
    // Bundle L53W5PKG, a fresh install on a standalone display: with no saved
    // host the app dials the 127.0.0.1 default, the latch fires 60 s later, and
    // the change-address prompt landed on top of the wizard's Language step. It
    // sat there 16.5 minutes before the user dismissed it and reached the
    // wizard's own Connection step, which is the UI that collects the address
    // and reports success or failure inline. Two host-entry prompts competing,
    // one of them telling a display that Klipper "runs on this printer".
    //
    // Suppressed rather than degraded to a toast: with a modal open the user is
    // doing something else and needs to learn the connection failed, but inside
    // the wizard they are already in the flow that fixes it.
    auto d = decide_moonraker_event(MoonrakerEventType::CONNECTION_FAILED, IS_ERROR, WIZARD_UP);
    REQUIRE(d.route == MoonrakerEventRoute::Ignore);
    REQUIRE(d.suppressed_because == MoonrakerEventSuppression::Wizard);

    SECTION("and stays suppressed with a modal open on top of the wizard") {
        auto with_modal = decide_moonraker_event(MoonrakerEventType::CONNECTION_FAILED, IS_ERROR,
                                                 WIZARD_UP, MODAL_UP);
        REQUIRE(with_modal.route == MoonrakerEventRoute::Ignore);
    }

    SECTION("but the prompt returns once the wizard is done") {
        auto after = decide_moonraker_event(MoonrakerEventType::CONNECTION_FAILED, IS_ERROR,
                                            NO_WIZARD, NO_MODAL);
        REQUIRE(after.route == MoonrakerEventRoute::ConnectionFailedModal);
    }
}

TEST_CASE("The wizard does not suppress other error toasts", "[moonraker][routing]") {
    // Only the connection prompt competes with the wizard's own Connection
    // step. An RPC failure during setup is still worth surfacing, and dropping
    // every error would hide real breakage behind the setup flow.
    auto d = decide_moonraker_event(MoonrakerEventType::RPC_ERROR, IS_ERROR, WIZARD_UP);
    REQUIRE(d.route == MoonrakerEventRoute::ErrorToast);
}

TEST_CASE("Deferred discovery is suppressed before the error routing",
          "[moonraker][routing][1219]") {
    auto d = decide_moonraker_event(MoonrakerEventType::DISCOVERY_DEFERRED, IS_ERROR, NO_WIZARD);
    REQUIRE(d.route == MoonrakerEventRoute::Ignore);
    REQUIRE(d.suppressed_because == MoonrakerEventSuppression::DiscoveryDeferred);
}

TEST_CASE("Error events carry the right untranslated title tag", "[moonraker][routing][1219]") {
    auto rpc = decide_moonraker_event(MoonrakerEventType::RPC_ERROR, IS_ERROR, NO_WIZARD);
    REQUIRE(rpc.route == MoonrakerEventRoute::ErrorToast);
    REQUIRE(std::string(rpc.title_tag) == "Request Failed");

    // Anything else that is an error falls back to the generic title.
    auto other = decide_moonraker_event(MoonrakerEventType::KLIPPY_READY, IS_ERROR, NO_WIZARD);
    REQUIRE(other.route == MoonrakerEventRoute::ErrorToast);
    REQUIRE(std::string(other.title_tag) == "Printer Error");
}

TEST_CASE("Title tags are returned untranslated", "[moonraker][routing][1219]") {
    // The regression this guards: if the decision ever calls lv_tr() itself, it
    // is back to translating on whatever thread raised the event. Source strings
    // are the English tags verbatim, and the routing TU must not link LVGL at all.
    REQUIRE(std::string(
                decide_moonraker_event(MoonrakerEventType::CONNECTION_FAILED, IS_ERROR, NO_WIZARD)
                    .title_tag) == "Connection Failed");
    REQUIRE(std::string(decide_moonraker_event(MoonrakerEventType::RPC_ERROR, IS_ERROR, NO_WIZARD)
                            .title_tag) == "Request Failed");
}

TEST_CASE("Non-error toasts are suppressed during the wizard", "[moonraker][routing][1219]") {
    // RECONNECTED is the non-error-class fixture: KLIPPY_READY short-circuits
    // to Ignore before the wizard check, so it cannot exercise this path.
    auto d = decide_moonraker_event(MoonrakerEventType::RECONNECTED, NOT_ERROR, WIZARD_UP);
    REQUIRE(d.route == MoonrakerEventRoute::Ignore);
    REQUIRE(d.suppressed_because == MoonrakerEventSuppression::Wizard);
}

TEST_CASE("Klipper-ready is never a notification", "[moonraker][routing][1219]") {
    // KLIPPY_READY is an internal lifecycle event, not a notification: the
    // recovery UI owns the user signal on the READY transition (dialog
    // dismissal, or the expected-restart success toast), and a warning toast
    // here would write a history row for good news.
    for (bool wizard : {NO_WIZARD, WIZARD_UP}) {
        for (bool modal : {NO_MODAL, MODAL_UP}) {
            INFO("wizard=" << wizard << " modal=" << modal);
            auto d =
                decide_moonraker_event(MoonrakerEventType::KLIPPY_READY, NOT_ERROR, wizard, modal);
            REQUIRE(d.route == MoonrakerEventRoute::Ignore);
            REQUIRE(d.suppressed_because == MoonrakerEventSuppression::KlippyReady);
        }
    }
}

TEST_CASE("Non-ready warnings still reach the warning-toast fallthrough",
          "[moonraker][routing][1219]") {
    // Only KLIPPY_READY is routed away as a lifecycle event; that short-circuit
    // must not swallow its non-error neighbours.
    auto d = decide_moonraker_event(MoonrakerEventType::RPC_ERROR, NOT_ERROR, NO_WIZARD);
    REQUIRE(d.route == MoonrakerEventRoute::WarningToast);
}

TEST_CASE("Routes that need no title report none", "[moonraker][routing][1219]") {
    // Guards against a caller passing nullptr into lv_tr(): every route that
    // yields a title must have one, and the rest must be explicit about not.
    struct Case {
        MoonrakerEventType type;
        bool is_error;
    };
    const Case cases[] = {
        {MoonrakerEventType::KLIPPY_DISCONNECTED, IS_ERROR},
        {MoonrakerEventType::KLIPPY_SHUTDOWN, IS_ERROR},
        {MoonrakerEventType::DISCOVERY_DEFERRED, IS_ERROR},
        {MoonrakerEventType::CONNECTION_FAILED, IS_ERROR},
        {MoonrakerEventType::RPC_ERROR, IS_ERROR},
        {MoonrakerEventType::KLIPPY_READY, NOT_ERROR},
    };
    for (const auto& c : cases) {
        auto d = decide_moonraker_event(c.type, c.is_error, NO_WIZARD);
        INFO("type=" << static_cast<int>(c.type));
        const bool needs_title = d.route == MoonrakerEventRoute::ErrorToast ||
                                 d.route == MoonrakerEventRoute::ConnectionFailedModal;
        REQUIRE(needs_title == (d.title_tag != nullptr));
    }
}
