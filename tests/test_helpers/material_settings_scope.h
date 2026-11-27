// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "material_settings_manager.h"

namespace helix::test {

/// Material settings are process-wide; the config dir is sandboxed by the base
/// fixture. Establish and clean the baseline even when a REQUIRE unwinds a case.
struct MaterialSettingsScope {
    MaterialSettingsScope() {
        MaterialSettingsManager::instance().init();
        reset();
    }
    ~MaterialSettingsScope() {
        reset();
    }
    MaterialSettingsScope(const MaterialSettingsScope&) = delete;
    MaterialSettingsScope& operator=(const MaterialSettingsScope&) = delete;

    static void reset() {
        auto& manager = MaterialSettingsManager::instance();
        const auto overrides = manager.get_all_overrides();
        for (const auto& [name, unused] : overrides) {
            (void)unused;
            manager.clear_override(name);
        }
        manager.reset_preset_materials();
    }
};

} // namespace helix::test
