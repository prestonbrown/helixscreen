// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_dsp_probe.h"

#include "belt_tension_types.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <vector>

namespace helix::calibration {

bool dsp_ms_is_capable(double psd_ms) {
    return psd_ms > 0.0 && psd_ms < MAX_PSD_MS;
}

DspProbeResult probe_dsp_throughput() {
    std::vector<AccelSample> samples;
    samples.reserve(PROBE_WINDOW_SAMPLES);
    for (int i = 0; i < PROBE_WINDOW_SAMPLES; ++i) {
        const float t = static_cast<float>(i) / PROBE_SAMPLE_RATE_HZ;
        const float decay = std::exp(-6.0f * t);
        const float v = decay * (1000.0f * std::sin(2.0f * 3.14159265f * 86.0f * t) +
                                 400.0f * std::sin(2.0f * 3.14159265f * 172.0f * t));
        samples.push_back(AccelSample{t, v, v * 0.5f, v * 0.25f});
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto psd = compute_psd(samples, PROBE_SAMPLE_RATE_HZ, PROBE_BANDWIDTH_HZ);
    const auto t1 = std::chrono::steady_clock::now();

    DspProbeResult r;
    r.psd_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.capable = dsp_ms_is_capable(r.psd_ms);

    // Keep the compiler from eliding the transform.
    if (psd.empty()) {
        spdlog::debug("[BeltDsp] probe produced an empty spectrum");
    }
    spdlog::info("[BeltDsp] {}-point PSD at {} Hz bandwidth took {:.1f} ms - {}",
                 PROBE_WINDOW_SAMPLES, PROBE_BANDWIDTH_HZ, r.psd_ms,
                 r.capable ? "capable" : "too slow for the live meter");
    return r;
}

const DspProbeResult& cached_dsp_probe() {
    static const DspProbeResult result = probe_dsp_throughput();
    return result;
}

} // namespace helix::calibration
