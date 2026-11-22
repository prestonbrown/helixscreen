// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_playback.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace helix::timelapse {

// --- Routing ---

VideoOpenAction choose_video_open_action(const VideoOpenInputs& in) {
    if (in.ffmpeg_available && in.board_can_decode && !in.print_active) {
        return in.moonraker_local ? VideoOpenAction::PlayInApp : VideoOpenAction::DownloadThenPlay;
    }
    return in.phone_url.empty() ? VideoOpenAction::NoPlayerToast : VideoOpenAction::PhoneQr;
}

int tap_badge_for(VideoOpenAction action) {
    switch (action) {
    case VideoOpenAction::PlayInApp:
    case VideoOpenAction::DownloadThenPlay:
        return 1;
    case VideoOpenAction::PhoneQr:
        return 2;
    case VideoOpenAction::NoPlayerToast:
        break;
    }
    return 0;
}

bool inapp_player_supported(PlatformTier tier) {
    return tier != PlatformTier::EMBEDDED;
}

// --- Probe parsing ---

namespace {

/// "30/1", "30000/1001" or "25": 0 when unusable.
double parse_rate(const std::string& text) {
    const auto slash = text.find('/');
    const double num = std::atof(text.substr(0, slash).c_str());
    if (slash == std::string::npos) {
        return num > 0 ? num : 0.0;
    }
    const double den = std::atof(text.substr(slash + 1).c_str());
    return (num > 0 && den > 0) ? num / den : 0.0;
}

bool sane_size(int w, int h) {
    return w > 0 && h > 0 && w <= 16384 && h <= 16384;
}

} // namespace

std::vector<std::string> ffprobe_info_args(const std::string& path) {
    return {"ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,r_frame_rate,avg_frame_rate:format=duration",
            "-of",
            "default=noprint_wrappers=1",
            path};
}

std::optional<VideoInfo> parse_ffprobe_info(const std::string& output) {
    VideoInfo info;
    double r_rate = 0.0;
    double avg_rate = 0.0;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos) {
            end = output.size();
        }
        const std::string line = output.substr(pos, end - pos);
        pos = end + 1;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "width") {
            info.width = std::atoi(value.c_str());
        } else if (key == "height") {
            info.height = std::atoi(value.c_str());
        } else if (key == "r_frame_rate") {
            r_rate = parse_rate(value);
        } else if (key == "avg_frame_rate") {
            avg_rate = parse_rate(value);
        } else if (key == "duration") {
            const double d = std::atof(value.c_str());
            info.duration_s = d > 0 ? d : 0.0;
        }
    }
    if (!sane_size(info.width, info.height)) {
        return std::nullopt;
    }
    info.fps = avg_rate > 0 ? avg_rate : r_rate;
    return info;
}

std::vector<std::string> ffmpeg_header_args(const std::string& path) {
    return {"ffmpeg", "-hide_banner", "-nostdin", "-i", path};
}

std::optional<VideoInfo> parse_ffmpeg_header(const std::string& text) {
    VideoInfo info;

    const auto dur = text.find("Duration: ");
    if (dur != std::string::npos) {
        int h = 0, m = 0;
        double s = 0.0;
        if (std::sscanf(text.c_str() + dur + 10, "%d:%d:%lf", &h, &m, &s) == 3) {
            info.duration_s = h * 3600.0 + m * 60.0 + s;
        }
    }

    const auto video = text.find("Video: ");
    if (video == std::string::npos) {
        return std::nullopt;
    }
    auto line_end = text.find('\n', video);
    if (line_end == std::string::npos) {
        line_end = text.size();
    }
    const std::string line = text.substr(video, line_end - video);

    // The size is the first comma-separated field that reads WxH with both
    // sides sane; this skips codec tags like "0x31637661".
    size_t field = 0;
    while (field < line.size()) {
        size_t next = line.find(',', field);
        if (next == std::string::npos) {
            next = line.size();
        }
        const std::string part = line.substr(field, next - field);
        int w = 0, h = 0;
        const char* p = part.c_str();
        while (*p == ' ') {
            ++p;
        }
        if (info.width == 0 && std::sscanf(p, "%dx%d", &w, &h) == 2 && sane_size(w, h) && w >= 16 &&
            h >= 16) {
            info.width = w;
            info.height = h;
        }
        const auto fps = part.find(" fps");
        if (fps != std::string::npos && info.fps == 0.0) {
            info.fps = std::atof(part.substr(0, fps).c_str());
        }
        field = next + 1;
    }
    if (info.width == 0) {
        return std::nullopt;
    }
    return info;
}

double effective_fps(double fps) {
    return (fps > 0.5 && fps <= 240.0) ? fps : 30.0;
}

// --- Formats and sizes ---

RawPixelFormat raw_format_for_display_bpp(int bits_per_pixel) {
    if (bits_per_pixel == 16) {
        return RawPixelFormat::Rgb565;
    }
    if (bits_per_pixel == 24) {
        return RawPixelFormat::Rgb888;
    }
    return RawPixelFormat::Xrgb8888;
}

const char* ffmpeg_pix_fmt_name(RawPixelFormat format) {
    switch (format) {
    case RawPixelFormat::Rgb565:
        return "rgb565le";
    case RawPixelFormat::Rgb888:
        return "bgr24";
    case RawPixelFormat::Xrgb8888:
        break;
    }
    return "bgra";
}

int bytes_per_pixel(RawPixelFormat format) {
    switch (format) {
    case RawPixelFormat::Rgb565:
        return 2;
    case RawPixelFormat::Rgb888:
        return 3;
    case RawPixelFormat::Xrgb8888:
        break;
    }
    return 4;
}

FrameSize fit_frame(FrameSize src, FrameSize box) {
    if (src.empty() || box.empty()) {
        return {};
    }
    const double scale = std::min(static_cast<double>(box.width) / src.width,
                                  static_cast<double>(box.height) / src.height);
    int w = static_cast<int>(std::floor(src.width * scale + 1e-9));
    int h = static_cast<int>(std::floor(src.height * scale + 1e-9));
    w = std::min(w, box.width) & ~1;
    h = std::min(h, box.height) & ~1;
    if (w <= 0 || h <= 0) {
        return {};
    }
    return {w, h};
}

size_t frame_bytes(FrameSize size, RawPixelFormat format) {
    if (size.empty()) {
        return 0;
    }
    return static_cast<size_t>(size.width) * static_cast<size_t>(size.height) *
           static_cast<size_t>(bytes_per_pixel(format));
}

std::vector<std::string> ffmpeg_decode_args(const std::string& path, double start_s, FrameSize out,
                                            RawPixelFormat format) {
    // One thread everywhere: a threaded decoder and the rawvideo encoder each
    // hold a frame set per thread (thread counts default to the core count),
    // which is most of ffmpeg's memory, and the printer's other cores belong
    // to Klipper. A local file needs no stream probing beyond its header.
    // Never -fflags nobuffer: it ends an MP4 early.
    std::vector<std::string> args = {"ffmpeg",   "-hide_banner",    "-loglevel", "error",
                                     "-nostdin", "-probesize",      "32768",     "-analyzeduration",
                                     "0",        "-filter_threads", "1",         "-threads",
                                     "1"};
    if (start_s > 0.0) {
        char ss[32];
        std::snprintf(ss, sizeof(ss), "%.3f", start_s);
        args.insert(args.end(), {"-ss", ss});
    }
    args.insert(args.end(),
                {"-i", path, "-an", "-sn", "-vf",
                 "scale=" + std::to_string(out.width) + ":" + std::to_string(out.height) +
                     ":flags=fast_bilinear",
                 "-pix_fmt", ffmpeg_pix_fmt_name(format), "-threads", "1", "-f", "rawvideo", "-"});
    return args;
}

double seek_seconds_for(int permille, double duration_s) {
    if (duration_s <= 0.0 || permille <= 0) {
        return 0.0;
    }
    const double t = duration_s * std::min(permille, 1000) / 1000.0;
    // A seek to the very end decodes nothing; stop half a second short.
    return std::max(0.0, std::min(t, duration_s - 0.5));
}

int permille_for(double position_s, double duration_s) {
    if (duration_s <= 0.0 || position_s <= 0.0) {
        return 0;
    }
    return std::min(1000, static_cast<int>(position_s / duration_s * 1000.0 + 0.5));
}

double elapsed_label_seconds(bool scrubbing, int scrub_permille, double presented_s,
                             double duration_s) {
    if (!scrubbing) {
        return presented_s;
    }
    if (duration_s <= 0.0 || scrub_permille <= 0) {
        return 0.0;
    }
    return duration_s * std::min(scrub_permille, 1000) / 1000.0;
}

std::string format_clock(double seconds) {
    const long total = seconds > 0 ? static_cast<long>(seconds) : 0;
    const long h = total / 3600;
    const long m = (total / 60) % 60;
    const long s = total % 60;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%ld:%02ld", m, s);
    }
    return buf;
}

// --- Pacing and presentation ---

PaceDecision pace_frame(int64_t index, double fps, double elapsed_s, double lead_s,
                        double max_late_s) {
    const double due = static_cast<double>(index) / effective_fps(fps);
    const double hand_over = due - lead_s;
    if (elapsed_s < hand_over) {
        return {hand_over - elapsed_s, false};
    }
    return {0.0, elapsed_s - due > max_late_s};
}

int pick_due_frame(const double* due_s, size_t count, double now_s) {
    int pick = -1;
    for (size_t i = 0; i < count && due_s[i] <= now_s; ++i) {
        pick = static_cast<int>(i);
    }
    return pick;
}

uint32_t present_poll_ms(double fps) {
    const double quarter_ms = 1000.0 / effective_fps(fps) / 4.0;
    return static_cast<uint32_t>(std::clamp(quarter_ms, 4.0, 10.0));
}

double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- Assembler ---

FrameAssembler::FrameAssembler(size_t frame_bytes) : frame_bytes_(frame_bytes), buf_(frame_bytes) {}

uint8_t* FrameAssembler::tail() {
    // A reader that swapped the finished frame's storage out may have left a
    // buffer of another size here.
    if (buf_.size() != frame_bytes_) {
        buf_.resize(frame_bytes_);
    }
    if (filled_ == frame_bytes_) {
        filled_ = 0;
    }
    return buf_.data() + filled_;
}

size_t FrameAssembler::want() const {
    return filled_ == frame_bytes_ ? frame_bytes_ : frame_bytes_ - filled_;
}

bool FrameAssembler::commit(size_t n) {
    if (filled_ == frame_bytes_) {
        filled_ = 0;
    }
    filled_ = std::min(filled_ + n, frame_bytes_);
    return filled_ == frame_bytes_;
}

// --- Exchange ---

void FrameExchange::publish(std::vector<uint8_t>& frame, FrameSize size, int64_t index,
                            double due_s, double now_s, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.published;
    if (generation < generation_) {
        // Decoded before a seek the exchange has already moved past.
        ++stats_.discarded;
        return;
    }
    if (queue_.size() >= kSlots) {
        if (queue_.front().due_s <= now_s) {
            ++stats_.replaced_late;
        } else {
            ++stats_.replaced_early;
        }
        recycle_front();
    }
    Slot slot;
    slot.pixels.swap(frame);
    slot.size = size;
    slot.index = index;
    slot.due_s = due_s;
    queue_.push_back(std::move(slot));
    if (!free_.empty()) {
        frame.swap(free_.back());
        free_.pop_back();
    }
}

void FrameExchange::recycle_front() {
    free_.push_back(std::move(queue_.front().pixels));
    queue_.erase(queue_.begin());
}

void FrameExchange::begin_generation(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    stats_.discarded += queue_.size();
    while (!queue_.empty()) {
        recycle_front();
    }
}

std::vector<double> FrameExchange::due_times() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<double> out;
    for (const auto& slot : queue_) {
        out.push_back(slot.due_s);
    }
    return out;
}

size_t FrameExchange::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void FrameExchange::shift_due(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : queue_) {
        slot.due_s += seconds;
    }
}

FrameExchange::Stats FrameExchange::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats out = stats_;
    out.queued = queue_.size();
    return out;
}

void FrameExchange::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.discarded += queue_.size();
    while (!queue_.empty()) {
        recycle_front();
    }
}

} // namespace helix::timelapse
