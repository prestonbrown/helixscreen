// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fbdev_signal_clear.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace helix {

namespace {

constexpr size_t kChunkBytes = 4096;

// One pixel past a full chunk, so a write can start at any byte offset within
// a pixel and still have kChunkBytes of pattern behind it.
constexpr size_t kPatternWords = kChunkBytes / sizeof(uint32_t) + 1;

constexpr std::array<uint32_t, kPatternWords> make_pattern(uint32_t word) {
    std::array<uint32_t, kPatternWords> a{};
    for (auto& w : a) {
        w = word;
    }
    return a;
}

// Opaque black for 32bpp, the same pixel DisplayBackend::clear_framebuffer()
// writes: some 32bpp panels read the top byte as alpha.
constexpr std::array<uint32_t, kPatternWords> kBlack32 = make_pattern(0xFF000000u);
constexpr std::array<uint32_t, kPatternWords> kZeros = make_pattern(0);

char s_fb_path[256] = {};
size_t s_fb_bytes = 0;
const unsigned char* s_fb_pattern = nullptr;

std::atomic<bool> s_fb_ready{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "the signal handler reads this flag; it must not take a lock");

} // namespace

bool cache_framebuffer_for_signal(const std::string& path, size_t byte_len,
                                  uint32_t bits_per_pixel) {
    s_fb_ready.store(false, std::memory_order_release);
    if (path.empty() || byte_len == 0 || path.size() >= sizeof(s_fb_path)) {
        return false;
    }
    std::memcpy(s_fb_path, path.c_str(), path.size() + 1);
    s_fb_bytes = byte_len;
    s_fb_pattern = reinterpret_cast<const unsigned char*>(bits_per_pixel == 32 ? kBlack32.data()
                                                                               : kZeros.data());
    s_fb_ready.store(true, std::memory_order_release);
    return true;
}

void clear_framebuffer_signal_safe() {
    if (!s_fb_ready.load(std::memory_order_acquire)) {
        return;
    }
    const int saved_errno = errno;
    const int fd = ::open(s_fb_path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t written = 0;
        while (written < s_fb_bytes) {
            const size_t remaining = s_fb_bytes - written;
            const size_t chunk = remaining < kChunkBytes ? remaining : kChunkBytes;
            // A short write can end mid-pixel; resume at the matching pattern byte.
            const ssize_t n = ::write(fd, s_fb_pattern + written % sizeof(uint32_t), chunk);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            written += static_cast<size_t>(n);
        }
        ::close(fd);
    }
    errno = saved_errno;
}

} // namespace helix
