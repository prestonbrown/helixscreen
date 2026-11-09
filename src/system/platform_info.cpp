// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_info.h"

#include "board_identity.h"

#include <spdlog/spdlog.h>

#include <fstream>
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
    return board::current_info().firmware_manages_updates;
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
    return board::current() != board::Board::Generic;
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

const char* flashforge_mod_tree_name(FlashForgeModTree tree) {
    switch (tree) {
    case FlashForgeModTree::Zmod:
        return "ZMOD";
    case FlashForgeModTree::ForgeX:
        return "Forge-X"; // i18n: do not translate
    case FlashForgeModTree::None:
        break;
    }
    return "none";
}

FlashForgeModTree flashforge_mod_tree(const std::string& probe_root) {
    // Forge-X's own descriptor decides first: its mod git tree stays reachable
    // from the chroot (and host-side), and .shell/platform.sh in it is the same
    // evidence the installer's host_profile probe keys on, ahead of /ZMOD. A
    // Forge-X chroot built from a Z-Mod rootfs carries FlashForge's /usr/prog
    // too, so /usr/prog alone only means Z-Mod once Forge-X's tree is absent.
    // scripts/helix-launcher.sh answers "any mod tree" from the same markers
    // for its heap-diag gate; keep the two (and their tests) in step.
    struct ::stat st {};
    for (const char* mod_tree : {"/opt/config/mod", "/usr/data/config/mod"}) {
        const std::string probe = rooted(probe_root, mod_tree) + "/.shell/platform.sh";
        if (::stat(probe.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            return FlashForgeModTree::ForgeX;
        }
    }
    if ((::stat(rooted(probe_root, "/ZMOD").c_str(), &st) == 0 && S_ISREG(st.st_mode)) ||
        (::stat(rooted(probe_root, "/usr/prog").c_str(), &st) == 0 && S_ISDIR(st.st_mode))) {
        return FlashForgeModTree::Zmod;
    }
    return FlashForgeModTree::None;
}

namespace {

/// The last `KEY=` value from @p path, trimmed of whitespace and (for the
/// os-release arm) shell quoting, or empty when the file is unreadable or
/// carries no such line. Last-wins mirrors the shell rule (tail -n 1), which
/// exists because start.sh rewrites its file on every boot.
std::string last_assignment_value(const std::string& path, const std::string& key,
                                  bool first_word) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return {};
    }
    const std::string needle = key + "=";
    std::string line;
    std::string value;
    while (std::getline(in, line)) {
        if (line.rfind(needle, 0) == 0) {
            value = line.substr(needle.size());
        }
    }
    // Strip os-release's shell quoting and a CR left by CRLF firmware files,
    // then keep the first word when the caller asked (VERSION_CODENAME's value
    // is "<MACHINE> <version>" and the version is not part of the board name).
    if (value.find_first_not_of(" \t") == std::string::npos) {
        return {};
    }
    if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
        value.erase(0, 1);
    }
    if (!value.empty() && (value.back() == '"' || value.back() == '\'' || value.back() == '\r')) {
        value.pop_back();
    }
    const std::size_t start = value.find_first_not_of(" \t");
    const std::size_t end = value.find_last_not_of(" \t\r");
    value = value.substr(start, end - start + 1);
    if (first_word) {
        const std::size_t space = value.find_first_of(" \t");
        if (space != std::string::npos) {
            value.erase(space);
        }
    }
    return value;
}

std::optional<FlashForgeMachine> machine_from_name(const std::string& name) {
    if (name == "AD5X") {
        return FlashForgeMachine::Ad5x;
    }
    if (name == "Creator5") {
        return FlashForgeMachine::Creator5;
    }
    if (name == "Creator5Pro") {
        return FlashForgeMachine::Creator5Pro;
    }
    return std::nullopt;
}

} // namespace

std::optional<FlashForgeMachine> flashforge_machine_id(const std::string& probe_root) {
    if (auto machine = machine_from_name(last_assignment_value(
            rooted(probe_root, "/usr/prog/app_startup.sh"), "MACHINE", false))) {
        return machine;
    }
    return machine_from_name(
        last_assignment_value(rooted(probe_root, "/etc/os-release"), "VERSION_CODENAME", true));
}

const char* flashforge_machine_display_name(FlashForgeMachine machine) {
    switch (machine) {
    case FlashForgeMachine::Ad5x:
        return "FlashForge Adventurer 5X";
    case FlashForgeMachine::Creator5:
        return "FlashForge Creator 5";
    case FlashForgeMachine::Creator5Pro:
        return "FlashForge Creator 5 Pro";
    }
    return "";
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
