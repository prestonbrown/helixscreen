// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_HAS_TRACKER

#include "tracker_module.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::audio {

// ---------------------------------------------------------------------------
// Big-endian helpers
// ---------------------------------------------------------------------------

static uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// ---------------------------------------------------------------------------
// MED → ProTracker effect remapping
//
// MED uses different command numbers for several effects.
// Commands 0x00-0x04 match ProTracker. Key differences:
//   MED 0x05 = Slide + volume fade  → PT 0x05 (same semantics)
//   MED 0x08 = Hold/Decay           → ignore (no PT equivalent)
//   MED 0x09 = Secondary tempo      → PT 0x0F (set speed, data < 32)
//   MED 0x0D = Volume slide         → PT 0x0A (NOT pattern break!)
//   MED 0x0F = Misc/tempo           → PT 0x0F (set speed/tempo)
//   MED 0x19 = Sample offset        → PT 0x09
//   MED 0x1D = Pattern break        → PT 0x0D
//   MED 0x1E = Pattern delay        → PT 0x0E Exx (extended)
//   MED 0x1F = Note delay/retrig    → depends on nybbles
// ---------------------------------------------------------------------------

static void remap_med_effect(uint8_t med_cmd, uint8_t med_data, uint8_t& pt_cmd, uint8_t& pt_data) {
    switch (med_cmd) {
    case 0x00: // Arpeggio
    case 0x01: // Portamento up
    case 0x02: // Portamento down
    case 0x03: // Tone portamento
    case 0x04: // Vibrato
    case 0x05: // Tone porta + vol slide
    case 0x06: // Vibrato + vol slide
    case 0x07: // Tremolo
        // Same as ProTracker
        pt_cmd = med_cmd;
        pt_data = med_data;
        break;
    case 0x08: // Hold/Decay — MED-specific, no PT equivalent
        pt_cmd = 0;
        pt_data = 0;
        break;
    case 0x09: // Secondary tempo (ticks per row)
        pt_cmd = 0x0F;
        pt_data = (med_data < 32) ? med_data : 0;
        break;
    case 0x0A: // (not used in MED — reserved)
        pt_cmd = 0;
        pt_data = 0;
        break;
    case 0x0B: // Position jump (same as PT)
        pt_cmd = 0x0B;
        pt_data = med_data;
        break;
    case 0x0C: // Set volume (same as PT)
        pt_cmd = 0x0C;
        pt_data = med_data;
        break;
    case 0x0D: // MED volume slide → PT 0x0A
        pt_cmd = 0x0A;
        pt_data = med_data;
        break;
    case 0x0E: // Synth JMP — MED-specific, ignore
        pt_cmd = 0;
        pt_data = 0;
        break;
    case 0x0F: // Misc: set tempo/speed (similar to PT 0x0F)
        // MED 0x0F with data 0x00 = pattern break (stop)
        // MED 0x0F with data 0x01-0xF0 = set tempo
        if (med_data == 0x00) {
            // Pattern break to row 0 of next order
            pt_cmd = 0x0D;
            pt_data = 0x00;
        } else {
            pt_cmd = 0x0F;
            pt_data = med_data;
        }
        break;
    case 0x19: // Sample offset → PT 0x09
        pt_cmd = 0x09;
        pt_data = med_data;
        break;
    case 0x1D: // MED pattern break → PT 0x0D
        pt_cmd = 0x0D;
        pt_data = med_data;
        break;
    case 0x1E: // Pattern delay — ignore for now
        pt_cmd = 0;
        pt_data = 0;
        break;
    case 0x1F: // Note delay / retrigger
        // Upper nybble = delay ticks, lower = retrigger count
        // Map to PT E9x (retrigger) if lower nybble set, else EDx (note delay)
        if ((med_data & 0x0F) != 0) {
            pt_cmd = 0x0E;
            pt_data = static_cast<uint8_t>(0x90 | (med_data & 0x0F));
        } else if ((med_data >> 4) != 0) {
            pt_cmd = 0x0E;
            pt_data = static_cast<uint8_t>(0xD0 | (med_data >> 4));
        } else {
            pt_cmd = 0;
            pt_data = 0;
        }
        break;
    default:
        // Unknown MED command — ignore
        pt_cmd = 0;
        pt_data = 0;
        break;
    }
}

// ---------------------------------------------------------------------------
// parse_med — OctaMED MMD0/1/2/3 parser
//
// Big-endian throughout. All pointer values are absolute file offsets.
//
// MMD0/1 block format: 3 bytes per note
// MMD2/3 block format: 4 bytes per note
//
// The 'song' struct offset depends on format version:
//   MMD0/1: sample table = 63 × 4 = 252 bytes
//   MMD2/3: sample table = 63 × 8 = 504 bytes
// ---------------------------------------------------------------------------

std::optional<TrackerModule> parse_med(const uint8_t* data, size_t size) {
    static constexpr size_t HEADER_SIZE = 20; // bare minimum to read the main header
    static constexpr size_t MAX_INSTRUMENTS = 64;
    static constexpr size_t CHANNELS = 4;

    if (size < HEADER_SIZE) {
        spdlog::debug("tracker: MED too small ({} bytes)", size);
        return std::nullopt;
    }

    // Magic: "MMD0" / "MMD1" / "MMD2" / "MMD3"
    if (data[0] != 'M' || data[1] != 'M' || data[2] != 'D') {
        spdlog::debug("tracker: MED missing MMD magic");
        return std::nullopt;
    }
    uint8_t version = data[3];
    if (version < '0' || version > '3') {
        spdlog::debug("tracker: MED unknown version '{}'", static_cast<char>(version));
        return std::nullopt;
    }

    // Main header (offsets 0-31):
    //   0  : magic (4 bytes)
    //   4  : modlen (uint32)
    //   8  : song_offset (uint32)
    //   12 : (reserved)
    //   16 : blockarr_offset (uint32)
    if (size < 20)
        return std::nullopt;

    uint32_t song_offset = read_be32(data + 8);
    uint32_t blockarr_offset = read_be32(data + 16);

    // Validate pointers are within file
    if (song_offset == 0 || song_offset >= size) {
        spdlog::warn("tracker: MED song_offset 0x{:x} out of range", song_offset);
        return std::nullopt;
    }
    if (blockarr_offset == 0 || blockarr_offset >= size) {
        spdlog::warn("tracker: MED blockarr_offset 0x{:x} out of range", blockarr_offset);
        return std::nullopt;
    }

    // Song struct sample table size depends on version:
    //   MMD0/1: 63 × 4 = 252 bytes (32-bit sample pointers)
    //   MMD2/3: 63 × 8 = 504 bytes (64-bit sample pointers on expanded systems)
    bool is_mmd23 = (version == '2' || version == '3');
    uint32_t sample_table_bytes = is_mmd23 ? 504u : 252u;

    // Fields after sample table (at song_offset + sample_table_bytes):
    //   +0: numblocks (uint16)
    //   +2: songlen   (uint16)
    //   +4: playseq[256] (uint8 each)
    //   +260: deftempo (uint16)
    //   +263: flags (uint8)    — bit 5 (0x20) = BPM mode
    //   +265: tempo2 (uint8)   — ticks per row (speed)
    size_t fields_off = static_cast<size_t>(song_offset) + sample_table_bytes;

    // Need at least +266 bytes from fields_off
    if (fields_off + 266 > size) {
        spdlog::warn("tracker: MED song struct extends past EOF (fields_off={}, size={})",
                     fields_off, size);
        return std::nullopt;
    }

    uint16_t numblocks = read_be16(data + fields_off + 0);
    uint16_t songlen = read_be16(data + fields_off + 2);

    if (numblocks == 0) {
        spdlog::warn("tracker: MED has zero blocks");
        return std::nullopt;
    }
    if (numblocks > 1024) {
        spdlog::warn("tracker: MED implausibly large numblocks={}", numblocks);
        return std::nullopt;
    }

    // playseq: uint8[256], at fields_off + 4
    const uint8_t* playseq = data + fields_off + 4;
    // Many MED files use songlen=1 or songlen=0 even with multiple blocks.
    // OctaMED has a more complex song/section system we don't parse.
    // If songlen <= 1 but multiple blocks exist, play all blocks sequentially.
    uint16_t order_count;
    bool auto_order = false;
    if (songlen <= 1 && numblocks > 1) {
        order_count = numblocks;
        auto_order = true;
    } else if (songlen == 0) {
        order_count = numblocks;
        auto_order = true;
    } else {
        order_count = songlen;
    }
    if (order_count > 256)
        order_count = 256;

    uint16_t deftempo = read_be16(data + fields_off + 260);
    uint8_t flags = data[fields_off + 263];
    uint8_t tempo2 = data[fields_off + 265];

    // Compute BPM and speed
    uint8_t tempo;
    if (flags & 0x20) {
        // BPM mode: deftempo is directly the BPM
        tempo = (deftempo == 0)
                    ? 125u
                    : static_cast<uint8_t>(std::clamp(static_cast<int>(deftempo), 32, 255));
    } else {
        // CIA mode: BPM ≈ 4926 / deftempo (PAL timing)
        if (deftempo == 0) {
            tempo = 125;
        } else {
            int bpm = 4926 / deftempo;
            tempo = static_cast<uint8_t>(std::clamp(bpm, 32, 255));
        }
    }
    uint8_t speed = (tempo2 == 0) ? 6u : tempo2;

    spdlog::debug("tracker: MED MMD{} — {} blocks, songlen={}, tempo={}, speed={}",
                  static_cast<char>(version), numblocks, order_count, tempo, speed);

    // Verify block pointer array fits in file
    size_t blockarr_end = static_cast<size_t>(blockarr_offset) + static_cast<size_t>(numblocks) * 4;
    if (blockarr_end > size) {
        spdlog::warn("tracker: MED block pointer array extends past EOF");
        return std::nullopt;
    }

    TrackerModule mod;
    mod.tempo = tempo;
    mod.speed = speed;
    mod.rows_per_pattern = 64; // default; updated to max across all blocks below
    mod.num_orders = static_cast<uint8_t>(std::min<int>(order_count, 255));

    // Build order table
    mod.order.resize(256, 0);
    if (auto_order) {
        // Auto-generate sequential order: block 0, 1, 2, ...
        for (uint16_t i = 0; i < order_count; ++i) {
            mod.order[i] = static_cast<uint8_t>(i);
        }
        spdlog::debug("tracker: MED auto-generated sequential order for {} blocks", order_count);
    } else {
        for (uint16_t i = 0; i < order_count; ++i) {
            mod.order[i] = playseq[i];
        }
    }

    // Parse blocks (patterns)
    uint16_t max_rows = 64;
    mod.patterns.resize(numblocks);
    for (uint16_t b = 0; b < numblocks; ++b) {
        uint32_t block_ptr = read_be32(data + blockarr_offset + b * 4);
        if (block_ptr == 0 || block_ptr >= size) {
            spdlog::debug("tracker: MED block[{}] ptr=0x{:x} is null/invalid", b, block_ptr);
            mod.patterns[b].resize(CHANNELS * 64, TrackerNote{});
            continue;
        }

        uint16_t num_tracks;
        uint16_t num_lines; // rows - 1
        size_t note_data_off;

        if (is_mmd23) {
            // MMD2/3 block header: uint16 numtracks, uint16 lines, uint32 reserved (4 bytes)
            if (block_ptr + 8 > size) {
                spdlog::warn("tracker: MED block[{}] header out of bounds", b);
                mod.patterns[b].resize(CHANNELS * 64, TrackerNote{});
                continue;
            }
            num_tracks = read_be16(data + block_ptr + 0);
            num_lines = read_be16(data + block_ptr + 2);
            note_data_off = block_ptr + 8;
        } else {
            // MMD0/1 block header: uint8 numtracks, uint8 lines (rows-1)
            if (block_ptr + 2 > size) {
                spdlog::warn("tracker: MED block[{}] header out of bounds", b);
                mod.patterns[b].resize(CHANNELS * 64, TrackerNote{});
                continue;
            }
            num_tracks = data[block_ptr + 0];
            num_lines = data[block_ptr + 1];
            note_data_off = block_ptr + 2;
        }

        uint16_t rows = num_lines + 1;
        // Clamp to sane range
        if (rows == 0 || rows > 256)
            rows = 64;
        if (num_tracks == 0 || num_tracks > 128)
            num_tracks = 4;
        max_rows = std::max(max_rows, rows);

        mod.patterns[b].resize(rows * CHANNELS, TrackerNote{});

        // Each note: for MMD0/1 = 3 bytes, MMD2/3 = 4 bytes
        size_t bytes_per_note = is_mmd23 ? 4 : 3;
        size_t block_data_size = static_cast<size_t>(rows) * num_tracks * bytes_per_note;

        if (note_data_off + block_data_size > size) {
            spdlog::warn(
                "tracker: MED block[{}] note data out of bounds (need {} bytes from 0x{:x})", b,
                block_data_size, note_data_off);
            // Fill what we can below with partial read; rest stays zero
        }

        bool truncated = false;
        for (uint16_t row = 0; row < rows && !truncated; ++row) {
            for (uint16_t ch = 0; ch < num_tracks; ++ch) {
                size_t cell_off =
                    note_data_off + (static_cast<size_t>(row) * num_tracks + ch) * bytes_per_note;
                if (cell_off + bytes_per_note > size) {
                    truncated = true;
                    break;
                }

                // Only store first CHANNELS (4) channels
                if (ch >= CHANNELS)
                    continue;

                TrackerNote& note = mod.patterns[b][row * CHANNELS + ch];

                if (is_mmd23) {
                    // MMD2/3: byte0=note, byte1=instrument, byte2=effect, byte3=effect_data
                    uint8_t raw_note = data[cell_off + 0];
                    uint8_t instr = data[cell_off + 1];
                    uint8_t effect = data[cell_off + 2];
                    uint8_t effect_data = data[cell_off + 3];

                    // MED note 0 = no note, 1..N = actual notes
                    // MED note 1 = C-1 in Amiga octave notation = our note 13
                    // (add 12: MED note 1 → our note 13)
                    note.note = (raw_note == 0) ? 0u
                                                : static_cast<uint8_t>(std::clamp(
                                                      static_cast<int>(raw_note) - 12, 1, 84));
                    note.instrument = instr;

                    // Remap MED effects to ProTracker equivalents.
                    // MED uses different command numbers for several effects.
                    remap_med_effect(effect, effect_data, note.effect, note.effect_data);
                } else {
                    // MMD0/1: 3 bytes per note
                    // byte[0] bits 7-2: note (6 bits)
                    // byte[0] bits 1-0 + byte[1] bits 7-4: instrument (6 bits)
                    // byte[1] bits 3-0: effect command (4 bits — only 0x0-0xF)
                    // byte[2]: effect data
                    uint8_t b0 = data[cell_off + 0];
                    uint8_t b1 = data[cell_off + 1];
                    uint8_t b2 = data[cell_off + 2];

                    uint8_t raw_note = (b0 >> 2) & 0x3F;
                    uint8_t instr = static_cast<uint8_t>(((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F));
                    uint8_t effect = b1 & 0x0F;
                    uint8_t effect_data = b2;

                    note.note = (raw_note == 0) ? 0u
                                                : static_cast<uint8_t>(std::clamp(
                                                      static_cast<int>(raw_note) - 12, 1, 84));
                    note.instrument = instr;
                    // MMD0/1 only has 4-bit effect commands (0x0-0xF).
                    // Commands 0x0-0xC match ProTracker. 0xD = MED volume slide
                    // (not pattern break). 0xE/0xF have MED-specific meanings.
                    remap_med_effect(effect, effect_data, note.effect, note.effect_data);
                }
            }
        }
    }

    mod.rows_per_pattern = max_rows;

    // Create instruments with synthetic waveform fallbacks.
    // Instrument 0 is unused (0 = "no instrument" in tracker data).
    mod.instruments.resize(MAX_INSTRUMENTS);
    for (size_t i = 0; i < MAX_INSTRUMENTS; ++i) {
        mod.instruments[i].volume = 1.0f;
        mod.instruments[i].finetune = 0.0f;
        // Assign waveforms by instrument role (fallback when no PCM sample):
        switch (i) {
        case 1:
            mod.instruments[i].waveform = Waveform::SAW;
            break;
        case 2:
            mod.instruments[i].waveform = Waveform::SAW;
            break;
        case 3:
            mod.instruments[i].waveform = Waveform::TRIANGLE;
            break;
        case 4:
            mod.instruments[i].waveform = Waveform::SAW;
            break;
        case 5:
            mod.instruments[i].waveform = Waveform::SINE;
            break;
        case 6:
            mod.instruments[i].waveform = Waveform::SINE;
            break;
        case 7:
            mod.instruments[i].waveform = Waveform::SQUARE;
            break;
        default:
            static const Waveform FALLBACK[] = {Waveform::TRIANGLE, Waveform::SAW, Waveform::SINE,
                                                Waveform::SQUARE};
            mod.instruments[i].waveform = FALLBACK[i % 4];
            break;
        }
    }

    // Extract PCM sample data from smplarr (sample pointer array)
    uint32_t smplarr_offset = read_be32(data + 24);
    if (smplarr_offset > 0 && smplarr_offset < size) {
        size_t max_instruments = std::min(static_cast<size_t>(63), (size - smplarr_offset) / 4);
        for (size_t i = 0; i < max_instruments && i < mod.instruments.size(); ++i) {
            uint32_t sample_ptr = read_be32(data + smplarr_offset + i * 4);
            if (sample_ptr == 0 || sample_ptr >= size)
                continue;

            // InstrHdr: length(4 bytes), type(2 bytes)
            if (sample_ptr + 6 > size)
                continue;
            uint32_t sample_length = read_be32(data + sample_ptr);
            uint16_t sample_type = read_be16(data + sample_ptr + 4);

            if (sample_length == 0)
                continue;

            // MED sample type flags: bit 4 = stereo, bit 5 = 16-bit
            bool is_16bit = (sample_type & 0x20) != 0;
            bool is_stereo = (sample_type & 0x10) != 0;

            // PCM data follows the 6-byte InstrHdr
            size_t pcm_offset = sample_ptr + 6;
            if (pcm_offset + sample_length > size) {
                sample_length = static_cast<uint32_t>(size - pcm_offset);
            }

            if (is_16bit) {
                // 16-bit big-endian signed PCM
                size_t num_samples = sample_length / 2;
                if (is_stereo)
                    num_samples /= 2; // mix to mono
                mod.instruments[i].sample_data.resize(num_samples);
                for (size_t s = 0; s < num_samples; ++s) {
                    size_t byte_off = pcm_offset + s * 2 * (is_stereo ? 2 : 1);
                    if (byte_off + 1 >= size)
                        break;
                    auto raw = static_cast<int16_t>((static_cast<int16_t>(data[byte_off]) << 8) |
                                                    data[byte_off + 1]);
                    mod.instruments[i].sample_data[s] = static_cast<float>(raw) / 32768.0f;
                }
            } else {
                // 8-bit signed PCM
                size_t num_samples = is_stereo ? sample_length / 2 : sample_length;
                mod.instruments[i].sample_data.resize(num_samples);
                for (size_t s = 0; s < num_samples; ++s) {
                    size_t byte_off = pcm_offset + s * (is_stereo ? 2 : 1);
                    if (byte_off >= size)
                        break;
                    auto raw = static_cast<int8_t>(data[byte_off]);
                    mod.instruments[i].sample_data[s] = static_cast<float>(raw) / 128.0f;
                }
            }

            // MED loop points are in expdata which we don't parse yet
            mod.instruments[i].loop_start = 0;
            mod.instruments[i].loop_length = 0;
            mod.instruments[i].c4_rate = 8287; // standard Amiga C-4 rate

            mod.has_samples = true;
        }

        if (mod.has_samples) {
            spdlog::debug("tracker: MED extracted PCM samples");
        }
    }

    spdlog::debug("tracker: MED parsed: {} blocks, {} orders, tempo={}, speed={}",
                  mod.patterns.size(), mod.num_orders, mod.tempo, mod.speed);
    return mod;
}

} // namespace helix::audio

#endif // HELIX_HAS_TRACKER
