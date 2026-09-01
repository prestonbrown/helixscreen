// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_info.h"

#include <spdlog/spdlog.h>

#include <cstdio>
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

// Root-relative spelling of an absolute probe path: "/" (or empty) probes the
// real path, a sandbox root nests it ("/tmp/xyz" + "/ZMOD"). Trailing slashes
// on the root are collapsed so the join never doubles up.
static std::string rooted(const std::string& probe_root, const char* abs_path) {
    if (probe_root.empty() || probe_root == "/") {
        return std::string(abs_path);
    }
    std::string root = probe_root;
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    return root + abs_path;
}

bool ad5x_mod_layout_present(const std::string& probe_root) {
    // ZMOD hosts carry FlashForge's /usr/prog dir or the /ZMOD marker file. A
    // Forge-X chroot has neither — and not even /usr/data, which the chroot
    // binds at /opt — but the mod's git tree stays reachable, and
    // .shell/platform.sh in it is the same evidence the installer's
    // host_profile probe keys on. scripts/helix-launcher.sh answers this same
    // rule for its heap-diag gate; keep the two (and their tests) in step.
    struct ::stat st {};
    if ((::stat(rooted(probe_root, "/ZMOD").c_str(), &st) == 0 && S_ISREG(st.st_mode)) ||
        (::stat(rooted(probe_root, "/usr/prog").c_str(), &st) == 0 && S_ISDIR(st.st_mode))) {
        return true;
    }
    for (const char* mod_tree : {"/opt/config/mod", "/usr/data/config/mod"}) {
        const std::string probe = rooted(probe_root, mod_tree) + "/.shell/platform.sh";
        if (::stat(probe.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            return true;
        }
    }
    return false;
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
