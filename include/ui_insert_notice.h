// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {
namespace ui {

/**
 * @brief Ask whether a freshly inserted spool is the one the lane held
 *
 * For an insert the hardware read nothing about (InsertVerdict::NoEvidence,
 * prestonbrown/helixscreen#1710). Non-blocking: the lane keeps its details
 * unless the user taps Clear, which runs Clear Spool through
 * ams_dispatch_backend_action(). Offers nothing on a lane with no details to
 * clear, nor on the lane feeding the print, where that clear would be refused.
 * A lane still showing the details it was asked about is not asked again for
 * five minutes, so a flapping gate sensor raises one notice, not one per edge.
 * Main thread only.
 *
 * A printer backend asks through UiRequestKind::OfferClearAfterUnverifiedInsert
 * (notice.h), from the update queue.
 *
 * @param slot Slot index the spool went into
 */
void offer_clear_after_unverified_insert(int slot);

/// Forget every lane already asked about, so a test starts with no notice pending.
void reset_insert_offers_for_test();

/// The slot editor owns this lane's insert (FilamentInsertWatcher opened it or
/// waits on the tag reader to), so offer_clear_after_unverified_insert() stays
/// quiet: one insert, one prompt. Held until the lane empties or the insert is
/// dropped. Main thread only.
void claim_insert_prompt(int slot);
void release_insert_prompt(int slot);
[[nodiscard]] bool insert_prompt_claimed(int slot);
void release_all_insert_prompts();

} // namespace ui
} // namespace helix
