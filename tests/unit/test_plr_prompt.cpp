// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 2 tests for the power-loss-recovery UI layer. The connect-time offer
// DECISION is already covered by the pure tests in test_plr_offer.cpp; this file
// covers the new pure helper introduced in Phase 2 (plr_prompt_body) plus the
// null-api safety guard on show_plr_recovery_prompt.
//
// A subject-driven smoke test (drive pl_env_valid -> assert a modal appears) is
// intentionally NOT written here: show_plr_recovery_prompt() requires the XML
// modal_dialog component to be loaded, which is heavy XMLTestFixture
// scaffolding with no existing lightweight harness; a green test built on stubs
// would assert nothing real. The pure pieces below are the parts with branching
// logic worth pinning down. The backend strategy and the probe-before-resume
// safety invariant are covered in test_plr_backend.cpp.

#include "../../src/ui/print/ui_plr_prompt.h"
#include "plr_backend.h"

#include "../catch_amalgamated.hpp"

using helix::ui::plr_prompt_body;
using helix::ui::show_plr_recovery_prompt;

TEST_CASE("plr_prompt_body: filename is basename'd, extension stripped, substituted",
          "[plr][prompt]") {
    // Directory prefix stripped, .gcode removed -> "Benchy".
    REQUIRE(plr_prompt_body("gcodes/sub/Benchy.gcode", "Resume {}?", "generic") ==
            "Resume Benchy?");
}

TEST_CASE("plr_prompt_body: bare filename with no directory", "[plr][prompt]") {
    REQUIRE(plr_prompt_body("3DBenchy.gco", "Resume {}?", "generic") == "Resume 3DBenchy?");
}

TEST_CASE("plr_prompt_body: empty path returns the generic body verbatim", "[plr][prompt]") {
    REQUIRE(plr_prompt_body("", "Resume {}?", "generic body") == "generic body");
}

TEST_CASE("plr_prompt_body: null with_file_fmt returns generic", "[plr][prompt]") {
    REQUIRE(plr_prompt_body("part.gcode", nullptr, "generic body") == "generic body");
}

TEST_CASE("plr_prompt_body: null generic body with empty path yields empty string",
          "[plr][prompt]") {
    REQUIRE(plr_prompt_body("", "Resume {}?", nullptr).empty());
}

TEST_CASE("plr_prompt_body: malformed format template falls back to generic (no throw)",
          "[plr][prompt]") {
    // An unmatched '{' makes fmt::format throw; plr_prompt_body must catch it
    // and return the generic body rather than letting it propagate through the
    // LVGL C dispatch frame.
    REQUIRE(plr_prompt_body("part.gcode", "broken {", "safe fallback") == "safe fallback");
}

TEST_CASE("plr_prompt_body: filename that is only an extension is not over-stripped",
          "[plr][prompt]") {
    // strip_gcode_extension only strips when size > ext size, so ".gcode" alone
    // stays intact and is substituted as-is.
    REQUIRE(plr_prompt_body(".gcode", "Resume {}?", "generic") == "Resume .gcode?");
}

TEST_CASE("show_plr_recovery_prompt: null api is a safe no-op", "[plr][prompt]") {
    // The controller calls show_plr_recovery_prompt(get_moonraker_api(), plan),
    // and the api can be null in a headless context. The null-guard must return
    // before any LVGL / PrinterState access, so this must not crash.
    helix::PlrRecoveryPlan plan = helix::plr_build_plan(helix::PlrBackendType::SNAPMAKER, "a.gcode",
                                                        helix::PlrDetectResult{});
    REQUIRE_NOTHROW(show_plr_recovery_prompt(nullptr, plan));
}

TEST_CASE("show_plr_recovery_prompt: refuses a plan with no authorized resume", "[plr][prompt]") {
    // A Creality plan built without a completed probe carries no resume gcode.
    // Showing a Resume button that must refuse is worse than showing nothing —
    // and reaching modal creation here would need LVGL, so a crash-free return
    // is itself the evidence the guard fired before any widget work.
    helix::PlrDetectResult never_probed;
    helix::PlrRecoveryPlan plan =
        helix::plr_build_plan(helix::PlrBackendType::CREALITY, "a.gcode", never_probed);
    REQUIRE(plan.resume_allowed() == false);
    REQUIRE_NOTHROW(show_plr_recovery_prompt(nullptr, plan));
}

TEST_CASE("plr_prompt_strings: an approximate-layer plan gets the caveated copy", "[plr][prompt]") {
    // A restore that re-homes and does not re-probe Z lands the resumed layer a
    // millimetre or two off. It must NOT get the wording that promises an
    // exact resume.
    const helix::ui::PlrPromptStrings approximate{"approx {}", "approx generic"};
    const helix::ui::PlrPromptStrings standard{"standard {}", "standard generic"};

    SECTION("the probed backend's plan selects the caveated copy") {
        auto plan = helix::plr_build_plan(helix::PlrBackendType::CREALITY, "a.gcode", {});
        auto got = helix::ui::plr_prompt_strings(plan, approximate, standard);
        REQUIRE(std::string(got.with_file) == "approx {}");
        REQUIRE(std::string(got.generic) == "approx generic");
    }

    SECTION("a validated-snapshot plan keeps the exact-resume copy") {
        auto plan = helix::plr_build_plan(helix::PlrBackendType::SNAPMAKER, "a.gcode", {});
        auto got = helix::ui::plr_prompt_strings(plan, approximate, standard);
        REQUIRE(std::string(got.with_file) == "standard {}");
        REQUIRE(std::string(got.generic) == "standard generic");
    }

    SECTION("an empty plan defaults to standard, not to the caveat") {
        // Guards the next backend added: defaulting into the caveated copy
        // would silently attach a hardware-specific warning to unrelated
        // firmware.
        auto got = helix::ui::plr_prompt_strings(helix::PlrRecoveryPlan{}, approximate, standard);
        REQUIRE(std::string(got.with_file) == "standard {}");
    }
}
