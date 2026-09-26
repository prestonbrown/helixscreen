// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/config_trust.h"

#include "app_constants.h"
#include "app_globals.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "hv/json.hpp"

namespace helix::config_trust {

using json = nlohmann::json;

namespace {

#if defined(HELIX_PLATFORM_ESP32)
// ESP-IDF has one user and no symlinks: newlib declares no lstat and links no
// geteuid, and on that VFS stat and uid 0 are what they would answer.
int lstat(const char* path, struct stat* st) {
    return stat(path, st);
}
uid_t geteuid() {
    return 0;
}
#endif

bool owned_by_root_or_self(uid_t uid) {
    return uid == 0 || uid == geteuid();
}

/// The directory itself carries no write bit for anyone but its owner, so
/// nobody can swap it for a symlink aimed at the app after this check.
bool dir_locked_to_owner(const std::string& dir) {
    struct stat st {};
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    return owned_by_root_or_self(st.st_uid) && (st.st_mode & 022) == 0;
}

std::string real_path(const std::string& path) {
    std::error_code ec;
    std::string resolved = std::filesystem::canonical(path, ec).string();
    return ec ? std::string{} : resolved;
}

std::string parent_dir(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

bool has_dot_segment(const std::string& path) {
    size_t pos = 0;
    while (pos < path.size()) {
        const size_t next = path.find('/', pos);
        const std::string segment =
            path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (segment == "." || segment == "..") {
            return true;
        }
        pos = next == std::string::npos ? path.size() : next + 1;
    }
    return false;
}

bool starts_with_dir(const std::string& path, const std::string& dir) {
    return path.rfind(dir + "/", 0) == 0;
}

} // namespace

UpdateUrls read_update_urls() {
    UpdateUrls urls;
    const std::string path = AppConstants::Update::state_dir() + "/update_urls.json";

    // stat dereferences a symlink, so the file judged is the file read.
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return urls;
    }
    if (!dir_locked_to_owner(parent_dir(path))) {
        spdlog::warn("[ConfigTrust] {} sits in a directory others can write or "
                     "swap - ignoring (fix: chown root:root {} && chmod 755 {})",
                     path, parent_dir(path), parent_dir(path));
        return urls;
    }
    if (!owned_by_root_or_self(st.st_uid)) {
        spdlog::warn("[ConfigTrust] {} is owned by uid {}, not root or this user - ignoring "
                     "(fix: chown root {} && chmod 644 {})",
                     path, st.st_uid, path, path);
        return urls;
    }
    if (st.st_mode & 022) {
        spdlog::warn("[ConfigTrust] {} is group- or world-writable - ignoring "
                     "(fix: chmod 644 {})",
                     path, path);
        return urls;
    }

    std::ifstream file(path);
    json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (file.bad() || j.is_discarded() || !j.is_object()) {
        spdlog::warn("[ConfigTrust] {} is not a valid JSON object - ignoring", path);
        return urls;
    }
    // A wrong-typed entry ({"r2_url": 1}) must be ignored like any other
    // misconfiguration, not throw json::type_error up through the caller.
    const auto url_field = [&j, &path](const char* key) {
        const auto it = j.find(key);
        if (it == j.end()) {
            return std::string{};
        }
        if (!it->is_string()) {
            spdlog::warn("[ConfigTrust] {} field {} is not a string - ignoring it", path, key);
            return std::string{};
        }
        return it->get<std::string>();
    };
    urls.r2_url = url_field("r2_url");
    urls.dev_url = url_field("dev_url");
    return urls;
}

bool log_path_allowed(const std::string& path) {
    // size() >= 5 keeps the ".log" compare in bounds: a shorter path such as
    // "/ab" would wrap size() - 4 and throw out_of_range up through
    // init_logging, boot-looping the app on a bad settings.json value.
    if (path.size() < 5 || path[0] != '/' || path.compare(path.size() - 4, 4, ".log") != 0) {
        return false;
    }
    if (has_dot_segment(path)) {
        return false;
    }
    struct stat lst {};
    if (lstat(path.c_str(), &lst) == 0) {
        // A planted file must not redirect the app's writes: no symlink, and
        // no hard link either (st_nlink > 1 means someone else holds a name
        // for the same inode), and an owner nobody but root or this user.
        if (!S_ISREG(lst.st_mode) || lst.st_nlink != 1 || !owned_by_root_or_self(lst.st_uid)) {
            return false;
        }
    }
    // ponytail: this lstat and the later open are separate steps, so a link
    // planted in between still races the open; the launcher shares the same
    // window. Close it with O_NOFOLLOW|O_EXCL plus an fstat re-check in
    // init_logging if a writable-parent threat ever shows up.

    const std::string dir = real_path(parent_dir(path));
    if (dir.empty()) {
        return false;
    }
    // The allowed roots are matched after symlink resolution (the launcher's
    // readlink -f), plus their canonical spelling: on macOS /tmp resolves to
    // /private/tmp, and the canonical root is what the resolved parent is
    // spelled there. On Linux the two spellings coincide.
    const std::vector<std::string> roots = {"/tmp", real_path("/tmp"), "/var/log",
                                            real_path("/var/log")};
    bool exact_root = false;
    bool under_root = false;
    for (const std::string& root : roots) {
        if (root.empty()) {
            continue;
        }
        if (dir == root) {
            exact_root = true;
        } else if (starts_with_dir(dir, root)) {
            under_root = true;
        }
    }
    if (exact_root) {
        return true;
    }
    const std::string& override_root = detail::install_root_override_ref();
    const std::string install_root =
        real_path(override_root.empty() ? app_get_install_root() : override_root);
    const bool under_install_root =
        !install_root.empty() && (dir == install_root || starts_with_dir(dir, install_root));
    if (!under_root && !under_install_root) {
        return false;
    }
    return dir_locked_to_owner(dir);
}

} // namespace helix::config_trust
