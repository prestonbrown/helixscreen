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

#include "../catch_amalgamated.hpp"

// Friend shim lives in test_helpers/ — it must be defined exactly once across
// the test binary (see the header's note on ODR).
#include "../test_helpers/print_start_controller_test_access.h"

// ============================================================================
// Remap-unsupported warning discriminator (Snapmaker U1 stale-toast bug).
//
// apply_filament_remaps() must NOT toast "remap not supported" for a backend
// that applies the remap via its firmware-native pre-print path
// (requires_preprint_send) — build_preprint_gcode() honors the user's choice
// even though get_tool_mapping_capabilities() reports editable=false. The toast
// is only correct for a backend that can NEITHER edit its mapping NOR apply it
// via a pre-print send.
// ============================================================================

namespace {
helix::printer::ToolMappingCapabilities caps(bool supported, bool editable) {
    return {supported, editable, ""};
}
} // namespace

TEST_CASE("remap warning: Snapmaker (editable=false + pre-print) is SILENT",
          "[print-start][filament-gate][remap]") {
    // Real AmsBackendSnapmaker: get_tool_mapping_capabilities() -> {false,false}
    // (default), requires_preprint_send() -> true. The remap flows through
    // build_preprint_gcode, so no warning.
    auto& A = PrintStartControllerTestAccess::should_warn_remap_unsupported;
    CHECK_FALSE(A(caps(/*supported=*/false, /*editable=*/false), /*applies_via_preprint=*/true));
    // Mock snapmaker_mode reports {true,false} + pre-print — also silent.
    CHECK_FALSE(A(caps(/*supported=*/true, /*editable=*/false), /*applies_via_preprint=*/true));
}

TEST_CASE("remap warning: genuinely-incapable backend (no edit, no pre-print) WARNS",
          "[print-start][filament-gate][remap]") {
    // ACE-like: {false,false} capabilities AND no pre-print send -> the remap
    // genuinely cannot be honored, so the warning is correct.
    auto& A = PrintStartControllerTestAccess::should_warn_remap_unsupported;
    CHECK(A(caps(/*supported=*/false, /*editable=*/false), /*applies_via_preprint=*/false));
    // supported-but-not-editable AND no pre-print -> also unhonorable -> warn.
    CHECK(A(caps(/*supported=*/true, /*editable=*/false), /*applies_via_preprint=*/false));
}

TEST_CASE("remap warning: editable backend never warns (generic remap path is live)",
          "[print-start][filament-gate][remap]") {
    // AFC / ToolChanger / QIDI / CFS / AD5X-IFS report editable=true: the generic
    // set_tool_mapping() path in apply_filament_remaps honors the remap, so the
    // decision helper is never even reached for the warning — but assert the
    // predicate is false for editable backends regardless of pre-print flag.
    auto& A = PrintStartControllerTestAccess::should_warn_remap_unsupported;
    CHECK_FALSE(A(caps(/*supported=*/true, /*editable=*/true), /*applies_via_preprint=*/false));
    CHECK_FALSE(A(caps(/*supported=*/true, /*editable=*/true), /*applies_via_preprint=*/true));
}
