// SPDX-License-Identifier: GPL-3.0-or-later
#include "ripple_batch_source_mock.h"

#include "belt_stream_client.h"
#include "core/text_io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace helix::calibration::ripple {

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr const char* kStepMarker = "G4 P250";
constexpr const char* kEnable = "STEPPER_RESONANCE_DAMP_ENABLE";
constexpr const char* kDisable = "STEPPER_RESONANCE_DAMP_DISABLE";

std::mutex g_mutex;
std::vector<MockRippleBatchSource*> g_sources;
bool g_compensated = false;

struct Point {
    float x = 0.0f;
    float y = 0.0f;
    float f = 0.0f;
};

/// The first two G1 moves of a step script: the approach and the first pass.
bool first_two_moves(const std::string& script, Point& a, Point& b) {
    int found = 0;
    for (const auto line : helix::text_io::lines(script)) {
        Point p;
        const std::string s(line);
        if (std::sscanf(s.c_str(), "G1 X%f Y%f F%f", &p.x, &p.y, &p.f) == 3) {
            (found == 0 ? a : b) = p;
            if (++found == 2) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

MockRippleBatchSource::MockRippleBatchSource(float sample_rate_hz) : rate_(sample_rate_hz) {}

MockRippleBatchSource::~MockRippleBatchSource() {
    stop();
}

bool MockRippleBatchSource::start(const std::string&, const std::string&, BatchCallback on_batch,
                                  ErrorCallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    on_batch_ = std::move(on_batch);
    if (std::find(g_sources.begin(), g_sources.end(), this) == g_sources.end()) {
        g_sources.push_back(this);
    }
    return true;
}

void MockRippleBatchSource::stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sources.erase(std::remove(g_sources.begin(), g_sources.end(), this), g_sources.end());
}

float MockRippleBatchSource::ripple_amplitude(float belt_speed, int k, bool compensated) {
    const float f = static_cast<float>(k) * belt_speed / kMockRippleElectricalMm;
    const float half_width = kMockRippleResonanceHz / (2.0f * kMockRippleQ);
    const float d = (f - kMockRippleResonanceHz) / half_width;
    const float amp = kMockRipplePeakMmS2 / (1.0f + d * d) + kMockRippleFloorMmS2;
    return compensated ? amp * kMockRippleCompensated : amp;
}

void MockRippleBatchSource::emit(float belt_speed, bool compensated) {
    AccelBatch batch;
    const int n = static_cast<int>(kMockRippleCaptureS * rate_);
    batch.samples.reserve(static_cast<size_t>(n));
    std::array<float, 3> amp{};
    for (size_t i = 0; i < kHarmonics.size(); ++i) {
        amp[i] = ripple_amplitude(belt_speed, kHarmonics[i], compensated);
    }
    for (int i = 0; i < n; ++i) {
        const float t = t_ + static_cast<float>(i) / rate_;
        float x = 0.0f;
        for (size_t h = 0; h < kHarmonics.size(); ++h) {
            const float f =
                static_cast<float>(kHarmonics[h]) * belt_speed / kMockRippleElectricalMm;
            x += amp[h] * std::sin(kTwoPi * f * t);
        }
        seed_ = seed_ * 1664525u + 1013904223u;
        const float hiss = (static_cast<float>(seed_ >> 8) / 16777216.0f - 0.5f) * 20.0f;
        batch.samples.push_back({t, x + hiss, 0.7f * x, 9810.0f + hiss});
    }
    t_ += kMockRippleCaptureS + 0.5f;
    if (on_batch_) {
        on_batch_(batch);
    }
}

void mock_ripple_on_script(const std::string& script) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto on = script.rfind(kEnable);
    const auto off = script.rfind(kDisable);
    if (on != std::string::npos || off != std::string::npos) {
        g_compensated = on != std::string::npos && (off == std::string::npos || on > off);
    }
    if (g_sources.empty() || script.find(kStepMarker) == std::string::npos) {
        return;
    }
    Point a;
    Point b;
    if (!first_two_moves(script, a, b) || !(b.f > 0.0f)) {
        return;
    }
    const bool diagonal = std::fabs(b.x - a.x) > 0.1f && std::fabs(b.y - a.y) > 0.1f;
    const float belt_speed = b.f / 60.0f * (diagonal ? std::sqrt(2.0f) : 1.0f);
    for (MockRippleBatchSource* s : g_sources) {
        s->emit(belt_speed, g_compensated);
    }
}

float mock_ripple_sample_rate() {
    if (const char* v = std::getenv("HELIX_MOCK_ACCEL_RATE")) {
        if (const auto r = helix::text_io::parse_leading<float>(v); r && *r > 0.0f) {
            return *r;
        }
    }
    return 3200.0f;
}

} // namespace helix::calibration::ripple
