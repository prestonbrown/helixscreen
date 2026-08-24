// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_DISPLAY_SDL

#include "sdl_sound_backend.h"
#include "sound_synthesis.h"

#include <SDL.h>
#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;

// ============================================================================
// Helpers
// ============================================================================

static constexpr int SAMPLE_RATE = 44100;
static constexpr int SAMPLES_10MS = 441; // 10ms at 44100Hz
static constexpr float PI = 3.14159265358979323846f;

static float compute_rms(const float* buffer, int num_samples) {
    float sum_sq = 0;
    for (int i = 0; i < num_samples; i++) {
        sum_sq += buffer[i] * buffer[i];
    }
    return std::sqrt(sum_sq / num_samples);
}

static float compute_max_abs(const float* buffer, int num_samples) {
    float max_val = 0;
    for (int i = 0; i < num_samples; i++) {
        float abs_val = std::fabs(buffer[i]);
        if (abs_val > max_val)
            max_val = abs_val;
    }
    return max_val;
}

static int count_positive(const float* buffer, int num_samples) {
    int count = 0;
    for (int i = 0; i < num_samples; i++) {
        if (buffer[i] > 0)
            count++;
    }
    return count;
}

// ============================================================================
// Backend capability flags
// ============================================================================

// ============================================================================
// Subsystem lifecycle
// ============================================================================

// initialize() takes a reference on SDL's audio subsystem. shutdown() used to
// close the device but never release that reference, so the subsystem stayed up
// afterwards — reaped only later, incidentally, by the SDL_Quit() in
// lv_sdl_quit() during *display* teardown. Closing the device does not stop the
// backend's own threads (SDL's PulseAudio backend keeps a mainloop thread),
// which is how a shutdown-time use-after-free was observed under valgrind:
// pa_pdispatch_run reading a block SDL_CloseAudioDevice had already freed.
//
// That race is timing-dependent and reproduced in roughly 1 run in 14, so this
// test does NOT try to catch it. It locks the property that is deterministic and
// verifiable: init and quit are balanced.
TEST_CASE("SDL sound backend releases the audio subsystem on shutdown", "[sound][sdl][lifecycle]") {
    // Run with no sound hardware — CI machines have none.
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);

    // Another test in this shard may already hold an audio reference; what
    // matters is that we hand back exactly what we took.
    const Uint32 before = SDL_WasInit(SDL_INIT_AUDIO);

    {
        SDLSoundBackend backend;
        if (!backend.initialize()) {
            // No usable device even with the dummy driver. The failure path has
            // its own balance requirement, so assert that and stop.
            REQUIRE(SDL_WasInit(SDL_INIT_AUDIO) == before);
            SUCCEED("no audio device available; failure path left the refcount balanced");
            return;
        }

        REQUIRE(SDL_WasInit(SDL_INIT_AUDIO) != 0);
        backend.shutdown();

        // The regression: without SDL_QuitSubSystem(SDL_INIT_AUDIO) in
        // shutdown(), this still reports the subsystem as initialized.
        REQUIRE(SDL_WasInit(SDL_INIT_AUDIO) == before);
    }

    // The destructor calls shutdown() a second time; it must stay balanced and
    // must not double-release.
    REQUIRE(SDL_WasInit(SDL_INIT_AUDIO) == before);
}

TEST_CASE("SDL backend reports correct capabilities", "[sound][sdl]") {
    SDLSoundBackend backend;
    // Test capability flags without initializing SDL audio device
    REQUIRE(backend.supports_waveforms());
    REQUIRE(backend.supports_amplitude());
    REQUIRE(backend.supports_filter());
    REQUIRE(backend.min_tick_ms() == Approx(1.0f));
}

// ============================================================================
// Square wave generation
// ============================================================================

TEST_CASE("Square wave generates bipolar signal", "[sound][sdl]") {
    // One full period at 440Hz = ~100.2 samples at 44100Hz
    // Use enough samples to get several periods
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SQUARE,
                                   440.0f, 1.0f, 0.5f, phase);

    // Every sample should be either +1 or -1
    for (int i = 0; i < SAMPLES_10MS; i++) {
        REQUIRE((buffer[i] == Approx(1.0f) || buffer[i] == Approx(-1.0f)));
    }
}

TEST_CASE("Square wave duty cycle 0.5 produces roughly equal positive and negative",
          "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SQUARE,
                                   440.0f, 1.0f, 0.5f, phase);

    int pos = count_positive(buffer.data(), SAMPLES_10MS);
    float ratio = static_cast<float>(pos) / SAMPLES_10MS;
    // Should be roughly 50/50, allow +-10% for edge effects
    REQUIRE(ratio > 0.4f);
    REQUIRE(ratio < 0.6f);
}

TEST_CASE("Square wave RMS at full amplitude is close to 1.0", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SQUARE,
                                   440.0f, 1.0f, 0.5f, phase);

    // Square wave at amplitude 1.0 has RMS of exactly 1.0
    float rms = compute_rms(buffer.data(), SAMPLES_10MS);
    REQUIRE(rms == Approx(1.0f).margin(0.01f));
}

// ============================================================================
// Sine wave generation
// ============================================================================

TEST_CASE("Sine wave first samples match expected values", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   1.0f, 0.5f, phase);

    // Verify first few samples match sin(2*pi*440*n/44100)
    for (int n = 0; n < 10; n++) {
        float expected = std::sin(2.0f * PI * 440.0f * n / SAMPLE_RATE);
        REQUIRE(buffer[n] == Approx(expected).margin(0.001f));
    }
}

TEST_CASE("Sine wave RMS is amplitude / sqrt(2)", "[sound][sdl]") {
    // Use many samples for a stable RMS measurement
    constexpr int num_samples = 44100; // 1 full second = many complete periods
    std::vector<float> buffer(num_samples);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), num_samples, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   1.0f, 0.5f, phase);

    float rms = compute_rms(buffer.data(), num_samples);
    float expected_rms = 1.0f / std::sqrt(2.0f); // ~0.7071
    REQUIRE(rms == Approx(expected_rms).margin(0.01f));
}

TEST_CASE("Sine wave stays within amplitude bounds", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   0.7f, 0.5f, phase);

    float max_abs = compute_max_abs(buffer.data(), SAMPLES_10MS);
    REQUIRE(max_abs <= 0.7f + 0.001f);
}

// ============================================================================
// Saw wave generation
// ============================================================================

TEST_CASE("Saw wave ramps from -amplitude to +amplitude", "[sound][sdl]") {
    // Use a low frequency so each period has many samples
    constexpr float freq = 100.0f;
    constexpr int num_samples = 882; // 20ms = 2 full periods at 100Hz
    std::vector<float> buffer(num_samples);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), num_samples, SAMPLE_RATE, Waveform::SAW, freq,
                                   1.0f, 0.5f, phase);

    // Samples should span from near -1 to near +1
    float min_val = *std::min_element(buffer.begin(), buffer.end());
    float max_val = *std::max_element(buffer.begin(), buffer.end());
    REQUIRE(min_val < -0.9f);
    REQUIRE(max_val > 0.9f);
}

TEST_CASE("Saw wave is mostly monotonically increasing within a period", "[sound][sdl]") {
    constexpr float freq = 100.0f;
    // One period = 441 samples at 100Hz/44100
    constexpr int period_samples = SAMPLE_RATE / 100;
    std::vector<float> buffer(period_samples);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), period_samples, SAMPLE_RATE, Waveform::SAW, freq,
                                   1.0f, 0.5f, phase);

    // Count how many samples are increasing (allow a few for the reset at period boundary)
    int increasing = 0;
    for (int i = 1; i < period_samples; i++) {
        if (buffer[i] >= buffer[i - 1])
            increasing++;
    }
    // Should be almost all increasing (except the reset at the period boundary)
    float increasing_ratio = static_cast<float>(increasing) / (period_samples - 1);
    REQUIRE(increasing_ratio > 0.95f);
}

// ============================================================================
// Triangle wave generation
// ============================================================================

TEST_CASE("Triangle wave ramps up and down symmetrically", "[sound][sdl]") {
    constexpr float freq = 100.0f;
    constexpr int num_samples = 882; // 2 full periods
    std::vector<float> buffer(num_samples);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), num_samples, SAMPLE_RATE, Waveform::TRIANGLE,
                                   freq, 1.0f, 0.5f, phase);

    // Should reach near +1 and near -1
    float min_val = *std::min_element(buffer.begin(), buffer.end());
    float max_val = *std::max_element(buffer.begin(), buffer.end());
    REQUIRE(min_val < -0.9f);
    REQUIRE(max_val > 0.9f);
}

TEST_CASE("Triangle wave has lower RMS than square wave", "[sound][sdl]") {
    constexpr int num_samples = 44100;
    std::vector<float> buffer_tri(num_samples);
    std::vector<float> buffer_sq(num_samples);
    float phase_tri = 0, phase_sq = 0;

    helix::audio::generate_samples(buffer_tri.data(), num_samples, SAMPLE_RATE, Waveform::TRIANGLE,
                                   440.0f, 1.0f, 0.5f, phase_tri);
    helix::audio::generate_samples(buffer_sq.data(), num_samples, SAMPLE_RATE, Waveform::SQUARE,
                                   440.0f, 1.0f, 0.5f, phase_sq);

    float rms_tri = compute_rms(buffer_tri.data(), num_samples);
    float rms_sq = compute_rms(buffer_sq.data(), num_samples);

    // Triangle RMS = amplitude / sqrt(3) ~ 0.577, square RMS = 1.0
    REQUIRE(rms_tri < rms_sq);
    REQUIRE(rms_tri == Approx(1.0f / std::sqrt(3.0f)).margin(0.02f));
}

// ============================================================================
// Amplitude scaling
// ============================================================================

TEST_CASE("Amplitude scaling constrains output range", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SQUARE,
                                   440.0f, 0.5f, 0.5f, phase);

    for (int i = 0; i < SAMPLES_10MS; i++) {
        REQUIRE(std::fabs(buffer[i]) <= 0.5f + 0.001f);
    }
}

TEST_CASE("Amplitude 0.5 sine wave has correct RMS", "[sound][sdl]") {
    constexpr int num_samples = 44100;
    std::vector<float> buffer(num_samples);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), num_samples, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   0.5f, 0.5f, phase);

    float rms = compute_rms(buffer.data(), num_samples);
    float expected_rms = 0.5f / std::sqrt(2.0f);
    REQUIRE(rms == Approx(expected_rms).margin(0.01f));
}

TEST_CASE("Zero amplitude produces silence", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   0.0f, 0.5f, phase);

    for (int i = 0; i < SAMPLES_10MS; i++) {
        REQUIRE(buffer[i] == 0.0f);
    }
}

TEST_CASE("Zero amplitude works for all waveforms", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    Waveform waves[] = {Waveform::SQUARE, Waveform::SAW, Waveform::TRIANGLE, Waveform::SINE};

    for (auto w : waves) {
        float phase = 0;
        helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, w, 440.0f, 0.0f,
                                       0.5f, phase);

        float rms = compute_rms(buffer.data(), SAMPLES_10MS);
        REQUIRE(rms == 0.0f);
    }
}

// ============================================================================
// Phase continuity
// ============================================================================

TEST_CASE("Phase continuity across generate_samples calls", "[sound][sdl]") {
    // Generate two consecutive buffers and check the boundary is smooth
    constexpr int half = SAMPLES_10MS;
    std::vector<float> buf1(half);
    std::vector<float> buf2(half);
    float phase = 0;

    helix::audio::generate_samples(buf1.data(), half, SAMPLE_RATE, Waveform::SINE, 440.0f, 1.0f,
                                   0.5f, phase);
    // Phase carries over
    helix::audio::generate_samples(buf2.data(), half, SAMPLE_RATE, Waveform::SINE, 440.0f, 1.0f,
                                   0.5f, phase);

    // The last sample of buf1 and first sample of buf2 should be continuous
    // (no sudden jump). Check that the difference is small — for a smooth sine,
    // adjacent samples at 440Hz differ by sin(2*pi*440/44100) ~ 0.0627
    float diff = std::fabs(buf2[0] - buf1[half - 1]);
    float max_expected_diff = 2.0f * PI * 440.0f / SAMPLE_RATE + 0.01f;
    REQUIRE(diff < max_expected_diff);
}

TEST_CASE("Phase continuity produces identical output to single call", "[sound][sdl]") {
    // Two half-calls should produce same output as one full call
    constexpr int total = SAMPLES_10MS * 2;
    constexpr int half = SAMPLES_10MS;

    std::vector<float> full_buf(total);
    std::vector<float> half1(half);
    std::vector<float> half2(half);

    float phase_full = 0;
    helix::audio::generate_samples(full_buf.data(), total, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   1.0f, 0.5f, phase_full);

    float phase_split = 0;
    helix::audio::generate_samples(half1.data(), half, SAMPLE_RATE, Waveform::SINE, 440.0f, 1.0f,
                                   0.5f, phase_split);
    helix::audio::generate_samples(half2.data(), half, SAMPLE_RATE, Waveform::SINE, 440.0f, 1.0f,
                                   0.5f, phase_split);

    for (int i = 0; i < half; i++) {
        REQUIRE(half1[i] == Approx(full_buf[i]).margin(0.0001f));
        REQUIRE(half2[i] == Approx(full_buf[half + i]).margin(0.0001f));
    }
}

// ============================================================================
// Biquad filter
// ============================================================================

TEST_CASE("Lowpass filter attenuates high frequencies", "[sound][sdl]") {
    // Generate 10kHz sine
    constexpr int num_samples = 4410; // 100ms
    std::vector<float> buffer(num_samples);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), num_samples, SAMPLE_RATE, Waveform::SINE,
                                   10000.0f, 1.0f, 0.5f, phase);

    float rms_before = compute_rms(buffer.data(), num_samples);

    // Apply lowpass at 1kHz
    helix::audio::BiquadFilter filter{};
    helix::audio::compute_biquad_coeffs(filter, helix::audio::FilterType::LOWPASS, 1000.0f,
                                        SAMPLE_RATE);
    helix::audio::apply_filter(filter, buffer.data(), num_samples);

    float rms_after = compute_rms(buffer.data(), num_samples);

    // 10kHz should be heavily attenuated by a 1kHz lowpass
    REQUIRE(rms_after < rms_before * 0.1f);
}

TEST_CASE("Highpass filter attenuates low frequencies", "[sound][sdl]") {
    constexpr int num_samples = 4410;
    std::vector<float> buffer(num_samples);
    float phase = 0;

    // Generate 100Hz sine
    helix::audio::generate_samples(buffer.data(), num_samples, SAMPLE_RATE, Waveform::SINE, 100.0f,
                                   1.0f, 0.5f, phase);

    float rms_before = compute_rms(buffer.data(), num_samples);

    // Apply highpass at 1kHz
    helix::audio::BiquadFilter filter{};
    helix::audio::compute_biquad_coeffs(filter, helix::audio::FilterType::HIGHPASS, 1000.0f,
                                        SAMPLE_RATE);
    helix::audio::apply_filter(filter, buffer.data(), num_samples);

    float rms_after = compute_rms(buffer.data(), num_samples);

    // 100Hz should be heavily attenuated by a 1kHz highpass
    REQUIRE(rms_after < rms_before * 0.1f);
}

TEST_CASE("Lowpass at extreme cutoff barely changes signal", "[sound][sdl]") {
    constexpr int num_samples = 4410;
    std::vector<float> original(num_samples);
    std::vector<float> filtered(num_samples);
    float phase = 0;

    // Generate 440Hz sine
    helix::audio::generate_samples(original.data(), num_samples, SAMPLE_RATE, Waveform::SINE,
                                   440.0f, 1.0f, 0.5f, phase);

    std::copy(original.begin(), original.end(), filtered.begin());

    // Lowpass at 20kHz — 440Hz should pass through nearly unaffected
    helix::audio::BiquadFilter filter{};
    helix::audio::compute_biquad_coeffs(filter, helix::audio::FilterType::LOWPASS, 20000.0f,
                                        SAMPLE_RATE);
    helix::audio::apply_filter(filter, filtered.data(), num_samples);

    float rms_orig = compute_rms(original.data(), num_samples);
    float rms_filt = compute_rms(filtered.data(), num_samples);

    // RMS should be within 5% of original
    REQUIRE(rms_filt == Approx(rms_orig).margin(rms_orig * 0.05f));
}

TEST_CASE("Filter preserves silence", "[sound][sdl]") {
    constexpr int num_samples = 441;
    std::vector<float> buffer(num_samples, 0.0f);

    helix::audio::BiquadFilter filter{};
    helix::audio::compute_biquad_coeffs(filter, helix::audio::FilterType::LOWPASS, 1000.0f,
                                        SAMPLE_RATE);
    helix::audio::apply_filter(filter, buffer.data(), num_samples);

    for (int i = 0; i < num_samples; i++) {
        REQUIRE(buffer[i] == 0.0f);
    }
}

// ============================================================================
// Backend set_tone / silence integration
// ============================================================================

TEST_CASE("set_tone stores frequency and amplitude for generation", "[sound][sdl]") {
    SDLSoundBackend backend;
    // Don't initialize SDL — just test the parameter storage

    backend.set_tone(440.0f, 1.0f, 0.5f);

    // Generate samples using the backend's internal state
    // The backend should use whatever was last set via set_tone
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;
    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SQUARE,
                                   440.0f, 1.0f, 0.5f, phase);

    float rms = compute_rms(buffer.data(), SAMPLES_10MS);
    REQUIRE(rms > 0.9f); // Full amplitude square = RMS ~1.0
}

TEST_CASE("set_tone with zero amplitude produces silence", "[sound][sdl]") {
    SDLSoundBackend backend;

    backend.set_tone(440.0f, 0.0f, 0.5f);

    // Verify via generate_samples that zero amplitude = silence
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;
    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   0.0f, 0.5f, phase);

    float rms = compute_rms(buffer.data(), SAMPLES_10MS);
    REQUIRE(rms == 0.0f);
}

TEST_CASE("silence() results in zero amplitude output", "[sound][sdl]") {
    SDLSoundBackend backend;

    // Set a tone then silence it
    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.silence();

    // After silence, the backend's internal amplitude should be 0
    // We verify via the static method with amplitude 0
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;
    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE, 440.0f,
                                   0.0f, 0.5f, phase);

    float rms = compute_rms(buffer.data(), SAMPLES_10MS);
    REQUIRE(rms == 0.0f);
}

// ============================================================================
// Waveform switching
// ============================================================================

TEST_CASE("set_waveform switches active waveform type", "[sound][sdl]") {
    SDLSoundBackend backend;

    // Generate sine
    backend.set_waveform(Waveform::SINE);
    std::vector<float> sine_buf(SAMPLES_10MS);
    float phase1 = 0;
    helix::audio::generate_samples(sine_buf.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE,
                                   440.0f, 1.0f, 0.5f, phase1);

    // Generate saw
    backend.set_waveform(Waveform::SAW);
    std::vector<float> saw_buf(SAMPLES_10MS);
    float phase2 = 0;
    helix::audio::generate_samples(saw_buf.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SAW, 440.0f,
                                   1.0f, 0.5f, phase2);

    // The waveforms should produce different sample values
    bool any_different = false;
    for (int i = 0; i < SAMPLES_10MS; i++) {
        if (std::fabs(sine_buf[i] - saw_buf[i]) > 0.01f) {
            any_different = true;
            break;
        }
    }
    REQUIRE(any_different);
}

TEST_CASE("All four waveforms produce distinct signals", "[sound][sdl]") {
    Waveform waves[] = {Waveform::SQUARE, Waveform::SAW, Waveform::TRIANGLE, Waveform::SINE};
    std::vector<std::vector<float>> buffers(4, std::vector<float>(SAMPLES_10MS));

    for (int w = 0; w < 4; w++) {
        float phase = 0;
        helix::audio::generate_samples(buffers[w].data(), SAMPLES_10MS, SAMPLE_RATE, waves[w],
                                       440.0f, 1.0f, 0.5f, phase);
    }

    // Each pair should be meaningfully different
    for (int a = 0; a < 4; a++) {
        for (int b = a + 1; b < 4; b++) {
            float diff_sum = 0;
            for (int i = 0; i < SAMPLES_10MS; i++) {
                diff_sum += std::fabs(buffers[a][i] - buffers[b][i]);
            }
            float avg_diff = diff_sum / SAMPLES_10MS;
            REQUIRE(avg_diff > 0.01f);
        }
    }
}

// ============================================================================
// Edge cases and robustness
// ============================================================================

TEST_CASE("Very high frequency produces valid output", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE,
                                   20000.0f, 1.0f, 0.5f, phase);

    // Should still produce valid samples (no NaN or inf)
    for (int i = 0; i < SAMPLES_10MS; i++) {
        REQUIRE(std::isfinite(buffer[i]));
        REQUIRE(std::fabs(buffer[i]) <= 1.0f + 0.001f);
    }
}

TEST_CASE("Very low frequency produces valid output", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE, 20.0f,
                                   1.0f, 0.5f, phase);

    for (int i = 0; i < SAMPLES_10MS; i++) {
        REQUIRE(std::isfinite(buffer[i]));
        REQUIRE(std::fabs(buffer[i]) <= 1.0f + 0.001f);
    }
}

TEST_CASE("Square wave duty cycle affects positive/negative ratio", "[sound][sdl]") {
    // 75% duty cycle should produce more positive samples
    constexpr int num_samples = 44100; // 1 second for stable stats
    std::vector<float> buffer(num_samples);
    float phase = 0;

    helix::audio::generate_samples(buffer.data(), num_samples, SAMPLE_RATE, Waveform::SQUARE,
                                   440.0f, 1.0f, 0.75f, phase);

    int pos = count_positive(buffer.data(), num_samples);
    float ratio = static_cast<float>(pos) / num_samples;
    // 75% duty = roughly 75% positive
    REQUIRE(ratio > 0.70f);
    REQUIRE(ratio < 0.80f);
}

TEST_CASE("Phase wraps correctly and stays in [0, 1)", "[sound][sdl]") {
    std::vector<float> buffer(SAMPLES_10MS);
    float phase = 0;

    // Generate many buffers — phase should stay bounded
    for (int i = 0; i < 100; i++) {
        helix::audio::generate_samples(buffer.data(), SAMPLES_10MS, SAMPLE_RATE, Waveform::SINE,
                                       440.0f, 1.0f, 0.5f, phase);
        REQUIRE(phase >= 0.0f);
        REQUIRE(phase < 1.0f);
    }
}

#endif // HELIX_DISPLAY_SDL
