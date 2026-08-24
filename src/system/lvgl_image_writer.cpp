// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_image_writer.h"

#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace helix {

bool write_lvgl_bin(const std::string& path, int width, int height, uint8_t color_format,
                    const uint8_t* pixel_data, size_t data_size) {
#if defined(HELIX_PLATFORM_ESP32)
    // No disk-cache materialization on ESP32 (Task 10 R6 rider). LittleFS
    // partitions are tiny (128KB-3.75MB) and every caller here already treats
    // a false return as "cache generation failed, use the scaled source
    // instead" (printer_image_manager.cpp, prerendered_images.cpp,
    // thumbnail_processor.cpp all just propagate the bool) — refusing
    // upfront reuses that existing fallback instead of adding a new one.
    // Task 9 soak observed this exact path churning "No more free space"
    // errors + an LVGL-callback filesystem exception every boot
    // (soak_t9_confirm.log lines ~132-137, 244).
    (void)path;
    (void)width;
    (void)height;
    (void)color_format;
    (void)pixel_data;
    (void)data_size;
    return false;
#else
    // Use atomic write: write to temp file, then rename
    // This prevents partial/corrupted files if process crashes mid-write.
    //
    // The staging name must be unique PER WRITER, not per destination. The
    // thumbnail pool runs two workers over one cache directory, and two
    // requests for the same source at the same target size produce the same
    // output path — so a fixed "<path>.tmp" had both interleaving their bytes
    // into one file before both renamed it. Identical inputs made that benign
    // in practice, but it is a data race on a file, and the failure it would
    // produce (a half-and-half .bin that still passes the header check) is
    // exactly the kind of thing that reads as heap corruption downstream.
    static std::atomic<uint64_t> seq{0};
    const std::string temp_path = path + "." + std::to_string(::getpid()) + "." +
                                  std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) +
                                  ".tmp";

    std::ofstream file(temp_path, std::ios::binary);
    if (!file) {
        spdlog::warn("[LvglImageWriter] Cannot open {} for writing", temp_path);
        return false;
    }

    // ========================================================================
    // LVGL 9 Image Header - Use lv_image_header_t struct directly
    // ========================================================================
    // This ensures correct byte layout regardless of compiler/platform,
    // as we're using the same struct LVGL uses to read the file.

    // Always ARGB8888 (4 bytes per pixel) — kept generic for header correctness
    int bytes_per_pixel = 4;
    int stride = width * bytes_per_pixel;

    lv_image_header_t header = {};
    header.magic = LV_IMAGE_HEADER_MAGIC;
    header.cf = static_cast<lv_color_format_t>(color_format);
    header.flags = 0;
    header.w = static_cast<uint16_t>(width);
    header.h = static_cast<uint16_t>(height);
    header.stride = static_cast<uint16_t>(stride);
    header.reserved_2 = 0;

    // Write header using the actual struct (guarantees correct layout)
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write pixel data
    file.write(reinterpret_cast<const char*>(pixel_data), data_size);

    if (!file.good()) {
        spdlog::warn("[LvglImageWriter] Write error for {}", temp_path);
        file.close();
        std::filesystem::remove(temp_path); // Clean up partial file
        return false;
    }
    file.close();

    // Atomic rename - if this fails, the temp file is left but no corrupted final file
    try {
        std::filesystem::rename(temp_path, path);
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[LvglImageWriter] Atomic rename failed: {}", e.what());
        std::filesystem::remove(temp_path);
        return false;
    }

    spdlog::trace("[LvglImageWriter] Wrote {} bytes to {}", sizeof(header) + data_size, path);

    return true;
#endif // HELIX_PLATFORM_ESP32
}

} // namespace helix
