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

/// Seed the config section @p printer_base (a Config::df() taken when the name was asked for)
/// with the resolved name, and show it if that printer is still the active one: a name can
/// land after a switch to another printer.
/// Must be called on the UI thread (via queue_update), or on main thread during init.
static void seed_name(const std::string& name, const char* source,
                      const std::string& printer_base) {
    Config* cfg = Config::get_instance();
    // Config::set() creates missing objects, so writing to a printer deleted meanwhile would
    // bring it back with no host.
    if (!cfg->exists(printer_base.substr(0, printer_base.size() - 1))) {
        spdlog::debug("[PrinterNameSync] {} was removed; dropping its name", printer_base);
        return;
    }
    cfg->set<std::string>(printer_base + wizard::PRINTER_NAME, name);
    cfg->save();

    if (cfg->df() == printer_base) {
        get_printer_state().set_active_printer_name(name);
    }
    spdlog::info("[PrinterNameSync] Seeded name at {} from {}: '{}'", printer_base, source, name);
}

/// Try Fluidd DB, then hostname, then give up. Called when Mainsail is unavailable or empty.
static void try_fluidd_then_hostname(IMoonrakerAPI* api, const std::string& hostname,
                                     const std::string& printer_base) {
    api->database_get_item(
        FLUIDD_NAMESPACE, FLUIDD_KEY,
        [hostname, printer_base](const nlohmann::json& fluidd_value) {
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
            helix::ui::queue_update("PrinterNameSync::fluidd", [name, source, printer_base]() {
                seed_name(name, source, printer_base);
            });
        },
        [hostname, printer_base](const MoonrakerError& err) {
            // A lost connection says nothing about the name; only a real "no Fluidd name"
            // falls back to the hostname.
            if (err.is_transport_loss()) {
                return;
            }
            if (hostname.empty() || hostname == "unknown")
                return;

            helix::ui::queue_update("PrinterNameSync::hostname", [hostname, printer_base]() {
                seed_name(hostname, "hostname", printer_base);
            });
        });
}

void PrinterNameSync::resolve(IMoonrakerAPI* api, const std::string& hostname) {
    Config* config = Config::get_instance();
    const std::string printer_base = config->df();

    // Check local config first — if set, we're done (local wins)
    std::string local_name = config->get<std::string>(config->df() + wizard::PRINTER_NAME, "");
    if (!local_name.empty()) {
        spdlog::debug("[PrinterNameSync] Local name already set: '{}'", local_name);
        return;
    }

    if (!api) {
        spdlog::debug("[PrinterNameSync] No API, falling back to hostname");
        if (!hostname.empty() && hostname != "unknown") {
            seed_name(hostname, "hostname", printer_base);
        }
        return;
    }

    // Try Mainsail first, then Fluidd, then hostname
    api->database_get_item(
        MAINSAIL_NAMESPACE, MAINSAIL_KEY,
        [api, hostname, printer_base](const nlohmann::json& value) {
            std::string name;
            if (value.is_string()) {
                name = value.get<std::string>();
            }

            if (!name.empty()) {
                helix::ui::queue_update("PrinterNameSync::mainsail", [name, printer_base]() {
                    seed_name(name, "Mainsail", printer_base);
                });
                return;
            }

            // Mainsail key exists but empty — fall through to Fluidd
            try_fluidd_then_hostname(api, hostname, printer_base);
        },
        [api, hostname, printer_base](const MoonrakerError& err) {
            if (err.is_transport_loss()) {
                return;
            }
            // Mainsail namespace doesn't exist — try Fluidd
            try_fluidd_then_hostname(api, hostname, printer_base);
        });
}

void PrinterNameSync::write_back(IMoonrakerAPI* api, const std::string& name) {
    if (!api || name.empty())
        return;

    api->database_post_item(
        MAINSAIL_NAMESPACE, MAINSAIL_KEY, name,
        [name]() { spdlog::debug("[PrinterNameSync] Wrote '{}' to Mainsail DB", name); },
        [](const MoonrakerError& err) {
            spdlog::warn("[PrinterNameSync] Failed to write to Mainsail DB: {}", err.message);
        });

    api->database_post_item(
        FLUIDD_NAMESPACE, FLUIDD_KEY, name,
        [name]() { spdlog::debug("[PrinterNameSync] Wrote '{}' to Fluidd DB", name); },
        [](const MoonrakerError& err) {
            spdlog::warn("[PrinterNameSync] Failed to write to Fluidd DB: {}", err.message);
        });
}

} // namespace helix
