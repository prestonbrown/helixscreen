// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_root_status.h"

#include "lvgl.h"

#include <spdlog/fmt/fmt.h>

namespace helix::settings::status {

namespace {
std::string sleep_part(int sleep_sec, bool leading) {
    if (sleep_sec <= 0) {
        return leading ? lv_tr("Never sleeps") : lv_tr("never sleeps");
    }
    return fmt::format(fmt::runtime(leading ? lv_tr("Sleep {} min") : lv_tr("sleep {} min")),
                       sleep_sec / 60);
}
} // namespace

std::string display(int brightness_pct, int sleep_sec, bool has_dimming) {
    if (!has_dimming) {
        return sleep_part(sleep_sec, true);
    }
    return fmt::format("{}% · {}", brightness_pct, sleep_part(sleep_sec, false));
}

std::string appearance(bool dark, std::string_view theme_name) {
    // Bare "Light" is also the LED/lamp key (panel_widget_led.xml,
    // controls_panel.xml); the Appearance overlay's own toggle-row labels
    // ("Dark Mode"/"Light Mode") are the unambiguous pair for a theme mode.
    const char* mode = dark ? lv_tr("Dark Mode") : lv_tr("Light Mode");
    if (theme_name.empty()) {
        return mode;
    }
    return fmt::format("{} · {}", mode, theme_name);
}

std::string sound(bool enabled, int volume_pct) {
    if (!enabled || volume_pct <= 0) {
        return lv_tr("Muted");
    }
    return fmt::format(fmt::runtime(lv_tr("Volume {}%")), volume_pct);
}

std::string devices(int hardware_status_level) {
    switch (hardware_status_level) {
    case 1:
        return lv_tr("Needs attention");
    case 2:
        return lv_tr("Problem found");
    default:
        return lv_tr("All healthy");
    }
}

std::string connection(bool ethernet_up, bool wifi_connected, std::string_view ssid) {
    if (ethernet_up) {
        return lv_tr("Ethernet");
    }
    if (wifi_connected) {
        return ssid.empty() ? std::string(lv_tr("Wi-Fi"))
                            : fmt::format(fmt::runtime(lv_tr("Wi-Fi {}")), ssid);
    }
    return lv_tr("Not connected");
}

std::string language_time(std::string_view language_name, int time_format) {
    return fmt::format("{} · {}", language_name,
                       time_format == 1 ? lv_tr("24-hour") : lv_tr("12-hour"));
}

std::string updates(int update_status, std::string_view new_version,
                    std::string_view current_version, bool firmware_managed) {
    if (firmware_managed) {
        return lv_tr("Managed by firmware");
    }
    switch (update_status) {
    case 1:
        return lv_tr("Checking…");
    case 2:
        return fmt::format(fmt::runtime(lv_tr("{} available")), new_version);
    case 3:
        return lv_tr("Up to date");
    case 4:
        return lv_tr("Check failed");
    default:
        return fmt::format(fmt::runtime(lv_tr("Version {}")), current_version);
    }
}

} // namespace helix::settings::status
