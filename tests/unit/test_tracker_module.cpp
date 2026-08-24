// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_HAS_TRACKER

#include "tracker_module.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::audio;

// ---------------------------------------------------------------------------
// Minimal MOD builder — constructs a valid 1-pattern ProTracker MOD in memory
// ---------------------------------------------------------------------------

static std::vector<uint8_t> build_minimal_mod() {
    // Size: 1084 (header) + 1 pattern * 64 rows * 4 channels * 4 bytes = 1084 + 1024 = 2108
    constexpr size_t HEADER_SIZE = 1084;
    constexpr size_t PATTERN_SIZE = 64 * 4 * 4;
    std::vector<uint8_t> buf(HEADER_SIZE + PATTERN_SIZE, 0x00);

    // Song title (20 bytes) — "TEST MOD"
    const char* title = "TEST MOD";
    std::memcpy(buf.data(), title, std::strlen(title));

    // Instrument 1 (offset 20): volume=64, finetune=0
    // Each instrument: 30 bytes: 22-byte name, 2-byte length, 1-byte finetune, 1-byte volume, ...
    size_t inst1_offset = 20;
    buf[inst1_offset + 24] = 0x00; // finetune = 0
    buf[inst1_offset + 25] = 64;   // volume = 64 (max)

    // Song length = 1 pattern
    buf[950] = 0x01;

    // Order table: pattern 0 at position 0
    buf[952] = 0x00;

    // Magic "M.K." at offset 1080
    buf[1080] = 'M';
    buf[1081] = '.';
    buf[1082] = 'K';
    buf[1083] = '.';

    // Pattern 0, row 0, channel 0: C-4 (scientific) with instrument 1.
    // C-4 scientific = 261.63 Hz = note 49 in our table (A-4=note 58, C-4 is 9 semitones below).
    // In ProTracker's period table, note 49 (0-based index 48) has Amiga period 107 = 0x006B.
    // (ProTracker calls this "C-5" in its own octave numbering — irrelevant here.)
    //
    // Encoding: bytes b0,b1,b2,b3
    //   period = ((b0 & 0x0F) << 8) | b1
    //   instrument = (b0 & 0xF0) | ((b2 >> 4) & 0x0F)
    // For period 107 = 0x006B, instrument 1 (hi=0, lo=1):
    //   b0 = 0x00 (period hi = 0, instrument hi = 0)
    //   b1 = 0x6B (period lo = 107)
    //   b2 = 0x10 (instrument lo = 1, effect = 0)
    //   b3 = 0x00 (effect data = 0)
    size_t cell_offset = HEADER_SIZE; // pattern 0, row 0, channel 0
    buf[cell_offset + 0] = 0x00;      // period hi = 0, instrument hi = 0
    buf[cell_offset + 1] = 0x6B;      // period lo = 107 → period = 107
    buf[cell_offset + 2] = 0x10;      // instrument lo = 1, effect = 0
    buf[cell_offset + 3] = 0x00;      // effect data = 0

    return buf;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("TrackerModule: reject garbage data", "[tracker]") {
    // Random bytes with no valid magic
    std::vector<uint8_t> garbage(2048, 0xAB);
    // No MOD magic at 1080, no MED magic at 0
    auto result = TrackerModule::load_from_memory(garbage.data(), garbage.size());
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("TrackerModule: reject too-small buffer", "[tracker]") {
    SECTION("4 bytes — too small for MOD, no MED magic") {
        std::vector<uint8_t> tiny = {0x00, 0x01, 0x02, 0x03};
        auto result = TrackerModule::load_from_memory(tiny.data(), tiny.size());
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("1083 bytes — one byte short of MOD header") {
        std::vector<uint8_t> almost(1083, 0x00);
        // Put MOD magic where it would be if 1 byte larger
        auto result = TrackerModule::load_from_memory(almost.data(), almost.size());
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("empty buffer") {
        auto result = TrackerModule::load_from_memory(nullptr, 0);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("TrackerModule: parse minimal MOD", "[tracker]") {
    auto buf = build_minimal_mod();
    auto result = TrackerModule::load_from_memory(buf.data(), buf.size());

    REQUIRE(result.has_value());
    const TrackerModule& mod = *result;

    SECTION("song order") {
        REQUIRE(mod.num_orders == 1);
        REQUIRE(mod.order[0] == 0); // order[0] = pattern 0
    }

    SECTION("patterns") {
        REQUIRE(mod.patterns.size() == 1);
        REQUIRE(mod.patterns[0].size() == 64 * 4);
    }

    SECTION("instruments") {
        REQUIRE(mod.instruments.size() == 31);
        // Instrument 1 (index 0) should have volume 1.0
        REQUIRE(mod.instruments[0].volume == Catch::Approx(1.0f));
        REQUIRE(mod.instruments[0].finetune == Catch::Approx(0.0f));
    }

    SECTION("note data — row 0 channel 0 is C-4") {
        const TrackerNote& note = mod.patterns[0][0 * 4 + 0];
        // Amiga period 107 → index 48 in our table → note 49 (C-4 scientific, 261.63 Hz)
        // (A-4 = note 58; C-4 is 9 semitones below = note 49)
        REQUIRE(note.note == 49);      // C-4 scientific
        REQUIRE(note.instrument == 1); // instrument 1
        REQUIRE(note.effect == 0);
        REQUIRE(note.effect_data == 0);
    }

    SECTION("C-4 frequency matches note_to_freq") {
        const TrackerNote& note = mod.patterns[0][0 * 4 + 0];
        float freq = TrackerModule::note_to_freq(note.note);
        // C-4 (scientific) ≈ 261.63 Hz
        REQUIRE(freq == Catch::Approx(261.63f).epsilon(0.01f));
    }

    SECTION("rows per pattern") {
        REQUIRE(mod.rows_per_pattern == 64);
    }
}

TEST_CASE("TrackerModule: note_to_freq", "[tracker]") {
    SECTION("note 0 = silence") {
        REQUIRE(TrackerModule::note_to_freq(0) == Catch::Approx(0.0f));
    }

    SECTION("note 58 = A-4 = 440 Hz") {
        float freq = TrackerModule::note_to_freq(58);
        REQUIRE(freq == Catch::Approx(440.0f).epsilon(0.001f));
    }

    SECTION("note 49 = C-4 ≈ 261.63 Hz") {
        // A-4 = note 58. C-4 is 9 semitones below A-4:
        //   C4 C#4 D4 D#4 E4 F4 F#4 G4 G#4 A4 — 9 steps up from C4 → C4 = 58 - 9 = 49
        float freq = TrackerModule::note_to_freq(49);
        REQUIRE(freq == Catch::Approx(261.63f).epsilon(0.01f));
    }

    SECTION("octave relationship: note 58+12 = A-5 = 880 Hz") {
        float freq = TrackerModule::note_to_freq(70);
        REQUIRE(freq == Catch::Approx(880.0f).epsilon(0.01f));
    }

    SECTION("octave relationship: note 58-12 = A-3 = 220 Hz") {
        float freq = TrackerModule::note_to_freq(46);
        REQUIRE(freq == Catch::Approx(220.0f).epsilon(0.01f));
    }
}

TEST_CASE("TrackerModule: pattern data integrity — row 1 is empty", "[tracker]") {
    auto buf = build_minimal_mod();
    auto result = TrackerModule::load_from_memory(buf.data(), buf.size());
    REQUIRE(result.has_value());

    const TrackerModule& mod = *result;
    // Row 1 was not written — all four channels should be silent (note 0, instrument 0)
    for (int ch = 0; ch < 4; ++ch) {
        const TrackerNote& note = mod.patterns[0][1 * 4 + ch];
        REQUIRE(note.note == 0);
        REQUIRE(note.instrument == 0);
        REQUIRE(note.effect == 0);
        REQUIRE(note.effect_data == 0);
    }
}

TEST_CASE("TrackerModule: MED magic is detected and routed", "[tracker]") {
    // Build a minimal MED-magic buffer (MMD0) — too small to parse successfully,
    // but must be identified as MED (not MOD) and return nullopt for incomplete data.
    std::vector<uint8_t> med(256, 0x00);
    med[0] = 'M';
    med[1] = 'M';
    med[2] = 'D';
    med[3] = '0';

    // The buffer is too small to parse; parse_med should return nullopt gracefully.
    auto result = TrackerModule::load_from_memory(med.data(), med.size());
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("TrackerModule: MED — load crocketts_theme.med", "[tracker]") {
    const std::string path = "assets/sounds/crocketts_theme.med";

    std::ifstream probe(path, std::ios::binary);
    if (!probe.is_open()) {
        SKIP("assets/sounds/crocketts_theme.med not present — skipping MED integration test");
    }
    probe.close();

    auto result = TrackerModule::load(path);
    REQUIRE(result.has_value());

    const TrackerModule& mod = *result;

    SECTION("block count") {
        // The MED file has 9 blocks
        REQUIRE(mod.patterns.size() == 9);
    }

    SECTION("order count") {
        REQUIRE(mod.num_orders > 0);
    }

    SECTION("instruments populated") {
        REQUIRE_FALSE(mod.instruments.empty());
    }

    SECTION("at least one pattern has notes") {
        bool found_note = false;
        for (const auto& pattern : mod.patterns) {
            for (const auto& note : pattern) {
                if (note.note != 0) {
                    found_note = true;
                    break;
                }
            }
            if (found_note)
                break;
        }
        REQUIRE(found_note);
    }

    SECTION("note values in range") {
        for (const auto& pattern : mod.patterns) {
            for (const auto& note : pattern) {
                // note 0 = silence, 1..84 = valid
                REQUIRE(note.note <= 84);
            }
        }
    }
}

#endif // HELIX_HAS_TRACKER
