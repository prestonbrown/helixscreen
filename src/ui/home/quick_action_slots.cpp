// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quick_action_slots.h"

#include "config.h"
#include "led/led_controller.h"
#include "standard_macros.h"

namespace helix {

StoredQuickSlots read_stored_quick_slots() {
    StoredQuickSlots out;
    Config* config = Config::get_instance();
    for (size_t i = 0; i < kQuickButtonKeys.size(); ++i) {
        out.user_set[i] = config->exists(kQuickButtonKeys[i]);
        out.value[i] = config->get<std::string>(kQuickButtonKeys[i], kQuickButtonDefaults[i]);
    }
    return out;
}

std::array<QuickSlotKind, 4> resolve_current_quick_slots(const StoredQuickSlots& stored) {
    auto& macros = StandardMacros::instance();
    std::array<QuickSlotInput, 4> inputs{};
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i].user_set = stored.user_set[i];
        inputs[i].value = stored.value[i];
        if (stored.value[i].empty() || stored.value[i] == kQuickSlotLight) {
            continue;
        }
        if (auto slot = StandardMacros::slot_from_name(stored.value[i])) {
            const auto& info = macros.get(*slot);
            inputs[i].macro_renders = !info.is_empty() || info.has_missing_macro();
        }
    }
    const bool led_controllable =
        lv_subject_get_int(led::LedController::instance().get_led_controllable_subject()) != 0;
    return resolve_quick_slots(inputs, led_controllable);
}

} // namespace helix
