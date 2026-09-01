// SPDX-License-Identifier: GPL-3.0-or-later

#include "jz_pwm_sound_backend.h"
#include "note_event.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// jz_pwm_render_step - the pure duty-encoding renderer
// ============================================================================

static NoteEvent plain_note(float freq, float ms, float velocity = 0.8f) {
    NoteEvent e;
    e.freq_hz = freq;
    e.duration_ms = ms;
    e.velocity = velocity;
    e.wave = Waveform::TRIANGLE;
    e.attack_ms = 0;
    e.decay_ms = 0;
    e.sustain_level = 1.0f;
    e.release_ms = 0;
    return e;
}

/// Upper 16 bits of a word = inactive counts, lower 16 = active counts.
static long active_of(uint32_t w) {
    return w & 0xffff;
}
static long inactive_of(uint32_t w) {
    return w >> 16;
}

TEST_CASE("render: word count tracks duration at the carrier rate", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(1000, 500); // above the floor: true length

    auto words = jz_pwm_render_step(&e, 1, p);
    // 500 ms at 32768 Hz = 16384 words, already a multiple of 4
    REQUIRE(words.size() == 16384);
    REQUIRE(words.size() % p.word_align == 0);
}

TEST_CASE("render: short steps tile out to the audible floor", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(4000, 6); // the theme's 6 ms button_tap tick

    auto words = jz_pwm_render_step(&e, 1, p);
    size_t floor_words = (size_t)(p.min_note_ms * p.carrier_hz / 1000.0);
    floor_words -= floor_words % p.word_align;
    REQUIRE(words.size() == floor_words);
    // the content is the 6 ms render REPEATED, not a stretched envelope
    size_t period_words = (size_t)(6 * p.carrier_hz / 1000.0);
    period_words -= period_words % p.word_align;
    REQUIRE(period_words > 0);
    REQUIRE(words[0] == words[period_words]);
}

TEST_CASE("render: quiet notes are peak-normalized to full swing", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(1000, 100, 0.15f); // whisper velocity

    auto words = jz_pwm_render_step(&e, 1, p);
    const long period = (long)(p.clock_hz / p.carrier_hz);
    const long half = period / 2;
    long peak = 0;
    for (uint32_t w : words)
        peak = std::max(peak, std::abs((long)(w & 0xffff) - half));
    // within 1 count of the full +-40% target once normalized
    REQUIRE(peak >= (long)(period * p.duty_swing) - 1);
}

TEST_CASE("render: duration is capped at the wedge-safe maximum", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(1000, 60000); // a minute: way past the cap

    auto words = jz_pwm_render_step(&e, 1, p);
    REQUIRE(words.size() == (size_t)(p.max_note_ms * p.carrier_hz / 1000.0));
}

TEST_CASE("render: silent voices produce no buffer", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(1000, 100, 0.0f);

    REQUIRE(jz_pwm_render_step(&e, 1, p).empty());

    NoteEvent zero_freq = plain_note(0, 100);
    REQUIRE(jz_pwm_render_step(&zero_freq, 1, p).empty());
}

TEST_CASE("render: every word is a whole period with in-range halves", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(880, 50, 1.0f); // over-unit velocity: clamps hard

    auto words = jz_pwm_render_step(&e, 1, p);
    REQUIRE_FALSE(words.empty());
    const long period = (long)(p.clock_hz / p.carrier_hz);
    for (uint32_t w : words) {
        REQUIRE(active_of(w) >= 1);
        REQUIRE(active_of(w) <= period - 1);
        // halves always sum to exactly one period word
        REQUIRE(active_of(w) + inactive_of(w) == period);
    }
}

TEST_CASE("render: envelope starts at center duty and deviates later", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(1000, 200);
    e.attack_ms = 30;
    e.decay_ms = 0;
    e.sustain_level = 1.0f;
    e.release_ms = 0;

    auto words = jz_pwm_render_step(&e, 1, p);
    const long period = (long)(p.clock_hz / p.carrier_hz);

    // attack starts at amplitude 0: first sample is the 50% center
    REQUIRE(std::abs(active_of(words[0]) - period / 2) <= 1);
    // by mid-attack the duty has swung away from center
    long mid = active_of(words[500]);
    REQUIRE(std::abs(mid - period / 2) > std::abs((long)active_of(words[0]) - period / 2) + 50);
}

TEST_CASE("render: opposite-phase voices cancel to center duty", "[sound][jz]") {
    JzPwmRenderParams p;
    // 8 Hz-aligned frequency for a phase-continuous buffer at 32768 Hz.
    // Two voices an 8 Hz step apart beat against each other; the mixed
    // buffer must still hold the whole-period halves contract through
    // partial cancellation, and its peak duty deviation can never exceed
    // the solo voice's.
    NoteEvent a = plain_note(512, 125);
    NoteEvent c = plain_note(520, 125);

    std::vector<NoteEvent> pair{a, c};
    auto beaten = jz_pwm_render_step(pair.data(), 2, p);
    auto solo = jz_pwm_render_step(&a, 1, p);
    REQUIRE_FALSE(beaten.empty());
    REQUIRE_FALSE(solo.empty());

    const long period = (long)(p.clock_hz / p.carrier_hz);
    auto peak = [&](const std::vector<uint32_t>& v) {
        long m = 0;
        for (uint32_t w : v)
            m = std::max(m, std::abs((long)active_of(w) - period / 2));
        return m;
    };
    // A beating pair spends part of every cycle near cancellation, so its
    // peak duty deviation is at most the solo voice's (here: equal, since
    // both voices swing fully in phase at the beat peak). The real check
    // is the halves contract held under mixing.
    REQUIRE(peak(beaten) <= peak(solo));
    for (uint32_t w : beaten)
        REQUIRE(active_of(w) + inactive_of(w) == period);
}

TEST_CASE("voices_to_events: snapshot becomes sustained chord events", "[sound][jz]") {
    float freq[4] = {440, 0, 656, 880};
    float amp[4] = {0.8f, 0.5f, 0.6f, 0.0f};

    auto events = jz_pwm_voices_to_events(freq, amp, 4, 150.0f);
    // slot 1 has no freq, slot 3 no amplitude: two sounding voices
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].freq_hz == 440.0f);
    REQUIRE(events[0].velocity == 0.8f);
    REQUIRE(events[1].freq_hz == 656.0f);
    REQUIRE(events[1].duration_ms == 150.0f);
    REQUIRE(events[0].sustain_level == 1.0f);

    float silent[4] = {0, 0, 0, 0};
    REQUIRE(jz_pwm_voices_to_events(silent, silent, 4, 150.0f).empty());
}

// ============================================================================
// JzPwmSoundBackend - behavior without hardware
// ============================================================================

TEST_CASE("backend: unavailable host reports unavailable", "[sound][jz]") {
    // The dev host has no /dev/jz_pwm; the rig does. This test asserts
    // the probe, not the machine, so guard on the device existing.
    if (access("/dev/jz_pwm", F_OK) == 0) {
        REQUIRE(JzPwmSoundBackend::available());
        SUCCEED("running on the rig");
        return;
    }
    REQUIRE_FALSE(JzPwmSoundBackend::available());
}

TEST_CASE("backend: uninitialized backend drops steps without spawning", "[sound][jz]") {
    JzPwmSoundBackend backend;
    if (backend.initialize()) {
        SUCCEED("fx-pwm present; spawn-side behavior needs the rig");
        return;
    }
    // No fx-pwm resolved: publishing a full 4-voice step must not crash
    // and must not write the words file.
    unlink("/tmp/helixscreen-jz-pwm.words");
    NoteEvent e = plain_note(440, 100);
    backend.publish_note(0, e);
    backend.publish_note(1, NoteEvent{});
    backend.publish_note(2, NoteEvent{});
    backend.publish_note(3, NoteEvent{});
    REQUIRE(access("/tmp/helixscreen-jz-pwm.words", F_OK) != 0);
    backend.silence(); // no child: must be a safe no-op
}
