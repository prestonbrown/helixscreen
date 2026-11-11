// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_db_item.h"

#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix {

void post_item_if_changed(IMoonrakerAPI& api, const std::string& namespace_name,
                          const std::string& key, const nlohmann::json& value,
                          helix::SharedCallback<void()> on_written,
                          helix::SharedCallback<void(const MoonrakerError&)> on_error,
                          helix::SharedCallback<void()> on_current) {
    auto post = [&api, namespace_name, key, value, on_written, on_error]() {
        api.database_post_item(namespace_name, key, value, on_written, on_error);
    };
    api.database_get_item(
        namespace_name, key,
        [post, value, namespace_name, key, on_current](const nlohmann::json& stored) {
            if (stored == value) {
                spdlog::debug("[Moonraker DB] {}/{} already current", namespace_name, key);
                if (on_current) {
                    on_current();
                }
                return;
            }
            post();
        },
        // No record yet (Moonraker answers a missing key with an error).
        [post](const MoonrakerError&) { post(); });
}

} // namespace helix
