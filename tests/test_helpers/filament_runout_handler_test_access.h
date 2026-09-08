// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_runout_handler.h"

namespace helix::ui {

// Reaches FilamentRunoutHandler's private dispatch_load() / dispatch_unload() /
// dispatch_purge(), which production code only enters through the guidance
// modal's buttons — widgets that need a live modal, a screen, and a paused print
// to press. Declared a friend of FilamentRunoutHandler; follows the
// tests/test_helpers/ TestAccess pattern ([L088]) rather than adding
// _for_testing() accessors.
class FilamentRunoutHandlerTestAccess {
  public:
    static void dispatch_load(FilamentRunoutHandler& handler) {
        handler.dispatch_load();
    }
    static void dispatch_unload(FilamentRunoutHandler& handler) {
        handler.dispatch_unload();
    }
    static void dispatch_purge(FilamentRunoutHandler& handler) {
        handler.dispatch_purge();
    }
    /// Raises the real guidance dialog with its real button callbacks wired, so
    /// a test can press Cancel Print rather than call a dispatch helper. The
    /// cancel path has no private dispatch method of its own - its whole
    /// behaviour is the callback wired here plus the confirmation it raises.
    static void show_guidance_modal(FilamentRunoutHandler& handler) {
        handler.show_runout_guidance_modal();
    }
    static RunoutGuidanceModal& guidance_modal(FilamentRunoutHandler& handler) {
        return handler.runout_modal_;
    }
};

} // namespace helix::ui
