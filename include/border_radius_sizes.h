// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>

namespace helix {

/**
 * @brief Discrete border radius sizes with per-breakpoint pixel values.
 *
 * The theme stores a size index (0-7). At runtime, the current breakpoint
 * suffix selects the correct pixel value. This ensures corners look
 * proportional across screen sizes from 272px micro to 1440p+.
 */
class BorderRadiusSizes {
  public:
    static constexpr int SIZE_COUNT = 8;

    struct SizeEntry {
        const char* name;
        // Pixel values indexed by breakpoint: micro, tiny, small, medium, large, xlarge, xxlarge
        std::array<int, 7> pixels;
    };

    static constexpr int count() {
        return SIZE_COUNT;
    }

    static const char* name(int index) {
        index = clamp_index(index);
        return table()[index].name;
    }

    /**
     * @brief Surface radius (cards, dialogs, inputs) for a size index at a breakpoint.
     *
     * "Full" is meant to make buttons pills. On a surface it would turn any
     * near-square card or dialog into a circle and clip its content, so
     * surfaces stop at "Pill". Buttons use button_pixels().
     *
     * @param index Size index (0-7, clamped)
     * @param suffix Breakpoint suffix: "_micro", "_tiny", "_small", "_medium", "_large", "_xlarge",
     * "_xxlarge"
     */
    static int pixels(int index, const char* suffix) {
        return button_pixels(std::min(clamp_index(index), SIZE_COUNT - 2), suffix);
    }

    /// Button radius: the table value, including "Full".
    static int button_pixels(int index, const char* suffix) {
        return table()[clamp_index(index)].pixels[breakpoint_index(suffix)];
    }

    /**
     * @brief Find the nearest size index for a raw pixel value (migration).
     *
     * Compares against the XXLarge column (the "reference" size that old
     * themes were designed for) and picks the closest match.
     */
    static int nearest_size_index(int raw_pixels) {
        if (raw_pixels <= 0)
            return 0;
        if (raw_pixels >= 30)
            return 7; // anything >= 30 is "Full"

        int best = 0;
        int best_dist = INT_MAX;
        // Compare against XXLarge column (index 6)
        for (int i = 0; i < SIZE_COUNT; ++i) {
            int ref = table()[i].pixels[6]; // xxlarge
            if (ref == 9999)
                continue; // skip "Full" for distance calc
            int dist = std::abs(raw_pixels - ref);
            if (dist < best_dist) {
                best_dist = dist;
                best = i;
            }
        }
        return best;
    }

    static const std::array<SizeEntry, SIZE_COUNT>& table() {
        static constexpr std::array<SizeEntry, SIZE_COUNT> t = {{
            //                micro tiny small med  large xl   xxl
            {"None", {{0, 0, 0, 0, 0, 0, 0}}},
            {"Minimal", {{1, 1, 2, 3, 4, 4, 4}}},
            {"Subtle", {{2, 2, 3, 4, 6, 7, 8}}},
            {"Soft", {{3, 3, 4, 6, 10, 11, 12}}},
            {"Rounded", {{3, 4, 6, 8, 12, 14, 16}}},
            {"Bold", {{4, 5, 8, 10, 16, 18, 20}}},
            {"Pill", {{5, 6, 10, 14, 20, 24, 28}}},
            {"Full", {{9999, 9999, 9999, 9999, 9999, 9999, 9999}}},
        }};
        return t;
    }

  private:
    static int clamp_index(int index) {
        return std::clamp(index, 0, SIZE_COUNT - 1);
    }

    static int breakpoint_index(const char* suffix) {
        // Order: micro=0, tiny=1, small=2, medium=3, large=4, xlarge=5, xxlarge=6
        if (strcmp(suffix, "_micro") == 0)
            return 0;
        if (strcmp(suffix, "_tiny") == 0)
            return 1;
        if (strcmp(suffix, "_small") == 0)
            return 2;
        if (strcmp(suffix, "_medium") == 0)
            return 3;
        if (strcmp(suffix, "_large") == 0)
            return 4;
        if (strcmp(suffix, "_xlarge") == 0)
            return 5;
        return 6; // _xxlarge or unknown
    }
};

} // namespace helix
