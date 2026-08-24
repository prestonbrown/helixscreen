// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plr_offer_controller.h"

/**
 * @brief Friend access to PlrOfferController's single decision point.
 *
 * evaluate_offer() is the one place the controller decides; every observer
 * routes into it. Calling it directly is what lets a test assert the DECISION
 * without also asserting the modal rendering, which
 * tests/unit/test_plr_prompt.cpp deliberately does not cover.
 *
 * prompted_this_connect_ is the faithful proxy for "it offered": the controller
 * sets it on the success path only, immediately before showing the prompt.
 */
class PlrOfferControllerTestAccess {
  public:
    static void evaluate(helix::ui::PlrOfferController& c) {
        c.evaluate_offer();
    }
    static bool prompted(const helix::ui::PlrOfferController& c) {
        return c.prompted_this_connect_;
    }
};
