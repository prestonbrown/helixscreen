// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/panel_widget_manager_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget_manager.h"

#include <cstddef>

namespace helix {

/// Pins the manager's per-container descriptor ownership: a slot exists exactly
/// while the container it was installed on is alive.
struct PanelWidgetManagerTestAccess {
    static size_t grid_descriptor_count(const PanelWidgetManager& mgr) {
        return mgr.grid_descriptors_.size();
    }
};

} // namespace helix
