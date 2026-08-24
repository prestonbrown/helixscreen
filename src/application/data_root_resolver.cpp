// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "data_root_resolver.h"

#include "system/helix_paths.h"

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace helix {

namespace {

bool path_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string& asset_root_storage() {
    static std::string root = ".";
    return root;
}

} // namespace

bool is_valid_data_root(const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    std::string path = dir + "/ui_xml";
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

std::string resolve_data_root_from_exe(const std::string& exe_path) {
    if (exe_path.empty()) {
        return {};
    }

    // Strip the binary filename to get the directory
    auto last_slash = exe_path.rfind('/');
    if (last_slash == std::string::npos) {
        return {};
    }
    std::string bin_dir = exe_path.substr(0, last_slash);

    // Try stripping known binary directory suffixes to find the project root.
    // Order matters: /build/bin is more specific than /bin, so try it first.
    //   Dev builds:    /path/to/project/build/bin/helix-screen  → /path/to/project
    //   Deployed:      /home/pi/helixscreen/bin/helix-screen    → /home/pi/helixscreen
    const char* suffixes[] = {"/build/bin", "/bin"};
    for (const char* suffix : suffixes) {
        size_t suffix_len = strlen(suffix);
        if (bin_dir.size() >= suffix_len &&
            bin_dir.compare(bin_dir.size() - suffix_len, suffix_len, suffix) == 0) {
            std::string candidate = bin_dir.substr(0, bin_dir.size() - suffix_len);
            if (is_valid_data_root(candidate)) {
                return candidate;
            }
        }
    }

    return {};
}

std::string get_user_config_dir() {
    const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
    if (env_dir != nullptr && env_dir[0] != '\0') {
        return env_dir;
    }
    return "config";
}

std::string get_data_dir() {
    const char* env_dir = std::getenv("HELIX_DATA_DIR");
    if (env_dir != nullptr && env_dir[0] != '\0') {
        return env_dir;
    }
    return ".";
}

const std::string& asset_root() {
    return asset_root_storage();
}

void set_asset_root(const std::string& root) {
    asset_root_storage() = root.empty() ? "." : helix::paths::strip_trailing_slash(root);
}

std::string asset_path(const std::string& relpath) {
    const std::string& root = asset_root_storage();
    if (root == ".") {
        return relpath;
    }
    return root + "/" + relpath;
}

std::string asset_component_uri(const std::string& relpath) {
    return "A:" + asset_path(relpath);
}

std::string writable_path(const std::string& relpath) {
    return helix::paths::strip_trailing_slash(get_user_config_dir()) + "/" + relpath;
}

std::string find_readable(const std::string& relpath) {
    std::string user = writable_path(relpath);
    if (path_exists(user))
        return user;
    std::string seed =
        helix::paths::strip_trailing_slash(get_data_dir()) + "/assets/config/" + relpath;
    if (path_exists(seed))
        return seed;
    return user;
}

} // namespace helix
