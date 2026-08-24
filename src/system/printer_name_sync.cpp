// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_name_sync.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

namespace helix {

// Moonraker database coordinates for external UI clients
static constexpr const char* MAINSAIL_NAMESPACE = "mainsail";
static constexpr const char* MAINSAIL_KEY = "general.printername";
static constexpr const char* FLUIDD_NAMESPACE = "fluidd";
static constexpr const char* FLUIDD_KEY = "general.instanceName";

/// Seed local config and update PrinterState subject with the resolved name.
/// Must be called on the UI thread (via queue_update), or on main thread during init.
static void seed_name(const std::string& name, const char* source) {
    Config* cfg = Config::get_instance();
    if (cfg) {
        cfg->set<std::string>(cfg->df() + wizard::PRINTER_NAME, name);
        cfg->save();
    }
    get_printer_state().set_active_printer_name(name);
    spdlog::info("[PrinterNameSync] Seeded name from {}: '{}'", source, name);
}

/// Try Fluidd DB, then hostname, then give up. Called when Mainsail is unavailable or empty.
static void try_fluidd_then_hostname(IMoonrakerAPI* api, const std::string& hostname) {
    api->database_get_item(
        FLUIDD_NAMESPACE, FLUIDD_KEY,
        [hostname](const nlohmann::json& fluidd_value) {
            std::string name;
            if (fluidd_value.is_string()) {
                name = fluidd_value.get<std::string>();
            }

            if (name.empty() && !hostname.empty() && hostname != "unknown") {
                name = hostname;
            }

            if (name.empty())
                return;

            const char* source = (name == hostname) ? "hostname" : "Fluidd";
            helix::ui::queue_update("PrinterNameSync::fluidd",
                                    [name, source]() { seed_name(name, source); });
        },
        [hostname](const MoonrakerError&) {
            if (hostname.empty() || hostname == "unknown")
                return;

            helix::ui::queue_update("PrinterNameSync::hostname",
                                    [hostname]() { seed_name(hostname, "hostname"); });
        });
}

void PrinterNameSync::resolve(IMoonrakerAPI* api, const std::string& hostname) {
    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[PrinterNameSync] No config instance, skipping resolve");
        return;
    }

    // Check local config first — if set, we're done (local wins)
    std::string local_name = config->get<std::string>(config->df() + wizard::PRINTER_NAME, "");
    if (!local_name.empty()) {
        spdlog::debug("[PrinterNameSync] Local name already set: '{}'", local_name);
        return;
    }

    if (!api) {
        spdlog::debug("[PrinterNameSync] No API, falling back to hostname");
        if (!hostname.empty() && hostname != "unknown") {
            seed_name(hostname, "hostname");
        }
        return;
    }

    // Try Mainsail first, then Fluidd, then hostname
    api->database_get_item(
        MAINSAIL_NAMESPACE, MAINSAIL_KEY,
        [api, hostname](const nlohmann::json& value) {
            std::string name;
            if (value.is_string()) {
                name = value.get<std::string>();
            }

            if (!name.empty()) {
                helix::ui::queue_update("PrinterNameSync::mainsail",
                                        [name]() { seed_name(name, "Mainsail"); });
                return;
            }

            // Mainsail key exists but empty — fall through to Fluidd
            try_fluidd_then_hostname(api, hostname);
        },
        [api, hostname](const MoonrakerError&) {
            // Mainsail namespace doesn't exist — try Fluidd
            try_fluidd_then_hostname(api, hostname);
        });
}

void PrinterNameSync::write_back(IMoonrakerAPI* api, const std::string& name) {
    if (!api || name.empty())
        return;

    api->database_post_item(
        MAINSAIL_NAMESPACE, MAINSAIL_KEY, name,
        [name]() { spdlog::debug("[PrinterNameSync] Wrote '{}' to Mainsail DB", name); },
        [](const MoonrakerError& err) {
            spdlog::warn("[PrinterNameSync] Failed to write to Mainsail DB: {}",
                         err.user_message());
        });

    api->database_post_item(
        FLUIDD_NAMESPACE, FLUIDD_KEY, name,
        [name]() { spdlog::debug("[PrinterNameSync] Wrote '{}' to Fluidd DB", name); },
        [](const MoonrakerError& err) {
            spdlog::warn("[PrinterNameSync] Failed to write to Fluidd DB: {}", err.user_message());
        });
}

} // namespace helix
