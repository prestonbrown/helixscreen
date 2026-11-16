// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/plr_offer.h"
#include "moonraker_client.h" // ConnectionState

#include "../catch_amalgamated.hpp"

using helix::ConnectionState;
using helix::plr_should_offer;
using helix::plr_should_rearm;
using helix::PlrOfferSignals;

namespace {

// All-true baseline; individual tests flip one field at a time.
PlrOfferSignals all_true_signals() {
    PlrOfferSignals s;
    s.recovery_available = true;
    s.printer_idle = true;
    s.already_prompted = false;
    s.wizard_active = false;
    return s;
}

} // namespace

TEST_CASE("plr_should_offer: all signals true => offer", "[plr][offer]") {
    REQUIRE(plr_should_offer(all_true_signals()) == true);
}

TEST_CASE("plr_should_offer: no recovery snapshot => no offer", "[plr][offer]") {
    PlrOfferSignals s = all_true_signals();
    s.recovery_available = false;
    REQUIRE(plr_should_offer(s) == false);
}

TEST_CASE("plr_should_offer: printer not idle => no offer", "[plr][offer]") {
    PlrOfferSignals s = all_true_signals();
    s.printer_idle = false;
    REQUIRE(plr_should_offer(s) == false);
}

// Regression: the offer must fire for any valid idle recovery snapshot,
// independent of the filament/AMS backend. `pl_env_valid` is emitted only by
// Snapmaker's forked virtual_sdcard, so it is self-gating — an earlier
// redundant "AMS backend == SNAPMAKER" gate wrongly suppressed the offer on an
// AFC-modded U1 whose backend is not the Snapmaker one, even
// though the Snapmaker firmware PLR works and pl_env_valid goes true.
TEST_CASE("plr_should_offer fires for a valid idle snapshot regardless of filament backend",
          "[plr][offer]") {
    PlrOfferSignals s;
    s.recovery_available = true;
    s.printer_idle = true;
    s.already_prompted = false;
    s.wizard_active = false;
    REQUIRE(plr_should_offer(s) == true);

    // The only capability gate is the normalized recovery_available signal
    // itself (fed by pl_env_valid on Snapmaker).
    s.recovery_available = false;
    REQUIRE(plr_should_offer(s) == false);
}

TEST_CASE("plr_should_offer: already_prompted latch blocks re-offer", "[plr][offer]") {
    PlrOfferSignals s = all_true_signals();
    s.already_prompted = true;
    REQUIRE(plr_should_offer(s) == false);
}

TEST_CASE("plr_should_offer: wizard active blocks offer", "[plr][offer]") {
    PlrOfferSignals s = all_true_signals();
    s.wizard_active = true;
    REQUIRE(plr_should_offer(s) == false);
}

TEST_CASE("plr_should_offer: all good with wizard inactive still offers", "[plr][offer]") {
    PlrOfferSignals s = all_true_signals();
    s.wizard_active = false;
    REQUIRE(plr_should_offer(s) == true);
}

TEST_CASE("plr_should_rearm: CONNECTED -> DISCONNECTED re-arms", "[plr][offer][rearm]") {
    REQUIRE(plr_should_rearm(static_cast<int>(ConnectionState::CONNECTED),
                             static_cast<int>(ConnectionState::DISCONNECTED)) == true);
}

TEST_CASE("plr_should_rearm: CONNECTED -> RECONNECTING re-arms", "[plr][offer][rearm]") {
    REQUIRE(plr_should_rearm(static_cast<int>(ConnectionState::CONNECTED),
                             static_cast<int>(ConnectionState::RECONNECTING)) == true);
}

TEST_CASE("plr_should_rearm: DISCONNECTED -> CONNECTED does not re-arm", "[plr][offer][rearm]") {
    REQUIRE(plr_should_rearm(static_cast<int>(ConnectionState::DISCONNECTED),
                             static_cast<int>(ConnectionState::CONNECTED)) == false);
}

TEST_CASE("plr_should_rearm: CONNECTED -> CONNECTED does not re-arm", "[plr][offer][rearm]") {
    REQUIRE(plr_should_rearm(static_cast<int>(ConnectionState::CONNECTED),
                             static_cast<int>(ConnectionState::CONNECTED)) == false);
}
