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
    static int offers_shown(const helix::ui::PlrOfferController& c) {
        return c.offers_shown_;
    }
    /// The path the plugin's status reply takes, minus the RPC.
    static void plugin_status_arrived(helix::ui::PlrOfferController& c,
                                      const helix::PlrPluginStatus& status) {
        c.on_plugin_status(status);
    }
    static bool has_plugin_status(const helix::ui::PlrOfferController& c) {
        return c.plugin_status_.has_value();
    }
    /// The backend evaluate_offer resolves from the controller's signals now.
    static helix::PlrBackendType last_backend(const helix::ui::PlrOfferController& c) {
        return helix::plr_offer_availability(c.read_signals(), c.probe_).backend;
    }
};
