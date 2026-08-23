// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_fb0_sink.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

// The fbdev ioctl geometry probe is Linux-only. Everything else (mmap blit,
// RGB565->BGRA conversion) is portable POSIX, so non-Linux native/dev builds
// (macOS CI) compile the sink and exercise the conversion via configured
// geometry — only the real-device probe is compiled out.
#ifdef __linux__
#include <linux/fb.h>
#include <sys/ioctl.h>
#endif

namespace helix {

Fb0MailboxSink::Fb0MailboxSink(std::string dev) : dev_(std::move(dev)) {}

Fb0MailboxSink::~Fb0MailboxSink() {
    stop();
}

void Fb0MailboxSink::configure_geometry(int w, int h, uint32_t stride, int bpp) {
    has_cfg_ = true;
    cfg_w_ = w;
    cfg_h_ = h;
    cfg_stride_ = stride;
    cfg_bpp_ = bpp;
}

void Fb0MailboxSink::warn_once(const char* what) {
    if (!warned_) {
        warned_ = true;
        spdlog::warn("[RemoteScreen] fb0 sink disabled ({}): dev={}", what, dev_);
    }
}

bool Fb0MailboxSink::start() {
    if (active_) {
        return true;
    }

    fd_ = ::open(dev_.c_str(), O_RDWR);
    if (fd_ < 0) {
        warn_once("open failed");
        return false;
    }

    bool have_ioctl = false;

#ifdef __linux__
    // A real fbdev answers these ioctls; a regular file (test backing) does not.
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    std::memset(&vinfo, 0, sizeof(vinfo));
    std::memset(&finfo, 0, sizeof(finfo));

    have_ioctl = ::ioctl(fd_, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
                 ::ioctl(fd_, FBIOGET_FSCREENINFO, &finfo) == 0;

    if (have_ioctl) {
        fb_w_ = static_cast<int>(vinfo.xres);
        fb_h_ = static_cast<int>(vinfo.yres);
        fb_bpp_ = static_cast<int>(vinfo.bits_per_pixel);
        fb_stride_ = static_cast<uint32_t>(finfo.line_length);
    }
#endif

    if (!have_ioctl) {
        if (has_cfg_) {
            // Test path (and all non-Linux builds): fall back to configured
            // geometry so the blit math is exercisable without a device.
            fb_w_ = cfg_w_;
            fb_h_ = cfg_h_;
            fb_bpp_ = cfg_bpp_;
            fb_stride_ = cfg_stride_;
        } else {
            warn_once("no fbdev ioctls and no configured geometry");
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }

    if (fb_bpp_ != 16 && fb_bpp_ != 32) {
        warn_once("unsupported bpp (need 16 or 32)");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (fb_w_ <= 0 || fb_h_ <= 0 || fb_stride_ == 0) {
        warn_once("invalid geometry");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    map_size_ = static_cast<size_t>(fb_stride_) * static_cast<size_t>(fb_h_);
    void* m = ::mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        warn_once("mmap failed");
        map_size_ = 0;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    map_ = static_cast<uint8_t*>(m);
    active_ = true;
    spdlog::info("[RemoteScreen] fb0 sink active: dev={} {}x{} stride={} bpp={}", dev_, fb_w_,
                 fb_h_, fb_stride_, fb_bpp_);
    return true;
}

void Fb0MailboxSink::stop() {
    if (map_) {
        ::munmap(map_, map_size_);
        map_ = nullptr;
        map_size_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    active_ = false;
}

bool Fb0MailboxSink::wants_frames() const {
    return active_;
}

void Fb0MailboxSink::on_frame(const RemoteScreenFrame& f) {
    if (!active_ || map_ == nullptr || f.px_map == nullptr) {
        return;
    }

    // Source bytes-per-pixel by format. Unknown formats are skipped rather than
    // mirrored as garbage.
    int src_bpp = 0;
    switch (f.src_format) {
    case RemoteScreenPixelFormat::BGRA8888:
        src_bpp = 4;
        break;
    case RemoteScreenPixelFormat::RGB565:
        src_bpp = 2;
        break;
    case RemoteScreenPixelFormat::Unknown:
        break;
    }

    // Destination bytes-per-pixel: 4 (32bpp BGRA) or 2 (16bpp RGB565). All four
    // src x dst combinations are handled in the blit loop below.
    const int dst_bpp = fb_bpp_ / 8;

    // The source origin — the display coordinate of px_map's pixel (0,0) — is
    // declared by the producer, because it differs by LVGL render mode:
    //
    //   DIRECT/FULL: px_map is the whole display buffer, so a dirty rect's
    //     pixels live at their ABSOLUTE coordinates. Reading row-relative from
    //     px_map[0] would copy the buffer's top-left corner to every rect,
    //     ghosting the nav bar / header across the screen (#1031 doubling).
    //     The producer leaves px_map_x/px_map_y at 0 and absolute == relative.
    //
    //   PARTIAL (the fbdev fallback, taken when DRM init fails): lv_refr.c
    //     reshapes the draw buffer to the dirty area and flushes from its own
    //     origin, so the rect's pixels start at row 0 / column 0. The producer
    //     sets px_map_x/px_map_y to the area's top-left. Indexing absolutely
    //     here read the wrong row for rects inside the buffer's line window and
    //     tripped the bounds guard for rects below it (#1334).
    //
    // Subtracting the origin covers both: it is a no-op in direct/full mode.
    int32_t x1 = f.x1;
    int32_t y1 = f.y1;
    int32_t x2 = f.x2;
    int32_t y2 = f.y2;

    // One-shot diagnostics: log exactly what the DRM flush hands us on the first
    // few frames so the on-device layout (area, real stride, format) is
    // observable without a debugger.
    if (log_count_ < 3) {
        ++log_count_;
        spdlog::info("[RemoteScreen] on_frame ENTER #{}: area=({},{})-({},{}) "
                     "disp={}x{} src_stride={} src_bpp={} cf={} fb={}x{} fb_bpp={} dst_bpp={} "
                     "fb_stride={} map_size={}",
                     log_count_, f.x1, f.y1, f.x2, f.y2, f.disp_w, f.disp_h, f.src_stride, src_bpp,
                     f.color_format, fb_w_, fb_h_, fb_bpp_, dst_bpp, fb_stride_,
                     static_cast<unsigned long>(map_size_));
    }

    if (f.src_stride == 0 || src_bpp == 0 || (fb_bpp_ != 16 && fb_bpp_ != 32)) {
        return;
    }

    // Clamp the dirty rect into the fb0 mapping. The clamped rect is in display
    // coordinates; the source offsets below convert it into px_map's own frame
    // by subtracting px_map_x/px_map_y.
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > fb_w_ - 1) {
        x2 = fb_w_ - 1;
    }
    if (y2 > fb_h_ - 1) {
        y2 = fb_h_ - 1;
    }
    const int32_t w = x2 - x1 + 1;
    const int32_t h = y2 - y1 + 1;
    if (w <= 0 || h <= 0) {
        return;
    }

    // Hard OOB guard on the SOURCE. When the producer told us how many bytes are
    // readable at px_map, that length is authoritative. Otherwise fall back to
    // inferring the bound as (src_stride * disp_h) — which only holds while the
    // render buffer really is display-sized, so a frame beyond it means our
    // area/stride view disagrees with the renderer. Either way, skip rather than
    // risk a segfault (which on the render thread would wedge the UI -> watchdog
    // kill).
    const size_t src_stride = f.src_stride;
    const size_t src_row_bytes = static_cast<size_t>(w) * static_cast<size_t>(src_bpp);
    // Source coordinates of the clamped rect, relative to px_map's origin.
    const int32_t src_x1 = x1 - f.px_map_x;
    const int32_t src_y1 = y1 - f.px_map_y;
    const int32_t src_x2 = x2 - f.px_map_x;
    const int32_t src_y2 = y2 - f.px_map_y;
    // A rect that starts before the source origin cannot be served from this
    // buffer at all — skip rather than index negatively.
    if (src_x1 < 0 || src_y1 < 0) {
        if (!oob_warned_) {
            oob_warned_ = true;
            spdlog::warn("[RemoteScreen] frame starts before the source origin "
                         "(rect=({},{}) px_map_origin=({},{})) — skipping mirror",
                         x1, y1, f.px_map_x, f.px_map_y);
        }
        return;
    }
    const int32_t bound_h = f.disp_h > 0 ? f.disp_h : fb_h_;
    const size_t src_bound =
        f.px_map_len > 0 ? f.px_map_len : src_stride * static_cast<size_t>(bound_h);
    // One-past-end byte offset of the last pixel the blit will read.
    const size_t last_src = static_cast<size_t>(src_y2) * src_stride +
                            static_cast<size_t>(src_x2 + 1) * static_cast<size_t>(src_bpp);
    if (src_row_bytes > src_stride || last_src > src_bound) {
        if (!oob_warned_) {
            oob_warned_ = true;
            spdlog::warn(
                "[RemoteScreen] frame out of source bounds "
                "(src_row_bytes={} src_stride={} last_src={} bound={} px_map_len={}) — "
                "skipping mirror",
                static_cast<unsigned long>(src_row_bytes), static_cast<unsigned long>(src_stride),
                static_cast<unsigned long>(last_src), static_cast<unsigned long>(src_bound),
                static_cast<unsigned long>(f.px_map_len));
        }
        return;
    }

    for (int32_t row = 0; row < h; ++row) {
        const int32_t abs_y = y1 + row;
        const size_t dst_off = static_cast<size_t>(abs_y) * fb_stride_ +
                               static_cast<size_t>(x1) * static_cast<size_t>(dst_bpp);
        // Same pixel, expressed relative to px_map's own origin.
        const size_t src_off = static_cast<size_t>(abs_y - f.px_map_y) * src_stride +
                               static_cast<size_t>(src_x1) * static_cast<size_t>(src_bpp);
        uint8_t* dst = map_ + dst_off;
        const uint8_t* src = f.px_map + src_off;

        if (dst_bpp == 4) {
            // Destination is 32bpp BGRA (the hardware-verified U1 480x320 fb0).
            if (src_bpp == 4) {
                // BGRA -> BGRA: straight copy.
                std::memcpy(dst, src, static_cast<size_t>(w) * 4);
            } else {
                // RGB565 (little-endian 0bRRRRRGGGGGGBBBBB) -> BGRA8888.
                for (int32_t col = 0; col < w; ++col) {
                    const uint16_t p = static_cast<uint16_t>(src[0] | (src[1] << 8));
                    src += 2;
                    const uint8_t r5 = static_cast<uint8_t>((p >> 11) & 0x1F);
                    const uint8_t g6 = static_cast<uint8_t>((p >> 5) & 0x3F);
                    const uint8_t b5 = static_cast<uint8_t>(p & 0x1F);
                    dst[0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2)); // B
                    dst[1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4)); // G
                    dst[2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2)); // R
                    dst[3] = 0xFF;                                        // A
                    dst += 4;
                }
            }
        } else {
            // Destination is 16bpp RGB565 (U1 firmwares whose /dev/fb0 is 16bpp).
            if (src_bpp == 2) {
                // RGB565 -> RGB565: straight copy. LVGL's RGB565 is little-endian
                // and matches fbdev RGB565 on these panels (the DRM draw buffer is
                // already RGB565), so no per-pixel repack is needed.
                std::memcpy(dst, src, static_cast<size_t>(w) * 2);
            } else {
                // BGRA8888 -> RGB565. Source bytes are B=src[0], G=src[1], R=src[2].
                for (int32_t col = 0; col < w; ++col) {
                    const uint8_t b = src[0];
                    const uint8_t g = src[1];
                    const uint8_t r = src[2];
                    src += 4;
                    const uint16_t p =
                        static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    dst[0] = static_cast<uint8_t>(p & 0xFF);        // low byte (little-endian)
                    dst[1] = static_cast<uint8_t>((p >> 8) & 0xFF); // high byte
                    dst += 2;
                }
            }
        }
    }

    if (log_count_ <= 3 && log_done_ < 3) {
        ++log_done_;
        spdlog::info(
            "[RemoteScreen] on_frame DONE #{} (copied {}x{} at ({},{}) src_bpp={} dst_bpp={})",
            log_done_, w, h, x1, y1, src_bpp, dst_bpp);
    }
}

const char* Fb0MailboxSink::name() const {
    return "Fb0MailboxSink"; // i18n: do not translate - internal sink identifier
}

} // namespace helix
