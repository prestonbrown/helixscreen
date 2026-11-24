// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_HAS_TRACKER

#include "tracker_module.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <fstream>
#include <string_view>

namespace helix::audio {

// Amiga period table — standard ProTracker periods for octaves 0-9
// Index 0 = C-0 (note 1), each entry is the Amiga hardware period value.
// 12 notes per octave: C C# D D# E F F# G G# A A# B
// clang-format off
static const uint16_t kAmigaPeriods[] = {
    // Octave 0 (notes 1-12)
    1712, 1616, 1525, 1440, 1357, 1281, 1209, 1141, 1077, 1017,  960,  907,
    // Octave 1 (notes 13-24)
     856,  808,  762,  720,  678,  640,  604,  570,  538,  508,  480,  453,
    // Octave 2 (notes 25-36)
     428,  404,  381,  360,  339,  320,  302,  285,  269,  254,  240,  226,
    // Octave 3 (notes 37-48)
     214,  202,  190,  180,  170,  160,  151,  143,  135,  127,  120,  113,
    // Octave 4 (notes 49-60)
     107,  101,   95,   90,   85,   80,   75,   71,   67,   63,   60,   56,
    // Octave 5 (notes 61-72)
      53,   50,   47,   45,   42,   40,   37,   35,   33,   31,   30,   28,
    // Octave 6 (notes 73-84) — extended, some trackers support this
      27,   25,   24,   22,   21,   20,   19,   18,   17,   16,   15,   14,
};
// clang-format on

static constexpr int kNumPeriodEntries =
    static_cast<int>(sizeof(kAmigaPeriods) / sizeof(kAmigaPeriods[0]));

/// Find the nearest note number (1-based) for an Amiga period value.
/// Returns 0 if the period is 0 or out of range.
static uint8_t period_to_note(uint16_t period) {
    if (period == 0)
        return 0;

    // Search for closest match — periods decrease as note number increases
    int best_idx = -1;
    uint16_t best_diff = 0xFFFF;

    for (int i = 0; i < kNumPeriodEntries; ++i) {
        uint16_t diff =
            (period > kAmigaPeriods[i]) ? (period - kAmigaPeriods[i]) : (kAmigaPeriods[i] - period);
        if (diff < best_diff) {
            best_diff = diff;
            best_idx = i;
        }
    }

    if (best_idx < 0)
        return 0;
    // Note numbers are 1-based
    return static_cast<uint8_t>(best_idx + 1);
}

uint16_t TrackerModule::note_to_period(uint8_t note) {
    if (note == 0 || note > kNumPeriodEntries)
        return 0;
    return kAmigaPeriods[note - 1];
}

// parse_mod

std::optional<TrackerModule> parse_mod(const uint8_t* data, size_t size) {
    // MOD layout:
    //   0..19   : song title (20 bytes)
    //   20..949 : 31 instruments × 30 bytes each
    //   950     : song length (number of patterns in order list)
    //   951     : restart position (ignored)
    //   952..1079: order table (128 bytes)
    //   1080..1083: magic ("M.K.", "4CHN", etc.)
    //   1084+   : pattern data

    static constexpr size_t kMinSize = 1084;
    static constexpr size_t kNumInstruments = 31;
    static constexpr size_t kInstrumentSize = 30;
    static constexpr size_t kOrderTableOffset = 952;
    static constexpr size_t kOrderTableSize = 128;
    static constexpr size_t kMagicOffset = 1080;
    static constexpr size_t kPatternDataOffset = 1084;
    static constexpr size_t kRowsPerPattern = 64;
    static constexpr size_t CHANNELS = 4;
    static constexpr size_t kBytesPerCell = 4;
    static constexpr size_t kBytesPerRow = CHANNELS * kBytesPerCell;
    static constexpr size_t kBytesPerPattern = kRowsPerPattern * kBytesPerRow;

    if (size < kMinSize) {
        spdlog::debug("tracker: MOD too small ({} bytes, need {})", size, kMinSize);
        return std::nullopt;
    }

    // Verify magic
    // Common magics: "M.K.", "4CHN", "6CHN", "8CHN", "FLT4", "FLT8"
    const char* magic = reinterpret_cast<const char*>(data + kMagicOffset);
    bool valid_magic = (magic[0] == 'M' && magic[1] == '.' && magic[2] == 'K' && magic[3] == '.') ||
                       (magic[0] == '4' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == 'N') ||
                       (magic[0] == '6' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == 'N') ||
                       (magic[0] == '8' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == 'N') ||
                       (magic[0] == 'F' && magic[1] == 'L' && magic[2] == 'T' && magic[3] == '4') ||
                       (magic[0] == 'F' && magic[1] == 'L' && magic[2] == 'T' && magic[3] == '8');

    if (!valid_magic) {
        // Bound the view explicitly. `magic` points 1080 bytes into the file
        // buffer at a fixed-width, NOT NUL-terminated field, and fmt resolves a
        // `const char*` to a string_view via strlen BEFORE applying the `.4`
        // precision — so the scan runs past the end of the buffer on any file
        // whose tail happens to contain no NUL. ASAN: heap-buffer-overflow READ
        // of 1003 bytes, reached from any malformed .mod with debug logging on.
        spdlog::debug("tracker: unrecognised MOD magic '{}'", std::string_view(magic, 4));
        return std::nullopt;
    }

    TrackerModule mod;
    mod.rows_per_pattern = kRowsPerPattern;

    // Parse song length and order table
    uint8_t song_length = data[950];
    if (song_length == 0) {
        spdlog::warn("[TrackerModule] MOD has zero song length");
        return std::nullopt;
    }
    if (song_length > 128) {
        song_length = 128;
    }
    mod.num_orders = song_length;

    // Determine how many patterns are referenced
    uint8_t max_pattern = 0;
    mod.order.resize(kOrderTableSize);
    for (size_t i = 0; i < kOrderTableSize; ++i) {
        uint8_t pat = data[kOrderTableOffset + i];
        mod.order[i] = pat;
        if (i < song_length && pat > max_pattern)
            max_pattern = pat;
    }

    size_t num_patterns = static_cast<size_t>(max_pattern) + 1;
    size_t pattern_data_end = kPatternDataOffset + num_patterns * kBytesPerPattern;
    if (pattern_data_end > size) {
        spdlog::warn("tracker: MOD pattern data extends past end of buffer ({} > {})",
                     pattern_data_end, size);
        // Clamp to what we actually have
        num_patterns = (size - kPatternDataOffset) / kBytesPerPattern;
        if (num_patterns == 0) {
            spdlog::warn("tracker: MOD has no complete patterns");
            return std::nullopt;
        }
    }

    // Parse pattern data
    mod.patterns.resize(num_patterns);
    for (size_t p = 0; p < num_patterns; ++p) {
        auto& pat = mod.patterns[p];
        pat.resize(kRowsPerPattern * CHANNELS);

        size_t pat_offset = kPatternDataOffset + p * kBytesPerPattern;
        bool truncated = false;
        for (size_t row = 0; row < kRowsPerPattern && !truncated; ++row) {
            for (size_t ch = 0; ch < CHANNELS; ++ch) {
                size_t cell_offset = pat_offset + row * kBytesPerRow + ch * kBytesPerCell;
                if (cell_offset + 3 >= size) {
                    spdlog::warn("[TrackerModule] MOD truncated at pattern {} row {} ch {}", p, row,
                                 ch);
                    truncated = true;
                    break;
                }

                uint8_t b0 = data[cell_offset + 0];
                uint8_t b1 = data[cell_offset + 1];
                uint8_t b2 = data[cell_offset + 2];
                uint8_t b3 = data[cell_offset + 3];

                uint16_t period = static_cast<uint16_t>(((b0 & 0x0F) << 8) | b1);
                uint8_t instrument = static_cast<uint8_t>((b0 & 0xF0) | ((b2 >> 4) & 0x0F));
                uint8_t effect = b2 & 0x0F;
                uint8_t effect_data = b3;

                TrackerNote& note = pat[row * CHANNELS + ch];
                note.note = period_to_note(period);
                note.period = period;
                note.instrument = instrument;
                note.effect = effect;
                note.effect_data = effect_data;
            }
        }
    }

    // Parse instrument headers
    // Instrument block starts at offset 20, 31 instruments × 30 bytes
    // Each instrument:
    //   0..21  : name (22 bytes)
    //   22..23 : sample length in words (big-endian)
    //   24     : finetune (signed, low nybble used)
    //   25     : volume (0-64)
    //   26..27 : loop start in words
    //   28..29 : loop length in words

    // Amiga PAL clock for C4 rate calculation
    static constexpr double kAmigaPalClock = 3546895.0;
    static constexpr double kC4Period = 428.0; // period for C-4

    // Finetune period multipliers (ProTracker finetune table)
    // Finetune -8..+7 corresponds to slight period adjustments
    static constexpr double kFinetuneMultiplier[] = {
        // 0    1       2       3       4       5       6       7
        1.0000,
        0.9930,
        0.9860,
        0.9790,
        0.9724,
        0.9659,
        0.9593,
        0.9529,
        // -8   -7      -6      -5      -4      -3      -2      -1
        1.0595,
        1.0524,
        1.0453,
        1.0383,
        1.0313,
        1.0245,
        1.0178,
        1.0088,
    };

    struct InstrumentHeader {
        uint16_t sample_length_words; // in 16-bit words
        int8_t finetune;              // -8..+7
        uint8_t volume;               // 0..64
        uint16_t loop_start_words;
        uint16_t loop_length_words;
    };

    std::vector<InstrumentHeader> inst_headers(kNumInstruments);
    mod.instruments.resize(kNumInstruments);

    for (size_t i = 0; i < kNumInstruments; ++i) {
        size_t inst_offset = 20 + i * kInstrumentSize;
        if (inst_offset + 29 >= size)
            break;

        auto& hdr = inst_headers[i];
        hdr.sample_length_words =
            static_cast<uint16_t>((data[inst_offset + 22] << 8) | data[inst_offset + 23]);
        uint8_t finetune_raw = data[inst_offset + 24] & 0x0F;
        hdr.finetune = (finetune_raw >= 8) ? static_cast<int8_t>(finetune_raw - 16)
                                           : static_cast<int8_t>(finetune_raw);
        hdr.volume = data[inst_offset + 25];
        if (hdr.volume > 64)
            hdr.volume = 64;
        hdr.loop_start_words =
            static_cast<uint16_t>((data[inst_offset + 26] << 8) | data[inst_offset + 27]);
        hdr.loop_length_words =
            static_cast<uint16_t>((data[inst_offset + 28] << 8) | data[inst_offset + 29]);

        TrackerInstrument& inst = mod.instruments[i];
        inst.waveform = Waveform::SQUARE; // fallback for synth mode
        inst.volume = static_cast<float>(hdr.volume) / 64.0f;
        inst.finetune = static_cast<float>(hdr.finetune) / 8.0f;

        // Compute C4 playback rate adjusted for finetune
        int ft_idx = (hdr.finetune >= 0) ? hdr.finetune : (hdr.finetune + 16);
        double adjusted_period = kC4Period * kFinetuneMultiplier[ft_idx];
        inst.c4_rate = static_cast<uint16_t>(kAmigaPalClock / adjusted_period);

        inst.loop_start = static_cast<uint32_t>(hdr.loop_start_words) * 2;
        uint32_t loop_len_bytes = static_cast<uint32_t>(hdr.loop_length_words) * 2;
        // Loop length of 2 bytes (1 word) means no loop in MOD format
        inst.loop_length = (loop_len_bytes > 2) ? loop_len_bytes : 0;
    }

    // Extract PCM sample data — located after all pattern data
    size_t sample_data_offset = kPatternDataOffset + num_patterns * kBytesPerPattern;
    for (size_t i = 0; i < kNumInstruments; ++i) {
        const auto& hdr = inst_headers[i];
        size_t sample_bytes = static_cast<size_t>(hdr.sample_length_words) * 2;
        if (sample_bytes == 0)
            continue;

        if (sample_data_offset + sample_bytes > size) {
            spdlog::debug("tracker: MOD instrument {} sample data truncated "
                          "(need {} bytes at offset {}, file size {})",
                          i + 1, sample_bytes, sample_data_offset, size);
            // Take what we can
            sample_bytes = size - sample_data_offset;
            if (sample_bytes == 0)
                break;
        }

        auto& inst = mod.instruments[i];
        inst.sample_data.resize(sample_bytes);
        for (size_t s = 0; s < sample_bytes; ++s) {
            // MOD samples are 8-bit signed PCM — convert to float [-1.0..1.0]
            auto raw = static_cast<int8_t>(data[sample_data_offset + s]);
            inst.sample_data[s] = static_cast<float>(raw) / 128.0f;
        }

        // Clamp loop parameters to actual sample length
        if (inst.loop_start >= sample_bytes) {
            inst.loop_start = 0;
            inst.loop_length = 0;
        } else if (inst.loop_length > 0 && inst.loop_start + inst.loop_length > sample_bytes) {
            inst.loop_length = static_cast<uint32_t>(sample_bytes) - inst.loop_start;
        }

        mod.has_samples = true;
        sample_data_offset += static_cast<size_t>(hdr.sample_length_words) * 2;
    }

    spdlog::debug("tracker: MOD parsed: {} orders, {} patterns, {} instruments, samples={}",
                  mod.num_orders, mod.patterns.size(), mod.instruments.size(),
                  mod.has_samples ? "yes" : "no");
    return mod;
}

// TrackerModule static methods

float TrackerModule::note_to_freq(uint8_t note) {
    if (note == 0)
        return 0.0f;
    // A-4 = note 58, 440 Hz. Equal temperament.
    return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 58.0f) / 12.0f);
}

std::optional<TrackerModule> TrackerModule::load_from_memory(const uint8_t* data, size_t size) {
    if (!data || size == 0)
        return std::nullopt;

    static constexpr size_t k4MB = 4 * 1024 * 1024;
    if (size > k4MB) {
        spdlog::warn("tracker: file too large ({} bytes), refusing to parse", size);
        return std::nullopt;
    }
    if (size < 4) {
        spdlog::debug("tracker: buffer too small to identify format");
        return std::nullopt;
    }

    // MOD magic lives at offset 1080
    return parse_mod(data, size);
}

std::optional<TrackerModule> TrackerModule::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        spdlog::warn("tracker: cannot open '{}'", path);
        return std::nullopt;
    }

    auto file_size = static_cast<size_t>(f.tellg());
    f.seekg(0);

    std::vector<uint8_t> buf(file_size);
    if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(file_size))) {
        spdlog::warn("tracker: read error on '{}'", path);
        return std::nullopt;
    }

    return load_from_memory(buf.data(), buf.size());
}

} // namespace helix::audio

#endif // HELIX_HAS_TRACKER
