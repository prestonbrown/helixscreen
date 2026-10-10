// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/// Why a rewritten copy of a job cannot be printed, or None if it can. Every
/// rewrite (pre-print modifications, a G-code tool remap) asks this one question.
enum class GcodeRewriteBlock : uint8_t {
    None,          ///< A rewrite can run.
    Probing,       ///< Plugin presence not established yet; ask again shortly.
    NeedsPlugin,   ///< The HelixPrint plugin is absent.
    NoLocalCopies, ///< The transport cannot keep the copy a rewrite streams through.
};

/**
 * @brief Whether a rewrite can run, and if not, why.
 *
 * @param plugin_installed Tri-state, as the helix_plugin_installed subject
 *        publishes it: -1 not probed yet, 0 absent, 1 present.
 * @param local_copies ITransfersAPI::supports_local_copies()
 */
[[nodiscard]] inline GcodeRewriteBlock gcode_rewrite_block(int plugin_installed,
                                                           bool local_copies) {
    // Ahead of the plugin terms: installing the plugin changes nothing here.
    if (!local_copies) {
        return GcodeRewriteBlock::NoLocalCopies;
    }
    if (plugin_installed < 0) {
        return GcodeRewriteBlock::Probing;
    }
    return plugin_installed == 1 ? GcodeRewriteBlock::None : GcodeRewriteBlock::NeedsPlugin;
}

/// The same answer in the tri-state a row's visibility subject carries: 1 yes,
/// 0 no, -1 not known yet (a row stays visible until the probe answers).
[[nodiscard]] inline int gcode_rewrite_available(GcodeRewriteBlock block) {
    switch (block) {
    case GcodeRewriteBlock::None:
        return 1;
    case GcodeRewriteBlock::Probing:
        return -1;
    case GcodeRewriteBlock::NeedsPlugin:
    case GcodeRewriteBlock::NoLocalCopies:
        return 0;
    }
    return 0;
}

} // namespace helix
