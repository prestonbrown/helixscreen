// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file observe_language.h
 * @brief observe_language_change(), kept apart from core/observer_factory.h so the
 *        settings manager header reaches only the TUs that re-render on a language switch
 */

#pragma once

#include "core/observer_factory.h"
#include "system_settings_manager.h"

#include <memory>

namespace helix::ui {

/**
 * @brief Re-render C++-formatted text after every language switch
 *
 * XML re-translates what it bound through translation_tag. Text C++ produced
 * with lv_tr() was translated once, when it was set, and stays in the old
 * language until the owner formats it again: this is the trigger for that.
 * The handler runs deferred, so the new translation pack is already active,
 * and only on a real switch (not on registration).
 *
 * @tparam Panel Owner class type
 * @tparam OnChange Callable: void(Panel*)
 */
template <typename Panel, typename OnChange>
ObserverGuard observe_language_change(Panel* panel, OnChange&& on_change) {
    auto& settings = SystemSettingsManager::instance();
    lv_subject_t* language = settings.subject_language();
    // Held by pointer so the non-mutable handler can update it.
    auto shown = std::make_shared<int>(lv_subject_get_int(language));
    return observe<int>(
        language, panel,
        [shown, on_change = std::forward<OnChange>(on_change)](Panel* p, int index) {
            if (index == *shown) {
                return;
            }
            *shown = index;
            on_change(p);
        },
        settings.get_subjects_lifetime());
}

} // namespace helix::ui
