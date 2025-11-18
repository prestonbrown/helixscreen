// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Generate a large test G-code file to stress-test voxel preview
 */

#include <fstream>
#include <cmath>
#include <iostream>

int main(int argc, char** argv) {
    std::string output_path = argc > 1 ? argv[1] : "build/large_test.gcode";

    std::ofstream file(output_path);

    file << "; Large test G-code for voxel preview stress test\n";
    file << "G90 ; Absolute positioning\n";
    file << "M82 ; Absolute extrusion\n";
    file << "G28 ; Home\n\n";

    // Generate a spiral vase-like object
    float base_radius = 30.0f;
    float height = 100.0f;
    int layers = 500;
    float layer_height = height / layers;
    int segments_per_layer = 64;
    float e = 0.0f;

    for (int layer = 0; layer < layers; ++layer) {
        float z = layer * layer_height;
        float radius = base_radius * (1.0f - layer * 0.3f / layers);

        for (int seg = 0; seg <= segments_per_layer; ++seg) {
            float angle = (seg * 2.0f * M_PI) / segments_per_layer;
            float x = radius * std::cos(angle);
            float y = radius * std::sin(angle);

            e += 0.1f; // Simulate extrusion

            file << "G1 X" << x << " Y" << y << " Z" << z << " E" << e << " F1800\n";
        }
    }

    file.close();

    std::cout << "Generated " << output_path << " with " << (layers * segments_per_layer)
              << " extrusion moves\n";

    return 0;
}
