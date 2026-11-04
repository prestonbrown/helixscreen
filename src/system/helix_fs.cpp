// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_fs.h"

#include "core/text_io.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(HELIX_PLATFORM_ESP32)
#include <sys/statvfs.h>
#endif

namespace helix::fs {

namespace {

thread_local const char* t_storage_forbidden_by = nullptr;

} // namespace

void forbid_storage_on_this_thread(const char* thread_name) {
    t_storage_forbidden_by = thread_name ? thread_name : "?";
}

bool storage_allowed(const char* op, std::string_view path) {
    if (t_storage_forbidden_by == nullptr) {
        return true;
    }
    spdlog::error("[helix_fs] {}('{}') refused: the {} thread must not touch storage", op, path,
                  t_storage_forbidden_by);
    errno = EPERM;
    return false;
}

namespace {

bool stat_ok(const std::string& p, struct stat& st) {
    return storage_allowed("stat", p) && ::stat(p.c_str(), &st) == 0;
}

// ESP32's C library links no lstat; it has no symlinks either, so stat is exact there.
int lstat_compat(const char* p, struct stat* st) {
#if defined(HELIX_PLATFORM_ESP32)
    return ::stat(p, st);
#else
    return ::lstat(p, st);
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Path strings
// ---------------------------------------------------------------------------

std::string join_path(std::string_view a, std::string_view b) {
    if (!b.empty() && b.front() == '/') {
        return std::string(b);
    }
    if (a.empty()) {
        return std::string(b);
    }
    std::string out(a);
    if (out.back() != '/') {
        out += '/';
    }
    out.append(b);
    return out;
}

std::string_view filename(std::string_view p) {
    const size_t slash = p.rfind('/');
    return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

std::string_view parent_path(std::string_view p) {
    const size_t first_non_slash = p.find_first_not_of('/');
    if (first_non_slash == std::string_view::npos) {
        return p; // "" or all separators: no relative path, the path is its own parent
    }
    size_t end = p.size();
    if (p.back() != '/') {
        const size_t slash = p.rfind('/');
        if (slash == std::string_view::npos) {
            return {};
        }
        end = slash + 1;
    }
    // Drop the separators before the removed element, but never the root.
    while (end > first_non_slash && p[end - 1] == '/') {
        --end;
    }
    if (end < first_non_slash || (end == first_non_slash && first_non_slash > 0)) {
        return p.substr(0, 1); // the root, however many separators spelled it
    }
    return p.substr(0, end);
}

namespace {
size_t extension_dot(std::string_view name) {
    if (name == "." || name == "..") {
        return std::string_view::npos;
    }
    const size_t dot = name.rfind('.');
    return (dot == 0) ? std::string_view::npos : dot;
}
} // namespace

std::string_view stem(std::string_view p) {
    const std::string_view name = filename(p);
    const size_t dot = extension_dot(name);
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

std::string_view extension(std::string_view p) {
    const std::string_view name = filename(p);
    const size_t dot = extension_dot(name);
    return dot == std::string_view::npos ? std::string_view{} : name.substr(dot);
}

std::string lexically_normal(std::string_view p) {
    if (p.find_first_not_of('/') == std::string_view::npos) {
        return std::string(p); // "" and all-separator paths normalize to themselves
    }
    const bool rooted = p.front() == '/';
    bool trailing = p.back() == '/';

    std::vector<std::string_view> elems;
    for (size_t pos = 0; pos <= p.size();) {
        size_t next = p.find('/', pos);
        if (next == std::string_view::npos) {
            next = p.size();
        }
        if (next > pos) {
            elems.push_back(p.substr(pos, next - pos));
        }
        pos = next + 1;
    }

    std::vector<std::string_view> out;
    for (size_t i = 0; i < elems.size(); ++i) {
        const std::string_view e = elems[i];
        const bool last = i + 1 == elems.size();
        if (e == ".") {
            trailing = trailing || last;
            continue;
        }
        if (e == "..") {
            if (!out.empty() && out.back() != "..") {
                out.pop_back();
                trailing = trailing || last;
                continue;
            }
            if (rooted) {
                continue; // ".." directly under the root names the root
            }
        }
        out.push_back(e);
    }

    std::string result = rooted ? "/" : "";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) {
            result += '/';
        }
        result.append(out[i]);
    }
    if (trailing && !out.empty() && out.back() != "..") {
        result += '/';
    }
    return result.empty() ? "." : result;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool exists(const std::string& p) {
    struct stat st;
    return stat_ok(p, st);
}

bool is_directory(const std::string& p) {
    struct stat st;
    return stat_ok(p, st) && S_ISDIR(st.st_mode);
}

bool is_regular_file(const std::string& p) {
    struct stat st;
    return stat_ok(p, st) && S_ISREG(st.st_mode);
}

bool is_symlink(const std::string& p) {
#if defined(HELIX_PLATFORM_ESP32)
    (void)p;
    return false;
#else
    struct stat st;
    return storage_allowed("lstat", p) && ::lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

bool is_owner_executable(const std::string& p) {
    struct stat st;
    return stat_ok(p, st) && (st.st_mode & S_IXUSR) != 0;
}

std::optional<std::int64_t> mtime_ns(const std::string& p) {
    struct stat st;
    if (!stat_ok(p, st)) {
        return std::nullopt;
    }
#if defined(__APPLE__)
    const struct timespec& t = st.st_mtimespec;
#else
    const struct timespec& t = st.st_mtim;
#endif
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

std::optional<std::uint64_t> space_available(const std::string& p) {
    if (!storage_allowed("space_available", p)) {
        return std::nullopt;
    }
#if defined(HELIX_PLATFORM_ESP32)
    (void)p;
    return std::nullopt;
#else
    struct statvfs sv;
    if (::statvfs(p.c_str(), &sv) != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(sv.f_bavail) * sv.f_frsize;
#endif
}

std::optional<std::string> canonical(const std::string& p) {
#ifndef PATH_MAX
    constexpr size_t kPathMax = 4096;
#else
    constexpr size_t kPathMax = PATH_MAX;
#endif
    if (!storage_allowed("canonical", p)) {
        return std::nullopt;
    }
    char buf[kPathMax];
    if (::realpath(p.c_str(), buf) == nullptr) {
        return std::nullopt;
    }
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

bool create_directories(const std::string& p) {
    if (!storage_allowed("create_directories", p)) {
        return false;
    }
    if (p.empty()) {
        errno = ENOENT;
        return false;
    }
    if (is_directory(p)) {
        return true;
    }
    for (size_t pos = p.find_first_not_of('/'); pos != std::string::npos;) {
        const size_t slash = p.find('/', pos);
        const std::string prefix = p.substr(0, slash);
        if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        if (!is_directory(prefix)) {
            errno = ENOTDIR;
            return false;
        }
        pos = (slash == std::string::npos) ? slash : p.find_first_not_of('/', slash);
    }
    return true;
}

bool remove(const std::string& p) {
    if (!storage_allowed("remove", p)) {
        return false;
    }
    struct stat st;
    if (lstat_compat(p.c_str(), &st) != 0) {
        return false; // errno from lstat: ENOENT when absent
    }
    const int rc = S_ISDIR(st.st_mode) ? ::rmdir(p.c_str()) : ::unlink(p.c_str());
    return rc == 0;
}

bool rename(const std::string& from, const std::string& to) {
    if (!storage_allowed("rename", from)) {
        return false;
    }
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool copy_file(const std::string& from, const std::string& to, bool overwrite) {
    namespace tio = helix::text_io;
    tio::File in = tio::open_file(from, "rb");
    if (!in) {
        return false;
    }
    if (!overwrite && exists(to)) {
        errno = EEXIST;
        return false;
    }
    tio::File out = tio::open_file(to, "wb");
    if (!out) {
        return false;
    }
    char buf[4096];
    bool ok = true;
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), in.get())) > 0;) {
        if (!tio::write_all(out.get(), std::string_view(buf, n))) {
            ok = false;
            break;
        }
    }
    if (ok && std::ferror(in.get())) {
        ok = false;
        errno = EIO;
    }
    if (!tio::close(out)) {
        ok = false;
    }
    if (!ok) {
        const int saved = errno;
        ::unlink(to.c_str());
        errno = saved;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------

std::optional<std::vector<DirEntry>> list_dir(const std::string& dir) {
    if (!storage_allowed("list_dir", dir)) {
        return std::nullopt;
    }
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        return std::nullopt;
    }
    std::vector<DirEntry> entries;
    while (true) {
        errno = 0;
        const struct dirent* ent = ::readdir(d);
        if (ent == nullptr) {
            break; // end of directory, or a read error with errno set
        }
        const std::string_view name(ent->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        DirEntry e;
        e.name = std::string(name);
        e.path = join_path(dir, name);
        bool typed = false;
#if defined(DT_DIR) && defined(DT_REG)
        if (ent->d_type == DT_DIR || ent->d_type == DT_REG) {
            e.is_dir = ent->d_type == DT_DIR;
            e.is_regular = ent->d_type == DT_REG;
            typed = true;
        }
#endif
        if (!typed) {
            // Unknown type or a symlink: stat follows it, as directory_entry does.
            struct stat st;
            if (stat_ok(e.path, st)) {
                e.is_dir = S_ISDIR(st.st_mode);
                e.is_regular = S_ISREG(st.st_mode);
            }
        }
        entries.push_back(std::move(e));
    }
    const int saved = errno;
    ::closedir(d);
    errno = saved;
    return entries;
}

} // namespace helix::fs
