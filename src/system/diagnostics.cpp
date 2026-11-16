// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file diagnostics.cpp
 * @brief The one place the resolved runtime facts are gathered.
 *
 * Each value comes from the accessor that owns its cascade, so a rung added
 * anywhere shows up here with no edit. Nothing in this file re-derives a path.
 */

#include "system/diagnostics.h"

#include "app_constants.h"
#include "app_globals.h"
#include "board_identity.h"
#include "config.h"
#include "core/data_root_resolver.h"
#include "core/text_io.h"
#include "i_moonraker_api.h"
#include "logging_init.h"
#include "platform_capabilities.h"
#include "platform_info.h"
#include "platform_table.h"
#include "system/update_checker.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

namespace tio = helix::text_io;

#if !defined(HELIX_PLATFORM_ESP32)
#include <sys/utsname.h>
#endif

namespace helix::diagnostics {

namespace {

/// Whole small file as a string, or "" when it cannot be read. /proc files are
/// a few KB and generated on read, so a plain slurp is the right shape.
std::string slurp(const std::string& path) {
    auto body = tio::read_file(path);
    if (!body) {
        return {};
    }
    return std::move(*body);
}

/// `value` unless it is empty, in which case the placeholder the struct carries.
void set_if_present(std::string& field, const std::string& value) {
    if (!value.empty()) {
        field = value;
    }
}

} // namespace

Machine read_machine(const std::string& proc_root) {
    Machine m;

    const std::string cpuinfo = slurp(proc_root + "/cpuinfo");
    if (!cpuinfo.empty()) {
        const CpuInfo cpu = parse_cpuinfo(cpuinfo);
        m.cpu_cores = cpu.core_count;
        set_if_present(m.cpu_model, cpu.model);
    }

    const std::string meminfo = slurp(proc_root + "/meminfo");
    if (!meminfo.empty()) {
        m.mem_total_kb = parse_meminfo_kb(meminfo, "MemTotal");
        m.mem_available_kb = parse_meminfo_kb(meminfo, "MemAvailable");
    }

    const std::string uptime = slurp(proc_root + "/uptime");
    if (!uptime.empty()) {
        auto t = tio::split_ws(uptime);
        if (!t.empty()) {
            if (auto v = tio::parse_double(t[0])) {
                m.uptime_seconds = *v;
            }
        }
    }

    // The three load figures only; the trailing running/total and last-pid
    // fields change every read and say nothing about the machine.
    const std::string loadavg = slurp(proc_root + "/loadavg");
    if (!loadavg.empty()) {
        auto t = tio::split_ws(loadavg);
        if (t.size() >= 3) {
            m.loadavg = std::string(t[0]) + " " + std::string(t[1]) + " " + std::string(t[2]);
        }
    }

#if !defined(HELIX_PLATFORM_ESP32)
    struct utsname uts {};
    if (::uname(&uts) == 0) {
        set_if_present(m.kernel_release, uts.release);
        set_if_present(m.kernel_arch, uts.machine);
    }
#endif

    return m;
}

std::string staging_dir_for(const std::string& install_root) {
    if (install_root.empty()) {
        return "";
    }
    // The self-update shape: an archive downloaded beside the install tree.
    // The helper applies the outside-the-install-root rule install.sh depends
    // on, so the answer is a sibling of the install dir, never a child.
    return UpdateChecker::compute_update_staging_dir(install_root + "/update.tar.gz", install_root);
}

Diagnostics collect() {
    Diagnostics d;

    // --- Paths -------------------------------------------------------------
    d.paths.install_root = app_get_install_root();
    d.paths.config_dir = app_get_config_dir();

    Config* config = Config::get_instance();
    d.paths.settings_file = config->get_path();

    // Peek, never resolve. get_helix_cache_dir() creates the directory tree as
    // the cost of answering, so an HELIX_CACHE_DIR naming an unmounted stick
    // would be materialized on the root filesystem by the act of reporting it,
    // and this is the first thing in the process to ask. Path and tier come out
    // of the same peek, so they always name the same rung; a creation failure
    // at first use moves the real cache and says so with a [CacheDir] warning.
    const char* cache_tier = nullptr;
    d.paths.cache_dir = peek_helix_cache_dir("", &cache_tier);
    while (d.paths.cache_dir.size() > 1 && d.paths.cache_dir.back() == '/') {
        d.paths.cache_dir.pop_back();
    }
    d.paths.cache_tier = cache_tier != nullptr ? cache_tier : "";

    d.paths.state_dir = app_constants::update::state_dir();
    d.paths.log_file = logging::effective_log_file_path();

    d.paths.updater_staging_dir = staging_dir_for(d.paths.install_root);

    // --- Identity ----------------------------------------------------------
    d.identity.platform_key = helix::platform::current_key();
    d.identity.board = board::current_info().key;
    d.identity.mod_flavor = flashforge_mod_tree_name(flashforge_mod_tree());
    d.identity.printer_model = get_saved_printer_type();
    d.identity.host_arch = host_arch_string();

    // --- Machine -----------------------------------------------------------
    d.machine = read_machine();

    // --- Log ---------------------------------------------------------------
    d.log.destination = logging::effective_destination();
    d.log.level = spdlog::level::to_string_view(logging::effective_log_level()).data();
    d.log.file = d.paths.log_file;

    // --- Moonraker ---------------------------------------------------------
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        d.moonraker_url = api->get_http_base_url();
    }

    return d;
}

void log_diagnostics() {
    const Diagnostics d = collect();

    spdlog::info("[Diagnostics] Platform: {} / {} ({}), {} — {}", d.identity.platform_key,
                 d.identity.board, d.identity.mod_flavor, d.identity.host_arch,
                 d.identity.printer_model.empty() ? "no printer configured"
                                                  : d.identity.printer_model);
    spdlog::info("[Diagnostics] Machine: {} x{}, {} MB RAM ({} MB available), load {}, up {:.0f}s",
                 d.machine.cpu_model, d.machine.cpu_cores, d.machine.mem_total_kb / 1024,
                 d.machine.mem_available_kb / 1024, d.machine.loadavg, d.machine.uptime_seconds);
    spdlog::info("[Diagnostics] Kernel: {} {}", d.machine.kernel_release, d.machine.kernel_arch);
    spdlog::info("[Diagnostics] Install root: {}", d.paths.install_root);
    spdlog::info("[Diagnostics] Config dir: {} (settings: {})", d.paths.config_dir,
                 d.paths.settings_file.empty() ? "none loaded" : d.paths.settings_file);
    spdlog::info("[Diagnostics] Cache dir: {} (tier: {})", d.paths.cache_dir,
                 d.paths.cache_tier.empty() ? "fallback" : d.paths.cache_tier);
    spdlog::info("[Diagnostics] State dir: {}", d.paths.state_dir);
    spdlog::info("[Diagnostics] Updater staging: {}", d.paths.updater_staging_dir);
    spdlog::info("[Diagnostics] Log: {} at {}{}", d.log.destination, d.log.level,
                 d.log.file.empty() ? "" : " -> " + d.log.file);
    if (!d.moonraker_url.empty()) {
        spdlog::info("[Diagnostics] Moonraker: {}", d.moonraker_url);
    }
}

} // namespace helix::diagnostics
