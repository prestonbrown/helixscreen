// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "touch_calibration_session.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix;

namespace {

// Records the sequence of operations the session drives, and models the
// device's stored calibration + affine-enabled state the way the real backend
// does (disable_affine() leaves the stored calibration intact; enable_affine()
// re-activates it).
struct FakeSink : ICalibrationSink {
    TouchCalibration stored{};
    bool affine_enabled = true;
    std::vector<std::string> ops;

    static TouchCalibration make(float a, bool valid = true) {
        TouchCalibration c{};
        c.a = a;
        c.e = a;
        c.valid = valid;
        return c;
    }

    TouchCalibration current_calibration() const override {
        return stored;
    }
    bool apply_calibration(const TouchCalibration& cal) override {
        if (!cal.valid) {
            ops.push_back("apply:rejected");
            return false;
        }
        stored = cal;
        affine_enabled = true;
        ops.push_back("apply");
        return true;
    }
    void disable_affine() override {
        affine_enabled = false;
        ops.push_back("disable");
    }
    void enable_affine() override {
        affine_enabled = true;
        ops.push_back("enable");
    }
    void clear_calibration() override {
        stored = TouchCalibration{};
        ops.push_back("clear");
    }

    /// What calibrated_read_cb would actually apply to an incoming touch: the
    /// stored matrix while the affine is enabled, nothing while it is disabled.
    TouchCalibration live() const {
        TouchCalibration l = stored;
        if (!affine_enabled) {
            l.valid = false;
        }
        return l;
    }
};

} // namespace

TEST_CASE("TouchCalibrationSession: begin snapshots and disables affine",
          "[touch-calibration][session]") {
    FakeSink sink;
    sink.stored = FakeSink::make(0.5f);

    TouchCalibrationSession session;
    session.begin_capture(sink);

    REQUIRE(session.has_backup());
    REQUIRE(sink.affine_enabled == false);
    REQUIRE(sink.ops.back() == "disable");
}

TEST_CASE("TouchCalibrationSession: backup() exposes the pre-session mapping for display",
          "[touch-calibration][session][943]") {
    // Capture markers (#1082) must show where a press lands under the mapping
    // the user's touches were tracking in when the session opened — NOT at the
    // raw capture point, which is not screen space on over-reporting
    // digitizers (Qidi Q2: raw space compressed ~0.5x, #943).
    FakeSink sink;
    TouchCalibration original = FakeSink::make(0.5f);
    original.c = 12.0f;
    sink.stored = original;

    TouchCalibrationSession session;
    REQUIRE(session.backup().valid == false); // no session yet

    session.begin_capture(sink);

    REQUIRE(session.backup().valid);
    REQUIRE(session.backup().a == Approx(original.a));
    REQUIRE(session.backup().c == Approx(original.c));

    session.commit();
    CHECK(session.backup().valid == false); // accepted: nothing left to map through

    session.restore(sink);
    CHECK(session.backup().valid == false); // torn down
}

TEST_CASE("TouchCalibrationSession: revert_for_retry keeps the backup readable",
          "[touch-calibration][session][943]") {
    // Markers keep drawing through the backup across retries, so the snapshot
    // must survive revert_for_retry() exactly as it survives in the device.
    FakeSink sink;
    TouchCalibration original = FakeSink::make(0.5f);
    sink.stored = original;

    TouchCalibrationSession session;
    session.begin_capture(sink);
    sink.apply_calibration(FakeSink::make(0.9f));
    session.revert_for_retry(sink);

    REQUIRE(session.backup().valid);
    REQUIRE(session.backup().a == Approx(original.a));
}

TEST_CASE("TouchCalibrationSession: restore after abort re-enables affine and reverts",
          "[touch-calibration][session]") {
    // The #943 regression: a session that is begun but never accepted must, on
    // teardown, re-enable the affine transform (touch must not be left disabled).
    FakeSink sink;
    TouchCalibration original = FakeSink::make(0.5f);
    sink.stored = original;

    TouchCalibrationSession session;
    session.begin_capture(sink);
    REQUIRE(sink.affine_enabled == false);

    // User aborts (navigates away) without accepting.
    session.restore(sink);

    REQUIRE(sink.affine_enabled == true);         // touch usable again
    REQUIRE(sink.stored.a == Approx(original.a)); // reverted to pre-session cal
    REQUIRE(session.has_backup() == false);
}

TEST_CASE("TouchCalibrationSession: commit keeps the new calibration on restore",
          "[touch-calibration][session]") {
    FakeSink sink;
    sink.stored = FakeSink::make(0.5f);

    TouchCalibrationSession session;
    session.begin_capture(sink);

    // User accepts a freshly computed calibration.
    TouchCalibration fresh = FakeSink::make(0.9f);
    sink.apply_calibration(fresh);
    session.commit();

    // Teardown must NOT revert the accepted calibration.
    session.restore(sink);

    REQUIRE(sink.affine_enabled == true);
    REQUIRE(sink.stored.a == Approx(fresh.a));
    REQUIRE(session.has_backup() == false);
}

TEST_CASE("TouchCalibrationSession: revert_for_retry reverts and disables for re-capture",
          "[touch-calibration][session]") {
    FakeSink sink;
    TouchCalibration original = FakeSink::make(0.5f);
    sink.stored = original;

    TouchCalibrationSession session;
    session.begin_capture(sink);

    // Pretend a new (bad) calibration was applied, then the user retries.
    sink.apply_calibration(FakeSink::make(0.9f));
    session.revert_for_retry(sink);

    REQUIRE(sink.affine_enabled == false);        // disabled for next capture
    REQUIRE(sink.stored.a == Approx(original.a)); // reverted to backup
    REQUIRE(session.has_backup() == true);        // backup retained across retry
}

TEST_CASE("TouchCalibrationSession: restore is idempotent and safe without a session",
          "[touch-calibration][session]") {
    FakeSink sink;
    sink.stored = FakeSink::make(0.5f);

    TouchCalibrationSession session;
    // No begin_capture(): restore must still leave the affine enabled.
    session.restore(sink);
    REQUIRE(sink.affine_enabled == true);

    // Second restore is a no-op-but-safe.
    sink.ops.clear();
    session.restore(sink);
    REQUIRE(sink.affine_enabled == true);
}

TEST_CASE("TouchCalibrationSession: invalid backup is not re-applied on restore",
          "[touch-calibration][session]") {
    // First-run wizard: no prior calibration exists (stored invalid). Restore
    // must still re-enable the affine, but not push an invalid cal through apply.
    FakeSink sink;
    sink.stored = FakeSink::make(1.0f, /*valid=*/false);

    TouchCalibrationSession session;
    session.begin_capture(sink);
    session.restore(sink);

    REQUIRE(sink.affine_enabled == true);
    for (const auto& op : sink.ops) {
        REQUIRE(op != "apply"); // never applied the invalid backup
    }
}

TEST_CASE("TouchCalibrationSession: aborting an uncalibrated device does not leave the "
          "candidate matrix live",
          "[touch-calibration][session][943]") {
    // Never-calibrated device: nothing is stored on entry. VERIFY installs the
    // freshly captured matrix on the device so the user can test it, so an abort
    // DOES have something to undo even though there is no backup to re-apply.
    // Leaving it in place would reproduce the original #943 symptom through a
    // different door: a recalibration the user abandoned still changes touch for
    // the rest of the session.
    FakeSink sink;
    sink.stored = FakeSink::make(1.0f, /*valid=*/false);

    TouchCalibrationSession session;
    session.begin_capture(sink);

    const TouchCalibration candidate = FakeSink::make(1.67f);
    REQUIRE(sink.apply_calibration(candidate));
    REQUIRE(sink.stored.a == Approx(candidate.a));

    // Timeout budget spent / held to cancel / navigated away.
    session.restore(sink);

    INFO("stored a=" << sink.stored.a << " valid=" << sink.stored.valid
                     << " affine_enabled=" << sink.affine_enabled);
    REQUIRE(sink.affine_enabled == true); // #943 invariant: touch stays usable
    REQUIRE(sink.live().valid == false);  // ...and uncalibrated, as it started
    REQUIRE(sink.stored.valid == false);  // the candidate is gone, not hidden
}

TEST_CASE("TouchCalibrationSession: retrying on an uncalibrated device drops the candidate",
          "[touch-calibration][session][943]") {
    // Same hole on the retry path. disable_affine() alone only suppresses the
    // stored matrix, so the candidate would sit in the stored slot waiting for
    // the next enable_affine() to install it.
    FakeSink sink;
    sink.stored = FakeSink::make(1.0f, /*valid=*/false);

    TouchCalibrationSession session;
    session.begin_capture(sink);
    REQUIRE(sink.apply_calibration(FakeSink::make(1.67f)));

    session.revert_for_retry(sink);

    REQUIRE(sink.affine_enabled == false); // disabled for the next capture
    REQUIRE(sink.stored.valid == false);   // candidate discarded, not parked

    // The landmine: whatever re-enables the affine next must not resurrect it.
    sink.enable_affine();
    REQUIRE(sink.live().valid == false);
}
