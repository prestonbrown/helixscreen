// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.h"
#include "json_fwd.h"
#include "led/led_devices.h"
#include "panel_widget_manager.h"

#include <string>

namespace helix::test {

/// A home layout holding exactly @p widgets on its one page, restored on exit
/// together with leds/light_button_pending.
class ScopedHomeLayout {
  public:
    explicit ScopedHomeLayout(const nlohmann::json& widgets) {
        auto* cfg = Config::get_instance();
        const std::string key = cfg->df() + "panel_widgets/home";
        const nlohmann::json* prior = cfg->try_get_json(key);
        had_prior_ = prior != nullptr;
        if (had_prior_) {
            prior_ = *prior;
        }
        cfg->set<nlohmann::json>(key, {{"main_page_index", 0},
                                       {"next_page_id", 1},
                                       {"pages", {{{"id", "main"}, {"widgets", widgets}}}}});
        reload();
    }
    ~ScopedHomeLayout() {
        auto* cfg = Config::get_instance();
        const std::string key = cfg->df() + "panel_widgets/home";
        cfg->set<nlohmann::json>(key, had_prior_ ? prior_ : nlohmann::json::object());
        cfg->set(cfg->df() + led::kLightButtonPendingPath, nlohmann::json());
        reload();
    }
    ScopedHomeLayout(const ScopedHomeLayout&) = delete;
    ScopedHomeLayout& operator=(const ScopedHomeLayout&) = delete;

  private:
    /// The manager caches a loaded layout; the next reader must see this one.
    static void reload() {
        auto& mgr = PanelWidgetManager::instance();
        mgr.clear_panel_config("home");
        mgr.get_widget_config("home").mark_dirty();
    }

    bool had_prior_ = false;
    nlohmann::json prior_;
};

/// A placed 2x2 home widget entry at (@p col, 0).
inline nlohmann::json placed_light(const std::string& id, int col) {
    return {{"id", id}, {"enabled", true}, {"col", col},
            {"row", 0}, {"colspan", 2},    {"rowspan", 2}};
}

} // namespace helix::test
