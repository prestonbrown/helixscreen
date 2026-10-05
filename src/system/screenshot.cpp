// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screenshot.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "lodepng_encode.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

namespace helix {

bool write_bmp(const char* filename, const uint8_t* data, int width, int height) {
    // RAII for file handle - automatically closes on all return paths
    std::unique_ptr<FILE, decltype(&fclose)> f(fopen(filename, "wb"), fclose);
    if (!f)
        return false;

    // BMP header (54 bytes total)
    uint32_t file_size = 54U + (static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4U);
    uint32_t pixel_offset = 54;
    uint32_t dib_size = 40;
    uint16_t planes = 1;
    uint16_t bpp = 32;
    uint32_t reserved = 0;
    uint32_t compression = 0;
    uint32_t ppm = 2835; // pixels per meter
    uint32_t colors = 0;

    // BMP file header (14 bytes)
    fputc('B', f.get());
    fputc('M', f.get());                  // Signature
    fwrite(&file_size, 4, 1, f.get());    // File size
    fwrite(&reserved, 4, 1, f.get());     // Reserved
    fwrite(&pixel_offset, 4, 1, f.get()); // Pixel data offset

    // DIB header (40 bytes)
    fwrite(&dib_size, 4, 1, f.get());    // DIB header size
    fwrite(&width, 4, 1, f.get());       // Width
    fwrite(&height, 4, 1, f.get());      // Height
    fwrite(&planes, 2, 1, f.get());      // Planes
    fwrite(&bpp, 2, 1, f.get());         // Bits per pixel
    fwrite(&compression, 4, 1, f.get()); // Compression (none)
    uint32_t image_size = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4U;
    fwrite(&image_size, 4, 1, f.get()); // Image size
    fwrite(&ppm, 4, 1, f.get());        // X pixels per meter
    fwrite(&ppm, 4, 1, f.get());        // Y pixels per meter
    fwrite(&colors, 4, 1, f.get());     // Colors in palette
    fwrite(&colors, 4, 1, f.get());     // Important colors

    // Write pixel data (BMP is bottom-up, so flip rows)
    for (int y = height - 1; y >= 0; y--) {
        fwrite(data + (static_cast<size_t>(y) * static_cast<size_t>(width) * 4), 4,
               static_cast<size_t>(width), f.get());
    }

    // File automatically closed by unique_ptr destructor
    return true;
}

std::vector<uint8_t> argb8888_to_rgba(const uint8_t* src, size_t pixel_count) {
    std::vector<uint8_t> rgba(pixel_count * 4);
    for (size_t i = 0; i < pixel_count; i++) {
        size_t o = i * 4;
        rgba[o + 0] = src[o + 2]; // R  <- ARGB8888 byte 2
        rgba[o + 1] = src[o + 1]; // G
        rgba[o + 2] = src[o + 0]; // B  <- ARGB8888 byte 0
        rgba[o + 3] = src[o + 3]; // A
    }
    return rgba;
}

bool write_png(const char* filename, const uint8_t* data, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba = argb8888_to_rgba(data, px);
    unsigned char* encoded = nullptr;
    size_t encoded_size = 0;
    unsigned err = lodepng_encode32(&encoded, &encoded_size, rgba.data(),
                                    static_cast<unsigned>(width), static_cast<unsigned>(height));
    // LVGL builds lodepng with lv_malloc/lv_free as its allocators, so the
    // encoded buffer MUST go back through lv_free — libc free() on an LVGL pool
    // pointer corrupts the heap on any build with a real custom allocator.
    std::unique_ptr<unsigned char, void (*)(void*)> owned(encoded, lv_free);
    if (err != 0 || !encoded) {
        spdlog::error("[Screenshot] PNG encode failed ({}): {}", err, filename);
        return false;
    }

    std::unique_ptr<FILE, decltype(&fclose)> f(fopen(filename, "wb"), fclose);
    if (!f) {
        spdlog::error("[Screenshot] Cannot open for writing: {}", filename);
        return false;
    }
    if (fwrite(encoded, 1, encoded_size, f.get()) != encoded_size) {
        spdlog::error("[Screenshot] Short write: {}", filename);
        return false;
    }
    return true;
}

namespace {

bool has_suffix_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

} // namespace

bool capture_frame(CapturedFrame& out, lv_obj_t* crop_to) {
    // Take snapshot using LVGL's native API (platform-independent)
    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* snapshot = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);

    if (!snapshot) {
        spdlog::error("[Screenshot] Failed to take screenshot");
        return false;
    }

    // lv_snapshot_take() only captures the active screen. Full-screen overlays that
    // live on the top layer (e.g. the PIN lock screen) are rendered separately and
    // would otherwise be missing. Composite the top layer over the base when present.
    lv_obj_t* top = lv_layer_top();
    if (top && lv_obj_get_child_count(top) > 0) {
        lv_draw_buf_t* top_snap = lv_snapshot_take(top, LV_COLOR_FORMAT_ARGB8888);
        if (top_snap && top_snap->header.w == snapshot->header.w &&
            top_snap->header.h == snapshot->header.h) {
            // ARGB8888 memory order is B,G,R,A per pixel. Alpha-blend top over base.
            uint8_t* base = snapshot->data;
            const uint8_t* over = top_snap->data;
            size_t px = static_cast<size_t>(snapshot->header.w) * snapshot->header.h;
            for (size_t i = 0; i < px; i++) {
                size_t o = i * 4;
                uint32_t a = over[o + 3];
                if (a == 0)
                    continue; // fully transparent — keep base pixel
                uint32_t ia = 255U - a;
                base[o + 0] = static_cast<uint8_t>((over[o + 0] * a + base[o + 0] * ia) / 255U);
                base[o + 1] = static_cast<uint8_t>((over[o + 1] * a + base[o + 1] * ia) / 255U);
                base[o + 2] = static_cast<uint8_t>((over[o + 2] * a + base[o + 2] * ia) / 255U);
                base[o + 3] = 255;
            }
        }
        if (top_snap)
            lv_draw_buf_destroy(top_snap);
    }

    const int width = static_cast<int>(snapshot->header.w);
    const int height = static_cast<int>(snapshot->header.h);
    std::vector<uint8_t> rgba =
        argb8888_to_rgba(snapshot->data, static_cast<size_t>(width) * static_cast<size_t>(height));
    lv_draw_buf_destroy(snapshot);

    if (!crop_to) {
        out.rgba = std::move(rgba);
        out.width = width;
        out.height = height;
        return true;
    }

    // Clamp to the captured buffer — a widget can extend past the screen
    // edge, and a partially-offscreen crop must not read out of bounds.
    lv_area_t area;
    lv_obj_get_coords(crop_to, &area);
    int x0 = std::max<int>(0, area.x1);
    int y0 = std::max<int>(0, area.y1);
    int x1 = std::min<int>(width, area.x2 + 1);
    int y1 = std::min<int>(height, area.y2 + 1);
    if (x1 <= x0 || y1 <= y0) {
        spdlog::error("[Screenshot] Crop region is empty (widget offscreen or zero-size)");
        return false;
    }

    const int crop_w = x1 - x0;
    const int crop_h = y1 - y0;
    std::vector<uint8_t> cropped(static_cast<size_t>(crop_w) * static_cast<size_t>(crop_h) * 4);
    for (int y = 0; y < crop_h; y++) {
        const uint8_t* src_row =
            rgba.data() +
            (static_cast<size_t>(y0 + y) * static_cast<size_t>(width) + static_cast<size_t>(x0)) *
                4;
        uint8_t* dst_row =
            cropped.data() + static_cast<size_t>(y) * static_cast<size_t>(crop_w) * 4;
        std::memcpy(dst_row, src_row, static_cast<size_t>(crop_w) * 4);
    }
    out.rgba = std::move(cropped);
    out.width = crop_w;
    out.height = crop_h;
    return true;
}

uint64_t frame_hash(const CapturedFrame& frame) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    for (uint8_t b : frame.rgba) {
        h ^= b;
        h *= 1099511628211ULL; // FNV-1a 64-bit prime
    }
    return h;
}

std::string write_frame(const CapturedFrame& frame, const std::string& out_path) {
    if (frame.width <= 0 || frame.height <= 0) {
        spdlog::error("[Screenshot] Cannot write an empty frame");
        return {};
    }

    // No destination given -> timestamped BMP in the writable runtime dir.
    std::string filename =
        out_path.empty() ? app_get_runtime_dir() + "/ui-screenshot-" +
                               std::to_string(static_cast<unsigned long>(time(nullptr))) + ".bmp"
                         : out_path;
    bool as_png = has_suffix_ci(filename, ".png");

    // write_png()/write_bmp() both expect LVGL's native ARGB8888 byte order
    // (B,G,R,A); CapturedFrame stores true RGBA (R,G,B,A) per capture_frame().
    // Swap the channels back before handing off to those (already-tested)
    // encoders — the R/B swap is its own inverse, so this is the same helper
    // capture_frame() used to build the frame in the first place.
    std::vector<uint8_t> argb = argb8888_to_rgba(
        frame.rgba.data(), static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height));

    bool ok = as_png ? write_png(filename.c_str(), argb.data(), frame.width, frame.height)
                     : write_bmp(filename.c_str(), argb.data(), frame.width, frame.height);
    if (!ok) {
        spdlog::error("[Screenshot] Failed to write: {}", filename);
        return {};
    }
    spdlog::info("[Screenshot] saved: {}", filename);
    return filename;
}

std::string save_screenshot(const std::string& out_path) {
    CapturedFrame frame;
    if (!capture_frame(frame)) {
        NOTIFY_ERROR("Failed to save screenshot");
        LOG_ERROR_INTERNAL("Failed to capture screenshot frame");
        return {};
    }
    std::string filename = write_frame(frame, out_path);
    if (filename.empty()) {
        NOTIFY_ERROR("Failed to save screenshot");
        LOG_ERROR_INTERNAL("Failed to save screenshot to {}",
                           out_path.empty() ? std::string("(default path)") : out_path);
    }
    return filename;
}

} // namespace helix
