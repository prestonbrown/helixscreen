// SPDX-License-Identifier: GPL-3.0-or-later

#include "sliced_signal_names.h"

#include "config.h"
#include "core/observer_factory.h"
#include "gcode_response_lines.h"
#include "i_moonraker_client.h"
#include "lvgl.h"
#include "signal_macro_names.h"

#include <spdlog/fmt/fmt.h>
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

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) {
        out += out.empty() ? name : ", " + name;
    }
    return out;
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

std::vector<std::string> record_signal_names(const std::vector<std::string>& visible) {
    std::vector<std::string> names = recorded_signal_names();
    std::vector<std::string> added;
    for (auto name : kVisibleNames) {
        const bool given = std::find(visible.begin(), visible.end(), name) != visible.end();
        if (given && std::find(names.begin(), names.end(), name) == names.end()) {
            added.emplace_back(name);
        }
    }
    if (added.empty()) {
        return added;
    }
    names.insert(names.end(), added.begin(), added.end());
    Config* config = Config::get_instance();
    config->set<nlohmann::json>(store_leaf(), names);
    config->save();
    return added;
}

std::string sliced_signal_notice(const std::vector<std::string>& names, bool update_offered) {
    const std::string list = joined(names);
    if (update_offered) {
        return fmt::format(lv_tr("This print called {}, which the printer no longer has. Tap "
                                 "Update Helper Macros in Advanced to bring them back."),
                           list);
    }
    return fmt::format(lv_tr("This print called {}, which the printer no longer has."), list);
}

} // namespace signal_macros

SlicedSignalWatcher::SlicedSignalWatcher(IMoonrakerClient* client, NoticeFn notice)
    : client_(client), notice_(std::move(notice)) {
    if (!client_) {
        return;
    }
    // Every console line arrives here on the WebSocket thread. Only the rare
    // line naming a signal macro Klipper does not know reaches the main thread.
    client_->register_method_callback(
        "notify_gcode_response", kHandlerName,
        [this, tok = lifetime_.token()](const nlohmann::json& msg) {
            std::vector<std::string> names;
            for_each_gcode_response_line(msg, [&names](const std::string& line) {
                if (auto name = signal_macros::unknown_signal_command(line)) {
                    names.push_back(std::move(*name));
                }
            });
            if (names.empty()) {
                return;
            }
            tok.defer("SlicedSignalWatcher::on_unknown_names",
                      [this, names = std::move(names)] { on_unknown_names(names); });
        });
}

SlicedSignalWatcher::~SlicedSignalWatcher() {
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", kHandlerName);
    }
}

void SlicedSignalWatcher::watch_print_active(lv_subject_t* print_active,
                                             const SubjectLifetime& lifetime) {
    print_observer_ = ui::observe<int>(
        print_active, this,
        [](SlicedSignalWatcher* self, int active) { self->on_print_active(active != 0); },
        lifetime);
}

void SlicedSignalWatcher::on_print_active(bool active) {
    if (active == print_active_) {
        return;
    }
    print_active_ = active;
    if (!active) {
        finish_print();
    }
}

void SlicedSignalWatcher::on_unknown_names(const std::vector<std::string>& visible) {
    if (!print_active_) {
        // A console command typed outside a print says nothing about sliced files.
        spdlog::debug("[SlicedSignalWatcher] {} unknown outside a print, not recorded",
                      joined(visible));
        return;
    }
    pending_.insert(visible.begin(), visible.end());
}

void SlicedSignalWatcher::finish_print() {
    std::vector<std::string> called;
    for (auto name : signal_macros::kVisibleNames) {
        if (pending_.count(std::string(name)) != 0) {
            called.emplace_back(name);
        }
    }
    pending_.clear();
    if (called.empty()) {
        return;
    }
    const auto added = signal_macros::record_signal_names(called);
    if (!added.empty()) {
        spdlog::info("[SlicedSignalWatcher] The print called {}, which Klipper does not know; "
                     "the next macro update forwards them",
                     joined(added));
    }
    bool unannounced = false;
    for (const auto& name : called) {
        unannounced |= noticed_.insert(name).second;
    }
    if (unannounced && notice_) {
        notice_(called);
    }
}

} // namespace helix
