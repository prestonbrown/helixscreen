// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "text_io.h"

#include "helix_fs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Built with -D_FILE_OFFSET_BITS=64 (mk/rules.mk), so fopen, stat and the seek
// offsets here are 64-bit even on 32-bit glibc; no off_t crosses the API.

namespace helix::text_io {

File open_file(const std::string& path, const char* mode) {
    if (!helix::fs::storage_allowed("fopen", path)) {
        return File(nullptr);
    }
    return File(std::fopen(path.c_str(), mode));
}

bool seek(std::FILE* f, std::int64_t offset, int whence) {
    return ::fseeko(f, static_cast<off_t>(offset), whence) == 0;
}

std::optional<std::int64_t> tell(std::FILE* f) {
    const off_t pos = ::ftello(f);
    if (pos < 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(pos);
}

std::optional<std::string> read_file(const std::string& path, size_t max_bytes) {
    File f = open_file(path, "rb");
    if (!f) {
        return std::nullopt;
    }
    std::string out;
    // /proc and sysfs report a size of 0, so the size is only a reserve hint
    // and the loop reads to EOF regardless.
    struct stat st;
    if (::fstat(::fileno(f.get()), &st) == 0 && st.st_size > 0) {
        out.reserve(std::min(static_cast<size_t>(st.st_size), max_bytes));
    }
    char buf[4096];
    size_t n;
    while (out.size() < max_bytes &&
           (n = std::fread(buf, 1, std::min(sizeof(buf), max_bytes - out.size()), f.get())) > 0) {
        out.append(buf, n);
    }
    if (std::ferror(f.get())) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> read_first_line(const std::string& path) {
    LineReader reader(path);
    if (!reader) {
        return std::nullopt;
    }
    std::string line;
    reader.next(line);
    return line;
}

std::optional<std::uint64_t> file_size(const std::string& path) {
    struct stat st;
    if (!helix::fs::storage_allowed("stat", path) || ::stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(st.st_size);
}

bool write_all(std::FILE* f, std::string_view data) {
    return data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
}

bool close(File& f) {
    if (!f) {
        return false;
    }
    bool ok = std::fflush(f.get()) == 0 && !std::ferror(f.get());
    ok = std::fclose(f.release()) == 0 && ok;
    return ok;
}

bool write_file(const std::string& path, std::string_view data) {
    File f = open_file(path, "wb");
    if (!f) {
        return false;
    }
    if (!write_all(f.get(), data)) {
        int err = errno;
        f.reset();
        errno = err;
        return false;
    }
    return close(f);
}

namespace {

void fsync_dir_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "." : path.substr(0, slash == 0 ? 1 : slash);
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
}

bool fail_and_remove(const std::string& tmp) {
    int err = errno;
    std::remove(tmp.c_str());
    errno = err;
    return false;
}

} // namespace

bool write_file_atomic(const std::string& path, std::string_view data, Durability durability) {
    // One staging file per call: writers of the same path that shared one
    // would write into each other's file and rename it out from under each other.
    // 32 bits: MIPS32 has no native 8-byte atomics, and helix-splash and
    // helix-watchdog link this file without libatomic.
    static std::atomic<uint32_t> seq{0};
    const std::string tmp = path + "." + std::to_string(::getpid()) + "." +
                            std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    File f = open_file(tmp, "wb");
    if (!f) {
        return false;
    }
    if (!write_all(f.get(), data) || std::fflush(f.get()) != 0) {
        f.reset();
        return fail_and_remove(tmp);
    }
    if (durability == Durability::Fsync) {
        (void)::fsync(::fileno(f.get()));
    }
    if (!close(f)) {
        return fail_and_remove(tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        return fail_and_remove(tmp);
    }
    if (durability == Durability::Fsync) {
        fsync_dir_of(path);
    }
    return true;
}

LineReader::LineReader(const std::string& path, char delim)
    : file_(open_file(path, "rb")), delim_(delim) {}

bool LineReader::next(std::string& line) {
    if (!file_) {
        return false;
    }
    line.clear();
    const int delim = static_cast<unsigned char>(delim_);
    int c;
    while ((c = std::getc(file_.get())) != EOF) {
        if (c == delim) {
            last_had_delimiter_ = true;
            return true;
        }
        line.push_back(static_cast<char>(c));
    }
    last_had_delimiter_ = false;
    return !line.empty();
}

std::optional<double> parse_double(std::string_view s) {
    if (s.size() > 1 && s[0] == '+' && s[1] != '-' && s[1] != '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return std::nullopt;
    }
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double value = 0.0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
#else
    // libc++ without floating-point from_chars (macOS dev builds). strtod reads
    // LC_NUMERIC, which the app never changes from "C"; the checks below hold it
    // to the same strict whole-view contract.
    if (std::isspace(static_cast<unsigned char>(s[0]))) {
        return std::nullopt;
    }
    std::string buf(s);
    if (buf.find_first_of("xX") != std::string::npos) {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    double value = std::strtod(buf.c_str(), &end);
    if (end != buf.c_str() + buf.size() || (errno == ERANGE && std::isinf(value))) {
        return std::nullopt;
    }
    return value;
#endif
}

} // namespace helix::text_io
