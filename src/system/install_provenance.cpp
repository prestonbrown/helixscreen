// SPDX-License-Identifier: GPL-3.0-or-later

#include "install_provenance.h"

#include <fstream>
#include <string_view>
#include <sys/stat.h>

namespace helix::install_provenance {

namespace {

// Matches install_source_allowlist in scripts/lib/installer/printer_seed.sh.
constexpr const char* kInstallSources[] = {"helix-installer", "kiauh", "helper-script", "curl",
                                           "other"};

struct HostModRow {
    const char* id;
    const char* markers[8];
};

// Same evidence and precedence as detect_mod_flavor and detect_platform in
// scripts/lib/installer/platform.sh. Forge-X comes first because its tree can
// coexist with the markers below it.
constexpr HostModRow kHostMods[] = {
    {"forge_x",
     {"/usr/data/config/mod/.shell/common.sh", "/usr/data/config/mod/.shell/platform.sh",
      "/opt/config/mod/.shell/common.sh", "/opt/config/mod/.shell/platform.sh",
      "/opt/config/mod/.root", "/usr/data/.mod/.forge-x", "/data/.mod/.forge-x", nullptr}},
    {"zmod", {"/ZMOD", nullptr}},
    {"klipper_mod", {"/root/printer_software", "/mnt/data/.klipper_mod", nullptr}},
    {"cosmos", {"/usr/bin/update-cosmos", nullptr}},
    {"paxx", {"/oem/apps/helixscreen", "/etc/hooks/lmd.d/30-helixscreen.sh", nullptr}},
    {"simple_af", {"/usr/data/pellcorp", nullptr}},
    {"rinkhals", {"/useremain/rinkhals", nullptr}},
};

bool path_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

} // namespace

const char* read_install_source(const std::string& config_dir) {
    std::ifstream file(config_dir + "/" + kMarkerFile);
    // Longer than any listed word, so an oversized marker never matches one.
    char buffer[32] = {};
    file.read(buffer, sizeof(buffer) - 1);
    std::string_view word(buffer, static_cast<size_t>(file.gcount()));
    word = word.substr(0, word.find_first_of("\r\n"));
    for (const char* source : kInstallSources) {
        if (word == source) {
            return source;
        }
    }
    return "unknown";
}

const char* detect_host_mod(const std::string& root) {
    for (const auto& mod : kHostMods) {
        for (const char* const* marker = mod.markers; *marker != nullptr; ++marker) {
            if (path_exists(root + *marker)) {
                return mod.id;
            }
        }
    }
    return "none";
}

} // namespace helix::install_provenance
