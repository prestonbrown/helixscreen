// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wifi_ui_utils.h"

#include <string>
#include <string_view>

/// One-line live status shown under each Settings root row. Pure: callers pass
/// raw values, so every branch is testable without LVGL state.
namespace helix::settings::status {

std::string display(int brightness_pct, int sleep_sec, bool has_dimming);
std::string appearance(bool dark, std::string_view theme_name);
std::string sound(bool enabled, int volume_pct);
std::string devices(int hardware_status_level);
std::string connection(bool ethernet_up, helix::ui::wifi::LinkPhase wifi, std::string_view ssid);
/// Network Settings' Wi-Fi IP row: the address, or that one is on its way.
std::string wifi_address(helix::ui::wifi::LinkPhase wifi, std::string_view ip);
std::string language_time(std::string_view language_name, int time_format);
std::string updates(int update_status, std::string_view new_version,
                    std::string_view current_version, bool firmware_managed);

} // namespace helix::settings::status
