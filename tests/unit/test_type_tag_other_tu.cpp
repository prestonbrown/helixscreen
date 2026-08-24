// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Second translation unit for test_type_tag.cpp. helix::type_tag<T>() relies on
// a function-local static in an inline template, so "same tag for the same T"
// is only a real claim if it is checked across TU boundaries - one TU alone
// cannot fail that way.

#include "helix_type_tag.h"
#include "panel_widget_manager.h"

#include <cstddef>
#include <string>

namespace type_tag_other_tu {

std::size_t tag_int() {
    return helix::type_tag<int>();
}

std::size_t tag_string() {
    return helix::type_tag<std::string>();
}

std::size_t tag_manager() {
    return helix::type_tag<helix::PanelWidgetManager>();
}

} // namespace type_tag_other_tu
