// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_effective_auto_match.cpp
 * @brief The auto-match rule both print surfaces resolve their lane colors with.
 *
 * "Effective auto match" is the toggle-aware answer the print-select detail view
 * and the print-status panel both hand to FilamentMapper — the detail view for
 * effective_mappings() (swatches + the pre-flight gate), the status panel for
 * effective_tool_colors() (the gcode viewer's per-tool colors). It is the same
 * question asked twice:
 *
 *   - A backend that can honor an explicit user tool->lane choice — by EITHER
 *     route, an editable mapping card or a firmware-native pre-print send — has
 *     a picker the user reaches, and that picker carries the auto-color toggle.
 *     Its setting therefore wins both ways.
 *   - A backend that can honor the choice by NEITHER route (ACE) has no picker
 *     anywhere, so nothing can flip the persisted preference. It must always
 *     auto-match on color+type: honoring the persisted default (FALSE) there
 *     falls back to positional matching and picks the wrong lane.
 *
 * The predicate used to be `caps.editable` alone, which put the Snapmaker U1 in
 * the second bucket. That stopped being true when every remap-capable backend
 * got one shared picker (the swatch-card tap reaches FilamentMappingModal): a
 * U1 user can flip the toggle, SettingsManager persists it, and AmsState then
 * ignored it. Pinning auto-match on there also means the U1 is the one backend
 * whose print routing gets rewritten by color proximity with no way to say no.
 *
 * The SEED half of that is deliberately still held — see
 * helix::kPreprintSeedFollowsUserSetting — so merging does not change what a U1
 * does before hardware confirms it. The cases below therefore split: the
 * predicate is asserted on AmsBackend directly, and effective_auto_match() is
 * asserted in its held state.
 *
 * Neither call site had a test. The two implementations were byte-identical
 * bodies in different files, so a change to the rule in one silently left the
 * other on the old behaviour — the detail view's swatches would promise a lane
 * the status panel then colored from a different one.
 *
 * These drive the two production entry points directly (not a re-implementation
 * of the rule), so they keep pinning the behaviour after the bodies are shared.
 */

#include "ui_panel_print_status.h"
#include "ui_print_select_detail_view.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "settings_manager.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Installs an AMS backend with a chosen tool-mapping editability and restores
/// both process singletons it touches on the way out. AmsState's backend is NOT
/// cleared by HelixTestFixture::reset_all(), and the auto-color-map setting is
/// persisted, so a test that leaves either set leaks into every later test.
struct AutoMatchFixture : public LVGLTestFixture {
    AutoMatchFixture() {
        // get_auto_color_map() reads an lv_subject, and set_auto_color_map()
        // writes one — so WITHOUT this the setter silently does not stick and
        // every case below would read a constant 0 while appearing to drive the
        // setting. Idempotent (early-returns on subjects_initialized_), and the
        // same call test_afc_spool_reassert.cpp makes for the same reason.
        helix::SettingsManager::instance().init_subjects();
        prev_auto_color_ = helix::SettingsManager::instance().get_auto_color_map();

        // set_backend() publishes into AmsState's subjects; without this they
        // are uninitialized. register_xml=false keeps the names out of the
        // process-wide XML scope.
        AmsState::instance().init_subjects(true);
    }

    ~AutoMatchFixture() override {
        AmsState::instance().set_backend(nullptr);
        helix::SettingsManager::instance().set_auto_color_map(prev_auto_color_);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The three shapes the rule has to tell apart. The mock models all three,
    /// so no case here is a re-implementation of the predicate.
    enum class Shape {
        Editable,     ///< AFC / CFS / Happy Hare / QIDI: {true,true}, no pre-send
        PreprintOnly, ///< Snapmaker U1: {true,false} + requires_preprint_send()
        NeitherRoute, ///< ACE: {false,false}, no pre-send — no picker exists
    };

    static AmsBackendMock* install_backend(Shape shape) {
        auto mock = std::make_unique<AmsBackendMock>(4);
        if (shape == Shape::PreprintOnly) {
            mock->set_snapmaker_mode(true);
        } else if (shape == Shape::NeitherRoute) {
            mock->set_tool_changer_mode(true);
        }
        auto* raw = mock.get();
        AmsState::instance().set_backend(std::move(mock));

        // Load-bearing: if the mock ever stopped modelling a shape, every
        // assertion below would still pass while testing only one branch. Pin
        // BOTH inputs to the rule, not just editability — the whole point of
        // these cases is that editability alone no longer decides.
        const auto caps = raw->get_tool_mapping_capabilities();
        switch (shape) {
        case Shape::Editable:
            REQUIRE(caps.editable);
            REQUIRE_FALSE(raw->requires_preprint_send());
            break;
        case Shape::PreprintOnly:
            REQUIRE_FALSE(caps.editable);
            REQUIRE(raw->requires_preprint_send());
            break;
        case Shape::NeitherRoute:
            REQUIRE_FALSE(caps.editable);
            REQUIRE_FALSE(raw->requires_preprint_send());
            break;
        }
        REQUIRE(AmsState::instance().get_backend() == raw);
        return raw;
    }

    static void set_auto_color_map(bool on) {
        helix::SettingsManager::instance().set_auto_color_map(on);
        REQUIRE(helix::SettingsManager::instance().get_auto_color_map() == on);
    }

    bool prev_auto_color_ = false;
};

} // namespace

// ============================================================================
// PrintSelectDetailView — src/ui/ui_print_select_detail_view.cpp
// Consumed by effective_mappings() (swatches + pre-flight gate).
// ============================================================================

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: a backend that honors the choice by "
                 "NEITHER route always auto-matches",
                 "[auto_match][print_select][detail_view][ams]") {
    // ACE: no editable mapping card, no pre-print send. Nothing reaches a picker,
    // so nothing can have flipped the persisted preference — and the persisted
    // default (FALSE) would fall back to positional matching, which on a
    // one-nozzle-many-lane system picks the wrong lane.
    helix::ui::PrintSelectDetailView view;
    install_backend(Shape::NeitherRoute);

    set_auto_color_map(false);
    CHECK(view.effective_auto_match());

    set_auto_color_map(true);
    CHECK(view.effective_auto_match());
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: editable backend honors the setting",
                 "[auto_match][print_select][detail_view][ams]") {
    helix::ui::PrintSelectDetailView view;
    install_backend(Shape::Editable);

    set_auto_color_map(false);
    CHECK_FALSE(view.effective_auto_match());

    set_auto_color_map(true);
    CHECK(view.effective_auto_match());
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "AmsBackend::honors_user_tool_mapping: the pre-print route counts",
                 "[auto_match][ams][snapmaker]") {
    // The machinery, asserted where it is NOT held (see the seed case below).
    // The Snapmaker U1's mapping card really is non-editable — its remap goes out
    // through build_preprint_gcode(), not set_tool_mapping() — and it honors
    // every pick the user makes anyway. A predicate that reads caps.editable and
    // stops says the opposite of the truth about it, which is what produced a
    // stale "remap not supported" toast on a printer that was honoring the remap.
    //
    // Stated as the discrimination the rule has to make: two backends that BOTH
    // report editable=false must answer DIFFERENTLY.
    auto* preprint = install_backend(Shape::PreprintOnly);
    REQUIRE(preprint->honors_user_tool_mapping());

    auto* neither = install_backend(Shape::NeitherRoute);
    REQUIRE_FALSE(neither->honors_user_tool_mapping());

    auto* editable = install_backend(Shape::Editable);
    REQUIRE(editable->honors_user_tool_mapping());
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: the pre-print SEED is held at "
                 "auto-match pending hardware",
                 "[auto_match][print_select][detail_view][ams][snapmaker]") {
    // The deliberate hold, pinned so it cannot lapse silently in either
    // direction. helix::kPreprintSeedFollowsUserSetting is false, so a
    // pre-print-send backend still seeds by color match regardless of the
    // persisted preference — byte-for-byte what the U1 did before this branch,
    // which is the point: merging must not change what the printer does until a
    // two-color print on real hardware says it should.
    //
    // The case above proves the machinery is present and correct; this one
    // proves it is not yet wired to the seed. When the constant flips, THIS is
    // the test that must be inverted, and its failure is the reminder.
    static_assert(!helix::kPreprintSeedFollowsUserSetting,
                  "kPreprintSeedFollowsUserSetting flipped: invert this case — a pre-print "
                  "backend should now follow the persisted setting like any editable one.");

    helix::ui::PrintSelectDetailView view;
    install_backend(Shape::PreprintOnly);

    set_auto_color_map(false);
    CHECK(view.effective_auto_match()); // held: still auto-matches

    set_auto_color_map(true);
    CHECK(view.effective_auto_match());
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: the hold is scoped to the "
                 "pre-print route only",
                 "[auto_match][print_select][detail_view][ams]") {
    // The hold must not spill onto editable backends — they have followed the
    // setting all along and nothing about this branch may change that. A hold
    // written one level up (inside honors_user_tool_mapping, or as a blanket
    // pin) would fail here.
    helix::ui::PrintSelectDetailView view;
    install_backend(Shape::Editable);

    set_auto_color_map(false);
    CHECK_FALSE(view.effective_auto_match());

    set_auto_color_map(true);
    CHECK(view.effective_auto_match());
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: no backend auto-matches",
                 "[auto_match][print_select][detail_view][ams]") {
    // No AMS at all: nothing can honor a tool->lane choice, so the setting is
    // not consulted. A single-extruder printer never reaches the mapper anyway,
    // but the predicate must not depend on that.
    AmsState::instance().set_backend(nullptr);
    helix::ui::PrintSelectDetailView view;

    set_auto_color_map(false);
    CHECK(view.effective_auto_match());
}

// ============================================================================
// PrintStatusPanel no longer asks this question.
// ============================================================================
//
// The live render used to resolve its per-tool colors with the same auto-match,
// so the two had to agree and this file tested both. It does not any more: once
// a print is underway the routing is a fact the firmware holds, not something to
// infer from colors, so PrintStatusPanel reads the APPLIED routing instead
// (AmsState::routed_tool_colors -> AmsBackend::get_tool_mapping). The match
// survives only where it answers the right question — pre-print, in the detail
// view above, deciding what mapping to SEND. Its coverage there is what remains.
