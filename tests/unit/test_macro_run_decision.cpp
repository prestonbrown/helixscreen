// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_macro_run_decision.cpp
 * @brief decide_macro_run(): how a macro click proceeds
 *
 * The pure rule four surfaces share (macro panel, favorite-macro widget,
 * filament router, quick buttons). Each caller maps the returned action onto
 * its own dialogs and lifetime handling; these tests pin the rule itself,
 * expressed through each caller's request shape.
 */

#include "macro_executor.h"

#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::CachedMacroInfo;
using helix::decide_macro_run;
using helix::MacroParam;
using helix::MacroParamKnowledge;
using helix::MacroRunAction;
using helix::MacroRunRequest;

namespace {

CachedMacroInfo no_params() {
    CachedMacroInfo cached;
    cached.knowledge = MacroParamKnowledge::KNOWN_NO_PARAMS;
    return cached;
}

CachedMacroInfo unknown_params() {
    return CachedMacroInfo{}; // default knowledge is UNKNOWN
}

CachedMacroInfo params(std::initializer_list<std::string> names) {
    CachedMacroInfo cached;
    cached.knowledge = MacroParamKnowledge::KNOWN_PARAMS;
    for (const std::string& name : names) {
        MacroParam param;
        param.name = name;
        cached.params.push_back(param);
    }
    return cached;
}

/// MacrosPanel: always willing to prompt; the Safety toggle gates a plain run.
MacroRunRequest panel_click(bool dangerous, bool safety_setting) {
    MacroRunRequest req;
    req.dangerous = dangerous;
    req.confirm_plain_run = safety_setting;
    return req;
}

/// MacrosPanel after the dangerous-macro dialog's Run button.
MacroRunRequest panel_after_dangerous(bool safety_setting) {
    MacroRunRequest req = panel_click(true, safety_setting);
    req.dangerous_confirmed = true;
    return req;
}

/// FavoriteMacroWidget first tap. The per-widget "Require Confirmation?"
/// opt-out clears both the param modal and the plain-run confirm.
MacroRunRequest widget_click(bool dangerous, bool require_confirmation, bool safety_setting) {
    MacroRunRequest req;
    req.dangerous = dangerous;
    req.prompt_for_params = require_confirmation;
    req.confirm_plain_run = require_confirmation && safety_setting;
    return req;
}

/// FavoriteMacroWidget after the dangerous dialog: require_confirmation_ and
/// the Safety toggle no longer apply.
MacroRunRequest widget_after_dangerous() {
    MacroRunRequest req;
    req.dangerous = true;
    req.dangerous_confirmed = true;
    return req;
}

/// Filament router: never dangerous, never plain-confirmed; the prefill comes
/// from the printer's live state.
MacroRunRequest router_click(bool suppress, std::map<std::string, std::string> known = {}) {
    MacroRunRequest req;
    req.prompt_for_params = !suppress;
    req.known_values = std::move(known);
    return req;
}

/// Quick buttons: no param modal ever; the Safety toggle is the only gate.
MacroRunRequest quick_button(bool safety_setting) {
    MacroRunRequest req;
    req.prompt_for_params = false;
    req.confirm_plain_run = safety_setting;
    return req;
}

} // namespace

// ============================================================================
// Rule 1: an unconfirmed dangerous macro beats everything else
// ============================================================================

TEST_CASE("dangerous and unconfirmed returns ConfirmDangerous regardless of anything else",
          "[macro][run_decision]") {
    SECTION("no params, no confirms requested") {
        CHECK(decide_macro_run(no_params(), panel_click(true, false)).action ==
              MacroRunAction::ConfirmDangerous);
    }
    SECTION("a full prefill does not outrank it") {
        // The router never passes dangerous; force it to prove the ordering.
        MacroRunRequest req = router_click(false, {{"TEMP", "210"}});
        req.dangerous = true;
        auto decision = decide_macro_run(params({"TEMP"}), req);
        CHECK(decision.action == MacroRunAction::ConfirmDangerous);
        CHECK(decision.params.empty());
    }
    SECTION("UNKNOWN knowledge") {
        CHECK(decide_macro_run(unknown_params(), panel_click(true, true)).action ==
              MacroRunAction::ConfirmDangerous);
    }
    SECTION("the widget opt-out cannot disarm it") {
        CHECK(decide_macro_run(params({"TEMP"}), widget_click(true, false, false)).action ==
              MacroRunAction::ConfirmDangerous);
        CHECK(decide_macro_run(no_params(), widget_click(true, false, false)).action ==
              MacroRunAction::ConfirmDangerous);
    }
}

// ============================================================================
// Rule 2: a plain run (no param modal will be raised)
// ============================================================================

TEST_CASE("KNOWN_NO_PARAMS runs, with a plain confirm only when asked and not dangerous",
          "[macro][run_decision]") {
    SECTION("no confirm requested") {
        auto decision = decide_macro_run(no_params(), panel_click(false, false));
        CHECK(decision.action == MacroRunAction::Run);
        CHECK(decision.params.empty());
    }
    SECTION("safety setting on") {
        CHECK(decide_macro_run(no_params(), panel_click(false, true)).action ==
              MacroRunAction::ConfirmRun);
    }
    SECTION("dangerous already confirmed suppresses the plain-run confirm") {
        auto decision = decide_macro_run(no_params(), panel_after_dangerous(true));
        CHECK(decision.action == MacroRunAction::Run);
        CHECK(decision.params.empty());
    }
}

TEST_CASE("prompt_for_params=false runs with empty params even for a params-taking macro",
          "[macro][run_decision]") {
    SECTION("KNOWN_PARAMS with a full prefill available still sends nothing") {
        auto decision = decide_macro_run(params({"TEMP", "SPEED"}),
                                         router_click(true, {{"TEMP", "210"}, {"SPEED", "50"}}));
        CHECK(decision.action == MacroRunAction::Run);
        CHECK(decision.params.empty());
    }
    SECTION("UNKNOWN") {
        auto decision = decide_macro_run(unknown_params(), quick_button(false));
        CHECK(decision.action == MacroRunAction::Run);
        CHECK(decision.params.empty());
    }
    SECTION("quick buttons with the safety setting on still confirm the plain run") {
        CHECK(decide_macro_run(params({"TEMP"}), quick_button(true)).action ==
              MacroRunAction::ConfirmRun);
        CHECK(decide_macro_run(unknown_params(), quick_button(true)).action ==
              MacroRunAction::ConfirmRun);
    }
}

// ============================================================================
// Rule 3: KNOWN_PARAMS - prefill restricted to declared names
// ============================================================================

TEST_CASE("a prefill covering every declared parameter runs without a prompt",
          "[macro][run_decision]") {
    auto decision = decide_macro_run(params({"TEMP", "SPEED"}),
                                     router_click(false, {{"TEMP", "210"}, {"SPEED", "50"}}));
    CHECK(decision.action == MacroRunAction::Run);
    CHECK(decision.params == std::map<std::string, std::string>{{"SPEED", "50"}, {"TEMP", "210"}});
}

TEST_CASE("a partial prefill prompts, carrying only the declared names it matched",
          "[macro][run_decision]") {
    SECTION("one of two known") {
        auto decision =
            decide_macro_run(params({"TEMP", "SPEED"}), router_click(false, {{"TEMP", "210"}}));
        CHECK(decision.action == MacroRunAction::Prompt);
        CHECK(decision.params == std::map<std::string, std::string>{{"TEMP", "210"}});
    }
    SECTION("known_values naming undeclared parameters are filtered out") {
        auto decision = decide_macro_run(params({"TEMP", "SPEED"}),
                                         router_click(false, {{"TEMP", "210"}, {"PURGE", "1"}}));
        CHECK(decision.action == MacroRunAction::Prompt);
        CHECK(decision.params == std::map<std::string, std::string>{{"TEMP", "210"}});
    }
    SECTION("only undeclared names offered - prefill ends up empty, so it prompts") {
        auto decision = decide_macro_run(params({"TEMP"}), router_click(false, {{"PURGE", "1"}}));
        CHECK(decision.action == MacroRunAction::Prompt);
        CHECK(decision.params.empty());
    }
    SECTION("panel and widget offer no prefill, so a params macro always prompts") {
        CHECK(decide_macro_run(params({"TEMP"}), panel_click(false, true)).action ==
              MacroRunAction::Prompt);
        CHECK(decide_macro_run(params({"TEMP"}), panel_click(false, true)).params.empty());
        CHECK(decide_macro_run(params({"TEMP"}), widget_click(false, true, true)).action ==
              MacroRunAction::Prompt);
    }
}

// ============================================================================
// Rule 4: UNKNOWN always prompts free-form, prefill ignored
// ============================================================================

TEST_CASE("UNKNOWN prompts free-form and ignores known_values", "[macro][run_decision]") {
    auto decision = decide_macro_run(unknown_params(), router_click(false, {{"TEMP", "210"}}));
    CHECK(decision.action == MacroRunAction::PromptUnknown);
    CHECK(decision.params.empty());
}

// ============================================================================
// Caller mappings
// ============================================================================

TEST_CASE("favorite widget: the per-widget opt-out runs anything with no dialog",
          "[macro][run_decision]") {
    for (bool safety : {false, true}) {
        CHECK(decide_macro_run(no_params(), widget_click(false, false, safety)).action ==
              MacroRunAction::Run);
        CHECK(decide_macro_run(params({"TEMP"}), widget_click(false, false, safety)).action ==
              MacroRunAction::Run);
        CHECK(decide_macro_run(unknown_params(), widget_click(false, false, safety)).action ==
              MacroRunAction::Run);
    }
}

TEST_CASE("favorite widget: confirmation on, no params - the safety setting decides",
          "[macro][run_decision]") {
    CHECK(decide_macro_run(no_params(), widget_click(false, true, true)).action ==
          MacroRunAction::ConfirmRun);
    CHECK(decide_macro_run(no_params(), widget_click(false, true, false)).action ==
          MacroRunAction::Run);
    CHECK(decide_macro_run(params({"TEMP"}), widget_click(false, true, false)).action ==
          MacroRunAction::Prompt);
    CHECK(decide_macro_run(unknown_params(), widget_click(false, true, true)).action ==
          MacroRunAction::PromptUnknown);
}

TEST_CASE("favorite widget after a dangerous confirm ignores every confirmation setting",
          "[macro][run_decision]") {
    // require_confirmation_ and the Safety toggle are not consulted past the
    // dangerous-macro dialog: KNOWN_NO_PARAMS runs straight through.
    CHECK(decide_macro_run(no_params(), widget_after_dangerous()).action == MacroRunAction::Run);
    CHECK(decide_macro_run(params({"TEMP"}), widget_after_dangerous()).action ==
          MacroRunAction::Prompt);
    CHECK(decide_macro_run(unknown_params(), widget_after_dangerous()).action ==
          MacroRunAction::PromptUnknown);
}

TEST_CASE("filament router: suppress runs a params-taking macro with no parameters",
          "[macro][run_decision]") {
    auto decision = decide_macro_run(params({"TEMP"}), router_click(true, {{"TEMP", "210"}}));
    CHECK(decision.action == MacroRunAction::Run);
    CHECK(decision.params.empty());
}
