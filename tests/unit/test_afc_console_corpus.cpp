// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_afc_console_corpus.cpp
 * @brief The AFC console-response string contract, driven from real hardware output.
 *
 * Run with: ./build/bin/helix-tests "[afc][narration][corpus]"
 *
 * Every string below is VERBATIM from
 * a live BoxTurtle rig (Moonraker server/gcode_store, 2026-07-27) — a capture of
 * Moonraker's `server/gcode_store` taken from the BoxTurtle rig during a live
 * 12-toolchange print (digit runs substituted for the corpus's `N`). The
 * contract itself is documented in
 * `docs/devel/FILAMENT_MANAGEMENT.md` § "AFC console response contract".
 *
 * Two properties are under test, and they pull against each other:
 *
 *   1. AFC emits its load/unload narration with NO `//` prefix, so the router's
 *      old `//`-only filter discarded `match_narration_phase()`'s most important
 *      needles. Those lines must now resolve to a phase.
 *   2. Bare console text is open-ended — M105 reports, `echo:` chatter and
 *      USER-CONTROLLED gcode filenames all arrive on the same channel. The loose
 *      substring needles that are fine for `//` narration would fire on
 *      `haircut.gcode`. Bare lines therefore go through a separate,
 *      shape-anchored matcher, and every noise line here must resolve to nullopt.
 *
 * Mutation checks (each must break the listed test):
 *   - restore the `//`-only filter in GcodeNarrationRouter::process_line()
 *     -> "bare AFC narration reaches the step bar" fails
 *   - point the bare path at match_narration_phase() (the loose matcher)
 *     -> "console noise never matches a phase" fails on the filename line
 *   - drop the token-count/shape checks in match_bare_narration_phase()
 *     -> "console noise never matches a phase" fails
 *   - remove the parse_unknown_command() early-return in process_line()
 *     -> "an unknown-command response never advances the step bar" fails
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "gcode_narration_router.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test-only friend of GcodeNarrationRouter (declared in
// gcode_narration_router.h as `friend struct ::GcodeNarrationRouterTestAccess`).
// Exposes BOTH private entry points. This definition MUST stay token-identical
// to the one in test_gcode_narration_router.cpp (same struct, global namespace)
// to avoid an ODR violation across translation units.
struct GcodeNarrationRouterTestAccess {
    static void feed(GcodeNarrationRouter& r, const std::string& line) {
        r.process_line(line);
    }
    static void notify(GcodeNarrationRouter& r, const nlohmann::json& msg) {
        r.on_notify_gcode_response(msg);
    }
};

namespace {

/// One corpus line and the phase id it must resolve to. `phase == nullptr`
/// means "must resolve to nullopt".
struct CorpusCase {
    const char* line;
    const char* phase;
};

void check_bare(const AmsBackendAfc& afc, const CorpusCase& c) {
    CAPTURE(c.line);
    const auto got = afc.match_bare_narration_phase(c.line);
    if (c.phase == nullptr) {
        CHECK_FALSE(got.has_value());
    } else {
        REQUIRE(got.has_value());
        CHECK(*got == std::string(c.phase));
    }
}

void check_prefixed(const AmsBackendAfc& afc, const CorpusCase& c) {
    CAPTURE(c.line);
    const auto got = afc.match_narration_phase(c.line);
    if (c.phase == nullptr) {
        CHECK_FALSE(got.has_value());
    } else {
        REQUIRE(got.has_value());
        CHECK(*got == std::string(c.phase));
    }
}

// LOAD_SWAP template order, needed to turn a phase id into the step index the
// router publishes: heat,cut,unload,feed,poop,brush,kick,load. (brush precedes
// kick because AFC's wipe runs before the kick as well as after it.)
constexpr int STEP_UNLOAD = 2;
constexpr int STEP_FEED = 3;
constexpr int STEP_POOP = 4;
constexpr int STEP_LOAD = 7;

void reset_step_baseline() {
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
    AmsState::instance().set_narration_phase(-1, "");
    AmsState::instance().set_backend(std::make_unique<AmsBackendAfc>(nullptr, nullptr));
}

int step_after(GcodeNarrationRouter& router, const std::string& line) {
    GcodeNarrationRouterTestAccess::feed(router, line);
    helix::ui::UpdateQueue::instance().drain();
    return lv_subject_get_int(AmsState::instance().get_toolchange_step_subject());
}

/// step_after() from a cleared bar. Needed wherever the lines under test are
/// independent probes rather than a real sequence: AmsState latches the phase
/// index so it can only advance within one operation, so probing "unload" right
/// after "load" would (correctly) be refused.
int step_isolated(GcodeNarrationRouter& router, const std::string& line) {
    AmsState::instance().set_narration_phase(-1, "");
    helix::ui::UpdateQueue::instance().drain();
    return step_after(router, line);
}

} // namespace

// ============================================================================
// Bare lines (no `//`, no `!!`) — the shapes AFC actually emits
// ============================================================================

TEST_CASE("AFC bare narration resolves to a phase", "[afc][narration][corpus]") {
    // These five carry the semantics of a toolchange: without them the step bar
    // can only ever advance on the decorative cut/brush lines, never on
    // load-complete or unload-complete. None of them has a `//` prefix.
    const AmsBackendAfc afc(nullptr, nullptr);

    const CorpusCase cases[] = {
        {"lane1 is now loaded in toolhead t:0", "load"},
        {"lane3 is now loaded in toolhead t:0", "load"},
        {"Unloading lane1", "unload"},
        {"Loading lane3", "feed"},
        {"Lane lane1 unload done t:0", "unload"},
        // Pre-`t:` AFC builds omit the trailing tool id; the shape must still match.
        {"lane1 is now loaded in toolhead", "load"},
        {"Lane lane1 unload done", "unload"},
    };
    for (const auto& c : cases) {
        check_bare(afc, c);
    }
}

TEST_CASE("AFC bare lines with no phase in the template stay unmatched",
          "[afc][narration][corpus]") {
    // Real AFC narration that the step template has no slot for. Mapping them
    // anywhere would invent a phase; they must simply not move the bar.
    // `laneN already loaded` is the #1183 no-op — CHANGE_TOOL did nothing, so it
    // must not read as a completed load.
    const AmsBackendAfc afc(nullptr, nullptr);

    const CorpusCase cases[] = {
        {"Tool Change - lane1 -> lane3", nullptr},
        {"Tool Change - None -> lane1", nullptr},
        {"Total change time: t:0", nullptr},
        {"lane1 already loaded", nullptr},
    };
    for (const auto& c : cases) {
        check_bare(afc, c);
    }
}

TEST_CASE("AFC console noise never matches a phase", "[afc][narration][corpus]") {
    // The anti-false-positive half of the contract. The filename line is the one
    // that proves the point: it is user-controlled, and a loose has("cut") turns
    // any `haircut.gcode` into a Cut-tip step.
    const AmsBackendAfc afc(nullptr, nullptr);

    const CorpusCase cases[] = {
        {"B:60 /60 T0:220 /220", nullptr},
        {"Rotation distance reset : 4", nullptr},
        {"Setting extruder temperature to 220 and waiting for extruder to reach temperature",
         nullptr},
        {"File opened: FAST_MMU_4_COLOR_RING_PLA_1h44m12s.gcode Size: 12345678", nullptr},
        {"File selected", nullptr},
        {"Total number of toolchanges set to 12", nullptr},
        {"echo: \"Print starting...\"", nullptr},
        {"echo: \"Print finished\"", nullptr},
        {"Done printing file", nullptr},
        {"<span class=warning--text>Please remove SET_AFC_TOOLCHANGES from your slicers 'Change "
         "Filament G-Code' section as SET_AFC_TOOLCHANGES is now deprecated...</span>",
         nullptr},
        // Not in the corpus, but the trap the corpus warns about, spelled out.
        {"File opened: haircut.gcode Size: 12345678", nullptr},
        {"File opened: unload_and_purge_test.gcode Size: 99", nullptr},
        // Lines the router feeds through unchanged that must stay inert.
        {"ok", nullptr},
        {"Klipper state: ready", nullptr},
        {"", nullptr},
    };
    for (const auto& c : cases) {
        check_bare(afc, c);
    }
}

// ============================================================================
// `//` lines — the loose matcher's behaviour must not regress
// ============================================================================

TEST_CASE("AFC // narration classifies as it did before", "[afc][narration][corpus]") {
    // No-regression guard for the prefixed path: it keeps the deliberately loose
    // needles, because a `//` body came from a macro's own respond_info and
    // upstream owns the wording.
    const AmsBackendAfc afc(nullptr, nullptr);

    const CorpusCase cases[] = {
        {"AFC_Cut: Cut Filament", "cut"},
        {"AFC_Cut: Moving to cutter pin", "cut"},
        {"AFC_Cut: Retract Filament for Cut", "cut"},
        {"AFC_Cut: Push cut tip back into hotend", "cut"},
        {"AFC_Brush: Clean Nozzle", "brush"},
        {"AFC_Brush: Move to Brush.", "brush"},
        {"AFC_Brush: Y Brush Moves", "brush"},
        // AFC_Park has no phase in the template; it must stay unmatched rather
        // than borrow a neighbouring step.
        {"AFC_Park: Park Toolhead", nullptr},
        {"Smart Park location: 100,100.", nullptr},
        {"Moving filament tip 0.5mms", nullptr},
        {"DESCRIBE_COLOR: got hex #FF0000", nullptr},
        {"TOOLCHANGE: filament Galaxy Black (PLA), color #101010", nullptr},
        {"Run Current: 0.80A Hold Current: 0.40A Home Current: 0.40A", nullptr},
        {"pressure_advance: 0.032", nullptr},
        {"     Change 1 out of 12", nullptr},
    };
    for (const auto& c : cases) {
        check_prefixed(afc, c);
    }
}

// ============================================================================
// End-to-end through the router
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "bare AFC narration reaches the step bar",
                 "[afc][narration][corpus]") {
    // The whole point of fix 1: these lines used to be dropped by the `//` filter
    // before any matcher saw them.
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr);

    CHECK(step_isolated(router, "Loading lane3") == STEP_FEED);
    CHECK(step_isolated(router, "lane3 is now loaded in toolhead t:0") == STEP_LOAD);
    CHECK(step_isolated(router, "Unloading lane1") == STEP_UNLOAD);
    CHECK(step_isolated(router, "Lane lane1 unload done t:0") == STEP_UNLOAD);
}

TEST_CASE_METHOD(LVGLTestFixture, "console noise never moves the step bar",
                 "[afc][narration][corpus]") {
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr);

    CHECK(step_after(router, "File opened: haircut.gcode Size: 12345678") == -1);
    CHECK(step_after(router, "B:60 /60 T0:220 /220") == -1);
    CHECK(step_after(router, "Done printing file") == -1);
    CHECK(step_after(router,
                     "<span class=warning--text>Please remove SET_AFC_TOOLCHANGES</span>") == -1);
    CHECK(step_after(router, "lane1 already loaded") == -1);
    CHECK(step_after(router, "Tool Change - lane1 -> lane3") == -1);
    // `!!` belongs to GcodeErrorRouter and must never reach the phase model,
    // prefix-stripping change or not.
    CHECK(step_after(router, "!! Toolhead jam detected in lane1") == -1);
}

TEST_CASE_METHOD(LVGLTestFixture, "an unknown-command response never advances the step bar",
                 "[afc][narration][corpus]") {
    // `// Unknown command:"STATUS_PURGING"` is a `//` line, so it reaches the
    // loose matcher, where has("purg") reads "PURGING" as a real purge phase —
    // an error message driving the progress bar forwards. The router claims the
    // line before the matcher sees it.
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr);

    CHECK(step_after(router, "// Unknown command:\"STATUS_PURGING\"") == -1);
    CHECK(step_after(router, "// Unknown command:\"AFC_BRUSH\"") == -1);

    // A real purge line on the same channel still works — the guard is specific
    // to the unknown-command shape, not a blanket mute.
    CHECK(step_after(router, "// AFC_Poop: Move To Purge Location") == STEP_POOP);
}

TEST_CASE("parse_unknown_command extracts the missing command", "[afc][narration][corpus]") {
    // Pure shape test: this is the string that decides whether a filament
    // operation is failed, so it must not fire on anything else.
    CHECK(parse_unknown_command("Unknown command:\"STATUS_PURGING\"") ==
          std::optional<std::string>("STATUS_PURGING"));
    CHECK(parse_unknown_command("unknown command: \"FOO\"") == std::optional<std::string>("FOO"));

    CHECK_FALSE(parse_unknown_command("Unknown command").has_value());
    CHECK_FALSE(parse_unknown_command("Unknown command:\"\"").has_value());
    CHECK_FALSE(parse_unknown_command("The Unknown command:\"X\"").has_value());
    CHECK_FALSE(parse_unknown_command("Purging with an unknown command:\"X\"").has_value());
    CHECK_FALSE(parse_unknown_command("AFC_Brush: Clean Nozzle").has_value());
    CHECK_FALSE(parse_unknown_command("").has_value());
}
