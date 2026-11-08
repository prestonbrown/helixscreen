// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file observe_language.h
 * @brief observe_language_change(), kept apart from observer_factory.h so the
 *        settings manager header reaches only the TUs that re-render on a language switch
 */

#pragma once

#include "display_numbering.h"
#include "observer_factory.h"
#include "system_settings_manager.h"

#include <memory>

namespace helix::ui {

/**
 * @brief Re-render C++-formatted text after a language switch or a renumbering
 *
 * XML re-translates what it bound through translation_tag. Text C++ produced
 * with lv_tr() was translated once, when it was set, and stays in the old
 * language until the owner formats it again: this is the trigger for that.
 * Lane numbers are formatted the same way, so the "number slots from 0" option
 * re-renders through here too: SettingsManager notifies the language subject
 * when it changes. The handler runs deferred, so the new translation pack is
 * already active, and only on a real change (not on registration).
 *
 * @tparam Panel Owner class type
 * @tparam OnChange Callable: void(Panel*)
 */
template <typename Panel, typename OnChange>
ObserverGuard observe_language_change(Panel* panel, OnChange&& on_change) {
    auto& settings = SystemSettingsManager::instance();
    lv_subject_t* language = settings.subject_language();
    struct Shown {
        int language;
        bool from_zero;
    };
    // Held by pointer so the non-mutable handler can update it.
    auto shown =
        std::make_shared<Shown>(Shown{lv_subject_get_int(language), lane_numbering_from_zero()});
    return observe<int>(
        language, panel,
        [shown, on_change = std::forward<OnChange>(on_change)](Panel* p, int index) {
            const bool from_zero = lane_numbering_from_zero();
            if (index == shown->language && from_zero == shown->from_zero) {
                return;
            }
            *shown = Shown{index, from_zero};
            on_change(p);
        },
        settings.get_subjects_lifetime());
}

} // namespace helix::ui
