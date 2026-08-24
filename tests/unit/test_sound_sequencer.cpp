// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui_test_utils.h"
#include "audio_settings_manager.h"
#include "settings_manager.h"
#include "sound_sequencer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Catch::Approx;

// ============================================================================
// MockBackend: records all set_tone() and silence() calls with timestamps
// ============================================================================

// Anonymous namespace gives this MockBackend internal linkage. Without it, the
// linker would merge this definition with the differently-laid-out MockBackend
// in test_sound_polyphony.cpp (an ODR violation): the virtual set_tone()
// dispatched through SoundSequencer would run the wrong class's body and write
// floats over this object's std::mutex, deadlocking get_tones() forever.
namespace {

class MockBackend : public SoundBackend {
  public:
    struct ToneEvent {
        float freq_hz;
        float amplitude;
        float duty_cycle;
        std::chrono::steady_clock::time_point timestamp;
    };

    struct SilenceEvent {
        std::chrono::steady_clock::time_point timestamp;
    };

    mutable std::mutex mutex;
    std::vector<ToneEvent> tone_events;
    std::vector<SilenceEvent> silence_events;
    bool amp_support = true;

    void set_tone(float freq_hz, float amplitude, float duty_cycle) override {
        std::lock_guard<std::mutex> lock(mutex);
        tone_events.push_back({freq_hz, amplitude, duty_cycle, std::chrono::steady_clock::now()});
    }

    void silence() override {
        std::lock_guard<std::mutex> lock(mutex);
        silence_events.push_back({std::chrono::steady_clock::now()});
    }

    bool supports_amplitude() const override {
        return amp_support;
    }

    // Helpers for tests
    size_t tone_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return tone_events.size();
    }

    size_t silence_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return silence_events.size();
    }

    std::vector<ToneEvent> get_tones() const {
        std::lock_guard<std::mutex> lock(mutex);
        return tone_events;
    }

    std::vector<SilenceEvent> get_silences() const {
        std::lock_guard<std::mutex> lock(mutex);
        return silence_events;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        tone_events.clear();
        silence_events.clear();
    }

    // --- device park/wake accounting ---
    // The sequencer is the only thing that opens and closes the audio device.
    // Counting the calls is how a test can tell "parked at idle" apart from
    // "held open forever", which is otherwise invisible without real hardware.
    std::atomic<int> suspend_calls{0};
    std::atomic<int> resume_calls{0};

    void suspend() override {
        suspend_calls.fetch_add(1);
    }

    void resume() override {
        resume_calls.fetch_add(1);
    }
};

} // namespace

// ============================================================================
// Helpers
// ============================================================================

static SoundDefinition make_tone(float freq, float dur_ms, float vel = 0.8f) {
    SoundStep step;
    step.freq_hz = freq;
    step.duration_ms = dur_ms;
    step.velocity = vel;
    step.wave = Waveform::SQUARE;
    step.envelope = {0, 0, 1.0f, 0}; // flat envelope (all times 0)
    step.is_pause = false;

    SoundDefinition def;
    def.name = "test_tone";
    def.steps.push_back(step);
    def.repeat = 1;
    return def;
}

static SoundDefinition make_multi_step(std::vector<std::pair<float, float>> freq_dur_pairs) {
    SoundDefinition def;
    def.name = "test_multi";
    def.repeat = 1;
    for (auto& [freq, dur] : freq_dur_pairs) {
        SoundStep step;
        step.freq_hz = freq;
        step.duration_ms = dur;
        step.velocity = 0.8f;
        step.wave = Waveform::SQUARE;
        step.envelope = {0, 0, 1.0f, 0};
        step.is_pause = false;
        def.steps.push_back(step);
    }
    return def;
}

// Wait for playback to finish with timeout.
// First waits for playing to start, then waits for it to end.
static bool wait_until_done(SoundSequencer& seq, int timeout_ms = 5000) {
    auto start = std::chrono::steady_clock::now();

    // Phase 1: wait for playback to begin (sequencer thread needs to pick up the request)
    while (!seq.is_playing()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed > timeout_ms)
            return false;
        // If it started and stopped in a flash (e.g. empty sequence), don't wait forever
        if (elapsed > 50)
            break;
    }

    // Phase 2: wait for playback to end
    while (seq.is_playing()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed > timeout_ms)
            return false;
    }

    // Give sequencer thread time to finish any final operations
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    return true;
}

// ============================================================================
// 1. Single tone step: correct freq sent to backend, silenced after duration
// ============================================================================

TEST_CASE("SoundSequencer: single tone step plays correct freq", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    auto sound = make_tone(1000.0f, 100.0f);
    seq.play(sound);

    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() > 0);

    // Pacing guard: a 100ms tone ticked at the backend's ~1ms min interval
    // yields ~100-200 events. A runaway spin (sleep computed as ~0) would flood
    // the backend with orders of magnitude more. Bound generously so slow CI
    // runners pass, but tight enough to catch a real pacing regression.
    CHECK(tones.size() < 5000);

    // All tone events should be at ~1000 Hz
    for (auto& t : tones) {
        CHECK(t.freq_hz == Approx(1000.0f).margin(1.0f));
    }

    // Should have been silenced after completion
    CHECK(backend->silence_count() > 0);

    seq.shutdown();
}

// ============================================================================
// 2. Multi-step sequence: steps play in order
// ============================================================================

TEST_CASE("SoundSequencer: multi-step sequence plays in order", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    // Use longer durations so we get enough events per step
    auto sound = make_multi_step({{500.0f, 80.0f}, {1000.0f, 80.0f}, {1500.0f, 80.0f}});
    seq.play(sound);

    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() > 3);

    // Find the frequency sequence: should see 500, then 1000, then 1500
    bool saw_500 = false;
    bool saw_1000_after_500 = false;
    bool saw_1500_after_1000 = false;

    for (auto& t : tones) {
        float f = t.freq_hz;
        if (!saw_500 && f == Approx(500.0f).margin(2.0f)) {
            saw_500 = true;
        } else if (saw_500 && !saw_1000_after_500 && f == Approx(1000.0f).margin(2.0f)) {
            saw_1000_after_500 = true;
        } else if (saw_1000_after_500 && !saw_1500_after_1000 &&
                   f == Approx(1500.0f).margin(2.0f)) {
            saw_1500_after_1000 = true;
        }
    }

    CHECK(saw_500);
    CHECK(saw_1000_after_500);
    CHECK(saw_1500_after_1000);

    seq.shutdown();
}

// ============================================================================
// 3. Pause step: silence() called, correct duration gap
// ============================================================================

TEST_CASE("SoundSequencer: pause step produces silence", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    // tone -> pause -> tone
    SoundDefinition def;
    def.name = "pause_test";
    def.repeat = 1;

    SoundStep tone1;
    tone1.freq_hz = 1000;
    tone1.duration_ms = 80;
    tone1.velocity = 0.8f;
    tone1.envelope = {0, 0, 1.0f, 0};
    def.steps.push_back(tone1);

    SoundStep pause;
    pause.is_pause = true;
    pause.duration_ms = 80;
    pause.freq_hz = 0;
    // Pause needs envelope times to make total_ms work (env_min=0, dur=80)
    pause.envelope = {0, 0, 0, 0};
    def.steps.push_back(pause);

    SoundStep tone2;
    tone2.freq_hz = 2000;
    tone2.duration_ms = 80;
    tone2.velocity = 0.8f;
    tone2.envelope = {0, 0, 1.0f, 0};
    def.steps.push_back(tone2);

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    // During the pause, silence() should have been called
    CHECK(backend->silence_count() > 0);

    // Should see tone events for both 1000 Hz and 2000 Hz
    auto tones = backend->get_tones();
    bool has_1000 = false, has_2000 = false;
    for (auto& t : tones) {
        if (t.freq_hz == Approx(1000.0f).margin(2.0f))
            has_1000 = true;
        if (t.freq_hz == Approx(2000.0f).margin(2.0f))
            has_2000 = true;
    }
    CHECK(has_1000);
    CHECK(has_2000);

    seq.shutdown();
}

// ============================================================================
// 4. ADSR attack: amplitude ramps from 0 to ~1.0 during attack phase
// ============================================================================

TEST_CASE("SoundSequencer: ADSR attack ramps amplitude up", "[sound][sequencer][slow]") {
    lv_init_safe();
    SettingsManager::instance().init_subjects();
    AudioSettingsManager::instance().set_volume(100);
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundStep step;
    step.freq_hz = 1000;
    // Longer duration and attack — CI runners can be slow to deliver the
    // first sequencer tick. With a 200ms attack on an overloaded runner the
    // first captured tone can already be past the attack phase. A 600ms
    // attack plus 1200ms total duration leaves plenty of headroom while still
    // keeping the test comfortably under 2s.
    step.duration_ms = 1200;
    step.velocity = 1.0f;
    step.wave = Waveform::SQUARE;
    step.envelope = {600, 0, 1.0f, 0}; // 600ms attack, sustain=1

    SoundDefinition def;
    def.name = "adsr_attack";
    def.steps.push_back(step);
    def.repeat = 1;

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() >= 5);

    // Envelope must ramp UP during the attack phase: any tone captured well
    // before attack end (<300ms, half the attack window) should be measurably
    // quieter than any tone captured after sustain begins. Comparing against
    // the FIRST tone alone is flaky because thread-startup latency can push
    // tones[0] past the attack phase on slow CI runners; comparing samples
    // across the whole attack-to-sustain arc is the real invariant.
    auto start_time = tones[0].timestamp;
    auto elapsed_ms = [&](const MockBackend::ToneEvent& t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp - start_time)
            .count();
    };

    float min_attack_amp = 2.0f; // impossibly high sentinel
    for (const auto& t : tones) {
        if (elapsed_ms(t) < 300) {
            min_attack_amp = std::min(min_attack_amp, t.amplitude);
        }
    }

    // Events well past the attack phase (>800ms) should be at full amplitude.
    bool found_full = false;
    float max_sustain_amp = 0.0f;
    for (const auto& t : tones) {
        if (elapsed_ms(t) > 800) {
            max_sustain_amp = std::max(max_sustain_amp, t.amplitude);
            if (t.amplitude > 0.8f) {
                found_full = true;
            }
        }
    }
    CHECK(found_full);
    CHECK(max_sustain_amp > 0.8f);

    // The envelope *must* be ramping: the quietest attack-window sample must
    // be lower than the loudest sustain-window sample. This works even if the
    // very first tick lands halfway through attack.
    if (min_attack_amp <= 1.0f) {
        CHECK(min_attack_amp < max_sustain_amp);
    }

    seq.shutdown();
}

// ============================================================================
// 5. ADSR decay: amplitude drops from 1.0 toward sustain level
// ============================================================================

TEST_CASE("SoundSequencer: ADSR decay drops amplitude toward sustain", "[sound][sequencer][slow]") {
    AudioSettingsManager::instance().set_volume(100);
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundStep step;
    step.freq_hz = 1000;
    step.duration_ms = 600;
    step.velocity = 1.0f;
    step.wave = Waveform::SQUARE;
    // Quick attack, long decay to low sustain
    step.envelope = {10, 300, 0.2f, 10};

    SoundDefinition def;
    def.name = "adsr_decay";
    def.steps.push_back(step);
    def.repeat = 1;

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() >= 5);

    // Find events in the decay phase and sustain phase
    auto start_time = tones[0].timestamp;
    bool found_decay = false;
    bool found_sustain = false;

    for (auto& t : tones) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp - start_time).count();

        // In the middle of decay (50-200ms): amplitude should be between
        // sustain(0.2) and peak(1.0)
        if (elapsed > 50 && elapsed < 200 && t.amplitude > 0.25f && t.amplitude < 0.95f) {
            found_decay = true;
        }
        // In sustain phase (>350ms): amplitude should be near 0.2
        if (elapsed > 400 && elapsed < 550 && t.amplitude < 0.4f) {
            found_sustain = true;
        }
    }

    CHECK(found_decay);
    CHECK(found_sustain);

    seq.shutdown();
}

// ============================================================================
// 6. ADSR sustain: amplitude holds at sustain level
// ============================================================================

TEST_CASE("SoundSequencer: ADSR sustain holds amplitude", "[sound][sequencer][slow]") {
    AudioSettingsManager::instance().set_volume(100);
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundStep step;
    step.freq_hz = 1000;
    step.duration_ms = 500;
    step.velocity = 1.0f;
    step.wave = Waveform::SQUARE;
    // Quick attack+decay, then sustain at 0.6 for most of duration
    step.envelope = {10, 20, 0.6f, 10};

    SoundDefinition def;
    def.name = "adsr_sustain";
    def.steps.push_back(step);
    def.repeat = 1;

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() >= 5);

    // Events in the sustain phase (50ms to 450ms) should be near 0.6
    auto start_time = tones[0].timestamp;
    int sustain_samples = 0;
    int near_sustain = 0;

    for (auto& t : tones) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp - start_time).count();

        if (elapsed > 60 && elapsed < 440) {
            sustain_samples++;
            if (t.amplitude > 0.4f && t.amplitude < 0.8f) {
                near_sustain++;
            }
        }
    }

    REQUIRE(sustain_samples > 0);
    float ratio = static_cast<float>(near_sustain) / sustain_samples;
    CHECK(ratio > 0.7f);

    seq.shutdown();
}

// ============================================================================
// 7. ADSR release: amplitude fades to 0 at end of step
// ============================================================================

TEST_CASE("SoundSequencer: ADSR release fades amplitude to zero", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundStep step;
    step.freq_hz = 1000;
    step.duration_ms = 400;
    step.velocity = 1.0f;
    step.wave = Waveform::SQUARE;
    // Short attack, sustain 0.8, long release (200ms)
    step.envelope = {10, 0, 0.8f, 200};

    SoundDefinition def;
    def.name = "adsr_release";
    def.steps.push_back(step);
    def.repeat = 1;

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() >= 5);

    // The last quarter of events should show generally decreasing amplitude
    // since the release phase occupies the last 200ms of a 400ms step.
    size_t release_start_idx = tones.size() / 2; // roughly where release begins
    int decreases = 0;
    int comparisons = 0;

    for (size_t i = release_start_idx + 1; i < tones.size(); i++) {
        comparisons++;
        if (tones[i].amplitude < tones[i - 1].amplitude + 0.01f) {
            decreases++;
        }
    }

    // Most samples in release phase should show non-increasing amplitude
    REQUIRE(comparisons > 0);
    float dec_ratio = static_cast<float>(decreases) / comparisons;
    CHECK(dec_ratio > 0.7f);

    // Last events should have low amplitude (near end of release)
    CHECK(tones.back().amplitude < 0.5f);

    seq.shutdown();
}

// ============================================================================
// 8. LFO on frequency: freq oscillates around base frequency
// ============================================================================

TEST_CASE("SoundSequencer: LFO modulates frequency", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundStep step;
    step.freq_hz = 1000;
    step.duration_ms = 500; // long enough for several LFO cycles
    step.velocity = 0.8f;
    step.wave = Waveform::SQUARE;
    step.envelope = {0, 0, 1.0f, 0};
    // LFO on freq: 5 Hz rate, +-200 Hz depth
    step.lfo = {"freq", 5.0f, 200.0f};

    SoundDefinition def;
    def.name = "lfo_freq";
    def.steps.push_back(step);
    def.repeat = 1;

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() >= 5);

    float min_freq = 999999, max_freq = 0;
    for (auto& t : tones) {
        min_freq = std::min(min_freq, t.freq_hz);
        max_freq = std::max(max_freq, t.freq_hz);
    }

    // Freq should oscillate: some below 1000, some above 1000
    CHECK(min_freq < 950.0f);
    CHECK(max_freq > 1050.0f);
    // Should stay within the LFO depth range
    CHECK(min_freq > 700.0f);
    CHECK(max_freq < 1300.0f);

    seq.shutdown();
}

// ============================================================================
// 9. LFO on amplitude: amplitude modulates
// ============================================================================

TEST_CASE("SoundSequencer: LFO modulates amplitude", "[sound][sequencer][slow]") {
    lv_init_safe();
    SettingsManager::instance().init_subjects();
    AudioSettingsManager::instance().set_volume(100);
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundStep step;
    step.freq_hz = 1000;
    step.duration_ms = 500;
    step.velocity = 0.8f;
    step.wave = Waveform::SQUARE;
    step.envelope = {0, 0, 1.0f, 0};
    // LFO on amplitude: 5 Hz rate, 0.4 depth
    step.lfo = {"amplitude", 5.0f, 0.4f};

    SoundDefinition def;
    def.name = "lfo_amp";
    def.steps.push_back(step);
    def.repeat = 1;

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() >= 5);

    float min_amp = 999, max_amp = 0;
    for (auto& t : tones) {
        min_amp = std::min(min_amp, t.amplitude);
        max_amp = std::max(max_amp, t.amplitude);
    }

    // Amplitude should vary: base is 0.8, LFO depth 0.4
    // Range: 0.8 - 0.4 = 0.4 to 0.8 + 0.4 = 1.0 (clamped)
    CHECK(min_amp < 0.7f);
    CHECK(max_amp > 0.6f);

    seq.shutdown();
}

// ============================================================================
// 10. Sweep on frequency: freq interpolates from start to end
// ============================================================================

TEST_CASE("SoundSequencer: sweep interpolates frequency", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundStep step;
    step.freq_hz = 500;
    step.duration_ms = 300;
    step.velocity = 0.8f;
    step.wave = Waveform::SQUARE;
    step.envelope = {0, 0, 1.0f, 0};
    step.sweep = {"freq", 2000.0f}; // sweep from 500 to 2000

    SoundDefinition def;
    def.name = "sweep_freq";
    def.steps.push_back(step);
    def.repeat = 1;

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() >= 3);

    // First events should be near 500 Hz
    CHECK(tones[0].freq_hz < 900.0f);
    // Last events should be near 2000 Hz
    CHECK(tones.back().freq_hz > 1200.0f);

    // Should be generally increasing
    int increases = 0;
    for (size_t i = 1; i < tones.size(); i++) {
        if (tones[i].freq_hz >= tones[i - 1].freq_hz - 1.0f)
            increases++;
    }
    float increase_ratio = static_cast<float>(increases) / (tones.size() - 1);
    CHECK(increase_ratio > 0.8f);

    seq.shutdown();
}

// ============================================================================
// 11. Priority: EVENT sound replaces UI sound
// ============================================================================

TEST_CASE("SoundSequencer: EVENT priority replaces UI sound", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    // Play a long UI sound
    auto ui_sound = make_tone(500.0f, 1000.0f);
    seq.play(ui_sound, SoundPriority::UI);

    // Wait for it to start playing
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(seq.is_playing());

    // Interrupt with EVENT priority
    auto event_sound = make_tone(2000.0f, 100.0f);
    seq.play(event_sound, SoundPriority::EVENT);

    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();

    // Should see 2000 Hz events (the EVENT sound played)
    bool saw_event_freq = false;
    for (auto& t : tones) {
        if (t.freq_hz == Approx(2000.0f).margin(2.0f)) {
            saw_event_freq = true;
            break;
        }
    }
    CHECK(saw_event_freq);

    seq.shutdown();
}

// ============================================================================
// 12. Priority: UI sound does NOT replace EVENT sound
// ============================================================================

TEST_CASE("SoundSequencer: UI priority does NOT replace EVENT sound", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    // Play a long EVENT sound
    auto event_sound = make_tone(2000.0f, 300.0f);
    seq.play(event_sound, SoundPriority::EVENT);

    // Wait for it to start
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(seq.is_playing());

    // Try to play UI sound - should be dropped
    backend->clear();
    auto ui_sound = make_tone(500.0f, 100.0f);
    seq.play(ui_sound, SoundPriority::UI);

    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();

    // All events after the clear should still be 2000 Hz (the UI sound was dropped)
    for (auto& t : tones) {
        CHECK(t.freq_hz == Approx(2000.0f).margin(2.0f));
    }

    seq.shutdown();
}

// ============================================================================
// 13. Priority: ALARM replaces everything
// ============================================================================

TEST_CASE("SoundSequencer: ALARM replaces EVENT sound", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    // Play a long EVENT sound
    auto event_sound = make_tone(2000.0f, 1000.0f);
    seq.play(event_sound, SoundPriority::EVENT);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(seq.is_playing());

    // Interrupt with ALARM
    auto alarm = make_tone(3000.0f, 100.0f);
    seq.play(alarm, SoundPriority::ALARM);

    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();

    // Should see 3000 Hz events
    bool saw_alarm = false;
    for (auto& t : tones) {
        if (t.freq_hz == Approx(3000.0f).margin(2.0f)) {
            saw_alarm = true;
            break;
        }
    }
    CHECK(saw_alarm);

    seq.shutdown();
}

// ============================================================================
// 14. Repeat: sequence plays N times
// ============================================================================

TEST_CASE("SoundSequencer: repeat plays sequence N times", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    auto sound = make_tone(1000.0f, 80.0f);
    sound.repeat = 3;

    seq.play(sound);
    REQUIRE(wait_until_done(seq));

    auto silences = backend->get_silences();

    // With 3 repeats: silence between repeat 1->2, 2->3, and final end_playback
    // Plus step boundary silences = at least 3
    CHECK(silences.size() >= 3);

    // Total playback should be ~3x a single repeat
    auto tones = backend->get_tones();
    CHECK(tones.size() > 5);

    seq.shutdown();
}

// ============================================================================
// 15. stop(): playback halts, backend silenced
// ============================================================================

TEST_CASE("SoundSequencer: stop halts playback and silences", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    auto sound = make_tone(1000.0f, 2000.0f); // 2 second sound
    seq.play(sound);

    // Let it play briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(seq.is_playing());

    seq.stop();

    // Should stop within a few ms
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(seq.is_playing());

    // Backend should have been silenced
    CHECK(backend->silence_count() > 0);

    seq.shutdown();
}

// ============================================================================
// 16. is_playing(): true during playback, false after
// ============================================================================

TEST_CASE("SoundSequencer: is_playing reflects playback state", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    CHECK_FALSE(seq.is_playing());

    auto sound = make_tone(1000.0f, 200.0f);
    seq.play(sound);

    // Give the sequencer thread time to pick it up
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(seq.is_playing());

    REQUIRE(wait_until_done(seq));
    CHECK_FALSE(seq.is_playing());

    seq.shutdown();
}

// ============================================================================
// 17. play() is non-blocking (returns immediately)
// ============================================================================

TEST_CASE("SoundSequencer: play is non-blocking", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    auto sound = make_tone(1000.0f, 1000.0f);

    auto start = std::chrono::steady_clock::now();
    seq.play(sound);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    // play() should return in well under 10ms
    CHECK(elapsed < 10);

    seq.stop();
    seq.shutdown();
}

// ============================================================================
// 18. Rapid play() calls: last one wins for same priority
// ============================================================================

TEST_CASE("SoundSequencer: rapid play calls - last wins", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    // Fire off several play calls rapidly (all UI priority)
    seq.play(make_tone(500.0f, 300.0f));
    seq.play(make_tone(1000.0f, 300.0f));
    seq.play(make_tone(1500.0f, 300.0f));
    seq.play(make_tone(2000.0f, 200.0f)); // this one should end up playing

    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    REQUIRE(tones.size() > 0);

    // The 2000 Hz sound should have been played (last queued, same priority)
    bool saw_2000 = false;
    for (auto& t : tones) {
        if (t.freq_hz == Approx(2000.0f).margin(2.0f)) {
            saw_2000 = true;
            break;
        }
    }
    CHECK(saw_2000);

    seq.shutdown();
}

// ============================================================================
// 19. Empty sequence: no crash, no playback
// ============================================================================

TEST_CASE("SoundSequencer: empty sequence does not crash", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundDefinition empty;
    empty.name = "empty";
    empty.repeat = 1;
    // No steps

    seq.play(empty);

    // Should not crash, should not play
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(seq.is_playing());

    // No tone events should have been generated
    CHECK(backend->tone_count() == 0);

    seq.shutdown();
}

// ============================================================================
// 20. Zero-duration step: skipped gracefully
// ============================================================================

TEST_CASE("SoundSequencer: zero-duration step is skipped", "[sound][sequencer][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    SoundDefinition def;
    def.name = "zero_dur";
    def.repeat = 1;

    SoundStep zero_step;
    zero_step.freq_hz = 500;
    zero_step.duration_ms = 0;
    zero_step.velocity = 0.8f;
    zero_step.envelope = {0, 0, 1.0f, 0};
    def.steps.push_back(zero_step);

    SoundStep normal_step;
    normal_step.freq_hz = 1000;
    normal_step.duration_ms = 100;
    normal_step.velocity = 0.8f;
    normal_step.envelope = {0, 0, 1.0f, 0};
    def.steps.push_back(normal_step);

    seq.play(def);
    REQUIRE(wait_until_done(seq));

    auto tones = backend->get_tones();
    bool saw_1000 = false;
    for (auto& t : tones) {
        if (t.freq_hz == Approx(1000.0f).margin(2.0f)) {
            saw_1000 = true;
            break;
        }
    }
    CHECK(saw_1000);

    seq.shutdown();
}

// ============================================================================
// Device park lifecycle
// ============================================================================
//
// The backend opens the audio device in initialize(); the sequencer owns when it
// is parked. A printer with sounds turned off never plays anything, so if the
// first park only happened after a sound finished, the device would stay open
// for the whole process lifetime — ALSA writing silence every period, Android
// holding an AudioTrack (the #1253 spam).

TEST_CASE("SoundSequencer: parks the device before any sound plays",
          "[sound][sequencer][device_park]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();

    // Give the loop a few ticks; it must park without being asked to play.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(backend->suspend_calls.load() >= 1);
    CHECK(backend->resume_calls.load() == 0); // nothing has needed the device yet

    seq.shutdown();
}

TEST_CASE("SoundSequencer: wakes the device to play and parks it again after",
          "[sound][sequencer][device_park][slow]") {
    auto backend = std::make_shared<MockBackend>();
    SoundSequencer seq(backend);
    seq.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int suspends_before = backend->suspend_calls.load();
    REQUIRE(suspends_before >= 1);

    auto sound = make_tone(1000.0f, 60.0f);
    seq.play(sound);
    REQUIRE(wait_until_done(seq));

    // Woke to play...
    CHECK(backend->resume_calls.load() >= 1);
    CHECK(backend->tone_count() > 0);

    // ...and parked again once the queue drained.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(backend->suspend_calls.load() > suspends_before);

    seq.shutdown();
}
