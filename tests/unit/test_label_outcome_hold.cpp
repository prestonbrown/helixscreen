// SPDX-License-Identifier: GPL-3.0-or-later
// A spool added to Spoolman prints its label at once, and the save's closing
// toast reports how that print went: one toast whichever finishes first.
#include "label_outcome_hold.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::LabelOutcomeHold;

namespace {
std::string describe(const LabelOutcomeHold::Outcome& o) {
    return o.printed ? "printed" : "failed: " + o.error;
}
} // namespace

TEST_CASE("a closing toast waits for the label print it reports", "[label_outcome]") {
    LabelOutcomeHold hold;
    std::vector<std::string> shown;
    hold.when_settled([&shown](const auto& o) { shown.push_back(describe(o)); });
    CHECK(shown.empty());

    hold.settle(false, "out of tape");
    REQUIRE(shown == std::vector<std::string>{"failed: out of tape"});
}

TEST_CASE("a label that printed before the close is reported at the close", "[label_outcome]") {
    LabelOutcomeHold hold;
    hold.settle(true, "");
    std::vector<std::string> shown;
    hold.when_settled([&shown](const auto& o) { shown.push_back(describe(o)); });
    REQUIRE(shown == std::vector<std::string>{"printed"});
}

TEST_CASE("one label outcome reaches one presentation", "[label_outcome]") {
    LabelOutcomeHold hold;
    int closing = 0;
    int fallback = 0;
    hold.when_settled([&closing](const auto&) { ++closing; });
    hold.when_settled([&fallback](const auto&) { ++fallback; });
    hold.settle(true, "");
    hold.settle(false, "late duplicate callback");
    CHECK(closing == 1);
    CHECK(fallback == 0);
}

TEST_CASE("a presentation that stops waiting hands the outcome to its fallback",
          "[label_outcome]") {
    LabelOutcomeHold hold;
    std::vector<std::string> closing;
    std::vector<std::string> own;
    hold.when_settled([&closing](const auto& o) { closing.push_back(describe(o)); });
    REQUIRE(hold.stop_waiting([&own](const auto& o) { own.push_back(describe(o)); }));
    hold.settle(true, "");
    CHECK(closing.empty());
    CHECK(own == std::vector<std::string>{"printed"});

    // Once the closing presentation has run there is nothing left to stop.
    LabelOutcomeHold answered;
    answered.when_settled([](const auto&) {});
    answered.settle(true, "");
    CHECK_FALSE(answered.stop_waiting([](const auto&) {}));
}
