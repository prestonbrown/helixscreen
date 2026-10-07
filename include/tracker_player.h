// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_HAS_TRACKER

#include "sound_backend.h"
#include "tracker_module.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace helix::audio {

/// Core tick-based playback engine for 4-channel tracker modules.
///
/// Driven by an external sequencer thread calling tick(dt_ms) at ~1ms rate.
/// Implements standard ProTracker effects (portamento, vibrato, arpeggio, etc.)
/// and outputs to a SoundBackend with up to 4 independent voices.
class TrackerPlayer {
  public:
    explicit TrackerPlayer(std::shared_ptr<SoundBackend> backend);

    /// Load a module for playback (takes ownership via move)
    void load(TrackerModule module);

    /// Start playback from current position
    void play();

    /// Stop playback and silence all voices
    void stop();

    /// Whether the player is currently playing
    bool is_playing() const;

    /// Advance the player by dt_ms milliseconds.
    /// Called from the sequencer thread at ~1ms rate.
    void tick(float dt_ms);

    // -- Test accessors --

    struct ChannelSnapshot {
        float freq;
        float volume;
        bool active;
    };

    ChannelSnapshot get_channel(int ch) const;
    int current_row() const;
    int current_order() const;
    int current_tick() const;

    /// Override master volume (0-100) for testing. -1 = use AudioSettingsManager.
    void set_volume_override(int vol);

    /// Called from backend render thread. Fills buffer with mixed audio from
    /// PCM instrument samples. Only produces output when the module has samples.
    void render_audio(float* output, size_t frames, int sample_rate);

  private:
    struct ChannelState {
        float freq = 0;
        float base_freq = 0;   // frequency before effects
        float target_freq = 0; // tone portamento target
        float volume = 1.0f;
        float base_volume = 1.0f; // volume center for tremolo / Cxx
        float duty = 0.5f;
        float vibrato_phase = 0;
        float tremolo_phase = 0;
        uint8_t vibrato_speed = 0;
        uint8_t vibrato_depth = 0;
        uint8_t tremolo_speed = 0;
        uint8_t tremolo_depth = 0;
        uint8_t arp_tick = 0;
        uint8_t arp_x = 0, arp_y = 0;
        uint8_t instrument = 0;
        uint8_t effect = 0;
        uint8_t effect_data = 0;
        uint8_t porta_speed = 0;
        Waveform waveform = Waveform::SQUARE;
        bool active = false;
        int loop_start_row = -1; // -1 = no loop point set
        int loop_count = 0;

        uint16_t period = 0;        // current Amiga period (for sample pitch)
        uint16_t base_period = 0;   // period before effects
        uint16_t target_period = 0; // tone portamento target period

        // PCM sample playback state (render thread only)
        double sample_pos = 0;   // current position in sample (fractional)
        double sample_speed = 0; // playback rate: amiga_clock / (period*2) / output_sr
        const TrackerInstrument* current_instrument = nullptr;
    };

    /// Process a new row: read notes, set frequencies, capture effects
    void process_row();

    /// Apply per-tick effects (portamento, vibrato, arpeggio, etc.)
    void process_tick_effects();

    /// Write current channel state to the backend
    void apply_to_backend();

    /// Advance to the next row (handles pattern breaks, position jumps)
    void advance_row();

    std::shared_ptr<SoundBackend> backend_;
    TrackerModule module_;
    std::array<ChannelState, 4> channels_{};
    int order_idx_ = 0;
    int row_ = 0;
    int tick_ = 0;
    int speed_ = 6;
    int tempo_ = 125;
    float tick_accum_ = 0;
    int next_order_ = -1;
    int next_row_ = -1;
    int volume_override_ = -1; // -1 = use AudioSettingsManager
    std::atomic<bool> playing_{false};
};

} // namespace helix::audio

#endif // HELIX_HAS_TRACKER
