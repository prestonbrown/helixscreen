// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "gcode_voxel_preview.h"

#include "spdlog/spdlog.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/resource.h>

/**
 * @brief Get current memory usage in bytes
 */
size_t getCurrentMemoryUsage() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return usage.ru_maxrss; // macOS: bytes
#else
    return usage.ru_maxrss * 1024; // Linux: kilobytes -> bytes
#endif
}

/**
 * @brief Create a simple test G-code file
 */
void createTestGCode(const std::string& path) {
    std::ofstream file(path);

    file << "; Test G-code for voxel preview\n";
    file << "G90 ; Absolute positioning\n";
    file << "M82 ; Absolute extrusion\n";
    file << "G28 ; Home\n";
    file << "\n";

    // Draw a simple cube outline
    float size = 20.0f;
    float z_start = 0.2f;
    float layer_height = 0.2f;
    float e = 0.0f;

    for (int layer = 0; layer < 10; ++layer) {
        float z = z_start + layer * layer_height;

        // Bottom square
        file << "G1 X0 Y0 Z" << z << " F3000\n";
        file << "G1 X" << size << " Y0 E" << (e += size) << " F1800\n";
        file << "G1 X" << size << " Y" << size << " E" << (e += size) << "\n";
        file << "G1 X0 Y" << size << " E" << (e += size) << "\n";
        file << "G1 X0 Y0 E" << (e += size) << "\n";
    }

    file.close();
    spdlog::info("Created test G-code file: {}", path);
}

int main(int argc, char** argv) {
    // Setup logging
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::string gcode_path;
    std::string output_path = "build/gcode_preview_test.png";

    if (argc > 1) {
        gcode_path = argv[1];
    } else {
        // Create a test file
        gcode_path = "build/test_gcode.gcode";
        createTestGCode(gcode_path);
    }

    if (argc > 2) {
        output_path = argv[2];
    }

    spdlog::info("========================================");
    spdlog::info("G-code Voxel Preview Test");
    spdlog::info("========================================");
    spdlog::info("Input:  {}", gcode_path);
    spdlog::info("Output: {}", output_path);
    spdlog::info("========================================");

    // Measure memory before
    size_t mem_before = getCurrentMemoryUsage();
    spdlog::info("Initial memory: {:.2f} MB", mem_before / (1024.0f * 1024.0f));

    // Configure preview
    VoxelPreviewConfig config;
    config.resolution = 2.0f;       // 2 voxels/mm
    config.blur_sigma = 2.0f;       // Moderate blur
    config.chunk_size = 32;         // Process in 32^3 chunks
    config.output_width = 512;      // 512x512 output
    config.output_height = 512;
    config.adaptive_resolution = false;

    // Generate preview with timing
    auto start_time = std::chrono::high_resolution_clock::now();

    bool success = generateGCodePreview(gcode_path, output_path, config);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Measure memory after
    size_t mem_after = getCurrentMemoryUsage();
    size_t mem_peak = mem_after - mem_before;

    spdlog::info("========================================");
    spdlog::info("Results:");
    spdlog::info("========================================");
    spdlog::info("Success:      {}", success ? "YES" : "NO");
    spdlog::info("Duration:     {:.2f} seconds", duration.count() / 1000.0f);
    spdlog::info("Peak memory:  {:.2f} MB", mem_peak / (1024.0f * 1024.0f));
    spdlog::info("Final memory: {:.2f} MB", mem_after / (1024.0f * 1024.0f));
    spdlog::info("========================================");

    if (mem_peak > 50 * 1024 * 1024) {
        spdlog::warn("Peak memory usage ({:.2f} MB) exceeds 50MB target!",
                     mem_peak / (1024.0f * 1024.0f));
    } else {
        spdlog::info("Memory usage is within 50MB target");
    }

    // Test multiple camera angles
    if (success && argc == 1) {
        spdlog::info("\nGenerating multiple views...");

        GCodeParser parser;
        auto segments = parser.parseExtrusions(gcode_path);

        SparseVoxelGrid grid(config.resolution);
        for (const auto& seg : segments) {
            grid.addSegment(seg.first, seg.second, 0.4f);
        }
        grid.applyGaussianBlur(config.blur_sigma, config.chunk_size);

        grid.renderToImage("build/gcode_preview_front.png", CameraAngle::FRONT, 512, 512);
        grid.renderToImage("build/gcode_preview_top.png", CameraAngle::TOP, 512, 512);
        grid.renderToImage("build/gcode_preview_side.png", CameraAngle::SIDE, 512, 512);
        grid.renderToImage("build/gcode_preview_iso.png", CameraAngle::ISOMETRIC, 512, 512);

        spdlog::info("Generated all views");
    }

    return success ? 0 : 1;
}
