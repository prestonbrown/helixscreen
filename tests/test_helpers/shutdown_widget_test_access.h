// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/shutdown_widget.h"

namespace helix {

// Friend access to ShutdownWidget's cached tile pointers. The teardown contract
// is which raw pointers survive which deletion path, and that is only
// observable on the privates. Read-only.
//
// Defined in ONE place, in namespace helix to match the friend declaration.
// Follows the tests/test_helpers/ TestAccess pattern ([L088]).
class ShutdownWidgetTestAccess {
  public:
    static lv_obj_t* widget_obj(const ShutdownWidget& widget) {
        return widget.widget_obj_;
    }

    static lv_obj_t* parent_screen(const ShutdownWidget& widget) {
        return widget.parent_screen_;
    }
};

} // namespace helix
