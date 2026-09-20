// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend.h"

#include <cstdint>

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
 *
 * @note Each reaches through virtuals that may take the backend's own mutex, so
 *       a caller must not already hold it. No current caller is inside a
 *       backend; this note is what keeps it that way.
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
 * This is the STRATEGY-only half. Callers gating a real write want
 * can_write_mapping_table() below, which also asks whether the route is usable;
 * callers asking whether the user's pick will be honored at all want
 * can_remap() — reading persistence for that purpose is how the U1, which
 * honors every pick through its pre-send, got read as a printer that ignores
 * them.
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

/**
 * @brief Why an explicit tool->lane pick cannot be offered, or None if it can.
 *
 * can_remap() answers the backend's half. The two terms that decide the rest
 * live outside the backend - whether the service-side helper a GcodeRewrite
 * needs is installed, and whether this job has tools worth mapping - so they
 * belong here too, where every surface offering the pick reads the same answer.
 * Held apart from can_remap() because that one is a pure backend question with
 * callers who have neither term to hand.
 */
enum class RemapBlock : uint8_t {
    None,           ///< The pick can be offered.
    Probing,        ///< Plugin presence not established yet; ask again shortly.
    NoStrategy,     ///< Backend has no routing mechanism at all.
    NotReady,       ///< Backend has a route that is not usable yet.
    NeedsPlugin,    ///< GcodeRewrite without the HelixPrint plugin.
    NothingToRemap, ///< This job uses no tools worth mapping.
};

/// Stable spelling for logs and bundles. Not user-facing: a reason a user reads
/// has to say what it costs them, which is a UI string, not an enum name.
[[nodiscard]] inline const char* remap_block_name(RemapBlock block) {
    switch (block) {
    case RemapBlock::None:
        return "none";
    case RemapBlock::Probing:
        return "probing";
    case RemapBlock::NoStrategy:
        return "no-strategy";
    case RemapBlock::NotReady:
        return "not-ready";
    case RemapBlock::NeedsPlugin:
        return "needs-plugin";
    case RemapBlock::NothingToRemap:
        return "nothing-to-remap";
    }
    return "unknown";
}

/**
 * @brief The whole availability question, for a given backend and job.
 *
 * @param plugin_installed Tri-state, as the helix_plugin_installed subject
 *        publishes it: -1 not probed yet, 0 absent, 1 present. Only GcodeRewrite
 *        consults it. -1 answers Probing rather than NeedsPlugin because the
 *        probe completes after first paint, and a card that cannot tell those
 *        apart shows its refusal on every boot before withdrawing it.
 * @param mappable_tools How many tools this job actually uses.
 */
[[nodiscard]] inline RemapBlock remap_block(const AmsBackend& backend, int plugin_installed,
                                            int mappable_tools) {
    const auto strategy = backend.get_remap_strategy();
    if (strategy == AmsBackend::RemapStrategy::None) {
        return RemapBlock::NoStrategy;
    }
    if (!backend.remap_ready()) {
        return RemapBlock::NotReady;
    }
    // Ahead of the plugin terms deliberately: a job with nothing to map is still
    // unmappable once the plugin is installed, so naming the plugin here would
    // send someone off to fix something that changes nothing for this file.
    if (mappable_tools <= 0) {
        return RemapBlock::NothingToRemap;
    }
    if (strategy == AmsBackend::RemapStrategy::GcodeRewrite) {
        if (plugin_installed < 0) {
            return RemapBlock::Probing;
        }
        if (plugin_installed != 1) {
            return RemapBlock::NeedsPlugin;
        }
    }
    return RemapBlock::None;
}

/**
 * @brief Will a write to this backend's own mapping table land, right now?
 *
 * The question every caller of the generic set_tool_mapping() apply path is
 * really asking: the route has to write a table AND be usable. The U1 answers
 * false and still honors the user's pick, through its pre-print send, which is
 * why this is not can_remap().
 *
 * Named rather than left as `remap_is_persistent(b.get_remap_strategy()) &&
 * b.remap_ready()` at each site — three hand-written copies of one two-part
 * rule is how the question this file exists to unify got six answers in the
 * first place.
 */
[[nodiscard]] inline bool can_write_mapping_table(const AmsBackend& backend) {
    return remap_is_persistent(backend.get_remap_strategy()) && backend.remap_ready();
}

} // namespace printer
} // namespace helix
