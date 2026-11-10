// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_overview_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_settings_hardware.h"
#include "ui_settings_printing.h"
#include "ui_settings_safety.h"
#include "ui_settings_storage.h"
#include "ui_spoolman_overlay.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "lvgl.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <utility>

namespace helix::ui {

namespace {
constexpr const char* kLinePrefix = "plugin_overview_";
} // namespace

PluginOverviewOverlay::~PluginOverviewOverlay() {
    subjects_.deinit_all();
}

void PluginOverviewOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(version_subject_, version_buf_, "-", "plugin_overview_version",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(running_count_subject_, 0, "plugin_overview_running_count",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(off_count_subject_, 0, "plugin_overview_off_count", subjects_);
        UI_MANAGED_SUBJECT_INT(unavailable_count_subject_, 0, "plugin_overview_unavailable_count",
                               subjects_);
    });
}

void PluginOverviewOverlay::register_callbacks() {
    register_xml_callbacks({{"on_plugin_overview_feature_clicked", on_feature_clicked}});
}

lv_obj_t* PluginOverviewOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    running_rows_ = find_required(overlay_root_, "running_rows", get_name());
    off_rows_ = find_required(overlay_root_, "off_rows", get_name());
    unavailable_rows_ = find_required(overlay_root_, "unavailable_rows", get_name());
    // A switch flipped on another page comes back as a new status answer.
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](PluginOverviewOverlay* self, int) { self->refresh(); }, ps.get_subjects_lifetime());
    return overlay_root_;
}

void PluginOverviewOverlay::before_show() {
    refresh();
}

PluginOverviewRow plugin_overview_row(const PluginFeatureLine& line) {
    PluginOverviewRow row;
    row.status = line.status;
    if (line.status != PluginFeatureStatus::Unavailable) {
        return row;
    }
    row.reason =
        line.reason.empty() ? lv_tr("The plugin didn't say why.") : plugin_reason_text(line.reason);
    return row;
}

void PluginOverviewOverlay::refresh() {
    if (!overlay_root_ || !running_rows_ || !off_rows_ || !unavailable_rows_ ||
        !subjects_initialized_) {
        return;
    }
    safe_clean_children(running_rows_);
    safe_clean_children(off_rows_);
    safe_clean_children(unavailable_rows_);
    auto& status = get_printer_state().plugin_status_state();
    lines_ = plugin_feature_lines(status.features());

    lv_subject_copy_string(&version_subject_,
                           status.version().empty() ? "-" : status.version().c_str());
    int running = 0;
    int off = 0;
    int unavailable = 0;
    // DECLARATIVE_OK: the rows are the plugin's feature map, data the XML cannot
    // enumerate; each is a shared settings row component.
    for (const PluginFeatureLine& line : lines_) {
        switch (line.status) {
        case PluginFeatureStatus::Active:
            ++running;
            break;
        case PluginFeatureStatus::Off:
            ++off;
            break;
        case PluginFeatureStatus::Unavailable:
            ++unavailable;
            break;
        }
        create_line(line);
    }
    lv_subject_set_int(&running_count_subject_, running);
    lv_subject_set_int(&off_count_subject_, off);
    lv_subject_set_int(&unavailable_count_subject_, unavailable);
}

void PluginOverviewOverlay::create_line(const PluginFeatureLine& line) {
    // A known feature carries English text with a translation; an unknown one
    // is named by its key, which is not a translation key.
    const bool known = line.label != line.name;
    const std::string label = known ? lv_tr(line.label.c_str()) : line.label;
    const PluginOverviewRow row = plugin_overview_row(line);
    const std::string name = std::string(kLinePrefix) + line.name;
    lv_obj_t* group = running_rows_;
    const char* icon_variant = "success";
    switch (row.status) {
    case PluginFeatureStatus::Active:
        break;
    case PluginFeatureStatus::Off:
        group = off_rows_;
        icon_variant = "secondary";
        break;
    case PluginFeatureStatus::Unavailable:
        group = unavailable_rows_;
        icon_variant = "muted";
        break;
    }
    // A line with a setting leads to it, one without takes no tap.
    const bool leads = line.home != PluginFeatureHome::None;
    std::vector<std::pair<std::string, std::string>> attrs = {
        {"label", label},
        {"label_tag", known ? line.label : ""},
        {"icon", line.icon},
        {"icon_variant", icon_variant},
        {"pad_ver", "#space_sm"},
        {"description", row.reason},
        {"description_min_bp", "0"},
        {"chevron", leads ? "true" : "false"},
        {"clickable", leads ? "true" : "false"}};
    if (leads) {
        attrs.push_back({"callback", "on_plugin_overview_feature_clicked"});
    }
    std::vector<const char*> pairs;
    pairs.reserve(attrs.size() * 2 + 1);
    for (const auto& [key, value] : attrs) {
        pairs.push_back(key.c_str());
        pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);
    auto* obj = static_cast<lv_obj_t*>(lv_xml_create(group, "setting_action_row", pairs.data()));
    if (!obj) {
        spdlog::warn("[{}] Failed to create the row for '{}'", get_name(), name);
        return;
    }
    // The click callback reads the feature from the name, and `helix-screen
    // ctl` addresses the line by it.
    lv_obj_set_name(obj, name.c_str());
}

void PluginOverviewOverlay::on_feature_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginOverviewOverlay] on_feature_clicked");
    auto& overlay = get_plugin_overview_overlay();
    const size_t prefix_len = std::strlen(kLinePrefix);
    for (auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e)); obj != nullptr;
         obj = lv_obj_get_parent(obj)) {
        const char* name = lv_obj_get_name(obj);
        if (name == nullptr || std::strncmp(name, kLinePrefix, prefix_len) != 0) {
            continue;
        }
        for (const PluginFeatureLine& line : overlay.lines()) {
            if (line.name != name + prefix_len) {
                continue;
            }
            switch (line.home) {
            case PluginFeatureHome::Printing:
                helix::settings::get_printing_settings_overlay().show(overlay.parent_screen_);
                break;
            case PluginFeatureHome::Storage:
                helix::settings::get_storage_settings_overlay().show(overlay.parent_screen_);
                break;
            case PluginFeatureHome::Spoolman: {
                auto& spoolman = helix::ui::get_spoolman_overlay();
                if (IMoonrakerAPI* api = get_moonraker_api()) {
                    spoolman.set_api(api);
                }
                spoolman.show(overlay.parent_screen_);
                break;
            }
            case PluginFeatureHome::Devices:
                helix::settings::get_hardware_settings_overlay().show(overlay.parent_screen_);
                break;
            case PluginFeatureHome::Safety:
                helix::settings::get_safety_settings_overlay().show(overlay.parent_screen_);
                break;
            case PluginFeatureHome::None:
                break;
            }
            return;
        }
        return;
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
