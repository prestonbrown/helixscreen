// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include "cloexec_pipe.h"
#include "core/text_io.h"
#include "host_identity.h"
#include "hv/hurl.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace helix::timelapse {

std::string find_companion_thumbnail(const std::string& video_filename,
                                     const std::set<std::string>& listing) {
    const auto dot = video_filename.rfind('.');
    const std::string stem =
        dot == std::string::npos ? video_filename : video_filename.substr(0, dot);
    for (const char* suffix : {".jpg", ".thumb.jpg"}) {
        const std::string candidate = stem + suffix;
        if (listing.count(candidate) != 0) {
            return candidate;
        }
    }
    return {};
}

std::vector<std::string> ffmpeg_lean_input_args() {
    // Never -fflags nobuffer: it ends an MP4 early.
    return {"-probesize",      "32768", "-analyzeduration", "0",
            "-filter_threads", "1",     "-threads",         "1"};
}

std::vector<std::string> ffmpeg_lean_output_args() {
    return {"-threads", "1"};
}

std::vector<std::string> ffmpeg_extract_args(const std::string& input_path,
                                             const std::string& output_path, ExtractFrame frame) {
    std::vector<std::string> args = {"ffmpeg", "-y"};
    const auto lean_in = ffmpeg_lean_input_args();
    args.insert(args.end(), lean_in.begin(), lean_in.end());
    if (frame == ExtractFrame::Last) {
        // Before -i, a seek from the end jumps to a keyframe instead of decoding there.
        args.insert(args.end(), {"-sseof", "-1"});
    }
    // 480px wide: a full-resolution frame would cost every pre-scale a
    // multi-megabyte decode on a worker.
    args.insert(args.end(),
                {"-i", input_path, "-vframes", "1", "-q:v", "3", "-vf", "scale=480:-2"});
    const auto lean_out = ffmpeg_lean_output_args();
    args.insert(args.end(), lean_out.begin(), lean_out.end());
    args.push_back(output_path);
    return args;
}

std::string extracted_thumbnail_key(const std::string& filename, uint64_t size, double modified) {
    return filename + "@frame-" + std::to_string(size) + "-" +
           std::to_string(static_cast<long long>(modified));
}

bool should_extract_thumbnail(const ThumbnailExtractInputs& in) {
    return !in.has_companion && in.ffmpeg_available && in.moonraker_local && !in.print_active &&
           !in.already_failed;
}

bool is_video_file(const std::string& filename) {
    if (filename.size() < 5)
        return false;
    if (filename.find(".thumb.") != std::string::npos)
        return false;

    auto dot = filename.rfind('.');
    if (dot == std::string::npos)
        return false;
    auto ext = filename.substr(dot);
    ext = helix::text_io::to_lower(ext);
    return ext == ".mp4" || ext == ".mkv" || ext == ".avi";
}

std::string phone_video_url(const std::string& http_base_url, const std::string& filename,
                            const std::string& lan_ipv4) {
    const std::string host = helix::extract_host_from_url(http_base_url);
    if (host.empty()) {
        return {};
    }
    std::string base = http_base_url;
    if (helix::is_loopback_host(host)) {
        if (lan_ipv4.empty()) {
            return {};
        }
        const auto authority = base.find("://") + 3;
        const bool bracketed = base.compare(authority, 1, "[") == 0;
        const auto span = host.size() + (bracketed ? 2 : 0);
        base.replace(authority, span, lan_ipv4);
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/server/files/timelapse/" + HUrl::escape(filename, "/.-_");
}

} // namespace helix::timelapse
