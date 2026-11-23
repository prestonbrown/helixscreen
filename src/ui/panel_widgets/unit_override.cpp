// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/unit_override.h"

#include "ui_text_entry_modal.h"

#include "core/text_io.h"
#include "json_utils.h"
#include "panel_widget_manager.h"

#include <utility>

#include "hv/json.hpp"

namespace helix::unit_override {

std::optional<std::string> clamp(std::optional<std::string> unit) {
    if (unit) {
        unit = std::string(text_io::utf8_prefix(*unit, kMaxBytes));
    }
    return unit;
}

std::optional<std::string> from_config(const nlohmann::json& config) {
    return clamp(json_util::read_field<std::string>(config, kKey));
}

void to_config(nlohmann::json& config, const std::optional<std::string>& unit) {
    if (unit) {
        config[kKey] = *unit;
    } else {
        config.erase(kKey);
    }
}

std::string resolve(const std::optional<std::string>& unit, const std::string& default_unit) {
    return unit ? *unit : default_unit;
}

std::string unit_row_value(const std::optional<std::string>& unit,
                           const std::string& default_unit) {
    return unit && unit->empty() ? std::string(lv_tr("None")) : resolve(unit, default_unit);
}

void show_entry(std::function<void(const std::string&)> apply_live, LifetimeToken token,
                std::string panel, std::string instance_id, std::string initial) {
    helix::ui::TextEntryModal::Config entry;
    entry.title = lv_tr("Unit text");
    entry.initial = std::move(initial);
    entry.placeholder = lv_tr("Leave empty for no unit");
    entry.accept_text = lv_tr("Save");
    entry.max_length = kMaxChars;
    // A rebuild retires the token; the edit then goes straight to the stored
    // config and the rebuilt tile reads it from there.
    entry.on_accept = [apply_live = std::move(apply_live), token, panel = std::move(panel),
                       id = std::move(instance_id)](const std::string& text) {
        if (!token.expired()) { // L081_OK: on_accept runs on the UI thread, from the OK button
            apply_live(text);
            return;
        }
        auto& mgr = PanelWidgetManager::instance();
        nlohmann::json config = mgr.get_widget_config(panel).get_widget_config(id);
        to_config(config, clamp(text));
        mgr.get_widget_config(panel).set_widget_config(id, config);
        mgr.notify_config_changed(panel);
    };
    helix::ui::TextEntryModal::show_owned(std::move(entry));
}

} // namespace helix::unit_override
