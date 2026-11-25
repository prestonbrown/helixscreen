// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace helix {
/// PROTOTYPE: raise `mock:<component>` (design mockups). False when @p name is not one.
bool show_mockup(const std::string& name);
} // namespace helix
