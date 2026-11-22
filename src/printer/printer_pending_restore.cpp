// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_pending_restore.h"

#include "config.h"

#include "hv/json.hpp"

namespace helix::pending_restore {

std::string read(const char* key) {
    const Config* config = Config::get_instance();
    if (config == nullptr) {
        return {};
    }
    const nlohmann::json* j = config->try_get_json(config->df() + key);
    return j != nullptr && j->is_string() ? j->get<std::string>() : std::string{};
}

void write(const char* key, const std::string& value) {
    Config* config = Config::get_instance();
    if (config == nullptr || read(key) == value) {
        return;
    }
    const std::string path = config->df() + key;
    if (value.empty()) {
        config->set<nlohmann::json>(path, nullptr);
    } else {
        config->set<nlohmann::json>(path, value);
    }
    config->save();
}

} // namespace helix::pending_restore
