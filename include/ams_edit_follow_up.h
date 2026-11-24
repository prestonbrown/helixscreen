// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_edit_follow_up.h
 * @brief Capability of a filament system that offers printer state a slot edit
 *        implies but did not write
 *
 * Reached with AmsBackend::as<IEditFollowUp>(). INDX implements it with its
 * heater-model update after a material edit. Without it an edit offers nothing.
 */

#pragma once

#include "ams_error.h"
#include "ams_types.h"

#include <optional>
#include <string>

namespace helix {

/// The question the UI asks after a slot edit commits. Strings are translated.
struct EditFollowUp {
    std::string title;
    std::string message;
    std::string confirm_label;
};

class IEditFollowUp {
  public:
    /**
     * @brief What to offer after a slot edit, if anything
     *
     * @param slot_index The edited slot
     * @param before The slot as it was before the edit
     * @param after The slot as committed
     * @return The question to ask, or nullopt for none
     */
    [[nodiscard]] virtual std::optional<EditFollowUp>
    edit_follow_up(int slot_index, const SlotInfo& before, const SlotInfo& after) const = 0;

    /**
     * @brief Apply the follow-up edit_follow_up() offered, once the user agrees
     *
     * Refuses while a print holds the printer, paused included, then re-asks the
     * offer's question: the printer may have moved on while the dialog was up.
     */
    virtual AmsError run_edit_follow_up(int slot_index, const SlotInfo& before,
                                        const SlotInfo& after) = 0;

  protected:
    ~IEditFollowUp() = default;
};

} // namespace helix
