// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_info.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>
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

bool platform_defaults_to_external_updates() {
    if (s_external_updates_default_override >= 0) {
        return s_external_updates_default_override != 0;
    }
#ifdef HELIX_PLATFORM_SNAPMAKER_U1
    return true;
#else
    return false;
#endif
}

void set_external_updates_default_override(int override_value) {
    s_external_updates_default_override = override_value;
}

bool platform_host_power_supported() {
    return !is_android_platform();
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
