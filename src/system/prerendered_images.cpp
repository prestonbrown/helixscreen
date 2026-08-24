// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "prerendered_images.h"

#include "app_globals.h"
#include "data_root_resolver.h"
#include "lvgl_image_writer.h"
#include "stb_image.h"
#include "stb_image_resize.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace helix {

bool prerendered_exists(const std::string& path) {
    // Callers pass a relative "assets/images/..." path. Resolve it against the
    // asset root so the check works on firmware (bundle mounted at /assets ->
    // /assets/assets/images/...); identity on desktop (asset_root ".").
    //
    // error_code overload, NOT exists(p): an existence probe must never throw.
    // The throwing overload only maps ENOENT/ENOTDIR to "not found"; the ESP32
    // VFS reports missing frogfs paths as ENODATA, which std::filesystem treats
    // as an error — on firmware exists(p) throws filesystem_error for every
    // miss, and the escaping exception blanked the home-panel printer image.
    std::error_code ec;
    return std::filesystem::exists(asset_path(path), ec);
}

const char* get_splash_size_name(int screen_width) {
    if (screen_width < 600) {
        return "tiny"; // 480x320 class
    } else if (screen_width < 900) {
        return "small"; // 800x480 class (AD5M)
    } else if (screen_width < 1100) {
        return "medium"; // 1024x600 class
    } else {
        return "large"; // 1280x720+ class
    }
}

const char* get_splash_3d_size_name(int screen_width, int screen_height) {
    // Ultra-wide displays (e.g. 1920x440): wide but very short
    if (screen_width >= 1100 && screen_height < 500) {
        return "ultrawide";
    }

    if (screen_width < 600) {
        // Distinguish K1 (480x400) from generic tiny (480x320)
        return (screen_height >= 380) ? "tiny_alt" : "tiny";
    } else if (screen_width < 900) {
        return "small"; // 800x480 class (AD5M)
    } else if (screen_width < 1100) {
        return "medium"; // 1024x600 class
    } else {
        return "large"; // 1280x720+ class
    }
}

int get_splash_3d_target_height(const char* size_name) {
    // Known heights for pre-rendered splash images (from gen_splash_3d.py SCREEN_SIZES)
    if (strcmp(size_name, "tiny") == 0)
        return 320;
    if (strcmp(size_name, "tiny_alt") == 0)
        return 400;
    if (strcmp(size_name, "small") == 0)
        return 480;
    if (strcmp(size_name, "medium") == 0)
        return 600;
    if (strcmp(size_name, "large") == 0)
        return 720;
    if (strcmp(size_name, "ultrawide") == 0)
        return 440;
    return 0; // Unknown — caller should fall back to runtime scaling
}

std::string get_prerendered_splash_3d_path(int screen_width, int screen_height, bool dark_mode) {
    const char* size_name = get_splash_3d_size_name(screen_width, screen_height);
    const char* mode_name = dark_mode ? "dark" : "light";

    // Path relative to install directory
    std::string path = "assets/images/prerendered/splash-3d-";
    path += mode_name;
    path += "-";
    path += size_name;
    path += ".bin";

    if (prerendered_exists(path)) {
        spdlog::debug("[Prerendered] Using 3D splash: {}", path);
        return asset_component_uri(path);
    }

    // Fallback: try base "tiny" if tiny_alt not found (backward compat)
    if (std::string(size_name) == "tiny_alt") {
        path = "assets/images/prerendered/splash-3d-";
        path += mode_name;
        path += "-tiny.bin";
        if (prerendered_exists(path)) {
            spdlog::debug("[Prerendered] Using 3D splash (tiny fallback): {}", path);
            return asset_component_uri(path);
        }
    }

    spdlog::debug("[Prerendered] 3D splash not found for {} {} ({}x{}), falling back", mode_name,
                  size_name, screen_width, screen_height);
    return "";
}

std::string get_prerendered_splash_path(int screen_width) {
    const char* size_name = get_splash_size_name(screen_width);

    // Path relative to install directory
    std::string path = "assets/images/prerendered/splash-logo-";
    path += size_name;
    path += ".bin";

    if (prerendered_exists(path)) {
        spdlog::debug("[Prerendered] Using splash: {}", path);
        return asset_component_uri(path);
    }

    spdlog::debug("[Prerendered] Splash fallback to PNG ({}px screen)", screen_width);
    return asset_component_uri("assets/images/helixscreen-logo.png");
}

int get_printer_image_size(int screen_width) {
    // 300px for medium-large displays (800x480+)
    // 150px for small displays (480x320)
    return (screen_width >= 600) ? 300 : 150;
}

std::string get_prerendered_printer_path(const std::string& printer_name, int screen_width) {
    int size = get_printer_image_size(screen_width);

    // Path relative to install directory
    std::string path = "assets/images/printers/prerendered/";
    path += printer_name;
    path += "-";
    path += std::to_string(size);
    path += ".bin";

    if (prerendered_exists(path)) {
        spdlog::debug("[Prerendered] Using printer image: {}", path);
        return asset_component_uri(path);
    }

    // Fall back to original PNG, but verify it exists
    std::string png_path = "assets/images/printers/" + printer_name + ".png";
    if (prerendered_exists(png_path)) {
        spdlog::trace("[Prerendered] Printer {} fallback to PNG (no {}px)", printer_name, size);
        return asset_component_uri(png_path);
    }

    // Neither prerendered nor PNG exists — fall back to generic
    spdlog::debug("[Prerendered] Printer {} has no image, using generic fallback", printer_name);
    std::string generic_bin =
        "assets/images/printers/prerendered/generic-corexy-" + std::to_string(size) + ".bin";
    if (prerendered_exists(generic_bin)) {
        return asset_component_uri(generic_bin);
    }
    return asset_component_uri("assets/images/printers/generic-corexy.png");
}

// =========================================================================
// Persistent printer image cache (exact widget dimensions)
// =========================================================================

static constexpr const char* PRINTER_CACHE_SUBDIR = "printer_images";

std::string get_printer_image_cache_dir() {
    return get_helix_cache_dir(PRINTER_CACHE_SUBDIR);
}

/// Extract a basename from an LVGL image path for use as cache key prefix.
/// "A:assets/images/printers/prerendered/creality-k1c-150.bin" -> "creality-k1c-150"
/// "A:assets/images/printers/creality-k1c.png" -> "creality-k1c"
static std::string extract_source_basename(const std::string& source_image_path) {
    std::string path = source_image_path;

    // Strip LVGL "A:" prefix
    if (path.size() >= 2 && path[0] == 'A' && path[1] == ':') {
        path = path.substr(2);
    }

    // Extract filename and strip extension
    std::filesystem::path p(path);
    return p.stem().string();
}

namespace {

/// Full-resolution PNG that a prerendered path was rendered from, or "" if the path
/// is not a shipped prerendered image (a user's custom image, say).
/// "…/printers/prerendered/creality-k1c-300.bin" -> "…/printers/creality-k1c.png"
std::string png_source_for_prerendered(const std::string& fs_path) {
    auto prerendered_pos = fs_path.find("/prerendered/");
    if (prerendered_pos == std::string::npos) {
        return {};
    }
    std::string name = std::filesystem::path(fs_path).stem().string();
    auto dash = name.rfind('-'); // strip the size suffix
    if (dash == std::string::npos) {
        return {};
    }
    return fs_path.substr(0, prerendered_pos + 1) + name.substr(0, dash) + ".png";
}

/// Pixel size encoded in a prerendered filename ("…-300.bin" -> 300), or 0 when the
/// path is not a prerendered image. Read from the name rather than the file header
/// so the decision can be made before opening anything.
int prerendered_tier_size(const std::string& fs_path) {
    if (fs_path.find("/prerendered/") == std::string::npos) {
        return 0;
    }
    std::string name = std::filesystem::path(fs_path).stem().string();
    auto dash = name.rfind('-');
    if (dash == std::string::npos) {
        return 0;
    }
    int size = 0;
    auto [_, ec] = std::from_chars(name.data() + dash + 1, name.data() + name.size(), size);
    return ec == std::errc() ? size : 0;
}

} // namespace

std::string get_cached_printer_image_path(const std::string& source_image_path, int width,
                                          int height) {
    std::string cache_dir = get_printer_image_cache_dir();
    std::string basename = extract_source_basename(source_image_path);
    return cache_dir + "/" + basename + "-" + std::to_string(width) + "x" + std::to_string(height) +
           ".bin";
}

bool generate_cached_printer_image(const std::string& source_image_path, int width, int height,
                                   const std::string& output_path) {
    // Strip LVGL "A:" prefix for filesystem access
    std::string fs_path = source_image_path;
    if (fs_path.size() >= 2 && fs_path[0] == 'A' && fs_path[1] == ':') {
        fs_path = fs_path.substr(2);
    }

    // Upscaling from the prerendered tier would bake blur into the cache, which is
    // then kept forever. The tiers are 150px and 300px, but the widget can be much
    // larger than either: on a 1024x600 display the home printer image resolves to
    // roughly 667x455, so the 300px tier gets enlarged ~2.2x, and at 1280x720 it is
    // worse. The full-resolution PNG is shipped alongside every prerendered image
    // and is typically 1600-2500px, so when the request is bigger than the tier,
    // resizing from the PNG costs one extra decode ONCE per widget size and gives a
    // genuinely sharp result instead of a permanently soft one. Downscaling from
    // the tier is still preferred (it is smaller and already the right colours).
    std::error_code png_ec;
    if (std::string png = png_source_for_prerendered(fs_path);
        !png.empty() && std::max(width, height) > prerendered_tier_size(fs_path) &&
        std::filesystem::exists(png, png_ec)) {
        spdlog::debug("[PrinterCache] {}x{} exceeds the prerendered tier; sourcing from {}", width,
                      height, png);
        fs_path = png;
    }

    // Determine source format from extension
    std::filesystem::path src(fs_path);
    std::string ext = src.extension().string();

    int src_w = 0, src_h = 0;
    std::vector<uint8_t> rgba_pixels;

    if (ext == ".bin") {
        // Source is LVGL binary — read header + BGRA pixel data, convert back to RGBA for resize
        std::ifstream file(fs_path, std::ios::binary);
        if (!file) {
            spdlog::warn("[PrinterCache] Cannot open source .bin: {}", fs_path);
            return false;
        }

        lv_image_header_t header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file.good() || header.magic != LV_IMAGE_HEADER_MAGIC) {
            spdlog::warn("[PrinterCache] Invalid .bin header: {}", fs_path);
            return false;
        }

        src_w = header.w;
        src_h = header.h;
        size_t stride = header.stride;
        size_t expected_bytes = stride * src_h;

        // Check file has enough data (handles compressed/RLE .bin files gracefully)
        auto file_pos = file.tellg();
        file.seekg(0, std::ios::end);
        size_t file_size = static_cast<size_t>(file.tellg()) - static_cast<size_t>(file_pos);
        file.seekg(file_pos);

        if (file_size < expected_bytes) {
            // File is smaller than expected — likely compressed (RLE/LZ4).
            // Fall back to original PNG if available.
            std::string png_fallback = png_source_for_prerendered(fs_path);
            if (png_fallback.empty()) {
                png_fallback = fs_path;
            }

            spdlog::debug("[PrinterCache] .bin may be compressed ({}B < {}B expected), "
                          "trying PNG fallback: {}",
                          file_size, expected_bytes, png_fallback);

            int channels = 0;
            uint8_t* pixels = stbi_load(png_fallback.c_str(), &src_w, &src_h, &channels, 4);
            if (pixels) {
                size_t pixel_bytes = static_cast<size_t>(src_w) * src_h * 4;
                rgba_pixels.assign(pixels, pixels + pixel_bytes);
                stbi_image_free(pixels);
            } else {
                spdlog::warn("[PrinterCache] Cannot decode PNG fallback: {}", png_fallback);
                return false;
            }
        } else {
            // Uncompressed — read pixel data using stride
            rgba_pixels.resize(static_cast<size_t>(src_w) * src_h * 4);
            for (int row = 0; row < src_h; ++row) {
                file.read(reinterpret_cast<char*>(rgba_pixels.data() + row * src_w * 4), src_w * 4);
                // Skip stride padding if any
                if (stride > static_cast<size_t>(src_w * 4)) {
                    file.seekg(stride - src_w * 4, std::ios::cur);
                }
            }
            if (!file.good()) {
                spdlog::warn("[PrinterCache] Read error from .bin: {}", fs_path);
                return false;
            }

            // Convert BGRA (LVGL) → RGBA (stb) for resize
            for (size_t i = 0; i < rgba_pixels.size(); i += 4) {
                std::swap(rgba_pixels[i], rgba_pixels[i + 2]); // B ↔ R
            }
        }
    } else {
        // Source is PNG/JPG — decode with stb_image
        int channels = 0;
        uint8_t* pixels = stbi_load(fs_path.c_str(), &src_w, &src_h, &channels, 4);
        if (!pixels) {
            spdlog::warn("[PrinterCache] Cannot decode source image: {}", fs_path);
            return false;
        }
        size_t pixel_bytes = static_cast<size_t>(src_w) * src_h * 4;
        rgba_pixels.assign(pixels, pixels + pixel_bytes);
        stbi_image_free(pixels);
    }

    // Resize preserving aspect ratio ("contain" fit), centered on transparent canvas
    float scale_x = static_cast<float>(width) / src_w;
    float scale_y = static_cast<float>(height) / src_h;
    float scale = std::min(scale_x, scale_y);
    int fit_w = std::max(1, static_cast<int>(src_w * scale));
    int fit_h = std::max(1, static_cast<int>(src_h * scale));

    std::vector<uint8_t> fit_pixels(static_cast<size_t>(fit_w) * fit_h * 4);
    int ok = stbir_resize_uint8(rgba_pixels.data(), src_w, src_h, 0, fit_pixels.data(), fit_w,
                                fit_h, 0, 4);
    if (!ok) {
        spdlog::error("[PrinterCache] Resize failed: {}x{} -> {}x{}", src_w, src_h, fit_w, fit_h);
        return false;
    }

    // Center the fitted image on a transparent canvas at the full target dimensions
    std::vector<uint8_t> resized(static_cast<size_t>(width) * height * 4, 0);
    int offset_x = (width - fit_w) / 2;
    int offset_y = (height - fit_h) / 2;
    for (int y = 0; y < fit_h; ++y) {
        size_t src_row = static_cast<size_t>(y) * fit_w * 4;
        size_t dst_row = (static_cast<size_t>(y + offset_y) * width + offset_x) * 4;
        std::memcpy(&resized[dst_row], &fit_pixels[src_row], static_cast<size_t>(fit_w) * 4);
    }

    // Convert RGBA → BGRA (LVGL ARGB8888 in little-endian)
    for (size_t i = 0; i < resized.size(); i += 4) {
        std::swap(resized[i], resized[i + 2]); // R ↔ B
    }

    // Ensure output directory exists
    std::filesystem::path out_dir = std::filesystem::path(output_path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    // Write LVGL binary (atomic via write_lvgl_bin)
    bool result =
        write_lvgl_bin(output_path, width, height, static_cast<uint8_t>(LV_COLOR_FORMAT_ARGB8888),
                       resized.data(), resized.size());

    if (result) {
        spdlog::info("[PrinterCache] Generated {}x{} cache: {}", width, height, output_path);
    }
    return result;
}

void prune_printer_image_cache(int max_files, int max_keep) {
    std::string cache_dir = get_printer_image_cache_dir();

    struct CacheEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };
    std::vector<CacheEntry> entries;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir)) {
            if (std::filesystem::is_regular_file(entry.path())) {
                entries.push_back({entry.path(), std::filesystem::last_write_time(entry.path())});
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        return; // Directory doesn't exist or can't be read
    }

    if (static_cast<int>(entries.size()) <= max_files) {
        return;
    }

    // Sort oldest first
    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) { return a.mtime < b.mtime; });

    int to_remove = static_cast<int>(entries.size()) - max_keep;
    for (int i = 0; i < to_remove; ++i) {
        std::error_code ec;
        std::filesystem::remove(entries[i].path, ec);
        if (!ec) {
            spdlog::debug("[PrinterCache] Pruned old cache entry: {}",
                          entries[i].path.filename().string());
        }
    }

    spdlog::info("[PrinterCache] Pruned {} old cache entries", to_remove);
}

int invalidate_printer_image_cache(const std::string& source_image_path) {
    std::string basename = extract_source_basename(source_image_path);
    if (basename.empty()) {
        return 0;
    }

    std::string cache_dir = get_printer_image_cache_dir();
    std::string prefix = basename + "-";
    int removed = 0;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir)) {
            if (!std::filesystem::is_regular_file(entry.path()))
                continue;
            std::string filename = entry.path().filename().string();
            if (filename.rfind(prefix, 0) == 0) {
                std::error_code ec;
                std::filesystem::remove(entry.path(), ec);
                if (!ec) {
                    ++removed;
                    spdlog::debug("[PrinterCache] Invalidated cache: {}", filename);
                }
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Cache dir doesn't exist — nothing to invalidate
    }

    if (removed > 0) {
        spdlog::info("[PrinterCache] Invalidated {} cache entries for '{}'", removed, basename);
    }
    return removed;
}

} // namespace helix
