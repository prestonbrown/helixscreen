// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// FileDataSource addresses gcode by uint64_t offset, but fseeko/ftello carry an
// off_t, which is 32 bits on our 32-bit targets (pi32, ad5m, cc1, k1) unless
// large-file support is requested. The request cannot live in this file: every
// app source is compiled with -include include/lvgl_pch.h, so the system headers
// are read before the first line here and the value is already latched. It comes
// from a target-specific -D_FILE_OFFSET_BITS=64 on this object in mk/rules.mk
// instead, which glibc turns into fseeko64/ftello64; musl and 64-bit targets
// already have a 64-bit off_t and are unaffected. The static_assert below fails
// the build if that rule is ever dropped.
//
// The wider off_t stays inside this translation unit: it appears nowhere in
// gcode_data_source.h and only as a local cast in read_range(). Verified on
// armhf by diffing the object's symbol table with and without the define - the
// only changes are fopen/fseeko/ftello gaining their 64 suffix; no mangled name
// moves, so nothing off_t-typed crosses a TU boundary.

#include "gcode_data_source.h"

#include "memory_monitor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace helix {
namespace gcode {

// =============================================================================
// GCodeDataSource base class
// =============================================================================

std::optional<std::string> GCodeDataSource::read_line(uint64_t offset, size_t max_length) {
    auto data = read_range(offset, static_cast<uint32_t>(max_length));
    if (data.empty() && offset < file_size()) {
        return std::nullopt; // Read failed
    }
    if (data.empty()) {
        return ""; // At end of file
    }

    // Find newline
    auto newline_pos = std::find(data.begin(), data.end(), '\n');
    size_t line_len = std::distance(data.begin(), newline_pos);

    // Strip trailing \r if present
    std::string line(data.begin(), data.begin() + line_len);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    return line;
}

std::vector<char> GCodeDataSource::read_all() {
    uint64_t size = file_size();
    // Guard against truncation - read_range takes uint32_t
    if (size > std::numeric_limits<uint32_t>::max()) {
        spdlog::error("[DataSource] File too large for read_all(): {} bytes (max {})", size,
                      std::numeric_limits<uint32_t>::max());
        return {};
    }
    auto result = read_range(0, static_cast<uint32_t>(size));
    helix::MemoryMonitor::log_now("gcode_read_all");
    return result;
}

// =============================================================================
// FileDataSource
// =============================================================================

FileDataSource::FileDataSource(const std::string& filepath) : filepath_(filepath) {
    file_ = std::fopen(filepath.c_str(), "rb");
    if (file_) {
        // 64-bit off_t (see the top of this file), so a file above 2 GB reports
        // its true size on 32-bit ARM instead of ftello returning -1.
        static_assert(sizeof(off_t) == 8, "off_t must be 64-bit: see mk/rules.mk LFS override");
        fseeko(file_, 0, SEEK_END);
        size_ = static_cast<uint64_t>(ftello(file_));
        fseeko(file_, 0, SEEK_SET);
        spdlog::debug("[FileDataSource] Opened '{}' ({} bytes)", filepath, size_);
    } else {
        spdlog::error("[FileDataSource] Failed to open '{}'", filepath);
    }
}

FileDataSource::~FileDataSource() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

FileDataSource::FileDataSource(FileDataSource&& other) noexcept
    : filepath_(std::move(other.filepath_)), file_(other.file_), size_(other.size_) {
    other.file_ = nullptr;
    other.size_ = 0;
}

FileDataSource& FileDataSource::operator=(FileDataSource&& other) noexcept {
    if (this != &other) {
        if (file_) {
            std::fclose(file_);
        }
        filepath_ = std::move(other.filepath_);
        file_ = other.file_;
        size_ = other.size_;
        other.file_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

std::vector<char> FileDataSource::read_range(uint64_t offset, uint32_t length) {
    if (!file_ || offset >= size_) {
        return {};
    }

    // Clamp length to available data
    uint32_t available = static_cast<uint32_t>(std::min<uint64_t>(length, size_ - offset));
    if (available == 0) {
        return {};
    }

    std::vector<char> buffer(available);

    // off_t is 64-bit here (see the top of this file), so the cast keeps the full
    // offset on 32-bit ARM instead of truncating past 2 GB.
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
        spdlog::error("[FileDataSource] Seek failed at offset {}", offset);
        return {};
    }

    size_t read = std::fread(buffer.data(), 1, available, file_);
    if (read != available) {
        spdlog::warn("[FileDataSource] Short read: requested {}, got {}", available, read);
        buffer.resize(read);
    }

    return buffer;
}

uint64_t FileDataSource::file_size() const {
    return size_;
}

bool FileDataSource::supports_range_requests() const {
    return true; // Local files always support random access
}

std::string FileDataSource::source_name() const {
    return filepath_;
}

bool FileDataSource::is_valid() const {
    return file_ != nullptr;
}

std::string FileDataSource::indexable_file_path() const {
    return filepath_;
}

// =============================================================================
// MemoryDataSource
// =============================================================================

MemoryDataSource::MemoryDataSource(std::string content, std::string name)
    : data_(content.begin(), content.end()), name_(std::move(name)) {}

MemoryDataSource::MemoryDataSource(std::vector<char> data, std::string name)
    : data_(std::move(data)), name_(std::move(name)) {}

std::vector<char> MemoryDataSource::read_range(uint64_t offset, uint32_t length) {
    if (offset >= data_.size()) {
        return {};
    }

    size_t available = std::min<size_t>(length, data_.size() - offset);
    return std::vector<char>(data_.begin() + offset, data_.begin() + offset + available);
}

uint64_t MemoryDataSource::file_size() const {
    return data_.size();
}

bool MemoryDataSource::supports_range_requests() const {
    return true;
}

std::string MemoryDataSource::source_name() const {
    return name_;
}

bool MemoryDataSource::is_valid() const {
    return true;
}

} // namespace gcode
} // namespace helix
