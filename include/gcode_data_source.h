// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix {
namespace gcode {

/**
 * @brief Abstract interface for reading G-code data
 *
 * Provides a uniform interface for reading G-code from various sources:
 * - Local files (FileDataSource)
 * - In-memory buffers (MemoryDataSource, for testing)
 *
 * This abstraction enables streaming G-code parsing on memory-constrained
 * devices by loading only the needed byte ranges rather than entire files.
 */
class GCodeDataSource {
  public:
    virtual ~GCodeDataSource() = default;

    /**
     * @brief Read a byte range from the source
     *
     * @param offset Starting byte position
     * @param length Number of bytes to read
     * @return Vector of bytes, or empty if read failed
     *
     * @note May return fewer bytes than requested if at end of source
     */
    virtual std::vector<char> read_range(uint64_t offset, uint32_t length) = 0;

    /**
     * @brief Get total size of the data source
     * @return Size in bytes, or 0 if unknown
     */
    virtual uint64_t file_size() const = 0;

    /**
     * @brief Check if source supports efficient range requests
     *
     * For HTTP sources, this indicates whether Range headers work.
     * For local files, this is always true.
     *
     * @return true if range requests are efficient
     */
    virtual bool supports_range_requests() const = 0;

    /**
     * @brief Get descriptive name of the source
     * @return Source description (e.g., filename, URL)
     */
    virtual std::string source_name() const = 0;

    /**
     * @brief Check if source is valid/accessible
     * @return true if source can be read
     */
    virtual bool is_valid() const = 0;

    /**
     * @brief Get a local file path suitable for indexing
     *
     * Returns a path that can be used for file-based indexing.
     * For file sources, this is the original filepath.
     * For memory sources, this returns empty string (no file available).
     *
     * @return Local file path, or empty string if no file is available
     */
    virtual std::string indexable_file_path() const {
        return "";
    }

    /**
     * @brief Ensure the source is ready for indexing
     *
     * For sources that may need preparation before indexing (e.g., staging a
     * remote file on disk), this method performs that preparation. The layer
     * indexer requires filesystem access for memory-mapped parsing, so any
     * such source must materialize a local file here.
     *
     * For local files and memory sources, this is a no-op (returns true).
     *
     * @return true if the source is now ready for indexing
     */
    virtual bool ensure_indexable() {
        return true;
    }

    /**
     * @brief Read a single line starting at offset
     *
     * Reads characters until newline or end of source.
     * Convenience method built on read_range().
     *
     * @param offset Starting byte position
     * @param max_length Maximum line length (default 4096)
     * @return Line content (without newline), or nullopt if read failed
     */
    std::optional<std::string> read_line(uint64_t offset, size_t max_length = 4096);

    /**
     * @brief Read entire source into memory
     *
     * Warning: Can use a lot of memory for large files!
     * Use read_range() for streaming access instead.
     *
     * @return All bytes from source
     */
    std::vector<char> read_all();
};

/**
 * @brief Data source for local files
 *
 * Uses standard file I/O with fseek/fread for efficient random access.
 * This is the most efficient source for local G-code files.
 */
class FileDataSource : public GCodeDataSource {
  public:
    /**
     * @brief Create data source from file path
     * @param filepath Path to local file
     */
    explicit FileDataSource(const std::string& filepath);

    ~FileDataSource() override;

    FileDataSource(const FileDataSource&) = delete;
    FileDataSource& operator=(const FileDataSource&) = delete;

    FileDataSource(FileDataSource&& other) noexcept;
    FileDataSource& operator=(FileDataSource&& other) noexcept;

    std::vector<char> read_range(uint64_t offset, uint32_t length) override;
    uint64_t file_size() const override;
    bool supports_range_requests() const override;
    std::string source_name() const override;
    bool is_valid() const override;
    std::string indexable_file_path() const override;

    /**
     * @brief Get the file path
     * @return Path to the source file
     */
    const std::string& filepath() const {
        return filepath_;
    }

  private:
    std::string filepath_;
    FILE* file_{nullptr};
    uint64_t size_{0};
};

/**
 * @brief In-memory data source (for testing)
 *
 * Useful for unit tests without needing actual files.
 */
class MemoryDataSource : public GCodeDataSource {
  public:
    /**
     * @brief Create from string content
     * @param content G-code content
     * @param name Optional source name for debugging
     */
    explicit MemoryDataSource(std::string content, std::string name = "memory");

    /**
     * @brief Create from vector of bytes
     * @param data G-code bytes
     * @param name Optional source name for debugging
     */
    explicit MemoryDataSource(std::vector<char> data, std::string name = "memory");

    std::vector<char> read_range(uint64_t offset, uint32_t length) override;
    uint64_t file_size() const override;
    bool supports_range_requests() const override;
    std::string source_name() const override;
    bool is_valid() const override;

    /// Memory sources cannot provide a file path for indexing
    std::string indexable_file_path() const override {
        return "";
    }

  private:
    std::vector<char> data_;
    std::string name_;
};

} // namespace gcode
} // namespace helix
