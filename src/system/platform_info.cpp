// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_info.h"

#include "data_root_resolver.h"

#include <spdlog/spdlog.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/utsname.h>

namespace helix {

// -1 = use compile-time default, 0 = force non-Android, 1 = force Android
static int s_platform_override = -1;

bool is_android_platform() {
    if (s_platform_override >= 0) {
        return s_platform_override != 0;
    }
#ifdef __ANDROID__
    return true;
#else
    return false;
#endif
}

void set_platform_override(int override_value) {
    s_platform_override = override_value;
}

// -1 = use compile-time default, 0 = force self-managed, 1 = force externally managed
static int s_external_updates_default_override = -1;

namespace {

/// @p abs_path under @p probe_root ("/" or empty probes the real path).
std::string rooted(const std::string& probe_root, const char* abs_path) {
    std::string root = probe_root;
    while (!root.empty() && root.back() == '/')
        root.pop_back();
    return root + abs_path;
}

/// Resolved absolute path of @p path, or empty when it cannot be resolved.
std::string canonical_or_empty(const std::string& path) {
    char buf[PATH_MAX];
    return ::realpath(path.c_str(), buf) ? std::string(buf) : std::string();
}

} // namespace

bool u1_firmware_manages_updates(const std::string& running_exe, const std::string& probe_root) {
    // PAXX Extended Firmware installs its HelixScreen package in version
    // subdirectories of /oem/apps/helixscreen; on stock firmware our installer
    // owns /userdata/helixscreen.
    const std::string package_dir = rooted(probe_root, "/oem/apps/helixscreen");
    struct stat dir_st {};
    if (::stat(package_dir.c_str(), &dir_st) != 0 || !S_ISDIR(dir_st.st_mode))
        return false;

    // Identity rather than path: the firmware's lmd hook bind-mounts the package
    // binary over /usr/bin/gui.
    if (u1_firmware_package_root(running_exe, probe_root))
        return true;

    // Fail toward managed: self-updating a firmware's package is the worse
    // mistake, so an exe that cannot be resolved counts as the firmware's.
    const std::string exe = canonical_or_empty(running_exe);
    const std::string ours = canonical_or_empty(rooted(probe_root, "/userdata/helixscreen"));
    return exe.empty() || ours.empty() || exe.compare(0, ours.size() + 1, ours + "/") != 0;
}

bool platform_defaults_to_external_updates() {
    if (s_external_updates_default_override >= 0) {
        return s_external_updates_default_override != 0;
    }
#ifdef HELIX_PLATFORM_SNAPMAKER_U1
    return u1_firmware_manages_updates();
#else
    return false;
#endif
}

std::string writable_install_root(const std::string& install_root) {
    return platform_defaults_to_external_updates() ? std::string() : install_root;
}

void set_external_updates_default_override(int override_value) {
    s_external_updates_default_override = override_value;
}

bool platform_host_power_supported() {
    return !is_android_platform();
}

// -1 = use compile-time default, 0 = force "beside the printer", 1 = force "on it"
static int s_printer_embedded_override = -1;

bool is_printer_embedded() {
    if (s_printer_embedded_override >= 0) {
        return s_printer_embedded_override != 0;
    }
#if defined(HELIX_PLATFORM_AD5M) || defined(HELIX_PLATFORM_AD5X) || defined(HELIX_PLATFORM_K1) ||  \
    defined(HELIX_PLATFORM_CC1) || defined(HELIX_PLATFORM_SNAPMAKER_U1) ||                         \
    defined(HELIX_PLATFORM_K2) || defined(HELIX_PLATFORM_MIPS)
    return true;
#else
    return false;
#endif
}

void set_printer_embedded_override(int override_value) {
    s_printer_embedded_override = override_value;
}

void log_platform_info() {
    struct utsname uts {};
    if (uname(&uts) == 0) {
        spdlog::info("[Application] Platform: {} {} {} ({})", uts.sysname, uts.release, uts.machine,
                     uts.nodename);
    }

    // Total RAM from /proc/meminfo (Linux only)
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
        unsigned long mem_total_kb = 0;
        if (fscanf(f, "MemTotal: %lu kB", &mem_total_kb) == 1 && mem_total_kb > 0) {
            spdlog::info("[Application] Memory: {} MB", mem_total_kb / 1024);
        }
        fclose(f);
    }

    // Display backend env var (if forced)
    const char* backend_env = std::getenv("HELIX_DISPLAY_BACKEND");
    if (backend_env && backend_env[0] != '\0') {
        spdlog::info("[Application] Display backend (env): {}", backend_env);
    }
}

std::string host_arch_string() {
    struct utsname uts {};
    const std::string kernel =
        (uname(&uts) == 0 && uts.machine[0] != '\0') ? uts.machine : "unknown";
    // sizeof(void*) is the only reliable runtime check for binary bitness:
    // compile-time __aarch64__/__arm__ macros tell us the ABI family but not
    // 32-vs-64-bit on every supported target. " · " is U+00B7 MIDDLE DOT.
    constexpr int bin_bits = sizeof(void*) * 8;
    return kernel + " \xc2\xb7 " + std::to_string(bin_bits) + "-bit userspace";
}

} // namespace helix
