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
