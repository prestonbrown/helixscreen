// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sound_theme.h"

#include "core/text_io.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "hv/json.hpp"

using json = nlohmann::json;
namespace json_util = helix::json_util;

// ============================================================================
// Note name -> semitone offset from C
// ============================================================================

static const std::unordered_map<std::string, int>& note_semitones() {
    static const std::unordered_map<std::string, int> map = {
        {"C", 0},  {"C#", 1}, {"Db", 1},  {"D", 2},   {"D#", 3}, {"Eb", 3},
        {"E", 4},  {"F", 5},  {"F#", 6},  {"Gb", 6},  {"G", 7},  {"G#", 8},
        {"Ab", 8}, {"A", 9},  {"A#", 10}, {"Bb", 10}, {"B", 11},
    };
    return map;
}

// ============================================================================
// Waveform string -> enum
// ============================================================================

static Waveform parse_waveform(const std::string& s) {
    if (s == "square")
        return Waveform::SQUARE;
    if (s == "saw")
        return Waveform::SAW;
    if (s == "triangle")
        return Waveform::TRIANGLE;
    if (s == "sine")
        return Waveform::SINE;
    spdlog::warn("[SoundTheme] Unknown waveform '{}', defaulting to square", s);
    return Waveform::SQUARE;
}

// ============================================================================
// Clamping helpers
// ============================================================================

static float clamp_freq(float freq) {
    return std::clamp(freq, 20.0f, 20000.0f);
}

static float clamp_duration(float dur) {
    return std::clamp(dur, 1.0f, 30000.0f);
}

static float clamp_velocity(float vel) {
    return std::clamp(vel, 0.0f, 1.0f);
}

// ============================================================================
// ADSR parsing
// ============================================================================

static ADSREnvelope parse_envelope(const json& j, const ADSREnvelope& defaults) {
    ADSREnvelope env = defaults;
    env.attack_ms = json_util::read_field<float>(j, "a").value_or(env.attack_ms);
    env.decay_ms = json_util::read_field<float>(j, "d").value_or(env.decay_ms);
    env.sustain_level = json_util::read_field<float>(j, "s").value_or(env.sustain_level);
    env.release_ms = json_util::read_field<float>(j, "r").value_or(env.release_ms);
    return env;
}

// ============================================================================
// LFO parsing
// ============================================================================

static LFOParams parse_lfo(const json& j) {
    LFOParams lfo;
    lfo.target = json_util::read_field<std::string>(j, "target").value_or(lfo.target);
    lfo.rate = json_util::read_field<float>(j, "rate").value_or(lfo.rate);
    lfo.depth = json_util::read_field<float>(j, "depth").value_or(lfo.depth);
    return lfo;
}

// ============================================================================
// Sweep parsing
// ============================================================================

static SweepParams parse_sweep(const json& j) {
    SweepParams sweep;
    sweep.target = json_util::read_field<std::string>(j, "target").value_or(sweep.target);
    sweep.end_value = json_util::read_field<float>(j, "end").value_or(sweep.end_value);
    return sweep;
}

// ============================================================================
// Filter parsing
// ============================================================================

static FilterParams parse_filter(const json& j) {
    FilterParams filter;
    filter.type = json_util::read_field<std::string>(j, "type").value_or(filter.type);
    filter.cutoff = json_util::read_field<float>(j, "cutoff").value_or(filter.cutoff);
    filter.sweep_to = json_util::read_field<float>(j, "sweep_to").value_or(filter.sweep_to);
    return filter;
}

// ============================================================================
// Step parsing
// ============================================================================

static SoundStep parse_step(const json& j, float bpm, Waveform default_wave, float default_vel,
                            const ADSREnvelope& default_env) {
    SoundStep step;

    // Pause step
    if (auto pause = json_util::read_field<float>(j, "pause")) {
        step.is_pause = true;
        step.freq_hz = 0;
        step.duration_ms = clamp_duration(*pause);
        return step;
    }

    // Frequency: either "note" name or raw "freq" Hz
    if (auto note = json_util::read_field<std::string>(j, "note")) {
        step.freq_hz = SoundThemeParser::note_to_freq(*note);
    } else if (auto freq = json_util::read_field<float>(j, "freq")) {
        step.freq_hz = clamp_freq(*freq);
    }

    // Polyphonic: "notes" array takes priority over single "note"/"freq"
    if (const json* notes = json_util::find_array(j, "notes")) {
        const json& notes_arr = *notes;
        step.chord_count = static_cast<uint8_t>(std::min(notes_arr.size(), size_t(4)));
        for (uint8_t i = 0; i < step.chord_count; ++i) {
            if (notes_arr[i].is_string()) {
                step.chord_freqs[i] =
                    SoundThemeParser::note_to_freq(notes_arr[i].get<std::string>());
            } else if (notes_arr[i].is_number()) {
                step.chord_freqs[i] = clamp_freq(notes_arr[i].get<float>());
            }
        }
        // Also set freq_hz to root note for mono backends
        if (step.chord_count > 0) {
            step.freq_hz = step.chord_freqs[0];
        }
    }

    // Duration: either musical notation string or raw ms number
    if (const json* dur = json_util::find_member(j, "dur")) {
        if (dur->is_string() && bpm > 0) {
            step.duration_ms =
                SoundThemeParser::musical_duration_to_ms(dur->get<std::string>(), bpm);
        } else if (dur->is_number()) {
            step.duration_ms = clamp_duration(dur->get<float>());
        }
    }
    // Clamp musical durations too (after conversion)
    if (step.duration_ms > 0) {
        step.duration_ms = clamp_duration(step.duration_ms);
    }

    // Waveform (default from theme)
    if (auto wave = json_util::read_field<std::string>(j, "wave")) {
        step.wave = parse_waveform(*wave);
    } else {
        step.wave = default_wave;
    }

    // Velocity (default from theme)
    if (auto vel = json_util::read_field<float>(j, "vel")) {
        step.velocity = clamp_velocity(*vel);
    } else {
        step.velocity = default_vel;
    }

    // ADSR envelope (default from theme)
    if (const json* env = json_util::find_object(j, "env")) {
        step.envelope = parse_envelope(*env, default_env);
    } else {
        step.envelope = default_env;
    }

    // LFO
    if (const json* lfo = json_util::find_object(j, "lfo")) {
        step.lfo = parse_lfo(*lfo);
    }

    // Sweep
    if (const json* sweep = json_util::find_object(j, "sweep")) {
        step.sweep = parse_sweep(*sweep);
    }

    // Filter
    if (const json* filter = json_util::find_object(j, "filter")) {
        step.filter = parse_filter(*filter);
    }

    return step;
}

// ============================================================================
// Theme parsing from JSON object
// ============================================================================

static std::optional<SoundTheme> parse_theme(const json& j) {
    if (!j.is_object()) {
        spdlog::warn("[SoundTheme] Root is not a JSON object");
        return std::nullopt;
    }

    const json* sounds = json_util::find_object(j, "sounds");
    if (!sounds) {
        spdlog::warn("[SoundTheme] Missing or invalid 'sounds' key");
        return std::nullopt;
    }

    SoundTheme theme;

    // Metadata
    theme.name = json_util::read_field<std::string>(j, "name").value_or(theme.name);
    theme.description =
        json_util::read_field<std::string>(j, "description").value_or(theme.description);
    theme.version = json_util::read_integer_field(j, "version").value_or(theme.version);

    // Theme-level defaults
    if (const json* defs = json_util::find_object(j, "defaults")) {
        if (auto wave = json_util::read_field<std::string>(*defs, "wave")) {
            theme.default_wave = parse_waveform(*wave);
        }
        if (auto vel = json_util::read_field<float>(*defs, "vel")) {
            theme.default_velocity = clamp_velocity(*vel);
        }
        if (const json* env = json_util::find_object(*defs, "env")) {
            theme.default_envelope = parse_envelope(*env, theme.default_envelope);
        }
    }

    for (auto& [key, val] : sounds->items()) {
        if (!val.is_object()) {
            spdlog::warn("[SoundTheme] Sound '{}' is not an object, skipping", key);
            continue;
        }

        SoundDefinition def;
        def.name = key;

        def.description =
            json_util::read_field<std::string>(val, "description").value_or(def.description);
        def.repeat = json_util::read_integer_field(val, "repeat").value_or(def.repeat);
        def.bpm = json_util::read_field<float>(val, "bpm").value_or(def.bpm);

        // BPM for duration calculation: sound-level overrides theme-level
        float effective_bpm = def.bpm;

        if (const json* steps = json_util::find_array(val, "steps")) {
            for (const auto& step_json : *steps) {
                if (step_json.is_object()) {
                    def.steps.push_back(parse_step(step_json, effective_bpm, theme.default_wave,
                                                   theme.default_velocity, theme.default_envelope));
                }
            }
        }

        theme.sounds[key] = std::move(def);
    }

    spdlog::debug("[SoundTheme] Loaded theme '{}' with {} sounds", theme.name, theme.sounds.size());
    return theme;
}

// ============================================================================
// Public API
// ============================================================================

std::optional<SoundTheme> SoundThemeParser::load_from_file(const std::string& path) {
    const auto text = helix::text_io::read_file(path);
    if (!text) {
        spdlog::warn("[SoundTheme] Could not open '{}'", path);
        return std::nullopt;
    }

    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded()) {
        spdlog::warn("[SoundTheme] JSON parse error in '{}': not valid JSON", path);
        return std::nullopt;
    }

    return parse_theme(j);
}

std::optional<SoundTheme> SoundThemeParser::load_from_string(const std::string& json_str) {
    if (json_str.empty()) {
        return std::nullopt;
    }

    json j = json::parse(json_str, nullptr, false);
    if (j.is_discarded()) {
        spdlog::warn("[SoundTheme] JSON parse error: not valid JSON");
        return std::nullopt;
    }

    return parse_theme(j);
}

float SoundThemeParser::note_to_freq(const std::string& note) {
    if (note.empty())
        return 0.0f;

    // Parse note name and octave: e.g., "C4", "C#4", "Db4"
    // Format: <letter>[#|b]<octave>
    size_t pos = 0;
    std::string note_name;

    // First character must be A-G
    char letter = note[0];
    if (letter < 'A' || letter > 'G')
        return 0.0f;
    note_name += letter;
    pos = 1;

    // Optional sharp/flat
    if (pos < note.size() && (note[pos] == '#' || note[pos] == 'b')) {
        note_name += note[pos];
        pos++;
    }

    // Must have remaining octave digit(s)
    if (pos >= note.size())
        return 0.0f;

    const int octave = helix::text_io::parse_leading<int>(note.substr(pos)).value_or(-1);

    if (octave < 0 || octave > 8)
        return 0.0f;

    // Look up semitone offset
    auto& semitones = note_semitones();
    auto it = semitones.find(note_name);
    if (it == semitones.end())
        return 0.0f;

    // Calculate frequency using A4 = 440 Hz equal temperament
    // MIDI note number: (octave + 1) * 12 + semitone
    // A4 MIDI = 69
    int midi_note = (octave + 1) * 12 + it->second;
    int semitones_from_a4 = midi_note - 69;

    float freq = 440.0f * std::pow(2.0f, static_cast<float>(semitones_from_a4) / 12.0f);
    return freq;
}

float SoundThemeParser::musical_duration_to_ms(const std::string& dur, float bpm) {
    if (dur.empty() || bpm <= 0)
        return 0.0f;

    // Quarter note duration in ms
    float quarter_ms = 60000.0f / bpm;

    // Check for dotted notation: e.g., "4n."
    bool dotted = false;
    std::string working = dur;
    if (working.back() == '.') {
        dotted = true;
        working.pop_back();
    }

    // Check for triplet: e.g., "8t"
    bool triplet = false;
    if (working.back() == 't') {
        triplet = true;
        working.pop_back();
    } else if (working.back() == 'n') {
        working.pop_back();
    } else {
        return 0.0f;
    }

    const int divisor = helix::text_io::parse_leading<int>(working).value_or(0);

    if (divisor <= 0)
        return 0.0f;

    // Whole note = 4 quarter notes
    float duration_ms = (4.0f / static_cast<float>(divisor)) * quarter_ms;

    if (dotted) {
        duration_ms *= 1.5f;
    }
    if (triplet) {
        // Triplet: 3 notes in the space of 2
        // An eighth triplet = quarter note / 3
        duration_ms = (2.0f / 3.0f) * duration_ms;
    }

    return duration_ms;
}
