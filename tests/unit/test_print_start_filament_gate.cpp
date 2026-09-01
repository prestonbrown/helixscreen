// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_start_filament_gate.cpp
 * @brief Controller-level tests that survived the gate-pipeline migration.
 *
 * Run with: ./build/bin/helix-tests "[print-start][filament-gate]"
 *
 * The pre-pipeline test cases lived here because the checks were private
 * PrintStartController methods; they moved to the pure gate core in Tasks 1-2:
 *  - the filament-present check        -> tests/unit/test_print_start_gates.cpp
 *    (required_filament_present gate cases, incl. AD5X IFS auto-unload
 *    suppression and the non-AMS runout fallback)
 *  - the unresolved-tool rule          -> tests/unit/test_print_start_gates.cpp
 *    (unresolved_tools_in / gate unresolved_tools cases, incl. bypass
 *    suppression and single-color silence)
 *
 * What remains here is should_warn_remap_unsupported() — the remap-unsupported
 * warning discriminator, untouched by the pipeline.
 */

#include "ui_print_start_controller.h"

#include "../test_helpers/ad5x_ifs_test_access.h"
#include "../test_helpers/ams_backend_probes.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_snapmaker.h"

#include "../catch_amalgamated.hpp"

// Friend shim lives in test_helpers/ — it must be defined exactly once across
// the test binary (see the header's note on ODR).
#include "../test_helpers/print_start_controller_test_access.h"

// ============================================================================
// Remap-unsupported warning discriminator (Snapmaker U1 stale-toast bug).
//
// apply_filament_remaps() must NOT toast "remap not supported" for a backend
// that applies the remap via its firmware-native pre-print path — the U1's
// build_preprint_gcode() honors the user's choice while writing no persistent
// mapping table. The toast is only correct for a backend with no route at all,
// or with one it cannot use yet.
//
// Driven through REAL backends rather than hand-built capability values. The
// retired two-argument form took a capability struct and a pre-send flag, which
// a caller could pull from two different backends and which made a test's
// premise a fiction it wrote itself: it could assert a shape no backend has.
// ============================================================================

TEST_CASE("remap warning: Snapmaker applies via its pre-print send, so it is SILENT",
          "[print-start][filament-gate][remap]") {
    // RemapStrategy::SnapmakerNative: no persistent table is written and the
    // remap still reaches the printer, through build_preprint_gcode.
    SnapmakerProbe sm;
    CHECK_FALSE(PrintStartControllerTestAccess::should_warn_remap_unsupported(sm));
}

TEST_CASE("remap warning: a backend with no route at all WARNS",
          "[print-start][filament-gate][remap]") {
    // ACE declares RemapStrategy::None — the remap genuinely cannot be honored
    // by any path, so the warning is the honest thing to show.
    AceProbe ace;
    CHECK(PrintStartControllerTestAccess::should_warn_remap_unsupported(ace));
}

TEST_CASE("remap warning: a table-writing backend never warns",
          "[print-start][filament-gate][remap]") {
    // AFC / ToolChanger / QIDI / CFS report Native: the generic
    // set_tool_mapping() path honors the remap, so the helper is never even
    // reached for the warning — but the predicate must still say "no warning".
    AfcProbe afc;
    CHECK_FALSE(PrintStartControllerTestAccess::should_warn_remap_unsupported(afc));
}

TEST_CASE("remap warning: a declared route that is not READY still warns",
          "[print-start][filament-gate][remap]") {
    // The case the retired predicate could not see. AD5X IFS declares Native
    // unconditionally, but before the printer.ifs tool_map is discovered
    // set_tool_mapping() writes local state the firmware replays nothing from —
    // the user's pick is dropped in silence. Warning is correct here, and was
    // correct before only because a SECOND capability query happened to answer
    // "not supported".
    Ad5xIfsProbe ad5x;
    CHECK(PrintStartControllerTestAccess::should_warn_remap_unsupported(ad5x));

    // A printer.ifs frame carrying tool_map arms the gate.
    Ad5xIfsTestAccess::deliver_identity_tool_map(ad5x);
    CHECK_FALSE(PrintStartControllerTestAccess::should_warn_remap_unsupported(ad5x));
}
