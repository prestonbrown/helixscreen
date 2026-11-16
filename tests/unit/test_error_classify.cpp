// Copyright (C) 2025-2026 356C LLC
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
    // nothing. create_buttons() never sends the LABEL as a command for a blank
    // gcode, so it need not be a Klipper comment (#1172).
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

TEST_CASE("an opaque firmware code is shown but classifies like an uncoded !!",
          "[error-center][classify][firmware_error_envelope]") {
    // A dashed numeric code carries no error-table meaning: it must not read as
    // CFS, and a mid-print fault must still block rather than toast.
    const std::string line =
        R"(!! {"coded": "0003-0522-0002-0008", "oneshot": 0, "msg":"Lost communication with MCU 'e0'"})";
    ClassifyContext printing;
    printing.is_printing = true;
    auto e = classify(line, printing);
    REQUIRE(e.has_value());
    CHECK(e->code == "0003-0522-0002-0008");
    CHECK(e->source == helix::ErrorSource::GENERIC);
    CHECK(e->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(e->detail == "Lost communication with MCU 'e0'");
    CHECK(e->raw_detail.find("\"coded\"") != std::string::npos);

    ClassifyContext paused;
    paused.is_paused = true;
    auto p = classify(line, paused);
    REQUIRE(p.has_value());
    // The uncoded paused arm's Resume + dismiss, not a code-keyed action.
    REQUIRE(p->recovery_actions.size() == 2);
    CHECK(p->recovery_actions[0].gcode == "RESUME");
}

TEST_CASE("a code key holding a non-table code classifies like an uncoded !!",
          "[error-center][classify][firmware_error_envelope]") {
    ClassifyContext printing;
    printing.is_printing = true;
    auto e = classify(R"(!! {"code":"E0011","msg":"x"})", printing);
    REQUIRE(e.has_value());
    CHECK(e->code == "E0011");
    CHECK(e->source == helix::ErrorSource::GENERIC);
    CHECK(e->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(e->detail == "x");
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

TEST_CASE("key840 is WARNING with a recovery action", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key840","msg":"box switch state error"})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->code == "key840");
    // The box is mid-operation, not broken. A blocking modal would demand a
    // decision about a unit that clears itself; the reset stays reachable from
    // the toast for the case where it does not.
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
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

// ============================================================================
// Translation reaches the table, not just the hard-coded fallbacks
// ============================================================================

#include "../lvgl_test_fixture.h"
#include "translation_loader.h"

/**
 * clean_error_text() composes its output from KlipperErrorTable's message and
 * hint plus a runtime locator. All three are separate lv_tr() sites, and a
 * composed string is no longer a translation key, so each has to resolve
 * before concatenation. Asserting German proves the tags reach the loaded pack
 * rather than falling through to the English tag, which is what an un-wired
 * table would silently do.
 */
TEST_CASE_METHOD(LVGLTestFixture, "Klipper error table text and locator translate",
                 "[error-center][classify][i18n]") {
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    ClassifyContext ctx;

    auto plain = classify(R"(!! {"code":"key585","msg":"Move out of range"})", ctx);
    REQUIRE(plain.has_value());
    REQUIRE(plain->detail.find("Bewegung außerhalb des Bereichs") != std::string::npos);
    REQUIRE(plain->detail.find("Die angeforderte Position") != std::string::npos);

    // The [unit, slot] locator is welded onto the message before the hint, so
    // an untranslated formatter would leave English inside a German sentence.
    auto located = classify(R"(!! {"code":"key849","values":[1,"B"]})", ctx);
    REQUIRE(located.has_value());
    REQUIRE(located->detail.find("in Einheit 1 Slot B") != std::string::npos);

    lv_translation_set_language("en");
}
