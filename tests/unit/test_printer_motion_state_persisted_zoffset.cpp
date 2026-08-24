// SPDX-License-Identifier: GPL-3.0-or-later
//
// Covers PrinterMotionState parsing of ZMOD's persisted z-offset out of
// save_variables.variables.gcode_offsets into the persisted_z_offset /
// persisted_z_offset_valid subjects.
//
// ZMOD zeroes gcode_move's live offset in END_PRINT/CANCEL_PRINT and re-applies
// the stored value at START_PRINT, so the stored value is the only truthful
// z-offset while the printer is idle. Moonraker delivers save_variables as a
// DELTA-only object, so a status frame that omits it must leave the last known
// value standing rather than blanking it.

#include "../helix_test_fixture.h"
#include "printer_motion_state.h"
#include "z_offset_utils.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterMotionState;
using helix::zoffset::displayed_z_offset_microns;
using nlohmann::json;

namespace {

class PersistedZOffsetFixture : public HelixTestFixture {
  public:
    PersistedZOffsetFixture() {
        state.init_subjects(false); // no XML registration in test
    }

    PrinterMotionState state;

    int persisted() {
        return lv_subject_get_int(state.get_persisted_z_offset_subject());
    }
    bool valid() {
        return lv_subject_get_int(state.get_persisted_z_offset_valid_subject()) != 0;
    }
    int live() {
        return lv_subject_get_int(state.get_gcode_z_offset_subject());
    }
    /// What the Z-offset row would show, given the printer's print state.
    int displayed(bool print_active) {
        std::optional<int> p;
        if (valid()) {
            p = persisted();
        }
        return displayed_z_offset_microns(live(), p, print_active);
    }

    static json homing_origin(double z) {
        return json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, z, 0.0}}}}};
    }

    static json save_variables(const json& variables) {
        return json{{"save_variables", json{{"variables", variables}}}};
    }
    static json gcode_offsets(const json& offsets) {
        return save_variables(json{{"gcode_offsets", offsets}});
    }
};

} // namespace

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset starts invalid",
                 "[motion][zoffset][zmod]") {
    // Nothing seen yet — the Controls panel must fall back to the live offset,
    // not display a confident 0.000.
    REQUIRE_FALSE(valid());
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset parses gcode_offsets.z",
                 "[motion][zoffset][zmod]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));

    REQUIRE(valid());
    REQUIRE(persisted() == -150);
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset survives frames that omit it",
                 "[motion][zoffset][zmod]") {
    // The real failure mode: Moonraker only re-sends save_variables when it
    // changes, so the very next gcode_move frame would blank a naive parser.
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    REQUIRE(valid());

    state.update_from_status(json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, 0.0, 0.0}}}}});

    REQUIRE(valid());
    REQUIRE(persisted() == -150);
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset tracks later changes",
                 "[motion][zoffset][zmod]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(gcode_offsets(json{{"z", -0.075}}));

    REQUIRE(valid());
    REQUIRE(persisted() == -75);
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset accepts a stored zero",
                 "[motion][zoffset][zmod]") {
    // A user who dialed the offset back to 0 must not keep seeing the old value.
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(gcode_offsets(json{{"z", 0.0}}));

    REQUIRE(valid());
    REQUIRE(persisted() == 0);
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Persisted z-offset stays invalid for a printer without gcode_offsets",
                 "[motion][zoffset][zmod]") {
    // Non-ZMOD printers with a save_variables section (AD5X IFS, QIDI Box) must
    // not be given a phantom persisted offset.
    state.update_from_status(save_variables(json{{"bambufy_colors", json::array()}}));

    REQUIRE_FALSE(valid());
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset ignores a null placeholder",
                 "[motion][zoffset][zmod]") {
    // ZMOD seeds the dict as {'z': None} before the first SET_GCODE_OFFSET.
    state.update_from_status(gcode_offsets(json{{"z", nullptr}}));

    REQUIRE_FALSE(valid());
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Persisted z-offset does not disturb the live gcode offset",
                 "[motion][zoffset][zmod]") {
    // The two are independent readings; parsing one must not write the other.
    state.update_from_status(json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, -0.2, 0.0}}}}});
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));

    REQUIRE(lv_subject_get_int(state.get_gcode_z_offset_subject()) == -200);
    REQUIRE(persisted() == -150);
}

// ============================================================================
// Regression: the sequence Negan reported on AD5X/AD5M running ZMOD.
//
// "When the printer is idle, the screen shows 0.000 even though that's not the
//  actual Z-offset. To adjust it you have to start a print."
//
// ZMOD applies the stored offset in START_PRINT and zeroes gcode_move's live
// offset again in END_PRINT, so across a whole print cycle the live reading is
// only truthful mid-print. Before the fix the row read the live value always.
// ============================================================================

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Regression: ZMOD z-offset stays truthful across a whole print cycle",
                 "[motion][zoffset][zmod][regression]") {
    // --- Boot: Klipper's live offset is 0, ZMOD holds the real -0.150. --------
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(homing_origin(0.0));

    REQUIRE(live() == 0); // what the old code displayed: a phantom 0.000
    CHECK(displayed(/*print_active=*/false) == -150);

    // --- START_PRINT: ZMOD's LOAD_GCODE_OFFSET applies the stored value. ------
    state.update_from_status(homing_origin(-0.15));

    CHECK(displayed(/*print_active=*/true) == -150);

    // --- Mid-print baby step: the live offset leads, and ZMOD saves it. -------
    state.update_from_status(homing_origin(-0.16));
    CHECK(displayed(/*print_active=*/true) == -160);
    state.update_from_status(gcode_offsets(json{{"z", -0.16}}));
    CHECK(displayed(/*print_active=*/true) == -160);

    // --- END_PRINT: ZMOD zeroes the live offset but keeps the stored one. -----
    state.update_from_status(homing_origin(0.0));

    REQUIRE(live() == 0);
    CHECK(displayed(/*print_active=*/false) == -160); // NOT 0.000
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Regression: a non-ZMOD printer is unaffected in every print state",
                 "[motion][zoffset][regression]") {
    // No save_variables ever arrives, so the live offset must remain the only
    // source. This is the guard against the fix leaking onto normal printers.
    state.update_from_status(homing_origin(-0.2));

    REQUIRE_FALSE(valid());
    CHECK(displayed(/*print_active=*/false) == -200);
    CHECK(displayed(/*print_active=*/true) == -200);

    state.update_from_status(homing_origin(0.0));
    CHECK(displayed(/*print_active=*/false) == 0);
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Regression: idle baby-step adjusts from the stored offset, not zero",
                 "[motion][zoffset][zmod][regression]") {
    // The data-loss path. Idle on ZMOD the live offset is 0 while -0.150 is
    // stored. A relative Z_ADJUST=-0.010 would land on -0.010 and ZMOD's
    // SET_GCODE_OFFSET override would persist THAT, discarding the user's real
    // offset. The absolute form has to be used, computed from what we displayed.
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(homing_origin(0.0));

    const int base = displayed(/*print_active=*/false);
    REQUIRE(base == -150);

    CHECK(helix::zoffset::build_z_adjust_gcode(base, live(), -10, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z=-0.160 MOVE=1");

    // Mid-print the same helper must still emit the cheap relative form, because
    // there the live offset IS the base.
    state.update_from_status(homing_origin(-0.15));
    const int print_base = displayed(/*print_active=*/true);
    REQUIRE(print_base == live());
    CHECK(helix::zoffset::build_z_adjust_gcode(print_base, live(), -10, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}
