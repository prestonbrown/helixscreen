// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_HAS_TRACKER

#include "tracker_player.h"

#include <cmath>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::audio;

// ---------------------------------------------------------------------------
// Mock backend that records voice state for verification
// ---------------------------------------------------------------------------

class TrackerMockBackend : public SoundBackend {
  public:
    /// voices=4 mimics SDL/ALSA; voices=1 mimics the mono PWM/M300 path
    explicit TrackerMockBackend(int voices = 4) : voice_slots_(voices) {}

    struct VoiceState {
        float freq = 0;
        float amplitude = 0;
        float duty = 0;
        bool active = false;
        Waveform waveform = Waveform::SQUARE;
    };

    void set_tone(float freq_hz, float amplitude, float duty_cycle) override {
        set_voice(0, freq_hz, amplitude, duty_cycle);
    }

    void silence() override {
        silence_voice(0);
    }

    void set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) override {
        if (slot >= 0 && slot < 4) {
            voices[slot].freq = freq_hz;
            voices[slot].amplitude = amplitude;
            voices[slot].duty = duty_cycle;
            voices[slot].active = true;
        }
    }

    void set_voice_waveform(int slot, Waveform w) override {
        if (slot >= 0 && slot < 4) {
            voices[slot].waveform = w;
        }
    }

    void silence_voice(int slot) override {
        if (slot >= 0 && slot < 4) {
            voices[slot].freq = 0;
            voices[slot].amplitude = 0;
            voices[slot].active = false;
        }
    }

    int voice_count() const override {
        return voice_slots_;
    }
    bool supports_waveforms() const override {
        return true;
    }

    VoiceState voices[4]{};

  private:
    int voice_slots_ = 4;
};

// ---------------------------------------------------------------------------
// Helper: build minimal TrackerModule programmatically
// ---------------------------------------------------------------------------

/// Create a module with the given patterns.
/// Each pattern is a vector of TrackerNote (rows*4 entries).
static TrackerModule make_module(const std::vector<uint8_t>& order_list,
                                 const std::vector<std::vector<TrackerNote>>& patterns,
                                 uint16_t rows_per_pattern = 64, uint8_t speed = 6,
                                 uint8_t tempo = 125) {
    TrackerModule mod;
    mod.order = order_list;
    mod.num_orders = static_cast<uint8_t>(order_list.size());
    mod.patterns = patterns;
    mod.rows_per_pattern = rows_per_pattern;
    mod.speed = speed;
    mod.tempo = tempo;
    // Default instrument
    mod.instruments.push_back({Waveform::SQUARE, 1.0f, 0.0f});
    return mod;
}

/// Create an N-row pattern (4 channels), all empty except specified entries
static std::vector<TrackerNote> empty_pattern(int rows) {
    return std::vector<TrackerNote>(static_cast<size_t>(rows * 4));
}

/// Fire exactly one tracker tick at the default tempo (125 BPM = 20ms/tick)
static void fire_one_tick(TrackerPlayer& player, int tempo = 125) {
    const float ms_per_tick = 2500.0f / static_cast<float>(tempo);
    player.tick(ms_per_tick);
}

/// Create a player with volume override for testing (no LVGL subjects needed)
static std::unique_ptr<TrackerPlayer> make_player(std::shared_ptr<SoundBackend> backend) {
    auto p = std::make_unique<TrackerPlayer>(std::move(backend));
    p->set_volume_override(100);
    return p;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("TrackerPlayer basic playback starts and stops", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // A 2-row module with a note on ch0 row 0
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0, 0}; // A-4, instrument 1

    auto mod = make_module({0}, {pat}, 2);
    player->load(mod);

    REQUIRE_FALSE(player->is_playing());

    player->play();
    REQUIRE(player->is_playing());

    // Backend should have a tone on voice 0
    REQUIRE(backend->voices[0].active);
    REQUIRE(backend->voices[0].freq > 0);

    player->stop();
    REQUIRE_FALSE(player->is_playing());
    REQUIRE_FALSE(backend->voices[0].active);
}

TEST_CASE("TrackerPlayer fallback emits note frequency, not sample rate", "[tracker][player]") {
    // Real MOD rows carry an Amiga period. The fallback used to emit
    // AMIGA_CLOCK/period — the Paula SAMPLE-playback rate, 8-14 kHz for
    // typical notes, ultrasonic on a piezo with 2-4 kHz resonance. A melody
    // must emit the equal-tempered note frequency instead.
    const uint8_t c4 = 49; // A-4 is 58 in this numbering; C-4 = 58 - 9
    REQUIRE(TrackerModule::note_to_freq(c4) == Catch::Approx(261.63f).margin(0.1f));

    for (uint8_t note : {uint8_t(25) /* C-2, low */, c4, uint8_t(72) /* B-5, high */}) {
        auto backend = std::make_shared<TrackerMockBackend>();
        auto player = make_player(backend);

        auto pat = empty_pattern(2);
        pat[0] = {note, 1, 0x00, 0x00, TrackerModule::note_to_period(note)};

        auto mod = make_module({0}, {pat}, 2);
        player->load(mod);
        player->play();

        REQUIRE(backend->voices[0].active);
        // The NOTE frequency (e.g. C-4 = 261.6 Hz), never the sample-rate
        // value (AMIGA_CLOCK/428 = 8289 Hz)
        REQUIRE(backend->voices[0].freq ==
                Catch::Approx(TrackerModule::note_to_freq(note)).margin(0.1f));
        // Audible on a piezo
        REQUIRE(backend->voices[0].freq > 50.0f);
        REQUIRE(backend->voices[0].freq < 4000.0f);

        player->stop();
    }
}

TEST_CASE("TrackerPlayer mono backend follows the highest voice, not channel 0",
          "[tracker][player]") {
    // crocketts_theme parks the bass on channel 0; a mono backend pinned to
    // slot 0 would play the bass and drop the melody.
    const uint8_t bass = 34; // A-2, 110 Hz — the on-device bass register
    const uint8_t lead = 58; // A-4, 440 Hz
    REQUIRE(TrackerModule::note_to_freq(lead) > 2.0f * TrackerModule::note_to_freq(bass));

    {
        // Bass on ch0, lead on ch1: mono must emit the LEAD on slot 0
        auto backend = std::make_shared<TrackerMockBackend>(/*voices=*/1);
        auto player = make_player(backend);

        auto pat = empty_pattern(2);
        pat[0] = {bass, 1, 0x00, 0x00};
        pat[1] = {lead, 1, 0x00, 0x00};

        player->load(make_module({0}, {pat}, 2));
        player->play();

        REQUIRE(backend->voices[0].active);
        REQUIRE(backend->voices[0].freq ==
                Catch::Approx(TrackerModule::note_to_freq(lead)).margin(0.1f));
        // Mono path must not touch the other slots
        REQUIRE_FALSE(backend->voices[1].active);
        player->stop();
    }

    {
        // Only the bass active: mono still emits it (falls back, not silent)
        auto backend = std::make_shared<TrackerMockBackend>(/*voices=*/1);
        auto player = make_player(backend);

        auto pat = empty_pattern(2);
        pat[0] = {bass, 1, 0x00, 0x00};

        player->load(make_module({0}, {pat}, 2));
        player->play();

        REQUIRE(backend->voices[0].active);
        REQUIRE(backend->voices[0].freq ==
                Catch::Approx(TrackerModule::note_to_freq(bass)).margin(0.1f));
        player->stop();
    }
}

TEST_CASE("TrackerPlayer silences the backend when the module ends", "[tracker][player]") {
    // A tone-mode backend holds whatever the last set_voice left it emitting;
    // without an explicit silence at module end the final note sticks on
    // forever (Preston heard exactly that: one solid pitch after the song).
    auto backend = std::make_shared<TrackerMockBackend>(/*voices=*/1);
    auto player = make_player(backend);

    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0x00, 0x00}; // A-4 on row 0
    // Fresh note on the FINAL row: the channel is attacked and loud right up
    // to the module end, so only an explicit end-of-module silence can turn
    // it off (a hold-decay or empty-row path must not be what silences it).
    pat[4] = {46, 1, 0x00, 0x00}; // A-3 on row 1 (index 4 = row 1, channel 0)

    // speed=1: one tick per row; two ticks exhaust both rows
    player->load(make_module({0}, {pat}, 2, /*speed=*/1));
    player->play();
    REQUIRE(backend->voices[0].active); // setup reached: a note is sounding

    fire_one_tick(*player);             // advance to row 1 (fresh A-3 applied)
    REQUIRE(backend->voices[0].active); // probe: the final row's note is on
    fire_one_tick(*player);             // wrap past the final row -> module end

    REQUIRE_FALSE(player->is_playing());      // the module really ended
    REQUIRE_FALSE(backend->voices[0].active); // ... and the channel went off

    // And it stays off — no resurrection on a stray late tick
    fire_one_tick(*player);
    REQUIRE_FALSE(backend->voices[0].active);
}

TEST_CASE("TrackerPlayer row advances after speed ticks", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // 4-row module, speed=3
    auto pat = empty_pattern(4);
    pat[0] = {58, 1, 0, 0}; // Row 0: A-4

    auto mod = make_module({0}, {pat}, 4, /*speed=*/3);
    player->load(mod);
    player->play();

    REQUIRE(player->current_row() == 0);
    REQUIRE(player->current_tick() == 0);

    // After 3 ticks, should advance to row 1
    fire_one_tick(*player); // tick becomes 1
    fire_one_tick(*player); // tick becomes 2
    fire_one_tick(*player); // tick wraps to 0, row advances to 1

    REQUIRE(player->current_row() == 1);
    REQUIRE(player->current_tick() == 0);
}

TEST_CASE("TrackerPlayer end of module stops playback", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // 2-row module, speed=1
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0, 0}; // Row 0: note

    auto mod = make_module({0}, {pat}, 2, /*speed=*/1);
    player->load(mod);
    player->play();

    REQUIRE(player->is_playing());

    // Tick 1: advance to row 1, process it
    fire_one_tick(*player);
    REQUIRE(player->is_playing());
    REQUIRE(player->current_row() == 1);

    // Tick 2: advance past row 1 -> end of pattern -> end of module
    fire_one_tick(*player);
    REQUIRE_FALSE(player->is_playing());
}

TEST_CASE("TrackerPlayer set volume (Cxx)", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Row 0: note with effect C32 (set volume to 32/64 = 0.5)
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0x0C, 32};

    auto mod = make_module({0}, {pat}, 2);
    player->load(mod);
    player->play();

    auto snap = player->get_channel(0);
    REQUIRE(snap.volume == Catch::Approx(0.5f));
}

TEST_CASE("TrackerPlayer set speed (F0x) and tempo (Fxx)", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Row 0: effect F02 -> set speed to 2
    auto pat = empty_pattern(4);
    pat[0] = {58, 1, 0x0F, 2};

    auto mod = make_module({0}, {pat}, 4, /*speed=*/6);
    player->load(mod);
    player->play();

    // Speed should now be 2 (not 6)
    // After 2 ticks, row should advance
    fire_one_tick(*player); // tick 1
    fire_one_tick(*player); // tick wraps, row 1

    REQUIRE(player->current_row() == 1);

    // Now test tempo: row 1, ch0 has F80 (128 >= 32 -> tempo)
    auto pat2 = empty_pattern(2);
    pat2[0] = {58, 1, 0x0F, 128};

    auto mod2 = make_module({0}, {pat2}, 2, /*speed=*/6, /*tempo=*/125);
    player->load(mod2);
    player->play();

    // After play(), tempo should be 128 (set during process_row)
    // We verify indirectly: ms_per_tick = 2500/128 ~ 19.53ms
    fire_one_tick(*player, 128);
    REQUIRE(player->current_tick() == 1);
}

TEST_CASE("TrackerPlayer arpeggio (0xy)", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Row 0: A-4 (note 58) with arpeggio effect 037 -> x=3, y=7
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0x00, 0x37};

    auto mod = make_module({0}, {pat}, 2, /*speed=*/6);
    player->load(mod);
    player->play();

    const float base = TrackerModule::note_to_freq(58);
    const float semi3 = base * std::pow(2.0f, 3.0f / 12.0f);
    const float semi7 = base * std::pow(2.0f, 7.0f / 12.0f);

    // Tick 0: base freq
    auto snap0 = player->get_channel(0);
    REQUIRE(snap0.freq == Catch::Approx(base).margin(0.1f));

    // Tick 1: arp_tick cycles to 1 -> semi3
    fire_one_tick(*player);
    auto snap1 = player->get_channel(0);
    REQUIRE(snap1.freq == Catch::Approx(semi3).margin(0.1f));

    // Tick 2: arp_tick cycles to 2 -> semi7
    fire_one_tick(*player);
    auto snap2 = player->get_channel(0);
    REQUIRE(snap2.freq == Catch::Approx(semi7).margin(0.1f));

    // Tick 3: arp_tick cycles back to 0 -> base
    fire_one_tick(*player);
    auto snap3 = player->get_channel(0);
    REQUIRE(snap3.freq == Catch::Approx(base).margin(0.1f));
}

TEST_CASE("TrackerPlayer volume slide (Axy)", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Row 0: note at full volume, effect C32 (set volume to 0.5)
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0x0C, 32};

    // Row 1: volume slide up
    pat[4] = {0, 0, 0x0A, 0x10}; // x=1, y=0 -> slide up

    auto mod = make_module({0}, {pat}, 2, /*speed=*/4);
    player->load(mod);
    player->play();

    // Row 0 processed: volume = 0.5
    auto snap = player->get_channel(0);
    REQUIRE(snap.volume == Catch::Approx(0.5f));

    // Advance to row 1 (4 ticks at speed=4)
    for (int i = 0; i < 4; ++i)
        fire_one_tick(*player);

    // Now at row 1, tick 0. Volume slide hasn't applied yet (tick 0).
    REQUIRE(player->current_row() == 1);
    snap = player->get_channel(0);
    REQUIRE(snap.volume == Catch::Approx(0.5f));

    // Tick 1 of row 1: volume += 1/64
    fire_one_tick(*player);
    snap = player->get_channel(0);
    REQUIRE(snap.volume == Catch::Approx(0.5f + 1.0f / 64.0f).margin(0.001f));

    // Tick 2: volume += another 1/64
    fire_one_tick(*player);
    snap = player->get_channel(0);
    REQUIRE(snap.volume == Catch::Approx(0.5f + 2.0f / 64.0f).margin(0.001f));
}

TEST_CASE("TrackerPlayer pattern break (Dxx)", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Pattern 0: row 0 has pattern break D02 -> jump to order 1, row 2
    auto pat0 = empty_pattern(4);
    pat0[0] = {58, 1, 0x0D, 0x02}; // Break to row 2

    // Pattern 1: 4 rows, row 2 has a note
    auto pat1 = empty_pattern(4);
    pat1[8] = {58, 1, 0, 0}; // Row 2, ch0

    auto mod = make_module({0, 1}, {pat0, pat1}, 4, /*speed=*/1);
    player->load(mod);
    player->play();

    // Row 0 of pattern 0 sets next_order_=1, next_row_=2
    // After 1 tick (speed=1), advance_row jumps to order 1, row 2
    fire_one_tick(*player);

    REQUIRE(player->current_order() == 1);
    REQUIRE(player->current_row() == 2);
}

TEST_CASE("TrackerPlayer porta up (1xx) slides frequency", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Row 0: note + porta up effect
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0x01, 0x10}; // Porta up, data=16

    auto mod = make_module({0}, {pat}, 2, /*speed=*/4);
    player->load(mod);
    player->play();

    const float base = TrackerModule::note_to_freq(58);
    auto snap = player->get_channel(0);
    REQUIRE(snap.freq == Catch::Approx(base).margin(0.1f));

    // Tick 1: freq should increase
    fire_one_tick(*player);
    snap = player->get_channel(0);
    REQUIRE(snap.freq > base);
}

TEST_CASE("TrackerPlayer note cut (ECx)", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Row 0: note with EC2 -> cut at tick 2
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0x0E, 0xC2};

    auto mod = make_module({0}, {pat}, 2, /*speed=*/6);
    player->load(mod);
    player->play();

    // Tick 0: note is active
    REQUIRE(player->get_channel(0).active);

    // Tick 1: still active
    fire_one_tick(*player);
    REQUIRE(player->get_channel(0).active);

    // Tick 2: cut
    fire_one_tick(*player);
    REQUIRE_FALSE(player->get_channel(0).active);
}

TEST_CASE("TrackerPlayer position jump (Bxx)", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    // Pattern 0: row 0 has position jump to order 0 (loop)
    auto pat = empty_pattern(2);
    pat[0] = {58, 1, 0x0B, 0x00}; // Jump to order 0

    auto mod = make_module({0}, {pat}, 2, /*speed=*/1);
    player->load(mod);
    player->play();

    // After tick: advance_row sees next_order_=0, jumps back
    fire_one_tick(*player);
    REQUIRE(player->current_order() == 0);
    REQUIRE(player->current_row() == 0);
    REQUIRE(player->is_playing());
}

TEST_CASE("TrackerPlayer multiple ticks per call catch up", "[tracker][player]") {
    auto backend = std::make_shared<TrackerMockBackend>();
    auto player = make_player(backend);

    auto pat = empty_pattern(8);
    pat[0] = {58, 1, 0, 0};

    // speed=1, tempo=125 -> 20ms per tick
    auto mod = make_module({0}, {pat}, 8, /*speed=*/1);
    player->load(mod);
    player->play();

    // Feed 60ms in one call -> should fire 3 ticks, advancing 3 rows
    player->tick(60.0f);
    REQUIRE(player->current_row() == 3);
}

#endif // HELIX_HAS_TRACKER
