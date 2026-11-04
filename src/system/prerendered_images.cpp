// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "prerendered_images.h"

#include "app_globals.h"
#include "core/data_root_resolver.h"
#include "core/text_io.h"
#include "helix_fs.h"
#include "lvgl_image_writer.h"
#include "prerender_size_class.h"
#include "stb_image.h"
#include "stb_image_resize.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace helix {

// lz4 lives inside the LVGL submodule's private layout; declaring the one entry
// point keeps this file from pinning that path. extern "C" keeps the C linkage
// the symbol actually has, so the enclosing namespace costs nothing.
extern "C" int LZ4_decompress_safe(const char* src, char* dst, int compressedSize, int dstCapacity);

namespace {
/// Follows the LVGL image header when LV_IMAGE_FLAGS_COMPRESSED is set.
struct CompressBlock {
    uint32_t method;
    uint32_t compressed_size;
    uint32_t decompressed_size;
};
} // namespace

bool prerendered_exists(const std::string& path) {
    // Callers pass a relative "assets/images/..." path. Resolve it against the
    // asset root so the check works on firmware (bundle mounted at /assets ->
    // /assets/assets/images/...); identity on desktop (asset_root ".").
    //
    // An existence probe must never throw: the ESP32 VFS reports a missing
    // frogfs path as ENODATA, and helix::fs::exists reads every stat failure as
    // "not there".
    return helix::fs::exists(asset_path(path));
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

    // A 480x400 panel takes the tiny canvas when its own is absent.
    if (std::string(size_name) == "small") {
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

std::string get_prerendered_splash_path(int screen_width, int screen_height) {
    const char* size_name = get_splash_size_name(screen_width, screen_height);

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

namespace {

/// One line per distinct key for the whole process. The resolver runs on every
/// panel rebuild, so an unconditional message here reprints several times per
/// navigation and the signal is lost in its own repeats.
///
/// Main thread only, which every caller of this file's resolvers is; the set has
/// no lock.
bool first_mention(const std::string& key) {
    static std::set<std::string> seen;
    return seen.insert(key).second;
}

/// Says once what the process will be decoding, because "which tier am I on"
/// is the first question any slow-paint report has to answer and it is
/// otherwise only inferable from a full debug trace.
void announce_tier_once(int size) {
    if (!first_mention("tier")) {
        return;
    }
    if (prerendered_exists("assets/images/printers/prerendered")) {
        spdlog::info("[Prerendered] Printer art: {}px renders", size);
        return;
    }
    // Absent for a source checkout and for any build that skipped
    // gen-printer-images, which is normal; a shipped package always carries them.
    spdlog::info("[Prerendered] Printer art: no renders installed, decoding full-resolution "
                 "PNGs instead (make gen-printer-images)");
}

} // namespace

std::string get_prerendered_printer_path(const std::string& printer_name, int screen_width) {
    int size = get_printer_image_size(screen_width);
    announce_tier_once(size);

    // Path relative to install directory
    std::string path = "assets/images/printers/prerendered/";
    path += printer_name;
    path += "-";
    path += std::to_string(size);
    path += ".bin";

    if (prerendered_exists(path)) {
        spdlog::debug("[Prerendered] {} -> {}", printer_name, path);
        return asset_component_uri(path);
    }

    // Fall back to original PNG, but verify it exists
    std::string png_path = "assets/images/printers/" + printer_name + ".png";
    if (prerendered_exists(png_path)) {
        // A render dir that exists but lacks THIS printer is partial coverage, and
        // the only state here that is never expected. A wholly absent dir already
        // spoke once in announce_tier_once(); repeating it per printer buries it.
        if (prerendered_exists("assets/images/printers/prerendered") &&
            first_mention(printer_name + "-" + std::to_string(size))) {
            spdlog::warn("[Prerendered] No {}px render for '{}' - decoding {} at full "
                         "resolution on every repaint. Expected {}",
                         size, printer_name, png_path, path);
        }
        spdlog::debug("[Prerendered] {} -> {} (full-resolution PNG)", printer_name, png_path);
        return asset_component_uri(png_path);
    }

    // Neither prerendered nor PNG exists — fall back to generic
    if (first_mention("generic:" + printer_name)) {
        spdlog::warn("[Prerendered] No artwork for '{}' - showing the generic CoreXY frame",
                     printer_name);
    }
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

static constexpr const char* kPrinterCacheSubdir = "printer_images";

std::string get_printer_image_cache_dir() {
    return get_helix_cache_dir(kPrinterCacheSubdir);
}

/// Filesystem path for an LVGL image path, which may carry the "A:" drive prefix.
static std::string strip_lvgl_prefix(const std::string& path) {
    if (path.size() >= 2 && path[0] == 'A' && path[1] == ':') {
        return path.substr(2);
    }
    return path;
}

/// Extract a basename from an LVGL image path for use as cache key prefix.
/// "A:assets/images/printers/prerendered/creality-k1c-150.bin" -> "creality-k1c-150"
/// "A:assets/images/printers/creality-k1c.png" -> "creality-k1c"
static std::string extract_source_basename(const std::string& source_image_path) {
    return std::string(helix::fs::stem(strip_lvgl_prefix(source_image_path)));
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
    std::string name(helix::fs::stem(fs_path));
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
    std::string name(helix::fs::stem(fs_path));
    auto dash = name.rfind('-');
    if (dash == std::string::npos) {
        return 0;
    }
    int size = 0;
    auto [_, ec] = std::from_chars(name.data() + dash + 1, name.data() + name.size(), size);
    return ec == std::errc() ? size : 0;
}

/// Source identity carried in a cache filename: last-write time and byte size,
/// "0-0" for anything that is not a readable regular file so the name stays
/// deterministic either way.
///
/// The time half is libstdc++'s file_clock tick count (nanoseconds from its
/// 2174-01-01 epoch), which is what names already on disk carry: rendering it
/// any other way would orphan every cached image once. Both halves print
/// unsigned so the routinely negative tick count keeps a "-" out of the name.
std::string source_fingerprint(const std::string& fs_path) {
    // libstdc++'s filesystem::__file_clock::_S_epoch_diff.
    constexpr std::int64_t kFileClockEpochDiffNs = 6'437'664'000LL * 1'000'000'000LL;
    unsigned long long mtime = 0;
    unsigned long long size = 0;

    if (helix::fs::is_regular_file(fs_path)) {
        if (const auto written = helix::fs::mtime_ns(fs_path)) {
            mtime = static_cast<unsigned long long>(*written - kFileClockEpochDiffNs);
        }
        if (const auto bytes = helix::text_io::file_size(fs_path)) {
            size = static_cast<unsigned long long>(*bytes);
        }
    }
    return std::to_string(mtime) + "-" + std::to_string(size);
}

/// Consume "<digits>" at `pos`, advancing it. False when there is no digit there.
bool consume_digits(const std::string& s, size_t& pos) {
    const size_t start = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    return pos > start;
}

} // namespace

std::string get_cached_printer_image_path(const std::string& source_image_path, int width,
                                          int height) {
    // The name carries the source's mtime and size, so an image rewritten in place
    // under an existing filename resolves to a different entry and the one holding
    // the old pixels is never consulted again.
    //
    // The fingerprint covers the REQUESTED source only. generate_cached_printer_image()
    // may read the pixels from the full-resolution PNG instead when the request
    // exceeds the prerendered tier, and a change to that PNG alone leaves this name
    // unmoved.
    std::string cache_dir = get_printer_image_cache_dir();
    std::string basename = extract_source_basename(source_image_path);
    return cache_dir + "/" + basename + "-" + std::to_string(width) + "x" + std::to_string(height) +
           "-" + source_fingerprint(strip_lvgl_prefix(source_image_path)) + ".bin";
}

bool printer_cache_entry_matches(const std::string& cache_filename,
                                 const std::string& source_image_path) {
    const std::string basename = extract_source_basename(source_image_path);
    if (basename.empty()) {
        return false;
    }
    const std::string prefix = basename + "-";
    if (cache_filename.rfind(prefix, 0) != 0) {
        return false;
    }

    // Past the basename the name must continue "<W>x<H>-", the dimension segment
    // get_cached_printer_image_path() writes. That segment is what separates a
    // PNG's own entries from those of the prerendered variants beside it:
    // "creality-k1-se-" also prefixes "creality-k1-se-300-233x209-…", which caches
    // a different source image.
    size_t pos = prefix.size();
    if (!consume_digits(cache_filename, pos)) {
        return false;
    }
    if (pos >= cache_filename.size() || cache_filename[pos] != 'x') {
        return false;
    }
    ++pos;
    if (!consume_digits(cache_filename, pos)) {
        return false;
    }
    return pos < cache_filename.size() && cache_filename[pos] == '-';
}

bool generate_cached_printer_image(const std::string& source_image_path, int width, int height,
                                   const std::string& output_path) {
    std::string fs_path = strip_lvgl_prefix(source_image_path);

    // Upscaling from the prerendered tier would bake blur into the cache, which is
    // then kept forever. The tiers are 150px and 300px, but the widget can be much
    // larger than either: on a 1024x600 display the home printer image resolves to
    // roughly 667x455, so the 300px tier gets enlarged ~2.2x, and at 1280x720 it is
    // worse. The full-resolution PNG is shipped alongside every prerendered image
    // and is typically 1600-2500px, so when the request is bigger than the tier,
    // resizing from the PNG costs one extra decode ONCE per widget size and gives a
    // genuinely sharp result instead of a permanently soft one. Downscaling from
    // the tier is still preferred (it is smaller and already the right colours).
    if (std::string png = png_source_for_prerendered(fs_path);
        !png.empty() && std::max(width, height) > prerendered_tier_size(fs_path) &&
        helix::fs::exists(png)) {
        spdlog::debug("[PrinterCache] {}x{} exceeds the prerendered tier; sourcing from {}", width,
                      height, png);
        fs_path = png;
    }

    // Determine source format from extension
    std::string ext(helix::fs::extension(fs_path));

    int src_w = 0, src_h = 0;
    std::vector<uint8_t> rgba_pixels;

    if (ext == ".bin") {
        // Source is LVGL binary — read header + BGRA pixel data, convert back to RGBA for resize
        helix::text_io::File file = helix::text_io::open_file(fs_path, "rb");
        if (!file) {
            spdlog::warn("[PrinterCache] Cannot open source .bin: {}", fs_path);
            return false;
        }
        auto read_exact = [&file](void* dst, size_t n) {
            return n == 0 || std::fread(dst, n, 1, file.get()) == 1;
        };

        lv_image_header_t header{};
        if (!read_exact(&header, sizeof(header)) || header.magic != LV_IMAGE_HEADER_MAGIC) {
            spdlog::warn("[PrinterCache] Invalid .bin header: {}", fs_path);
            return false;
        }

        src_w = header.w;
        src_h = header.h;
        size_t stride = header.stride;
        size_t expected_bytes = stride * src_h;

        // Check file has enough data (handles compressed/RLE .bin files gracefully)
        const size_t file_size = static_cast<size_t>(
            helix::text_io::file_size(fs_path).value_or(sizeof(header)) - sizeof(header));

        if (header.flags & LV_IMAGE_FLAGS_COMPRESSED) {
            // Every shipped tier is LZ4: scripts/lib/lvgl_image_lib.sh renders with
            // --compress LZ4. Decode it here rather than reaching for the source
            // PNG, because packaging deletes those PNGs (prune_assets), so on a
            // device there is nothing to reach for and the cache is never built.
            CompressBlock comp{};
            if (!read_exact(&comp, sizeof(comp))) {
                spdlog::warn("[PrinterCache] Truncated compression header: {}", fs_path);
                return false;
            }
            if (comp.method != LV_IMAGE_COMPRESS_LZ4) {
                spdlog::warn("[PrinterCache] Unsupported compression {} in {}", comp.method,
                             fs_path);
                return false;
            }

            std::vector<char> packed(comp.compressed_size);
            if (!read_exact(packed.data(), packed.size())) {
                spdlog::warn("[PrinterCache] Truncated compressed payload: {}", fs_path);
                return false;
            }

            std::vector<uint8_t> raw(comp.decompressed_size);
            const int produced =
                LZ4_decompress_safe(packed.data(), reinterpret_cast<char*>(raw.data()),
                                    static_cast<int>(packed.size()), static_cast<int>(raw.size()));
            if (produced < 0 || static_cast<size_t>(produced) != raw.size()) {
                spdlog::warn("[PrinterCache] LZ4 decode failed for {}", fs_path);
                return false;
            }

            // Rows arrive stride-padded; the resizer wants them tight.
            rgba_pixels.resize(static_cast<size_t>(src_w) * src_h * 4);
            const size_t row_bytes = static_cast<size_t>(src_w) * 4;
            if (raw.size() < stride * static_cast<size_t>(src_h)) {
                spdlog::warn("[PrinterCache] Decoded {}B, short of {}x{} stride {}", raw.size(),
                             src_w, src_h, stride);
                return false;
            }
            for (int row = 0; row < src_h; ++row) {
                std::memcpy(rgba_pixels.data() + static_cast<size_t>(row) * row_bytes,
                            raw.data() + static_cast<size_t>(row) * stride, row_bytes);
            }

            // BGRA (LVGL) -> RGBA (stb)
            for (size_t i = 0; i < rgba_pixels.size(); i += 4) {
                std::swap(rgba_pixels[i], rgba_pixels[i + 2]);
            }
        } else if (file_size < expected_bytes) {
            // Not flagged compressed and too small to hold its own pixels: the
            // file is truncated. The source PNG is the only way back, and it is
            // present on a dev tree even though a package has none.
            std::string png_fallback = png_source_for_prerendered(fs_path);
            if (png_fallback.empty()) {
                png_fallback = fs_path;
            }

            spdlog::debug("[PrinterCache] .bin is short ({}B < {}B expected), "
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
            bool read_ok = true;
            for (int row = 0; row < src_h && read_ok; ++row) {
                read_ok = read_exact(rgba_pixels.data() + row * src_w * 4,
                                     static_cast<size_t>(src_w) * 4);
                // Skip stride padding if any
                if (read_ok && stride > static_cast<size_t>(src_w * 4)) {
                    read_ok = std::fseek(file.get(), static_cast<long>(stride - src_w * 4),
                                         SEEK_CUR) == 0;
                }
            }
            if (!read_ok) {
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
    helix::fs::create_directories(std::string(helix::fs::parent_path(output_path)));

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
        std::string path;
        std::int64_t mtime_ns;
    };
    std::vector<CacheEntry> entries;

    const auto listing = helix::fs::list_dir(cache_dir);
    if (!listing) {
        return; // Directory doesn't exist or can't be read
    }
    for (const auto& entry : *listing) {
        if (!entry.is_regular) {
            continue;
        }
        const auto mtime = helix::fs::mtime_ns(entry.path);
        if (!mtime) {
            return; // Unreadable entry: leave the cache as it is
        }
        entries.push_back({entry.path, *mtime});
    }

    if (static_cast<int>(entries.size()) <= max_files) {
        return;
    }

    // Sort oldest first
    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) { return a.mtime_ns < b.mtime_ns; });

    int to_remove = static_cast<int>(entries.size()) - max_keep;
    for (int i = 0; i < to_remove; ++i) {
        if (helix::fs::remove(entries[i].path) || errno == ENOENT) {
            spdlog::debug("[PrinterCache] Pruned old cache entry: {}",
                          helix::fs::filename(entries[i].path));
        }
    }

    spdlog::info("[PrinterCache] Pruned {} old cache entries", to_remove);
}

} // namespace helix
