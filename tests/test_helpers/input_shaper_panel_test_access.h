// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_input_shaper.h"

namespace helix {
namespace ui {

// Test-only reach into InputShaperPanel's private save_configuration(), so the
// expected-restart flow tests can drive the real SAVE_CONFIG initiation
// without building the panel's XML UI. Follows the existing TestAccess pattern
// (tests/test_helpers/, [L088]) rather than widening the production API.
struct InputShaperPanelTestAccess {
    static void save_configuration(InputShaperPanel& p) {
        p.save_configuration();
    }
};

} // namespace ui
} // namespace helix
