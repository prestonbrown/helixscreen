// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/helix_paths.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#if !defined(HELIX_PLATFORM_ESP32)
#include <sys/statvfs.h> // newlib (ESP-IDF) has no statvfs
#endif
#include <unistd.h>

namespace helix::paths {

bool is_writable_dir(const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    // access(W_OK) also returns non-zero for a nonexistent path (ENOENT).
    return ::access(dir.c_str(), W_OK) == 0;
}

std::uint64_t available_space(const std::string& dir) {
#if defined(HELIX_PLATFORM_ESP32)
    // newlib has no statvfs; report a generous fixed budget so the portable
    // path utilities (ensure_dir, probe_writable) behave. A later port swaps in
    // esp_littlefs_info() for the real mount free space.
    (void)dir;
    return 64ULL * 1024 * 1024;
#else
    struct statvfs vfs {};
    if (::statvfs(dir.c_str(), &vfs) != 0) {
        return 0;
    }
    // Cast BOTH operands to uint64_t before multiplying. On 32-bit platforms
    // (pi32/armhf, MIPS32) f_bavail/f_frsize are 32-bit and the product wraps
    // for any filesystem larger than ~4 GiB.
    return static_cast<std::uint64_t>(vfs.f_bavail) * static_cast<std::uint64_t>(vfs.f_frsize);
#endif
}

bool probe_writable(const std::string& dir, std::uint64_t min_free_bytes) {
    if (dir.empty()) {
        return false;
    }

    // Space check first (skipped when min_free_bytes == 0). available_space()
    // also returns 0 when statvfs fails on a nonexistent dir, so a short-space
    // request against a missing dir short-circuits here.
    if (min_free_bytes > 0 && available_space(dir) < min_free_bytes) {
        return false;
    }

    // Name is unique per process, per thread, AND per call: getpid() alone is
    // process-wide, so two threads of the same process probing the same dir
    // would collide. Appending the thread id plus a monotonic counter makes each
    // probe file distinct even under concurrent same-process, same-dir probes.
    static std::atomic<std::uint64_t> probe_counter{0};
    std::ostringstream name;
    name << strip_trailing_slash(dir) << "/.helix_write_test." << ::getpid() << '.'
         << std::this_thread::get_id() << '.' << probe_counter.fetch_add(1);
    const std::string test_file = name.str();

    bool wrote = false;
    {
        std::ofstream ofs(test_file, std::ios::binary | std::ios::trunc);
        if (!ofs.good()) {
            // Cannot create the file — dir is missing, read-only, or full.
            return false;
        }
        ofs.put('x');
        ofs.flush();
        wrote = ofs.good();
    }

    std::error_code ec;
    std::filesystem::remove(test_file, ec);

    return wrote;
}

std::string first_writable_dir(const std::vector<std::string>& candidates,
                               std::uint64_t min_free_bytes) {
    for (const std::string& dir : candidates) {
        if (probe_writable(dir, min_free_bytes)) {
            return dir;
        }
    }
    return "";
}

bool ensure_dir(const std::string& path) {
    try {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        // create_directories returns false (with no error) when the directory
        // already exists, so verify existence + type explicitly rather than
        // trusting its return value.
        return std::filesystem::is_directory(path, ec);
    } catch (...) {
        return false;
    }
}

std::string home() {
    const char* h = std::getenv("HOME");
    if (!h || h[0] == '\0') {
        return "";
    }
    if (h[0] != '/') {
        return ""; // not an absolute path
    }
    // Reject control characters (heap-corruption guard inherited from
    // app_constants.h sanitize_home, which observed single-char junk dirs).
    for (const char* p = h; *p; ++p) {
        if (static_cast<unsigned char>(*p) < 0x20) {
            return "";
        }
    }
    return h;
}

std::vector<std::string> xdg_cache_bases() {
    std::vector<std::string> bases;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] != '\0') {
        bases.emplace_back(xdg);
    }
    const std::string h = home();
    if (!h.empty()) {
        bases.push_back(h + "/.cache");
    }
    return bases;
}

std::string xdg_data_home() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0') {
        return xdg;
    }
    const std::string h = home();
    if (!h.empty()) {
        return h + "/.local/share";
    }
    return "";
}

std::string dirname(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    const auto slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        return "."; // bare filename (or empty) → current directory
    }
    if (slash == 0) {
        return "/"; // sits at the filesystem root
    }
    return p.substr(0, slash);
}

std::string strip_trailing_slash(const std::string& path) {
    std::string s = path;
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

} // namespace helix::paths
