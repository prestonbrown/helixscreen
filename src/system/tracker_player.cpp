// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_HAS_TRACKER

#include "tracker_player.h"

#include "audio_settings_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace helix::audio {

TrackerPlayer::TrackerPlayer(std::shared_ptr<SoundBackend> backend)
    : backend_(std::move(backend)) {}

void TrackerPlayer::load(TrackerModule module) {
    stop();
    module_ = std::move(module);
    speed_ = std::max(1, static_cast<int>(module_.speed));
    tempo_ = std::max(32, static_cast<int>(module_.tempo));
    order_idx_ = 0;
    row_ = 0;
    tick_ = 0;
    tick_accum_ = 0;
    next_order_ = -1;
    next_row_ = -1;
    channels_ = {};
}

void TrackerPlayer::play() {
    if (module_.order.empty() || module_.patterns.empty()) {
        spdlog::warn("[TrackerPlayer] cannot play: empty module");
        return;
    }
    order_idx_ = 0;
    row_ = 0;
    tick_ = 0;
    tick_accum_ = 0;
    next_order_ = -1;
    next_row_ = -1;
    channels_ = {};
    playing_.store(true, std::memory_order_release);
    // Process the first row immediately
    process_row();
    apply_to_backend();
}

void TrackerPlayer::stop() {
    playing_.store(false, std::memory_order_release);
    if (backend_) {
        for (int ch = 0; ch < 4; ++ch) {
            backend_->silence_voice(ch);
        }
    }
}

bool TrackerPlayer::is_playing() const {
    return playing_.load(std::memory_order_acquire);
}

void TrackerPlayer::tick(float dt_ms) {
    if (!playing_.load(std::memory_order_acquire))
        return;

    const float ms_per_tick = 2500.0f / static_cast<float>(tempo_);
    tick_accum_ += dt_ms;

    while (tick_accum_ >= ms_per_tick) {
        tick_accum_ -= ms_per_tick;

        if (!playing_.load(std::memory_order_relaxed))
            break;

        tick_++;
        if (tick_ >= speed_) {
            tick_ = 0;
            advance_row();
            if (!playing_.load(std::memory_order_relaxed))
                break;
            process_row();
        }

        process_tick_effects();
        apply_to_backend();
    }
}

TrackerPlayer::ChannelSnapshot TrackerPlayer::get_channel(int ch) const {
    if (ch < 0 || ch >= 4)
        return {0, 0, false};
    const auto& c = channels_[static_cast<size_t>(ch)];
    return {c.freq, c.volume, c.active};
}

int TrackerPlayer::current_row() const {
    return row_;
}
int TrackerPlayer::current_order() const {
    return order_idx_;
}
int TrackerPlayer::current_tick() const {
    return tick_;
}

void TrackerPlayer::set_volume_override(int vol) {
    volume_override_ = vol;
}

// ---------------------------------------------------------------------------
// Row processing — tick 0 of each row
// ---------------------------------------------------------------------------

void TrackerPlayer::process_row() {
    if (order_idx_ >= static_cast<int>(module_.num_orders)) {
        playing_.store(false, std::memory_order_release);
        return;
    }

    const uint8_t pat_idx = module_.order[static_cast<size_t>(order_idx_)];
    if (pat_idx >= module_.patterns.size()) {
        playing_.store(false, std::memory_order_release);
        return;
    }

    const auto& pattern = module_.patterns[pat_idx];
    const int rows_in_pattern = static_cast<int>(module_.rows_per_pattern);
    if (row_ >= rows_in_pattern) {
        playing_.store(false, std::memory_order_release);
        return;
    }

    for (int ch = 0; ch < 4; ++ch) {
        const size_t idx = static_cast<size_t>(row_ * 4 + ch);
        if (idx >= pattern.size())
            continue;

        const auto& note = pattern[idx];
        auto& cs = channels_[static_cast<size_t>(ch)];

        // Capture effect for this row
        cs.effect = note.effect;
        cs.effect_data = note.effect_data;

        // Set instrument
        if (note.instrument > 0 && note.instrument <= module_.instruments.size()) {
            cs.instrument = note.instrument;
            const auto& inst = module_.instruments[note.instrument - 1];
            cs.volume = inst.volume;
            cs.base_volume = inst.volume;
            cs.waveform = inst.waveform;
            cs.current_instrument = &inst;
        }

        // Handle note (skip for tone portamento — effect 0x03 uses target)
        if (note.note > 0) {
            const float freq = TrackerModule::note_to_freq(note.note);

            // Compute period: use MOD period if available, else look up from note
            uint16_t note_period = note.period;
            if (note_period == 0 && module_.has_samples && note.note > 0) {
                // MED notes lack Amiga periods — look up from period table
                note_period = TrackerModule::note_to_period(note.note);
            }

            if (cs.effect == 0x03) {
                // Tone portamento: set target, keep current freq/period
                cs.target_freq = freq;
                if (note_period > 0)
                    cs.target_period = note_period;
            } else {
                cs.freq = freq;
                cs.base_freq = freq;
                // Store period for sample playback pitch
                if (note_period > 0) {
                    cs.period = note_period;
                    cs.base_period = note_period;
                }
                cs.active = true;
                cs.vibrato_phase = 0;
                cs.tremolo_phase = 0;
                cs.sample_pos = 0;
            }
        }

        // Row-0 effect processing
        const uint8_t ex = (note.effect_data >> 4) & 0x0F;
        const uint8_t ey = note.effect_data & 0x0F;

        switch (note.effect) {
        case 0x00: // Arpeggio
            if (note.effect_data != 0) {
                cs.arp_x = ex;
                cs.arp_y = ey;
                cs.arp_tick = 0;
            }
            break;

        case 0x03: // Tone portamento
            if (note.effect_data > 0) {
                cs.porta_speed = note.effect_data;
            }
            break;

        case 0x04: // Vibrato
            if (ex > 0)
                cs.vibrato_speed = ex;
            if (ey > 0)
                cs.vibrato_depth = ey;
            break;

        case 0x07: // Tremolo
            if (ex > 0)
                cs.tremolo_speed = ex;
            if (ey > 0)
                cs.tremolo_depth = ey;
            break;

        case 0x0B: // Position jump
            next_order_ = note.effect_data;
            break;

        case 0x0C: // Set volume
            cs.volume = std::clamp(static_cast<float>(note.effect_data) / 64.0f, 0.0f, 1.0f);
            cs.base_volume = cs.volume;
            break;

        case 0x0D: // Pattern break
            next_row_ = ex * 10 + ey;
            if (next_order_ < 0) {
                next_order_ = order_idx_ + 1;
            }
            break;

        case 0x0E: // Extended effects
            switch (ex) {
            case 0x1: // Fine porta up
                cs.freq *= std::pow(2.0f, static_cast<float>(ey) / (12.0f * 16.0f));
                cs.base_freq = cs.freq;
                if (module_.has_samples) {
                    cs.period =
                        static_cast<uint16_t>(std::max(113, static_cast<int>(cs.period) - ey));
                    cs.base_period = cs.period;
                }
                break;
            case 0x2: // Fine porta down
                cs.freq /= std::pow(2.0f, static_cast<float>(ey) / (12.0f * 16.0f));
                cs.base_freq = cs.freq;
                if (module_.has_samples) {
                    cs.period =
                        static_cast<uint16_t>(std::min(856, static_cast<int>(cs.period) + ey));
                    cs.base_period = cs.period;
                }
                break;
            case 0x6: // Pattern loop
                if (ey == 0) {
                    cs.loop_start_row = row_;
                } else if (cs.loop_start_row >= 0) {
                    if (cs.loop_count == 0) {
                        cs.loop_count = ey;
                        next_row_ = cs.loop_start_row;
                        next_order_ = order_idx_; // stay in same pattern
                    } else {
                        cs.loop_count--;
                        if (cs.loop_count > 0) {
                            next_row_ = cs.loop_start_row;
                            next_order_ = order_idx_;
                        }
                    }
                }
                break;
            case 0xA: // Fine volume up
                cs.volume = std::min(1.0f, cs.volume + static_cast<float>(ey) / 64.0f);
                break;
            case 0xB: // Fine volume down
                cs.volume = std::max(0.0f, cs.volume - static_cast<float>(ey) / 64.0f);
                break;
            default:
                break;
            }
            break;

        case 0x0F: // Set speed/tempo
            if (note.effect_data > 0) {
                if (note.effect_data < 32) {
                    speed_ = note.effect_data;
                } else {
                    tempo_ = note.effect_data;
                }
            }
            break;

        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-tick effects — ticks 1..speed-1
// ---------------------------------------------------------------------------

void TrackerPlayer::process_tick_effects() {
    for (int ch = 0; ch < 4; ++ch) {
        auto& cs = channels_[static_cast<size_t>(ch)];
        if (!cs.active)
            continue;

        const uint8_t ex = (cs.effect_data >> 4) & 0x0F;
        const uint8_t ey = cs.effect_data & 0x0F;

        // Per-tick effects (tick > 0 only)
        if (tick_ > 0) {
            switch (cs.effect) {
            case 0x00: // Arpeggio
                if (cs.effect_data != 0) {
                    cs.arp_tick = static_cast<uint8_t>((cs.arp_tick + 1) % 3);
                    int semitones = 0;
                    switch (cs.arp_tick) {
                    case 0:
                        semitones = 0;
                        break;
                    case 1:
                        semitones = cs.arp_x;
                        break;
                    case 2:
                        semitones = cs.arp_y;
                        break;
                    }
                    float ratio = std::pow(2.0f, static_cast<float>(semitones) / 12.0f);
                    cs.freq = cs.base_freq * ratio;
                    if (module_.has_samples && cs.base_period > 0) {
                        cs.period = static_cast<uint16_t>(
                            std::clamp(static_cast<int>(cs.base_period / ratio), 113, 856));
                    }
                }
                break;

            case 0x01: // Porta up
                cs.freq *= std::pow(2.0f, static_cast<float>(cs.effect_data) / (12.0f * 16.0f));
                cs.base_freq = cs.freq;
                if (module_.has_samples) {
                    cs.period = static_cast<uint16_t>(
                        std::max(113, static_cast<int>(cs.period) - cs.effect_data));
                    cs.base_period = cs.period;
                }
                break;

            case 0x02: // Porta down
                cs.freq /= std::pow(2.0f, static_cast<float>(cs.effect_data) / (12.0f * 16.0f));
                cs.base_freq = cs.freq;
                if (module_.has_samples) {
                    cs.period = static_cast<uint16_t>(
                        std::min(856, static_cast<int>(cs.period) + cs.effect_data));
                    cs.base_period = cs.period;
                }
                break;

            case 0x03: // Tone portamento
                if (cs.target_freq > 0) {
                    const float slide =
                        std::pow(2.0f, static_cast<float>(cs.porta_speed) / (12.0f * 16.0f));
                    if (cs.freq < cs.target_freq) {
                        cs.freq *= slide;
                        if (cs.freq > cs.target_freq)
                            cs.freq = cs.target_freq;
                    } else if (cs.freq > cs.target_freq) {
                        cs.freq /= slide;
                        if (cs.freq < cs.target_freq)
                            cs.freq = cs.target_freq;
                    }
                    cs.base_freq = cs.freq;
                }
                if (module_.has_samples && cs.target_period > 0) {
                    if (cs.period > cs.target_period) {
                        cs.period = std::max(cs.target_period,
                                             static_cast<uint16_t>(cs.period - cs.porta_speed));
                    } else if (cs.period < cs.target_period) {
                        cs.period = std::min(cs.target_period,
                                             static_cast<uint16_t>(cs.period + cs.porta_speed));
                    }
                    cs.base_period = cs.period;
                }
                break;

            case 0x04: // Vibrato
            {
                cs.vibrato_phase += static_cast<float>(cs.vibrato_speed);
                const float vib =
                    std::sin(cs.vibrato_phase * 2.0f * static_cast<float>(M_PI) / 64.0f);
                cs.freq = cs.base_freq * std::pow(2.0f, vib * static_cast<float>(cs.vibrato_depth) /
                                                            (128.0f * 12.0f));
                if (module_.has_samples && cs.base_period > 0) {
                    int vib_period = static_cast<int>(vib * static_cast<float>(cs.vibrato_depth));
                    cs.period = static_cast<uint16_t>(
                        std::clamp(static_cast<int>(cs.base_period) + vib_period, 113, 856));
                }
                break;
            }

            case 0x05: // Tone porta + volume slide
                // Tone porta part (freq)
                if (cs.target_freq > 0) {
                    const float slide =
                        std::pow(2.0f, static_cast<float>(cs.porta_speed) / (12.0f * 16.0f));
                    if (cs.freq < cs.target_freq) {
                        cs.freq *= slide;
                        if (cs.freq > cs.target_freq)
                            cs.freq = cs.target_freq;
                    } else if (cs.freq > cs.target_freq) {
                        cs.freq /= slide;
                        if (cs.freq < cs.target_freq)
                            cs.freq = cs.target_freq;
                    }
                    cs.base_freq = cs.freq;
                }
                // Tone porta part (period)
                if (module_.has_samples && cs.target_period > 0) {
                    if (cs.period > cs.target_period) {
                        cs.period = std::max(cs.target_period,
                                             static_cast<uint16_t>(cs.period - cs.porta_speed));
                    } else if (cs.period < cs.target_period) {
                        cs.period = std::min(cs.target_period,
                                             static_cast<uint16_t>(cs.period + cs.porta_speed));
                    }
                    cs.base_period = cs.period;
                }
                // Volume slide part
                if (ex > 0) {
                    cs.volume = std::min(1.0f, cs.volume + static_cast<float>(ex) / 64.0f);
                } else if (ey > 0) {
                    cs.volume = std::max(0.0f, cs.volume - static_cast<float>(ey) / 64.0f);
                }
                cs.base_volume = cs.volume;
                break;

            case 0x06: // Vibrato + volume slide
            {
                // Vibrato part
                cs.vibrato_phase += static_cast<float>(cs.vibrato_speed);
                const float vib =
                    std::sin(cs.vibrato_phase * 2.0f * static_cast<float>(M_PI) / 64.0f);
                cs.freq = cs.base_freq * std::pow(2.0f, vib * static_cast<float>(cs.vibrato_depth) /
                                                            (128.0f * 12.0f));
                if (module_.has_samples && cs.base_period > 0) {
                    int vib_period = static_cast<int>(vib * static_cast<float>(cs.vibrato_depth));
                    cs.period = static_cast<uint16_t>(
                        std::clamp(static_cast<int>(cs.base_period) + vib_period, 113, 856));
                }
                // Volume slide part
                if (ex > 0) {
                    cs.volume = std::min(1.0f, cs.volume + static_cast<float>(ex) / 64.0f);
                } else if (ey > 0) {
                    cs.volume = std::max(0.0f, cs.volume - static_cast<float>(ey) / 64.0f);
                }
                cs.base_volume = cs.volume;
                break;
            }

            case 0x07: // Tremolo
            {
                cs.tremolo_phase += static_cast<float>(cs.tremolo_speed);
                const float trem =
                    std::sin(cs.tremolo_phase * 2.0f * static_cast<float>(M_PI) / 64.0f) *
                    static_cast<float>(cs.tremolo_depth) / 64.0f;
                cs.volume = std::clamp(cs.base_volume + trem, 0.0f, 1.0f);
                break;
            }

            case 0x0A: // Volume slide
                if (ex > 0) {
                    cs.volume = std::min(1.0f, cs.volume + static_cast<float>(ex) / 64.0f);
                } else if (ey > 0) {
                    cs.volume = std::max(0.0f, cs.volume - static_cast<float>(ey) / 64.0f);
                }
                cs.base_volume = cs.volume;
                break;

            default:
                break;
            }
        }

        // Any-tick extended effects
        if (cs.effect == 0x0E) {
            switch (ex) {
            case 0x9: // Retrigger every x ticks
                if (ey > 0 && tick_ > 0 && (tick_ % ey) == 0) {
                    cs.freq = cs.base_freq;
                    cs.vibrato_phase = 0;
                    cs.tremolo_phase = 0;
                    if (cs.instrument > 0 && cs.instrument <= module_.instruments.size()) {
                        cs.volume = module_.instruments[cs.instrument - 1].volume;
                        cs.base_volume = cs.volume;
                    }
                }
                break;
            case 0xC: // Note cut at tick x
                if (tick_ == ey) {
                    cs.active = false;
                    cs.volume = 0;
                }
                break;
            default:
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Apply channel state to sound backend
// ---------------------------------------------------------------------------

void TrackerPlayer::apply_to_backend() {
    if (!backend_)
        return;

    // When the module has PCM samples AND the backend supports direct audio
    // rendering, render_audio() handles output. Just compute sample_speed here.
    static constexpr double AMIGA_CLOCK = 3546895.0; // PAL clock
    if (module_.has_samples && backend_->supports_render_source()) {
        for (int ch = 0; ch < 4; ++ch) {
            auto& cs = channels_[static_cast<size_t>(ch)];
            if (cs.active && cs.period > 0 && cs.current_instrument &&
                cs.current_instrument->has_sample()) {
                // Paula chip: playback_rate = clock / period
                cs.sample_speed = AMIGA_CLOCK / static_cast<double>(cs.period);
            }
        }
        std::atomic_thread_fence(std::memory_order_release);
        return;
    }

    // Synth fallback: frequency-only backends (PWM, M300), or modules without samples.
    // Derive frequency from period when available for accurate pitch.
    const float master_vol = (volume_override_ >= 0)
                                 ? (static_cast<float>(volume_override_) / 100.0f)
                                 : helix::AudioSettingsManager::instance().get_volume_scaled();

    for (int ch = 0; ch < 4; ++ch) {
        const auto& cs = channels_[static_cast<size_t>(ch)];
        // Use period-derived freq for accuracy when period is available
        float freq = cs.freq;
        if (cs.period > 0) {
            freq = static_cast<float>(AMIGA_CLOCK / static_cast<double>(cs.period));
        }
        if (cs.active && freq > 0 && cs.volume > 0) {
            backend_->set_voice(ch, freq, cs.volume * master_vol, cs.duty);
            if (backend_->supports_waveforms()) {
                backend_->set_voice_waveform(ch, cs.waveform);
            }
        } else {
            backend_->silence_voice(ch);
        }
    }

    std::atomic_thread_fence(std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Row advancement
// ---------------------------------------------------------------------------

void TrackerPlayer::advance_row() {
    if (next_order_ >= 0) {
        order_idx_ = next_order_;
        row_ = (next_row_ >= 0) ? next_row_ : 0;
        next_order_ = -1;
        next_row_ = -1;

        if (order_idx_ >= static_cast<int>(module_.num_orders)) {
            playing_.store(false, std::memory_order_release);
        }
        return;
    }

    row_++;
    if (row_ >= static_cast<int>(module_.rows_per_pattern)) {
        row_ = 0;
        order_idx_++;
        if (order_idx_ >= static_cast<int>(module_.num_orders)) {
            playing_.store(false, std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------
// PCM sample render callback — called from audio thread
// ---------------------------------------------------------------------------

void TrackerPlayer::render_audio(float* output, size_t frames, int sample_rate) {
    if (!playing_.load(std::memory_order_acquire) || sample_rate <= 0) {
        std::memset(output, 0, frames * sizeof(float));
        return;
    }

    std::atomic_thread_fence(std::memory_order_acquire);

    const float master_vol = (volume_override_ >= 0)
                                 ? (static_cast<float>(volume_override_) / 100.0f)
                                 : helix::AudioSettingsManager::instance().get_volume_scaled();
    const double inv_sr = 1.0 / static_cast<double>(sample_rate);

    for (size_t i = 0; i < frames; ++i) {
        float mix = 0;

        for (int ch = 0; ch < 4; ++ch) {
            auto& cs = channels_[static_cast<size_t>(ch)];
            if (!cs.active || cs.freq <= 0)
                continue;

            const auto* inst = cs.current_instrument;
            if (!inst || !inst->has_sample())
                continue;

            const auto& sdata = inst->sample_data;
            const size_t slen = sdata.size();

            if (cs.sample_pos >= static_cast<double>(slen)) {
                // Past end of non-looping sample
                if (inst->loop_length == 0)
                    continue;
            }

            // Linear interpolation between samples
            auto pos_int = static_cast<size_t>(cs.sample_pos);
            if (pos_int >= slen) {
                if (inst->loop_length > 0) {
                    // Wrap into loop region
                    double excess =
                        cs.sample_pos - static_cast<double>(inst->loop_start + inst->loop_length);
                    cs.sample_pos = static_cast<double>(inst->loop_start) +
                                    std::fmod(excess, static_cast<double>(inst->loop_length));
                    if (cs.sample_pos < static_cast<double>(inst->loop_start))
                        cs.sample_pos = static_cast<double>(inst->loop_start);
                    pos_int = static_cast<size_t>(cs.sample_pos);
                } else {
                    continue;
                }
            }

            double frac = cs.sample_pos - static_cast<double>(pos_int);
            float s0 = sdata[pos_int];
            float s1 = (pos_int + 1 < slen) ? sdata[pos_int + 1] : s0;
            float sample = s0 + static_cast<float>(frac) * (s1 - s0);

            mix += sample * cs.volume * master_vol;

            // Advance position
            double speed = cs.sample_speed * inv_sr;
            cs.sample_pos += speed;

            // Handle loop wrapping
            if (inst->loop_length > 0) {
                double loop_end = static_cast<double>(inst->loop_start + inst->loop_length);
                if (cs.sample_pos >= loop_end) {
                    cs.sample_pos = static_cast<double>(inst->loop_start) +
                                    std::fmod(cs.sample_pos - static_cast<double>(inst->loop_start),
                                              static_cast<double>(inst->loop_length));
                }
            }
        }

        output[i] = std::clamp(mix, -1.0f, 1.0f);
    }
}

} // namespace helix::audio

#endif // HELIX_HAS_TRACKER
