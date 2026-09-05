// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::printer {

/**
 * @brief What stands behind the routing a backend reports.
 *
 * A tool-to-lane table read off a printer is not always something anybody
 * chose. It can be a plugin array that published only its first entries, or an
 * identity map synthesised because no table was found at all. Those tables are
 * shaped like routings and read like routings, and the only thing separating
 * them from a real one is where they came from.
 *
 * Consumers that would act on the routing's SHAPE need this, because a shape
 * carrying no information looks exactly like a shape carrying an unusual
 * answer. Sending every tool to one lane is the case in point: it is what a
 * half-published table degrades to, and equally what a user gets when they
 * deliberately point several tools at one spool.
 *
 * Provenance is a property of the whole routing, not of one entry: the question
 * a consumer asks is whether the table as a whole is worth believing.
 */
enum class ToolMappingOrigin {
    /// Nothing vouches for this table. It may still be correct — it just has
    /// no author, so a consumer must not read intent into its shape.
    Unvouched,

    /// Somebody chose this routing: the printer publishes it as its own live
    /// table, or a tool was aimed at a lane from our UI and still sits there.
    Deliberate,
};

} // namespace helix::printer
