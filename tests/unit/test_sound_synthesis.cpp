// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sound_synthesis.h"

#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::audio;
using Catch::Approx;

static constexpr int kSampleRate = 44100;
static constexpr int kSamples10ms = 441;
static constexpr float PI = 3.14159265358979323846f;

static float compute_rms(const float* buffer, int num_samples) {
    float sum_sq = 0;
    for (int i = 0; i < num_samples; i++) {
        sum_sq += buffer[i] * buffer[i];
    }
    return std::sqrt(sum_sq / num_samples);
}

static float compute_energy_band(const float* buffer, int num_samples) {
    float energy = 0;
    for (int i = 0; i < num_samples; i++) {
        energy += buffer[i] * buffer[i];
    }
    return energy;
}

// ============================================================================
// Square wave frequency
// ============================================================================

TEST_CASE("Square wave 1kHz frequency verified by zero crossings", "[sound][synthesis]") {
    // 1kHz at 44100 Hz sample rate = 44.1 samples/period
    // In 441 samples (10ms) we expect ~10 periods = ~20 zero crossings (positive edge only)
    constexpr float freq = 1000.0f;
    constexpr int num_samples = kSampleRate; // 1 full second for accuracy
    std::vector<float> buffer(num_samples);
    float phase = 0;

    generate_samples(buffer.data(), num_samples, kSampleRate, Waveform::SQUARE, freq, 1.0f, 0.5f,
                     phase);

    // Count rising zero crossings (negative -> positive transitions)
    int crossings = 0;
    for (int i = 1; i < num_samples; i++) {
        if (buffer[i - 1] < 0 && buffer[i] > 0) {
            crossings++;
        }
    }

    // At 1kHz over 1 second, expect ~1000 rising edges
    // Allow 2% tolerance
    REQUIRE(crossings >= 980);
    REQUIRE(crossings <= 1020);
}

// ============================================================================
// Square wave duty cycle
// ============================================================================

TEST_CASE("Square wave duty cycle 0.25 produces 25% time above zero", "[sound][synthesis]") {
    constexpr int num_samples = kSampleRate; // 1 second for stable stats
    std::vector<float> buffer(num_samples);
    float phase = 0;

    generate_samples(buffer.data(), num_samples, kSampleRate, Waveform::SQUARE, 440.0f, 1.0f, 0.25f,
                     phase);

    int positive_count = 0;
    for (int i = 0; i < num_samples; i++) {
        if (buffer[i] > 0)
            positive_count++;
    }

    float ratio = static_cast<float>(positive_count) / num_samples;
    // 25% duty = ~25% positive; allow +-5% for edge quantization
    REQUIRE(ratio > 0.20f);
    REQUIRE(ratio < 0.30f);
}

// ============================================================================
// Sine wave RMS
// ============================================================================

TEST_CASE("Synthesized sine wave RMS is amplitude / sqrt(2)", "[sound][synthesis]") {
    constexpr int num_samples = kSampleRate; // 1 full second = many complete periods
    std::vector<float> buffer(num_samples);
    float phase = 0;

    generate_samples(buffer.data(), num_samples, kSampleRate, Waveform::SINE, 440.0f, 1.0f, 0.5f,
                     phase);

    float rms = compute_rms(buffer.data(), num_samples);
    float expected_rms = 1.0f / std::sqrt(2.0f); // ~0.7071
    REQUIRE(rms == Approx(expected_rms).margin(0.01f));
}

// ============================================================================
// Triangle wave symmetry
// ============================================================================

TEST_CASE("Triangle wave peak positive and negative equal amplitude", "[sound][synthesis]") {
    constexpr float amplitude = 0.9f;
    constexpr float freq = 100.0f;
    constexpr int num_samples = 882; // 2 full periods at 100Hz/44100
    std::vector<float> buffer(num_samples);
    float phase = 0;

    generate_samples(buffer.data(), num_samples, kSampleRate, Waveform::TRIANGLE, freq, amplitude,
                     0.5f, phase);

    float max_val = *std::max_element(buffer.begin(), buffer.end());
    float min_val = *std::min_element(buffer.begin(), buffer.end());

    // Peak positive should be close to +amplitude
    REQUIRE(max_val > amplitude * 0.9f);
    REQUIRE(max_val <= amplitude + 0.001f);

    // Peak negative should be close to -amplitude
    REQUIRE(min_val < -amplitude * 0.9f);
    REQUIRE(min_val >= -amplitude - 0.001f);
}

// ============================================================================
// Saw wave range
// ============================================================================

TEST_CASE("Saw wave output stays within amplitude bounds", "[sound][synthesis]") {
    constexpr float amplitude = 0.8f;
    constexpr float freq = 100.0f;
    constexpr int num_samples = 882; // 2 full periods
    std::vector<float> buffer(num_samples);
    float phase = 0;

    generate_samples(buffer.data(), num_samples, kSampleRate, Waveform::SAW, freq, amplitude, 0.5f,
                     phase);

    float max_val = *std::max_element(buffer.begin(), buffer.end());
    float min_val = *std::min_element(buffer.begin(), buffer.end());

    REQUIRE(max_val <= amplitude + 0.001f);
    REQUIRE(min_val >= -amplitude - 0.001f);

    // Should span nearly the full range
    REQUIRE(max_val > amplitude * 0.8f);
    REQUIRE(min_val < -amplitude * 0.8f);
}

// ============================================================================
// Phase continuity
// ============================================================================

TEST_CASE("Phase continuity: two consecutive calls produce no discontinuity",
          "[sound][synthesis]") {
    constexpr int half = 100;
    std::vector<float> buf1(half);
    std::vector<float> buf2(half);
    float phase = 0;

    generate_samples(buf1.data(), half, kSampleRate, Waveform::SINE, 440.0f, 1.0f, 0.5f, phase);
    generate_samples(buf2.data(), half, kSampleRate, Waveform::SINE, 440.0f, 1.0f, 0.5f, phase);

    // The last sample of buf1 and first sample of buf2 should be continuous.
    // For a 440Hz sine at 44100 sample rate, adjacent samples differ by at most
    // 2*pi*440/44100 ~ 0.0627 in phase. The value difference is amplitude*sin(delta_phase).
    float diff = std::fabs(buf2[0] - buf1[half - 1]);
    float max_expected_diff = 2.0f * PI * 440.0f / kSampleRate + 0.01f;
    REQUIRE(diff < max_expected_diff);
}

TEST_CASE("Phase continuity: split calls match single call output", "[sound][synthesis]") {
    constexpr int total = kSamples10ms * 2;
    constexpr int half = kSamples10ms;

    std::vector<float> full_buf(total);
    std::vector<float> half1(half);
    std::vector<float> half2(half);

    float phase_full = 0;
    generate_samples(full_buf.data(), total, kSampleRate, Waveform::SINE, 440.0f, 1.0f, 0.5f,
                     phase_full);

    float phase_split = 0;
    generate_samples(half1.data(), half, kSampleRate, Waveform::SINE, 440.0f, 1.0f, 0.5f,
                     phase_split);
    generate_samples(half2.data(), half, kSampleRate, Waveform::SINE, 440.0f, 1.0f, 0.5f,
                     phase_split);

    for (int i = 0; i < half; i++) {
        REQUIRE(half1[i] == Approx(full_buf[i]).margin(0.0001f));
        REQUIRE(half2[i] == Approx(full_buf[half + i]).margin(0.0001f));
    }
}

// ============================================================================
// Silence at zero amplitude
// ============================================================================

TEST_CASE("Zero amplitude produces all-zero output", "[sound][synthesis]") {
    std::vector<float> buffer(kSamples10ms);
    float phase = 0;

    generate_samples(buffer.data(), kSamples10ms, kSampleRate, Waveform::SINE, 440.0f, 0.0f, 0.5f,
                     phase);

    for (int i = 0; i < kSamples10ms; i++) {
        REQUIRE(buffer[i] == 0.0f);
    }
}

// ============================================================================
// FilterType from string
// ============================================================================

TEST_CASE("filter_type_from_string converts known strings correctly", "[sound][synthesis]") {
    REQUIRE(filter_type_from_string("lowpass") == FilterType::LOWPASS);
    REQUIRE(filter_type_from_string("highpass") == FilterType::HIGHPASS);
}

TEST_CASE("filter_type_from_string returns NONE for empty string", "[sound][synthesis]") {
    REQUIRE(filter_type_from_string("") == FilterType::NONE);
}

TEST_CASE("filter_type_from_string returns LOWPASS as default for unknown string",
          "[sound][synthesis]") {
    // Unknown strings default to LOWPASS (based on the described default behavior)
    FilterType result = filter_type_from_string("unknown_type");
    // The default for an unrecognized string should be LOWPASS per the spec
    REQUIRE(result == FilterType::LOWPASS);
}

// ============================================================================
// Lowpass filter attenuates high frequencies
// ============================================================================

TEST_CASE("Lowpass filter reduces high-frequency energy in mixed signal", "[sound][synthesis]") {
    // Build a mixed signal: low freq (200Hz) + high freq (8kHz)
    constexpr int num_samples = 4410; // 100ms
    std::vector<float> buffer(num_samples);
    float phase_lo = 0, phase_hi = 0;

    // Generate high-freq component
    std::vector<float> hi_buf(num_samples);
    generate_samples(hi_buf.data(), num_samples, kSampleRate, Waveform::SINE, 8000.0f, 1.0f, 0.5f,
                     phase_hi);

    // Add both into buffer (low freq dominates at low freq region)
    generate_samples(buffer.data(), num_samples, kSampleRate, Waveform::SINE, 200.0f, 1.0f, 0.5f,
                     phase_lo);
    for (int i = 0; i < num_samples; i++) {
        buffer[i] += hi_buf[i];
    }

    float energy_before = compute_energy_band(hi_buf.data(), num_samples);

    // Apply lowpass at 1kHz — should heavily attenuate the 8kHz component
    BiquadFilter filter{};
    compute_biquad_coeffs(filter, FilterType::LOWPASS, 1000.0f, kSampleRate);
    apply_filter(filter, buffer.data(), num_samples);

    // Apply same filter to the hi_buf alone to measure its attenuation
    BiquadFilter filter2{};
    compute_biquad_coeffs(filter2, FilterType::LOWPASS, 1000.0f, kSampleRate);
    apply_filter(filter2, hi_buf.data(), num_samples);

    float energy_after = compute_energy_band(hi_buf.data(), num_samples);

    // High-frequency energy should be heavily reduced (less than 10% remains)
    REQUIRE(energy_after < energy_before * 0.1f);
}

// ============================================================================
// Highpass filter attenuates low frequencies
// ============================================================================

TEST_CASE("Highpass filter reduces low-frequency energy in mixed signal", "[sound][synthesis]") {
    constexpr int num_samples = 4410; // 100ms
    std::vector<float> lo_buf(num_samples);
    float phase_lo = 0;

    // Generate low-freq component (100Hz)
    generate_samples(lo_buf.data(), num_samples, kSampleRate, Waveform::SINE, 100.0f, 1.0f, 0.5f,
                     phase_lo);

    float energy_before = compute_energy_band(lo_buf.data(), num_samples);

    // Apply highpass at 1kHz — should heavily attenuate the 100Hz component
    BiquadFilter filter{};
    compute_biquad_coeffs(filter, FilterType::HIGHPASS, 1000.0f, kSampleRate);
    apply_filter(filter, lo_buf.data(), num_samples);

    float energy_after = compute_energy_band(lo_buf.data(), num_samples);

    // Low-frequency energy should be heavily reduced (less than 10% remains)
    REQUIRE(energy_after < energy_before * 0.1f);
}

// ============================================================================
// update_filter_if_needed: no recompute on same params
// ============================================================================

TEST_CASE("update_filter_if_needed does not recompute when params are unchanged",
          "[sound][synthesis]") {
    BiquadFilter filter{};
    update_filter_if_needed(filter, FilterType::LOWPASS, 1000.0f, kSampleRate);

    // Record current cutoff after first call
    float cutoff_after_first = filter.current_cutoff;
    REQUIRE(cutoff_after_first == Approx(1000.0f));

    // Manually change a coefficient to detect if recompute happens
    float b0_after_first = filter.b0;

    // Overwrite b0 with a sentinel value
    filter.b0 = 99999.0f;

    // Call again with same params — should NOT recompute (b0 should stay as sentinel)
    update_filter_if_needed(filter, FilterType::LOWPASS, 1000.0f, kSampleRate);

    REQUIRE(filter.b0 == Approx(99999.0f)); // Not recomputed
    REQUIRE(filter.current_cutoff == Approx(1000.0f));

    // Now call with different cutoff — should recompute (b0 should change)
    update_filter_if_needed(filter, FilterType::LOWPASS, 500.0f, kSampleRate);

    REQUIRE(filter.b0 != Approx(99999.0f)); // Recomputed
    REQUIRE(filter.current_cutoff == Approx(500.0f));

    (void)b0_after_first; // silence unused warning
}

// ============================================================================
// All waveforms produce non-zero output
// ============================================================================

TEST_CASE("All four waveforms produce non-zero output at non-zero amplitude",
          "[sound][synthesis]") {
    Waveform waves[] = {Waveform::SQUARE, Waveform::SAW, Waveform::TRIANGLE, Waveform::SINE};
    const char* names[] = {"SQUARE", "SAW", "TRIANGLE", "SINE"};

    for (int w = 0; w < 4; w++) {
        std::vector<float> buffer(kSamples10ms);
        float phase = 0;

        generate_samples(buffer.data(), kSamples10ms, kSampleRate, waves[w], 440.0f, 1.0f, 0.5f,
                         phase);

        float rms = compute_rms(buffer.data(), kSamples10ms);
        INFO("Waveform: " << names[w] << " RMS=" << rms);
        REQUIRE(rms > 0.1f);
    }
}
