// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_ams_runout_surface.cpp
 * @brief Who gets to tell the user about a filament runout (#1250).
 *
 * Run with: ./build/bin/helix-tests "[1250][runout-surface]"
 *
 * Two things are pinned here.
 *
 * 1. `AmsState`'s `ams_filament_runout` indicator. The underlying
 *    `AmsSystemInfo::filament_runout` is a STICKY LATCH on the CFS — it mirrors
 *    `box.filament_useup`, which only `BoxAction.extruder_extrude` ever clears,
 *    and which a live K2 Plus was observed holding at 1 while idle in standby.
 *    Gating the indicator on level-and-paused therefore lit it on every
 *    unrelated pause forever after. The indicator now needs a transition
 *    witnessed during a job.
 *
 * 2. `RuntimeConfig::should_show_runout_modal()`. It used to answer "is this a
 *    hub AMS?" and suppress the generic modal for every one of them. It now
 *    answers "does this backend raise its own runout fault?", so a hub backend
 *    with no error hook is no longer silenced with nothing put in its place.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;

namespace {

/// A mock backend whose reported AmsType, runout flag and bypass state are
/// directly settable. Everything else is AmsBackendMock's normal behavior.
class RunoutProbeBackend : public AmsBackendMock {
  public:
    explicit RunoutProbeBackend(AmsType type) : AmsBackendMock(4), type_(type) {}

    AmsType get_type() const override {
        return type_;
    }

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.type = type_;
        info.filament_runout = runout_;
        if (bypass_) {
            info.current_slot = -2;
        }
        return info;
    }

    /// Reported alongside the sentinel above, because a real backend answers
    /// both from the same state. Overriding only get_system_info() left the
    /// double claiming bypass through the getter while is_bypass_active() —
    /// which reads the backend's own internal slot, not this override — still
    /// said no. AmsState asks the predicate, so the double has to answer it.
    [[nodiscard]] bool is_bypass_active() const override {
        return bypass_ || AmsBackendMock::is_bypass_active();
    }

    void set_runout(bool v) {
        runout_ = v;
    }
    void set_bypass(bool v) {
        bypass_ = v;
    }

  private:
    AmsType type_;
    bool runout_ = false;
    bool bypass_ = false;
};

void set_print_state(PrintJobState s) {
    lv_subject_set_int(get_printer_state().get_print_state_enum_subject(), static_cast<int>(s));
}

/// Install a probe backend and return it. AmsState::set_backend() resets the
/// runout edge state, so every test starts from an unseeded indicator.
RunoutProbeBackend* install(AmsType type) {
    auto backend = std::make_unique<RunoutProbeBackend>(type);
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    return raw;
}

int indicator() {
    return lv_subject_get_int(AmsState::instance().get_filament_runout_subject());
}

} // namespace

// ============================================================================
// 1. ams_filament_runout — edge, not level
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Stale filament_useup latch is not a runout",
                 "[1250][runout-surface][ams]") {
    auto& ams = AmsState::instance();
    // PrinterState first: AmsState observes its print_state_enum subject, and an
    // uninitialized subject swallows lv_subject_set_int without complaint. Same
    // order as production (subject_initializer.cpp).
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install(AmsType::CFS);

    // The latch is already set before we ever look at this printer — exactly the
    // live K2 Plus reading (`filament_useup: 1` at `print_stats.state: standby`).
    backend->set_runout(true);
    set_print_state(PrintJobState::STANDBY);
    ams.sync_from_backend(); // first sample seeds the level; it is not an edge

    SECTION("an unrelated pause does not light the indicator") {
        // A user pause, an M600, a CFS fault pausing via BoxError.handle_event —
        // all of them land here, and none of them is a runout.
        set_print_state(PrintJobState::PAUSED);
        ams.sync_from_backend();
        CHECK(indicator() == 0);
    }

    SECTION("a whole print cycle with the latch stuck high stays dark") {
        set_print_state(PrintJobState::PRINTING);
        ams.sync_from_backend();
        CHECK(indicator() == 0);
        set_print_state(PrintJobState::PAUSED);
        ams.sync_from_backend();
        CHECK(indicator() == 0);
    }

    ams.set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "A genuine runout lights the indicator",
                 "[1250][runout-surface][ams]") {
    auto& ams = AmsState::instance();
    // PrinterState first: AmsState observes its print_state_enum subject, and an
    // uninitialized subject swallows lv_subject_set_int without complaint. Same
    // order as production (subject_initializer.cpp).
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install(AmsType::CFS);

    backend->set_runout(false);
    set_print_state(PrintJobState::PRINTING);
    ams.sync_from_backend(); // seed: no runout, job running

    // Spool runs out mid-print. The box asserts the latch; the firmware pauses.
    backend->set_runout(true);
    ams.sync_from_backend();
    CHECK(indicator() == 0); // armed, but not paused yet

    set_print_state(PrintJobState::PAUSED);
    ams.sync_from_backend();
    CHECK(indicator() == 1);

    SECTION("resuming clears it, and the sticky latch cannot resurrect it") {
        set_print_state(PrintJobState::PRINTING);
        ams.sync_from_backend();
        CHECK(indicator() == 0);

        // filament_useup is STILL 1 here — the box only clears it on a
        // successful extrude, and nothing clears it when the job ends. This is
        // the case the old level-and-paused gate got wrong.
        set_print_state(PrintJobState::PAUSED);
        ams.sync_from_backend();
        CHECK(indicator() == 0);
    }

    SECTION("the backend withdrawing the flag clears it") {
        backend->set_runout(false);
        ams.sync_from_backend();
        CHECK(indicator() == 0);
    }

    ams.set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "Runout raised while already paused counts (AD5X path)",
                 "[1250][runout-surface][ams]") {
    auto& ams = AmsState::instance();
    // PrinterState first: AmsState observes its print_state_enum subject, and an
    // uninitialized subject swallows lv_subject_set_int without complaint. Same
    // order as production (subject_initializer.cpp).
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    // AmsBackendAd5xIfs::evaluate_runout_locked() only ever raises while the job
    // is PAUSED, so the arming edge must be accepted in that state too.
    auto* backend = install(AmsType::AD5X_IFS);

    backend->set_runout(false);
    set_print_state(PrintJobState::PAUSED);
    ams.sync_from_backend(); // seed

    backend->set_runout(true);
    ams.sync_from_backend();
    CHECK(indicator() == 1);

    ams.set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "A runout edge outside a job never arms",
                 "[1250][runout-surface][ams]") {
    auto& ams = AmsState::instance();
    // PrinterState first: AmsState observes its print_state_enum subject, and an
    // uninitialized subject swallows lv_subject_set_int without complaint. Same
    // order as production (subject_initializer.cpp).
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* backend = install(AmsType::CFS);

    backend->set_runout(false);
    set_print_state(PrintJobState::STANDBY);
    ams.sync_from_backend(); // seed

    // Filament pulled out by hand on an idle machine. Real transition, no job.
    backend->set_runout(true);
    ams.sync_from_backend();
    CHECK(indicator() == 0);

    // Pausing something later must not retroactively make it a runout.
    set_print_state(PrintJobState::PAUSED);
    ams.sync_from_backend();
    CHECK(indicator() == 0);

    ams.set_backend(nullptr);
}

// ============================================================================
// 2. RuntimeConfig::should_show_runout_modal()
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Generic runout modal defers only to backends that speak",
                 "[1250][runout-surface][runtime-config]") {
    auto& ams = AmsState::instance();
    // PrinterState first: AmsState observes its print_state_enum subject, and an
    // uninitialized subject swallows lv_subject_set_int without complaint. Same
    // order as production (subject_initializer.cpp).
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);
    auto* cfg = get_runtime_config();

    SECTION("no AMS at all — the generic modal is the only surface there is") {
        ams.set_backend(nullptr);
        CHECK(cfg->should_show_runout_modal());
    }

    SECTION("tool changer — one spool per extruder, no hub to swap from") {
        install(AmsType::TOOL_CHANGER);
        ams.sync_from_backend();
        CHECK(cfg->should_show_runout_modal());
    }

    SECTION("backends that raise their own runout fault suppress the generic one") {
        // Two dialogs for one runout is worse than one, and the backend's
        // carries hardware-derived recovery actions.
        for (AmsType type : {AmsType::CFS, AmsType::AD5X_IFS, AmsType::AFC, AmsType::HAPPY_HARE}) {
            CAPTURE(static_cast<int>(type));
            install(type);
            ams.sync_from_backend();
            CHECK_FALSE(cfg->should_show_runout_modal());
        }
    }

    SECTION("hub backends with no error hook keep the generic modal") {
        // THE #1250 NARROWING. The old blanket hub-topology test suppressed
        // these too, leaving the user with no runout notification whatsoever.
        for (AmsType type : {AmsType::ACE, AmsType::QIDI_BOX}) {
            CAPTURE(static_cast<int>(type));
            install(type);
            ams.sync_from_backend();
            CHECK(cfg->should_show_runout_modal());
        }
    }

    SECTION("bypass active — the external spool's own sensor is what matters") {
        auto* backend = install(AmsType::CFS);
        backend->set_bypass(true);
        ams.sync_from_backend();
        CHECK(cfg->should_show_runout_modal());
    }

    ams.set_backend(nullptr);
}
