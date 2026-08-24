// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_capture.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace helix::calibration {

namespace {

const char* kind_label(CaptureBufferKind kind) {
    switch (kind) {
    case CaptureBufferKind::DETECTION_WINDOW:
        return "detection window";
    case CaptureBufferKind::RINGDOWN:
        return "ring-down";
    case CaptureBufferKind::QUIET:
        return "quiet window";
    }
    return "unknown buffer";
}

const char* kind_description(CaptureBufferKind kind) {
    switch (kind) {
    case CaptureBufferKind::DETECTION_WINDOW:
        return "ADXL345 on toolhead, steppers energized, live window (gate + onset evidence)";
    case CaptureBufferKind::RINGDOWN:
        return "ADXL345 on toolhead, steppers energized, ring-down window only";
    case CaptureBufferKind::QUIET:
        return "ADXL345 on toolhead, steppers energized, no strike present";
    }
    return "";
}

const char* kind_suffix(CaptureBufferKind kind) {
    switch (kind) {
    case CaptureBufferKind::DETECTION_WINDOW:
        return "detection";
    case CaptureBufferKind::RINGDOWN:
        return "ringdown";
    case CaptureBufferKind::QUIET:
        return "quiet";
    }
    return "unknown";
}

/// "12.34" or "n/a" - used for every optional verdict field so a value that
/// was never evaluated on this event's path reads as absent, not as zero.
std::string fmt_optional(const std::optional<float>& v, const char* fmt) {
    if (!v.has_value()) {
        return "n/a";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, static_cast<double>(*v));
    return buf;
}

} // namespace

const char* CaptureVerdict::verdict_string() const {
    if (accepted) {
        return "ACCEPTED";
    }
    switch (reject) {
    case PluckReject::TOO_SOFT:
        return "TOO_SOFT";
    case PluckReject::NOT_A_PLUCK:
        return "NOT_A_PLUCK";
    case PluckReject::NONE:
        break;
    }
    // Reachable only if a caller builds a CaptureVerdict with accepted=false
    // and reject=NONE, which push() never does - every rejection it returns
    // carries a real reject reason.
    return "UNKNOWN";
}

std::string render_capture(const std::vector<AccelSample>& samples, float sample_rate_hz,
                           CaptureBufferKind kind, const CaptureVerdict& verdict, float span_mm) {
    std::ostringstream out;

    out << "# HelixScreen belt pluck capture - " << kind_label(kind) << ", "
        << static_cast<int>(span_mm) << "mm span\n";
    out << "# " << kind_description(kind) << "\n";

    char rate_buf[32];
    std::snprintf(rate_buf, sizeof(rate_buf), "%.1f", static_cast<double>(sample_rate_hz));

    if (kind == CaptureBufferKind::QUIET) {
        out << "# sample_rate_hz=" << rate_buf << "\n";
    } else {
        char ratio_buf[32];
        std::snprintf(ratio_buf, sizeof(ratio_buf), "%.2f", static_cast<double>(verdict.rms_ratio));
        char estimate_buf[32];
        std::snprintf(estimate_buf, sizeof(estimate_buf), "%.1f",
                      static_cast<double>(verdict.estimate_hz));
        char median_buf[32];
        std::snprintf(median_buf, sizeof(median_buf), "%.1f",
                      static_cast<double>(verdict.median_hz));

        out << "# sample_rate_hz=" << rate_buf << " rms_over_noise_floor=" << ratio_buf
            << " verdict=" << verdict.verdict_string()
            << " onset_rise=" << fmt_optional(verdict.onset_rise, "%.2f")
            << " decay_end_ratio=" << fmt_optional(verdict.decay_end_ratio, "%.3f")
            << " harmonic_concentration=" << fmt_optional(verdict.harmonic_concentration, "%.3f")
            << " estimate_hz=" << estimate_buf << " median_hz=" << median_buf << "\n";
    }

    out << "#time,accel_x,accel_y,accel_z\n";

    char row[128];
    for (const auto& s : samples) {
        std::snprintf(row, sizeof(row), "%.6f,%.4f,%.4f,%.4f\n", static_cast<double>(s.time),
                      static_cast<double>(s.x), static_cast<double>(s.y), static_cast<double>(s.z));
        out << row;
    }

    return out.str();
}

float parse_capture_sample_rate(const std::string& csv_text) {
    const std::string key = "sample_rate_hz=";
    const size_t at = csv_text.find(key);
    if (at == std::string::npos) {
        return 0.0f;
    }
    try {
        return std::stof(csv_text.substr(at + key.size()));
    } catch (const std::exception&) {
        return 0.0f;
    }
}

std::string belt_capture_dir() {
    const char* dir = std::getenv("HELIX_BELT_CAPTURE_DIR");
    return dir ? std::string(dir) : std::string();
}

BeltCaptureWriter::BeltCaptureWriter(std::string dir, float span_mm)
    : dir_(std::move(dir)), span_mm_(span_mm) {
    if (dir_.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        spdlog::warn("[BeltCapture] Failed to create capture directory {}: {}", dir_, ec.message());
    } else {
        spdlog::info("[BeltCapture] Writing pluck captures to {}", dir_);
    }
}

void BeltCaptureWriter::write_file(const std::string& stem, const std::vector<AccelSample>& samples,
                                   float sample_rate_hz, CaptureBufferKind kind,
                                   const CaptureVerdict& verdict) {
    const std::filesystem::path path =
        std::filesystem::path(dir_) / (stem + "_" + kind_suffix(kind) + ".csv");

    std::ofstream file(path);
    if (!file) {
        spdlog::warn("[BeltCapture] Failed to open {} for writing", path.string());
        return;
    }
    file << render_capture(samples, sample_rate_hz, kind, verdict, span_mm_);
    if (!file) {
        spdlog::warn("[BeltCapture] Failed writing {}", path.string());
    }
}

void BeltCaptureWriter::write_event(const std::vector<AccelSample>& detection_window,
                                    const std::vector<AccelSample>* ringdown, float sample_rate_hz,
                                    const CaptureVerdict& verdict) {
    if (!enabled()) {
        return;
    }

    char stem_buf[32];
    std::snprintf(stem_buf, sizeof(stem_buf), "event_%04d_%s", sequence_++,
                  verdict.verdict_string());
    const std::string stem = stem_buf;

    write_file(stem, detection_window, sample_rate_hz, CaptureBufferKind::DETECTION_WINDOW,
               verdict);
    if (ringdown != nullptr) {
        write_file(stem, *ringdown, sample_rate_hz, CaptureBufferKind::RINGDOWN, verdict);
    }
}

void BeltCaptureWriter::write_quiet(const std::vector<AccelSample>& quiet, float sample_rate_hz) {
    if (!enabled()) {
        return;
    }

    char stem_buf[32];
    std::snprintf(stem_buf, sizeof(stem_buf), "quiet_%04d", sequence_++);
    write_file(stem_buf, quiet, sample_rate_hz, CaptureBufferKind::QUIET, CaptureVerdict{});
}

} // namespace helix::calibration
