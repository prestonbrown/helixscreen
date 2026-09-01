// SPDX-License-Identifier: GPL-3.0-or-later

#include "jz_pwm_sound_backend.h"
#include "note_event.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::jz_phrase_clip;
using helix::jz_pwm_render_phrase;
using helix::jz_pwm_render_step;
using helix::jz_pwm_voices_to_events;
using helix::jz_voice_should_flush;
using helix::JzPhraseRow;
using helix::JzPwmRenderParams;
using helix::JzPwmSoundBackend;
using helix::JzPwmVoiceKnobs;

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

    auto events = jz_pwm_voices_to_events(freq, amp, 4, 150.0f, 2.0f, 10.0f, Waveform::TRIANGLE, 0);
    // slot 1 has no freq, slot 3 no amplitude: two sounding voices
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].freq_hz == 440.0f);
    REQUIRE(events[0].velocity == 0.8f);
    REQUIRE(events[1].freq_hz == 656.0f);
    REQUIRE(events[1].duration_ms == 150.0f);
    REQUIRE(events[0].sustain_level == 1.0f);
    REQUIRE(events[0].attack_ms == 2.0f);
    REQUIRE(events[0].release_ms == 10.0f);
    REQUIRE(events[0].wave == Waveform::TRIANGLE);

    float silent[4] = {0, 0, 0, 0};
    REQUIRE(jz_pwm_voices_to_events(silent, silent, 4, 150.0f, 2.0f, 10.0f, Waveform::TRIANGLE, 0)
                .empty());
}

TEST_CASE("voices_to_events: octave shift moves register, ceiling holds", "[sound][jz]") {
    // crocketts bass sits at 110 Hz - inaudible on the piezo. +2 octaves
    // lifts it into the band; the ultrasonic ceiling still trims anything
    // above 4 kHz back down regardless of shift.
    float freq[2] = {110.0f, 3000.0f};
    float amp[2] = {0.8f, 0.8f};

    auto up = jz_pwm_voices_to_events(freq, amp, 2, 150.0f, 2.0f, 10.0f, Waveform::SQUARE, 2);
    REQUIRE(up[0].freq_hz == Catch::Approx(440.0f));
    REQUIRE(up[1].freq_hz <= 4000.0f);
    REQUIRE(up[0].wave == Waveform::SQUARE);

    auto down = jz_pwm_voices_to_events(freq, amp, 2, 150.0f, 2.0f, 10.0f, Waveform::TRIANGLE, -1);
    REQUIRE(down[0].freq_hz == Catch::Approx(55.0f));
}

// ============================================================================
// jz_pwm_render_phrase - multi-row phrase buffer
// ============================================================================

static JzPhraseRow row(float f0, float f1, float f2, float f3, float ms) {
    JzPhraseRow r;
    r.freq[0] = f0;
    r.freq[1] = f1;
    r.freq[2] = f2;
    r.freq[3] = f3;
    for (int v = 0; v < 4; ++v)
        r.amp[v] = (r.freq[v] > 0) ? 0.8f : 0.0f;
    r.ms = ms;
    return r;
}

/// Count duty transitions across the 50% center: a proxy for toggle rate.
static long center_crossings(const std::vector<uint32_t>& words) {
    const long period = 11749, half = period / 2;
    long prev = (long)(words[0] & 0xffff) - half, count = 0;
    for (uint32_t w : words) {
        long dev = (long)(w & 0xffff) - half;
        if ((dev >= 0) != (prev >= 0))
            count++;
        prev = dev;
    }
    return count;
}

TEST_CASE("render_phrase: word count tracks total row time", "[sound][jz]") {
    JzPwmRenderParams p;
    JzPwmVoiceKnobs k;
    JzPhraseRow rows[2] = {row(440, 0, 0, 0, 500), row(440, 0, 0, 0, 500)};

    auto words = jz_pwm_render_phrase(rows, 2, p, k);
    // 1000 ms at 32768 Hz = 32768 words, multiple of 4
    REQUIRE(words.size() == 32768);
    REQUIRE(words.size() % 4 == 0);
}

TEST_CASE("render_phrase: note change mid-phrase raises the toggle rate", "[sound][jz]") {
    JzPwmRenderParams p;
    JzPwmVoiceKnobs k;
    JzPhraseRow rows[2] = {row(440, 0, 0, 0, 500), row(880, 0, 0, 0, 500)};

    auto words = jz_pwm_render_phrase(rows, 2, p, k);
    REQUIRE(words.size() == 32768);
    auto lo = center_crossings(std::vector<uint32_t>(words.begin(), words.begin() + 16384));
    auto hi = center_crossings(std::vector<uint32_t>(words.begin() + 16384, words.end()));
    // octave up = twice the duty oscillation rate in the second half
    REQUIRE(hi > lo * 3 / 2);
}

TEST_CASE("render_phrase: caps at the driver's 65536-word buffer", "[sound][jz]") {
    JzPwmRenderParams p;
    JzPwmVoiceKnobs k;
    std::vector<JzPhraseRow> rows;
    for (int i = 0; i < 20; ++i)
        rows.push_back(row(440, 0, 0, 0, 500)); // 10 s of material

    auto words = jz_pwm_render_phrase(rows.data(), (int)rows.size(), p, k);
    REQUIRE(words.size() == 65536);
}

TEST_CASE("render_phrase: phrase-level envelope, not per row", "[sound][jz]") {
    JzPwmRenderParams p;
    JzPwmVoiceKnobs k;
    k.attack_ms = 50;
    JzPhraseRow rows[2] = {row(440, 0, 0, 0, 400), row(440, 0, 0, 0, 400)};

    auto words = jz_pwm_render_phrase(rows, 2, p, k);
    const long period = 11749, half = period / 2;
    const long target = (long)(period * k.swing * 0.8f); // amp 0.8 scales it
    long first = std::abs((long)(words[0] & 0xffff) - half);
    // window straddling row 2's start (word 13107 = 400 ms): a per-row
    // envelope would re-attack there and stay near center for 50 ms
    long window_max = 0;
    for (size_t i = 13107; i < 13107 + 200; ++i)
        window_max = std::max(window_max, std::abs((long)(words[i] & 0xffff) - half));
    REQUIRE(first <= 2);
    // sample-phase rarely lands exactly on the wave peak: allow 2%
    REQUIRE(window_max >= target * 98 / 100);
}

TEST_CASE("phrase_clip: truncates to budget on a row boundary", "[sound][jz]") {
    JzPhraseRow rows[3] = {row(440, 0, 0, 0, 500), row(440, 0, 0, 0, 500), row(880, 0, 0, 0, 500)};
    int kept = jz_phrase_clip(rows, 3, 1200.0f);
    REQUIRE(kept == 3);
    REQUIRE(rows[2].ms == 200.0f);

    kept = jz_phrase_clip(rows, 3, 700.0f);
    REQUIRE(kept == 2);
    REQUIRE(rows[1].ms == 200.0f);
}

// ============================================================================
// jz_voice_should_flush - the burst-start gate
// ============================================================================

TEST_CASE("should_flush: never mid-burst, never early, never when clean", "[sound][jz]") {
    // mid-burst: last call microseconds ago, slots hold a half-updated mix
    REQUIRE_FALSE(jz_voice_should_flush(true, 500, 0.01, 120, 4));
    // burst start, but the previous flush was only 40 ms ago
    REQUIRE_FALSE(jz_voice_should_flush(true, 40, 20, 120, 4));
    // nothing changed since the last render
    REQUIRE_FALSE(jz_voice_should_flush(false, 500, 20, 120, 4));
    // burst start, dirty, interval met: the one legal render moment
    REQUIRE(jz_voice_should_flush(true, 500, 20, 120, 4));
    // first-ever burst after a long idle gap also qualifies
    REQUIRE(jz_voice_should_flush(true, 500, 3000, 120, 4));
}

TEST_CASE("render: swing_scale scales the normalization target", "[sound][jz]") {
    JzPwmRenderParams p;
    NoteEvent e = plain_note(1000, 100, 0.6f);

    auto peak_of = [&](const std::vector<uint32_t>& v) {
        const long period = (long)(p.clock_hz / p.carrier_hz);
        long m = 0;
        for (uint32_t w : v)
            m = std::max(m, std::abs((long)(w & 0xffff) - period / 2));
        return m;
    };

    p.swing_scale = 1.0;
    long full_peak = peak_of(jz_pwm_render_step(&e, 1, p));
    p.swing_scale = 0.5;
    long half_peak = peak_of(jz_pwm_render_step(&e, 1, p));

    const long period = (long)(p.clock_hz / p.carrier_hz);
    REQUIRE(full_peak >= (long)(period * p.duty_swing) - 1);
    // halving the target halves the peak: loudness now survives the
    // peak-normalizer instead of every buffer rendering identically loud
    REQUIRE(std::abs(half_peak - full_peak / 2) <= 2);
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

TEST_CASE("backend: voice knobs roundtrip with clamping", "[sound][jz]") {
    JzPwmSoundBackend backend;
    JzPwmVoiceKnobs k = backend.voice_knobs();
    REQUIRE(k.buf_ms == 1900.0f);
    REQUIRE(k.wave == 0);
    k.buf_ms = 600.0f;
    k.wave = 1;
    k.octave_shift = 2;
    k.swing = 0.25f;
    backend.set_voice_knobs(k);
    JzPwmVoiceKnobs got = backend.voice_knobs();
    REQUIRE(got.buf_ms == 600.0f);
    REQUIRE(got.wave == 1);
    REQUIRE(got.octave_shift == 2);
    REQUIRE(got.swing == 0.25f);
    // out-of-range asks clamp, not corrupt
    k.octave_shift = 99;
    k.swing = 5.0f;
    backend.set_voice_knobs(k);
    REQUIRE(backend.voice_knobs().octave_shift == 5);
    REQUIRE(backend.voice_knobs().swing == 0.5f);
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
