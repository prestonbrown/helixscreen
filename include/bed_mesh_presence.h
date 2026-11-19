// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "hv/json.hpp" // JSON_HPP_OK: inline reader of the json

namespace helix {

/// Whether a `bed_mesh.probed_matrix` status value holds a loaded mesh. Klipper
/// and Kalico report `[[]]` when none is loaded, so presence means a row with
/// at least one point; an empty array or null reads absent too.
inline bool probed_matrix_has_mesh(const nlohmann::json& matrix) {
    if (!matrix.is_array()) {
        return false;
    }
    for (const auto& row : matrix) {
        if (row.is_array() && !row.empty()) {
            return true;
        }
    }
    return false;
}

/// The same verdict for a parsed matrix: a mesh is present when any row holds a point.
inline bool probed_matrix_has_mesh(const std::vector<std::vector<float>>& matrix) {
    for (const auto& row : matrix) {
        if (!row.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace helix
