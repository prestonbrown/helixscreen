// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend.h"

/**
 * @file ams_remap.h
 * @brief The capability questions generic code asks about tool remapping.
 *
 * A backend makes exactly two declarations — get_remap_strategy() (whether and
 * how a user's tool->lane choice is carried out) and remap_ready() (whether
 * that route is usable yet). Everything a caller wants to know is a pure
 * function of those two, and lives here so there is one definition of each
 * question rather than one per call site.
 *
 * These are deliberately free functions over the base class: they hold no
 * state, name no vendor, and adding a firmware means declaring the two virtuals
 * on one new backend and nothing else.
 */

namespace helix {
namespace printer {

/**
 * @brief Does this strategy leave the routing somewhere that outlives the send?
 *
 * Native writes the machine's own mapping table and GcodeRewrite writes the job
 * file, so both survive being sent. SnapmakerNative does not: the firmware is
 * told once, before PRINT_START, and nothing persists afterwards.
 *
 * Callers gating the generic set_tool_mapping() apply path want this. Callers
 * asking whether the user's pick will be honored at all want can_remap() —
 * reading this one for that purpose is how the U1, which honors every pick
 * through its pre-send, got read as a printer that ignores them.
 */
[[nodiscard]] inline bool remap_is_persistent(AmsBackend::RemapStrategy strategy) {
    return strategy == AmsBackend::RemapStrategy::Native ||
           strategy == AmsBackend::RemapStrategy::GcodeRewrite;
}

/**
 * @brief Can this backend carry out an explicit user tool->lane choice, now?
 *
 * THE question generic code should ask. Both halves matter: a backend built to
 * remap but not yet ready answers false. Asking the strategy alone offers the
 * user a write that cannot land (AD5X IFS before `_IFS_VARS` discovery); asking
 * readiness alone says yes for every backend that never had a route.
 */
[[nodiscard]] inline bool can_remap(const AmsBackend& backend) {
    return backend.get_remap_strategy() != AmsBackend::RemapStrategy::None && backend.remap_ready();
}

} // namespace printer
} // namespace helix
