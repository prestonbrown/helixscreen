// SPDX-License-Identifier: GPL-3.0-or-later

#include "sliced_signal_names.h"

#include "config.h"
#include "gcode_response_lines.h"
#include "i_moonraker_client.h"
#include "signal_macro_names.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "hv/json.hpp"

namespace helix {

namespace {

constexpr const char* kHandlerName = "sliced_signal_watcher";

/// Settings leaf for the active printer's recorded names.
std::string store_leaf() {
    return Config::get_instance()->df() + "helix_macros/sliced_signal_names";
}

bool is_signal_name(const std::string& name) {
    return std::find(signal_macros::kVisibleNames.begin(), signal_macros::kVisibleNames.end(),
                     name) != signal_macros::kVisibleNames.end();
}

} // namespace

namespace signal_macros {

std::vector<std::string> recorded_signal_names() {
    const nlohmann::json stored =
        Config::get_instance()->get<nlohmann::json>(store_leaf(), nlohmann::json::array());
    std::vector<std::string> names;
    if (!stored.is_array()) {
        return names;
    }
    for (auto visible : kVisibleNames) {
        const bool listed = std::any_of(stored.begin(), stored.end(), [visible](const auto& v) {
            return v.is_string() && v.template get_ref<const std::string&>() == visible;
        });
        if (listed) {
            names.emplace_back(visible);
        }
    }
    return names;
}

bool record_signal_name(const std::string& visible) {
    if (!is_signal_name(visible)) {
        return false;
    }
    std::vector<std::string> names = recorded_signal_names();
    if (std::find(names.begin(), names.end(), visible) != names.end()) {
        return false;
    }
    names.push_back(visible);
    Config* config = Config::get_instance();
    config->set<nlohmann::json>(store_leaf(), names);
    config->save();
    return true;
}

} // namespace signal_macros

SlicedSignalWatcher::SlicedSignalWatcher(IMoonrakerClient* client, NoticeFn notice)
    : client_(client), notice_(std::move(notice)) {
    if (!client_) {
        return;
    }
    // The response arrives on the WebSocket thread; the settings write and the
    // notice belong on the main thread.
    client_->register_method_callback(
        "notify_gcode_response", kHandlerName,
        lifetime_.bg_cb("SlicedSignalWatcher::on_response", [this](const nlohmann::json& msg) {
            for_each_gcode_response_line(msg, [this](const std::string& line) { on_line(line); });
        }));
}

SlicedSignalWatcher::~SlicedSignalWatcher() {
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", kHandlerName);
    }
}

void SlicedSignalWatcher::on_line(const std::string& line) {
    const auto name = signal_macros::unknown_signal_command(line);
    if (!name) {
        return;
    }
    if (signal_macros::record_signal_name(*name)) {
        spdlog::info("[SlicedSignalWatcher] Klipper does not know {}; the next macro update "
                     "forwards it",
                     *name);
    }
    if (noticed_.insert(*name).second && notice_) {
        notice_(*name);
    }
}

} // namespace helix
