// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_favorites.h"

#include "config.h"

#include <algorithm>

namespace helix::filament_favorites {

std::vector<std::string> load_favorite_ids() {
    helix::Config* cfg = helix::Config::get_instance();
    // get_string_array() is non-vivifying: an absent path reads as empty and
    // writes nothing back (#1129).
    return cfg ? cfg->get_string_array(kFavoriteIdsPath) : std::vector<std::string>{};
}

bool is_favorite(const std::string& product_id) {
    if (product_id.empty())
        return false;
    const auto ids = load_favorite_ids();
    return std::find(ids.begin(), ids.end(), product_id) != ids.end();
}

bool toggle_favorite(const std::string& product_id) {
    if (product_id.empty())
        return false;
    auto ids = load_favorite_ids();
    auto it = std::find(ids.begin(), ids.end(), product_id);
    const bool now_favorite = (it == ids.end());
    if (now_favorite) {
        ids.push_back(product_id);
    } else {
        ids.erase(it);
    }
    if (helix::Config* cfg = helix::Config::get_instance()) {
        cfg->set(kFavoriteIdsPath, ids);
        cfg->save(); // star toggles persist immediately, no separate save step
    }
    return now_favorite;
}

} // namespace helix::filament_favorites
