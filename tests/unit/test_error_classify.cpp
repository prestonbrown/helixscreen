// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_event.h"

#include "catch_amalgamated.hpp"

using helix::ErrorEvent;
using helix::ErrorSeverity;
using helix::ErrorSource;

TEST_CASE("ErrorEvent defaults are safe", "[error-center][model]") {
    ErrorEvent e;
    REQUIRE(e.severity == ErrorSeverity::WARNING); // conservative default
    REQUIRE(e.source == ErrorSource::GENERIC);
    REQUIRE(e.title.empty());
    REQUIRE(e.detail.empty());
    REQUIRE(e.code.empty());
    REQUIRE(e.recovery_actions.empty());
    REQUIRE_FALSE(e.sticky);
}

#include "error_classify.h"
using helix::ClassifyContext;
using helix::error_classify::classify;

TEST_CASE("uncoded jam !! while paused is CRITICAL", "[error-center][classify]") {
    ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = classify("!! Toolhead runout detected by tool_end sensor, but upstream "
                      "sensors still detect filament. Possible filament break or jam "
                      "at the toolhead. Please clear the jam and reload filament "
                      "manually, then resume the print.",
                      ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::GENERIC);
    REQUIRE(e->code.empty());
    REQUIRE(e->detail.find("reload filament manually") != std::string::npos);
    REQUIRE(e->detail.size() > 80);
}

TEST_CASE("uncoded !! while idle is WARNING", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify("!! Timer too close", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
}

// ---- #1152: a paused job must never get a text-only, button-less modal ----

TEST_CASE("uncoded !! while paused carries Resume plus a dismiss", "[error-center][classify]") {
    ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = classify("!! Some fault nobody classified", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    // Attribution is unchanged: these actions belong to no backend. Source stays
    // GENERIC and the title stays empty so modal_title_for() reads "Printer Error".
    REQUIRE(e->source == helix::ErrorSource::GENERIC);
    REQUIRE(e->title.empty());

    REQUIRE(e->recovery_actions.size() == 2);
    REQUIRE(e->recovery_actions[0].label == "Resume");
    REQUIRE(e->recovery_actions[0].gcode == "RESUME");
    // The second action is the way OUT. ActionPromptModal has no intrinsic close
    // affordance, so a lone Resume would trap a user who does not want to resume.
    REQUIRE(e->recovery_actions[1].label == "OK");
    // An EMPTY gcode is the dismiss spelling: the modal closes and sends
    // nothing. This used to have to be a Klipper comment, because a blank
    // gcode made create_buttons() send the LABEL as a command — so "OK" went
    // to Klipper. That fallback is gone (#1172).
    REQUIRE(e->recovery_actions[1].gcode.empty());

    // Neither button is styled primary: the cause is unknown, so the UI must not
    // visually push the user toward resuming.
    for (const auto& a : e->recovery_actions) {
        REQUIRE(a.style != "primary");
        REQUIRE(a.style.empty()); // neutral, not danger either
    }
}

TEST_CASE("uncoded !! while printing but not paused offers nothing", "[error-center][classify]") {
    // There is nothing to resume while the print is still running, and no
    // filament move belongs on a line whose cause is unknown. Severity is
    // unchanged from the pre-#1152 rule.
    ClassifyContext ctx;
    ctx.is_printing = true;
    auto e = classify("!! Some fault nobody classified", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->recovery_actions.empty());
}

TEST_CASE("uncoded !! while idle is unchanged by the Resume affordance",
          "[error-center][classify]") {
    ClassifyContext ctx; // neither paused nor printing
    auto e = classify("!! Timer too close", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING); // no escalation
    REQUIRE(e->recovery_actions.empty());
    REQUIRE_FALSE(e->sticky);
}

TEST_CASE("coded !! while paused keeps its code-derived action", "[error-center][classify]") {
    // The generic Resume lives on the uncoded arm only: a coded error still
    // takes the code branch, keeps its own recovery and its CFS attribution.
    // Nothing is appended and nothing is displaced.
    ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = classify(R"(!! {"code":"key840","msg":"box switch state error"})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->code == "key840");
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->recovery_actions.size() == 1);
    REQUIRE(e->recovery_actions[0].gcode == "BOX_ERROR_CLEAR");
}

TEST_CASE("Error: command error while paused gets no Resume", "[error-center][classify]") {
    // The affordance is scoped to the uncoded `!!` arm. A rejected command is a
    // WARNING toast; giving it an action would route it to TOAST_WITH_RECOVER,
    // whose presenter is hard-wired to the key298 recovery service.
    ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = classify("Error: Must home axis first", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->source == helix::ErrorSource::KLIPPER);
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->recovery_actions.empty());
}

TEST_CASE("CFS key8xx is CRITICAL", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key849","msg":"retract failed","values":[1]})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->code == "key849");
}

TEST_CASE("key843 is a WARNING toast, not a blocking modal", "[error-center][classify][1387]") {
    // key843 is the RFID-read failure: most often the box answering busy
    // while mid-operation rather than a broken tag (#1387). It self-resolves
    // when the box frees (the deferred insert probe lands) or the user re-seats
    // a genuinely bad tag, so the key8xx blanket CRITICAL promotion is wrong
    // here: CRITICAL with no actions routes to a blocking modal.
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key843","msg":"read rfid failed","values":[1,"B"]})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->recovery_actions.empty());
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->code == "key843");
}

TEST_CASE("key839 is a WARNING toast, not a second blocking modal",
          "[error-center][classify][1388]") {
    // key839 restates the slot's emptiness that an already-visible runout
    // prompt explains (#1388): it carries no recovery action that prompt
    // lacks, so the key8xx blanket CRITICAL promotion just stacks a second
    // blocking modal on one runout. WARNING routes it to a deduped toast.
    ClassifyContext ctx;
    auto e = classify(
        R"(!! {"code":"key839","msg":"no filament at extrude position","values":[1,4]})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->recovery_actions.empty());
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->code == "key839");
    REQUIRE_FALSE(e->sticky);
}

TEST_CASE("key840 carries a recovery action", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key840","msg":"box switch state error"})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->code == "key840");
    REQUIRE(e->recovery_actions.size() == 1);
    REQUIRE(e->recovery_actions[0].gcode == "BOX_ERROR_CLEAR");
}

TEST_CASE("key298 is WARNING with recovery action", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key298","msg":"klipper_mcu shutdown"})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->recovery_actions.size() == 1);
    REQUIRE(e->recovery_actions[0].gcode.empty());
}

TEST_CASE("Error: command error is WARNING/KLIPPER", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify("Error: Must home axis first", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->source == helix::ErrorSource::KLIPPER);
}

TEST_CASE("raw_detail preserves Klipper's wording when detail is rewritten",
          "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify("!! Must home axis first", ctx);
    REQUIRE(e.has_value());
    // clean_error_text() rewrites the display text ("axis" -> "axes")...
    REQUIRE(e->detail == "Must home axes first");
    // ...but raw_detail must keep Klipper's exact wording, because that is the
    // string the RPC channel records via record_caller_handled(). If these two
    // diverge without raw_detail, the cross-source dedup can never match and
    // the user gets two toasts for one rejection.
    REQUIRE(e->raw_detail == "Must home axis first");
}

TEST_CASE("raw_detail equals detail when the cleaner leaves the text alone",
          "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify("!! Move out of range: X=400.000000", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->detail == "Move out of range: X=400.000000");
    REQUIRE(e->raw_detail == e->detail);
}

TEST_CASE("non-error line yields nullopt", "[error-center][classify]") {
    ClassifyContext ctx;
    REQUIRE_FALSE(classify("// AFC_Brush: Clean Nozzle", ctx).has_value());
    REQUIRE_FALSE(classify("ok T:210", ctx).has_value());
}

TEST_CASE("RecoveryAction carries an optional style", "[error-center][model]") {
    helix::RecoveryAction a;
    REQUIRE(a.style.empty()); // default neutral
    helix::RecoveryAction b{"Resume", "RESUME", "afc::resume", "primary"};
    REQUIRE(b.style == "primary");
    REQUIRE(b.gcode == "RESUME");
}
