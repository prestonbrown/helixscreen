// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_drm_mode_matching.cpp
 * @brief Unit tests for DRM mode matching helper used by the DRM backend
 *        to honor -s (requested resolution) against the connector's modelist.
 */

#include "../../include/drm_mode_matching.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("find_matching_mode: empty list returns NO_MATCH", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes;
    REQUIRE(find_matching_mode(modes, 1024, 600) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_matching_mode: exact match at index 0", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1024, 600, 60, false},
        {800, 480, 60, true},
    };
    REQUIRE(find_matching_mode(modes, 1024, 600) == 0);
}

TEST_CASE("find_matching_mode: exact match mid-list", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 1080, 60, false},
        {1280, 720, 60, false},
        {1024, 600, 60, false},
        {800, 480, 60, true},
    };
    REQUIRE(find_matching_mode(modes, 1024, 600) == 2);
}

TEST_CASE("find_matching_mode: exact match at end", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 1080, 60, true},
        {800, 480, 60, false},
    };
    REQUIRE(find_matching_mode(modes, 800, 480) == 1);
}

TEST_CASE("find_matching_mode: no match returns NO_MATCH", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 1080, 60, true},
        {800, 480, 60, false},
    };
    REQUIRE(find_matching_mode(modes, 1234, 567) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_matching_mode: refresh rate is ignored when matching", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1024, 600, 30, false}, // different refresh, same pixels
    };
    REQUIRE(find_matching_mode(modes, 1024, 600) == 0);
}

TEST_CASE("find_preferred_mode_index: returns index of first preferred", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 1080, 60, false},
        {1280, 720, 60, true},
        {800, 480, 60, true},
    };
    REQUIRE(find_preferred_mode_index(modes) == 1);
}

TEST_CASE("find_preferred_mode_index: no preferred falls back to 0", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 1080, 60, false},
        {800, 480, 60, false},
    };
    REQUIRE(find_preferred_mode_index(modes) == 0);
}

TEST_CASE("find_preferred_mode_index: empty list returns NO_MATCH", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes;
    REQUIRE(find_preferred_mode_index(modes) == DrmModeMatch::NO_MATCH);
}

// --- High-DPI auto-downscale tests ---

TEST_CASE("find_best_downscale_mode: returns NO_MATCH when preferred is within threshold",
          "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 1080, 60, true},
        {1280, 720, 60, false},
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_best_downscale_mode: returns NO_MATCH when both axes equal threshold",
          "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 1920, 60, true},
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_best_downscale_mode: selects highest sub-threshold mode", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {2560, 1440, 60, true},  // preferred, over threshold
        {1920, 1080, 60, false}, // best candidate
        {1280, 720, 60, false},
        {640, 480, 60, false},
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == 1);
}

TEST_CASE("find_best_downscale_mode: portrait panel over threshold", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1440, 2560, 60, true}, // preferred, vdisplay over threshold
        {720, 1280, 60, false}, // best candidate
        {480, 800, 60, false},
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == 1);
}

TEST_CASE("find_best_downscale_mode: breaks ties by refresh rate", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {2560, 1440, 60, true},  // over threshold
        {1920, 1080, 30, false}, // same pixels, lower refresh
        {1920, 1080, 60, false}, // same pixels, higher refresh — winner
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == 2);
}

TEST_CASE("find_best_downscale_mode: no sub-threshold mode available returns NO_MATCH",
          "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {2560, 1440, 60, true},
        {3840, 2160, 30, false},
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_best_downscale_mode: empty list returns NO_MATCH", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes;
    REQUIRE(find_best_downscale_mode(modes, 1920) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_best_downscale_mode: single mode over threshold with no alternatives",
          "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {2560, 1440, 60, true},
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_best_downscale_mode: preferred not first in list", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1280, 720, 60, false},
        {1440, 2560, 60, true},  // preferred, over threshold
        {1920, 1080, 60, false}, // best candidate
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == 2);
}

TEST_CASE("find_best_downscale_mode: no preferred flag, mode[0] below threshold",
          "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1280, 720, 60, false},  // mode[0], fallback "preferred", below threshold
        {2560, 1440, 60, false}, // high-res but not preferred
    };
    // mode[0] is the fallback preferred; within threshold, so no downscale
    REQUIRE(find_best_downscale_mode(modes, 1920) == DrmModeMatch::NO_MATCH);
}

TEST_CASE("find_best_downscale_mode: no preferred flag, mode[0] above threshold",
          "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {2560, 1440, 60, false}, // mode[0], fallback "preferred", above threshold
        {1920, 1080, 60, false}, // candidate
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == 1);
}

TEST_CASE("find_best_downscale_mode: one axis at threshold, other above", "[drm_mode_matching]") {
    std::vector<DrmModeInfo> modes = {
        {1920, 2560, 60, true}, // hdisplay at threshold, vdisplay over
        {1280, 720, 60, false}, // candidate
    };
    REQUIRE(find_best_downscale_mode(modes, 1920) == 1);
}
