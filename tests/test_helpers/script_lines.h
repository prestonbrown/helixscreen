// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/text_io.h"

#include <string>
#include <string_view>
#include <vector>

namespace helix::test {

/// A G-code script's lines, as the mock records them one by one.
inline std::vector<std::string> script_lines(std::string_view script) {
    std::vector<std::string> out;
    for (const std::string_view line : helix::text_io::lines(script)) {
        out.emplace_back(line);
    }
    return out;
}

} // namespace helix::test
