// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// End-to-end integration coverage for toolchange narration -> step display.
//
// Drives a realistic AFC swap narration sequence through the REAL
// GcodeNarrationRouter (both entry points: process_line AND the
// Moonraker-envelope parser on_notify_gcode_response) and asserts:
//   S1 - the "// Purge" line advances to the dedicated "purge" phase (index 5),
//        NOT the "feed" phase (index 4); ams_action_detail reads "Purge".
//   S2 - the LOAD_SWAP step template the sidebar renders includes the
//        "Brush nozzle" and "Clean nozzle" phases.
//
// This file exercises process_line and on_notify_gcode_response together, plus
// the template-derived step labels the sidebar consumes.

#include "ui_ams_sidebar.h"
#include "ui_step_progress.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/ams_sidebar_xml.h"
#include "../test_helpers/gcode_narration_router_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "gcode_narration_router.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp" // libhv-bundled nlohmann::json, same header the router uses

using namespace helix;

namespace {

// AFC backend constructed with (nullptr, nullptr) — same idiom as
// AfcToolchangeTestHelper in test_afc_toolchange.cpp. AFC overrides
// toolchange_phase_template() + match_narration_phase(), which is exactly the
// step model the router needs to resolve a `//` narration line to an index.
void install_afc_backend() {
    AmsState::instance().set_backend(std::make_unique<AmsBackendAfc>(nullptr, nullptr));
}

// AmsState is a process-wide singleton; init_subjects is a no-op once
// initialized so toolchange_step persists between cases. Establish a known
// LOAD_SWAP + sentinel(-1) baseline so each test is order-independent.
void reset_step_baseline() {
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
    AmsState::instance().set_narration_phase(-1, "");
    install_afc_backend();
}

int current_step() {
    return lv_subject_get_int(AmsState::instance().get_toolchange_step_subject());
}

std::string current_detail() {
    const char* s = lv_subject_get_string(AmsState::instance().get_ams_action_detail_subject());
    return s ? std::string(s) : std::string();
}

// An AFC backend whose action and loaded lane the test sets, so AmsState's
// backend sync carries an operation's start and end the way a live AFC does
// (the action arrives through sync_from_backend(), not set_action()).
class ScriptedAfcBackend : public AmsBackendAfc {
  public:
    ScriptedAfcBackend() : AmsBackendAfc(nullptr, nullptr) {}

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendAfc::get_system_info();
        info.action = action;
        info.current_slot = current_slot;
        return info;
    }

    AmsAction action = AmsAction::IDLE;
    int current_slot = -1;
};

ScriptedAfcBackend* install_scripted_afc_backend() {
    auto backend = std::make_unique<ScriptedAfcBackend>();
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    return raw;
}

void sync_action(ScriptedAfcBackend* backend, AmsAction action) {
    backend->action = action;
    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Router advances the subject through a realistic swap sequence (S1 core).
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration E2E - realistic swap sequence advances steps",
                 "[narration][ui_integration]") {
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr); // null client => no real subscription

    // LOAD_SWAP template indices:
    //   heat=0, cut=1, unload=2, feed=3, poop=4, brush=5, kick=6, load=7
    struct Step {
        const char* line;
        int expected_index;
        const char* expected_detail;
    };
    // Mirrors a real AFC CHANGE_TOOL on the wire: TOOL_UNLOAD heats, cuts and
    // pulls the old filament back to its lane, then TOOL_LOAD feeds the new one
    // and only afterwards runs do_poop_kick_wipe — whose body is
    // poop -> wipe -> kick -> wipe (AFC.py v1.2.0:1390-1413, v1.1.0:1417-1440),
    // all four narrating at the shipped default variable_verbose=1.
    //
    // The trailing wipe is the second "brush" and it must NOT rewind the bar
    // from kick(6) to brush(5) — see the dedicated latch test below.
    const Step sequence[] = {
        {"// Heat nozzle", 0, "Heat nozzle"},
        {"// Cutting tip", 1, "Cut tip"},
        {"// Unloading lane1", 2, "Unload filament"},
        {"// Loading lane2", 3, "Feed filament"},
        {"// AFC_Poop: Starting poop", 4, "Purge to bucket"},
        {"// AFC_Brush: Clean Nozzle", 5, "Brush nozzle"},
        {"// AFC_Kick: Starting Filament Kick", 6, "Kick away"},
        {"// AFC_Brush: Clean Nozzle", 6, "Kick away"},
        {"// lane2 is now loaded in toolhead", 7, "Load complete"},
    };

    for (const auto& step : sequence) {
        GcodeNarrationRouterTestAccess::feed(router, step.line);

        INFO("line: " << step.line);

        // set_narration_phase writes both subjects synchronously on the main
        // thread (feed() invokes process_line directly), so the label is
        // observable immediately after feed().
        REQUIRE(current_detail() == std::string(step.expected_detail));

        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(current_step() == step.expected_index);
        // Narration is now the single top-priority writer of ams_action_detail
        // (recompute_action_detail reads last_narration_label_ before anything
        // else), so the precise phase label SURVIVES a drain — the print-state
        // observer's recompute can no longer clobber it back to "Idle".
        REQUIRE(current_detail() == std::string(step.expected_detail));
    }

    // S1 acceptance, restated for the corrected model: purge wording resolves to
    // the poop phase (index 4 / "Purge to bucket") and never to the preceding
    // "Feed filament" phase. Re-feed in isolation to make the invariant plain —
    // the sequence above ended on load(7), and the phase latch would otherwise
    // (correctly) refuse to rewind to 4.
    AmsState::instance().set_narration_phase(-1, "");
    helix::ui::UpdateQueue::instance().drain();
    GcodeNarrationRouterTestAccess::feed(router, "// Purging filament");
    REQUIRE(current_detail() == std::string("Purge to bucket"));
    REQUIRE(current_detail() != std::string("Feed filament"));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 4);
    // Single-writer guarantee: the precise phase label is stable post-drain.
    REQUIRE(current_detail() == std::string("Purge to bucket"));
}

// ---------------------------------------------------------------------------
// 1b. UNLOAD narration resolves the retract phase to the template's final step.
//     Regression for #1046 I-1: the matcher had no `retract` case, so the last
//     unload step (retract) could never highlight.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration E2E - UNLOAD retract advances to final step (#1046)",
                 "[narration][ui_integration]") {
    reset_step_baseline();
    AmsState::instance().set_active_step_operation(StepOperationType::UNLOAD);
    GcodeNarrationRouter router(nullptr, nullptr);

    // UNLOAD template indices: heat=0, cut=1, unload=2
    GcodeNarrationRouterTestAccess::feed(router, "// Heat nozzle");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 0);

    GcodeNarrationRouterTestAccess::feed(router, "// Cutting tip");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 1);

    GcodeNarrationRouterTestAccess::feed(router, "// Retracting filament");
    REQUIRE(current_detail() == std::string("Retract filament"));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 2);
}

// ---------------------------------------------------------------------------
// 2. on_notify_gcode_response Moonraker-envelope parsing (closes untested gap).
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration E2E - on_notify_gcode_response parses Moonraker envelope",
                 "[narration][ui_integration]") {
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr);

    // Flat params form: {"params": ["// AFC_Brush: Clean Nozzle"]}
    nlohmann::json flat = {{"params", nlohmann::json::array({"// AFC_Brush: Clean Nozzle"})}};
    GcodeNarrationRouterTestAccess::notify(router, flat);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 5); // brush

    // Reset, then the nested params form: {"params": [["// Purge"]]}
    AmsState::instance().set_narration_phase(-1, "");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == -1);

    nlohmann::json nested = {
        {"params", nlohmann::json::array({nlohmann::json::array({"// Purge"})})}};
    GcodeNarrationRouterTestAccess::notify(router, nested);
    REQUIRE(current_detail() == std::string("Purge to bucket"));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 4); // poop, not feed (3)
    // Single-writer guarantee: the phase label survives the drain.
    REQUIRE(current_detail() == std::string("Purge to bucket"));
}

// ---------------------------------------------------------------------------
// 3. Template-derived labels (the data the sidebar renders) -- S2 + S1.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration E2E - LOAD_SWAP step template carries brush/poop/kick",
                 "[narration][ui_integration]") {
    reset_step_baseline();
    auto* backend = dynamic_cast<AmsBackendAfc*>(AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);

    const auto tmpl = backend->toolchange_phase_template(StepOperationType::LOAD_SWAP);

    auto label_at = [&](const std::string& id) -> std::string {
        for (const auto& p : tmpl) {
            if (p.id == id)
                return p.label;
        }
        return {};
    };
    auto index_of = [&](const std::string& id) -> int {
        for (size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i].id == id)
                return static_cast<int>(i);
        }
        return -1;
    };
    auto has_label = [&](const std::string& want) {
        for (const auto& p : tmpl) {
            if (p.label == want)
                return true;
        }
        return false;
    };

    // S2: the swap template the sidebar renders must include the wipe and the
    // purge-to-bucket. "Clean nozzle" was a duplicate of the brush and is gone.
    REQUIRE(has_label("Brush nozzle"));
    REQUIRE(has_label("Purge to bucket"));
    REQUIRE_FALSE(has_label("Clean nozzle"));
    REQUIRE_FALSE(has_label("Purge"));

    // S1: the phase "// Purge" resolves to is the poop, labeled "Purge to
    // bucket", and sits AFTER "feed" so it is never conflated with the feed.
    const int poop_idx = index_of("poop");
    const int feed_idx = index_of("feed");
    REQUIRE(feed_idx == 3);
    REQUIRE(poop_idx == 4);
    REQUIRE(poop_idx > feed_idx);
    REQUIRE(label_at("poop") == std::string("Purge to bucket"));

    // Build the step bar exactly as the sidebar does, from the same template,
    // and confirm the rendered widget mirrors the template labels. This is the
    // real label source the AmsOperationSidebar feeds ui_step_progress_create.
    std::vector<ui_step_t> steps;
    steps.reserve(tmpl.size());
    for (const auto& p : tmpl) {
        steps.push_back({p.label.c_str(), helix::StepState::Pending});
    }
    lv_obj_t* bar =
        ui_step_progress_create(lv_screen_active(), steps.data(), static_cast<int>(steps.size()),
                                /*horizontal=*/true, /*scope_name=*/nullptr);
    REQUIRE(bar != nullptr);
    // ui_step_progress was created from the same labels; nothing further to
    // assert beyond construction succeeding (its child structure moves
    // connectors around, so the template is the authoritative label source).

    lv_obj_delete(bar);
}

// ---------------------------------------------------------------------------
// 4. The stock toolchange emission ORDER, and the monotonic guarantee.
//
// AFC's do_poop_kick_wipe runs poop -> wipe -> kick -> wipe (AFC.py
// v1.2.0:1390-1413; the same body inline in TOOL_LOAD at v1.1.0:1417-1440), and
// all four narrate at the shipped default variable_verbose: 1
// (config/AFC_Macro_Vars.cfg:17). match_narration_phase maps them to
// poop, brush, kick, brush.
//
// Two things have to hold together, and neither alone is enough:
//   - the template must order brush BEFORE kick (first occurrence), or the bar
//     never lights "Kick away" at all once the latch is in place;
//   - the published index must never decrease, or the repeated wipe rewinds it.
// With kick listed before brush and no latch, the index ran 4 -> 6 -> 5 -> 6.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration - stock AFC wipe order never rewinds the bar",
                 "[ams][afc][narration][stepbar]") {
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr);

    // Verbatim shapes AFC emits at variable_verbose: 1.
    const char* const stock_toolchange[] = {
        "// Heat nozzle",
        "// AFC_Cut: Cutting Filament",
        "Unloading lane1",
        "Loading lane2",
        "// AFC_Poop: Starting poop",
        "// AFC_Brush: Clean Nozzle",          // first wipe
        "// AFC_Kick: Starting Filament Kick", // kick
        "// AFC_Brush: Clean Nozzle",          // second wipe
        "lane2 is now loaded in toolhead t:0",
    };

    std::vector<int> published;
    for (const char* line : stock_toolchange) {
        GcodeNarrationRouterTestAccess::feed(router, line);
        helix::ui::UpdateQueue::instance().drain();
        published.push_back(current_step());
    }

    // The exact indices, so a template reshuffle is caught rather than absorbed.
    // heat, cut, unload, feed, poop, brush, kick, (wipe swallowed), load.
    REQUIRE(published == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 6, 7});

    // The invariant, stated independently of the numbers above: a progress bar
    // that goes backwards is the user-visible defect.
    for (size_t i = 1; i < published.size(); ++i) {
        CAPTURE(i, stock_toolchange[i], published[i - 1], published[i]);
        REQUIRE(published[i] >= published[i - 1]);
    }

    // Every phase the bar draws was reached — a latch that simply pinned the
    // maximum would also satisfy "never decreases".
    REQUIRE(published.back() == 7);
    REQUIRE(std::find(published.begin(), published.end(), 5) != published.end()); // brush lit
    REQUIRE(std::find(published.begin(), published.end(), 6) != published.end()); // kick lit
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration - the phase latch resets between operations",
                 "[ams][afc][narration][stepbar]") {
    reset_step_baseline();
    auto& ams = AmsState::instance();

    auto publish = [&](int index, const char* label) {
        ams.set_narration_phase(index, label);
        helix::ui::UpdateQueue::instance().drain();
        return current_step();
    };

    SECTION("a backwards phase mid-operation is swallowed") {
        REQUIRE(publish(6, "Kick away") == 6);
        REQUIRE(publish(5, "Brush nozzle") == 6);
        // The label is suppressed too, so the bar and the status line agree.
        REQUIRE(current_detail() == std::string("Kick away"));
    }

    SECTION("an explicit clear resets it") {
        REQUIRE(publish(6, "Kick away") == 6);
        REQUIRE(publish(-1, "") == -1);
        REQUIRE(publish(3, "Feed filament") == 3);
    }

    SECTION("the operation ending resets it") {
        // set_action only acts on a CHANGE, so establish a non-idle action
        // first — that is also the real shape: an operation was running.
        ams.set_action(AmsAction::LOADING);
        REQUIRE(publish(7, "Load complete") == 7);
        ams.set_action(AmsAction::IDLE);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(current_step() == -1);
        REQUIRE(publish(2, "Unload filament") == 2);
    }

    SECTION("a restart from the template's first phase resets it") {
        // AFC retrying a failed TOOL_LOAD re-runs it from the heat. Index 0 is
        // the first phase of every template, so it is unambiguous.
        REQUIRE(publish(6, "Kick away") == 6);
        REQUIRE(publish(0, "Heat nozzle") == 0);
        REQUIRE(publish(3, "Feed filament") == 3);
    }

    SECTION("changing the operation resets it — indices are template-relative") {
        REQUIRE(publish(7, "Load complete") == 7);
        ams.set_active_step_operation(StepOperationType::UNLOAD);
        // UNLOAD's template is heat, cut, unload — 2 is its LAST step, not a
        // rewind, and the swap's 7 must not hold it back.
        REQUIRE(publish(2, "Retract filament") == 2);
        ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    }

    SECTION("re-setting the SAME operation does not reset it") {
        // Otherwise the sidebar's periodic re-assertion of the current op would
        // silently disarm the latch.
        REQUIRE(publish(6, "Kick away") == 6);
        ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
        REQUIRE(publish(5, "Brush nozzle") == 6);
    }

    // Leave the singleton where reset_step_baseline() expects it.
    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    ams.set_narration_phase(-1, "");
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration - an operation ended by a backend sync clears the phase",
                 "[ams][afc][narration][stepbar]") {
    reset_step_baseline();
    auto* backend = install_scripted_afc_backend();
    AmsState::instance().sync_from_backend();
    GcodeNarrationRouter router(nullptr, nullptr);

    sync_action(backend, AmsAction::LOADING);
    GcodeNarrationRouterTestAccess::feed(router, "lane2 is now loaded in toolhead t:0");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 7);
    REQUIRE(current_detail() == std::string("Load complete"));

    // The status line must stop naming the finished operation's last phase.
    sync_action(backend, AmsAction::IDLE);
    CHECK(current_step() == -1);
    CHECK(current_detail() != std::string("Load complete"));

    // The next operation's first narrated phase must not be held behind the
    // finished one's high-water mark.
    GcodeNarrationRouterTestAccess::feed(router, "Unloading lane19");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(current_step() == 2);
    CHECK(current_detail() == std::string("Unload filament"));

    AmsState::instance().set_narration_phase(-1, "");
    install_afc_backend();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration - a narrated phase follows the operation's template",
                 "[ams][afc][narration][stepbar]") {
    reset_step_baseline();
    auto& ams = AmsState::instance();
    GcodeNarrationRouter router(nullptr, nullptr);

    SECTION("a phase the current template lacks resolves once the operation is known") {
        ams.set_active_step_operation(StepOperationType::LOAD_FRESH);
        GcodeNarrationRouterTestAccess::feed(router, "Unloading lane1");
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(current_step() == -1);

        ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(current_step() == 2);
        CHECK(current_detail() == std::string("Unload filament"));
    }

    SECTION("a narrated phase is re-resolved against the new template") {
        GcodeNarrationRouterTestAccess::feed(router, "Unloading lane1");
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(current_step() == 2);

        ams.set_active_step_operation(StepOperationType::UNLOAD);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(current_step() == 2);
        CHECK(current_detail() == std::string("Retract filament"));
    }

    SECTION("an index with no phase id to re-resolve is dropped") {
        ams.set_narration_phase(7, "Load complete");
        ams.set_active_step_operation(StepOperationType::UNLOAD);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(current_step() == -1);
    }

    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    ams.set_narration_phase(-1, "");
    helix::ui::UpdateQueue::instance().drain();
}

namespace {

/// The production sidebar over a scripted AFC backend with a lane loaded, so an
/// externally started operation takes the sidebar's own detection path.
struct NarratedSidebarFixture : public XMLTestFixture {
    NarratedSidebarFixture() {
        // init_subjects(true) is a no-op on an AmsState another case initialized
        // without XML registration, which leaves the sidebar's bindings unbound.
        AmsState::instance().deinit_subjects();
        auto backend = std::make_unique<ScriptedAfcBackend>();
        backend_ = backend.get();
        backend_->current_slot = 0;
        AmsState::instance().set_backend(std::move(backend));
        AmsState::instance().init_subjects(true);
        AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
        AmsState::instance().set_narration_phase(-1, "");
        AmsState::instance().sync_from_backend();

        helix::test::register_ams_sidebar_xml();
        panel_ = lv_obj_create(test_screen());
        const char* attrs[] = {"name", "ams_operation_sidebar", nullptr};
        root_ = static_cast<lv_obj_t*>(lv_xml_create(panel_, "ams_sidebar", attrs));
        REQUIRE(root_ != nullptr);
        sidebar_ = helix::ui::AmsOperationSidebar::attach(state(), panel_);
        helix::ui::UpdateQueue::instance().drain();
    }

    ~NarratedSidebarFixture() override {
        sidebar_.reset();
        if (panel_ && helix::test::widget_in_tree(panel_)) {
            lv_obj_delete(panel_);
        }
        AmsState::instance().set_narration_phase(-1, "");
        AmsState::instance().set_backend(nullptr);
    }

    [[nodiscard]] std::string status_text() const {
        lv_obj_t* label = lv_obj_find_by_name(root_, "status_label");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    ScriptedAfcBackend* backend_ = nullptr;
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(NarratedSidebarFixture,
                 "AMS sidebar keeps the phase an external swap narrated before its bar existed",
                 "[ams][afc][narration][stepbar][sidebar]") {
    GcodeNarrationRouter router(nullptr, nullptr);

    // A finished load, ended by the backend going idle.
    sync_action(backend_, AmsAction::LOADING);
    GcodeNarrationRouterTestAccess::feed(router, "lane2 is now loaded in toolhead t:0");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(status_text() == std::string("Load complete"));
    sync_action(backend_, AmsAction::IDLE);
    CHECK(status_text() != std::string("Load complete"));

    // AFC narrates the unload a few hundred ms before its action reaches the
    // sidebar, which only then builds the bar for the operation.
    GcodeNarrationRouterTestAccess::feed(router, "Unloading lane19");
    helix::ui::UpdateQueue::instance().drain();
    sync_action(backend_, AmsAction::UNLOADING);

    CHECK(AmsState::instance().get_active_step_operation() == StepOperationType::LOAD_SWAP);
    CHECK(current_step() == 2);
    CHECK(status_text() == std::string("Unload filament"));
}

TEST_CASE_METHOD(NarratedSidebarFixture, "AMS sidebar starts its own operation from the first step",
                 "[ams][afc][narration][stepbar][sidebar]") {
    GcodeNarrationRouter router(nullptr, nullptr);

    // Narration seen while idle belongs to no operation this UI starts.
    GcodeNarrationRouterTestAccess::feed(router, "Unloading lane19");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 2);

    sidebar_->start_operation(StepOperationType::LOAD_SWAP, 1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(current_step() == 0);
}

namespace {

// An AFC backend fed real AFC-object status frames, so the action AmsState
// syncs is the one the backend's own parser derives.
class FramedAfcBackend : public AmsBackendAfc {
  public:
    FramedAfcBackend() : AmsBackendAfc(nullptr, nullptr) {}

    void feed(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
        AmsState::instance().sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration survives AFC's Idle between unload and load",
                 "[ams][afc][narration][stepbar][toolchange_hold]") {
    reset_step_baseline();
    auto framed = std::make_unique<FramedAfcBackend>();
    auto* backend = framed.get();
    AmsState::instance().set_backend(std::move(framed));
    AmsState::instance().sync_from_backend();
    GcodeNarrationRouter router(nullptr, nullptr);

    backend->feed({{"current_state", "Unloading"}, {"next_lane", "lane2"}});
    GcodeNarrationRouterTestAccess::feed(router, "// Unloading lane1");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 2);

    // TOOL_UNLOAD's closing Idle, then the heat wait: still one operation.
    backend->feed({{"current_state", "Idle"}});
    CHECK(current_step() == 2);
    CHECK(current_detail() == std::string("Unload filament"));

    backend->feed({{"current_state", "Loading"}});
    GcodeNarrationRouterTestAccess::feed(router, "// Loading lane2");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(current_step() == 3);

    // CHANGE_TOOL's finally clears next_lane: the operation really ends.
    backend->feed({{"current_state", "Idle"}, {"next_lane", nullptr}});
    CHECK(current_step() == -1);

    AmsState::instance().set_narration_phase(-1, "");
    install_afc_backend();
}
