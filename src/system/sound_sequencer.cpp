// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sound_sequencer.h"

#include "audio_settings_manager.h"
#include "note_event.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace helix;

SoundSequencer::SoundSequencer(std::shared_ptr<SoundBackend> backend)
    : backend_(std::move(backend)) {}

SoundSequencer::~SoundSequencer() {
    shutdown();
}

void SoundSequencer::play(const SoundDefinition& sound, SoundPriority priority) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    request_queue_.push({sound, priority});
    queue_cv_.notify_one();
}

void SoundSequencer::stop() {
    stop_requested_.store(true);
    queue_cv_.notify_one();
}

void SoundSequencer::set_external_tick(std::function<void(float dt_ms)> fn) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    external_tick_ = std::move(fn);
    queue_cv_.notify_one();
}

bool SoundSequencer::is_playing() const {
    return playing_.load();
}

void SoundSequencer::start() {
    if (running_.load())
        return;
    running_.store(true);
    sequencer_thread_ = std::thread(&SoundSequencer::sequencer_loop, this);
    spdlog::debug("[SoundSequencer] started sequencer thread");
}

void SoundSequencer::shutdown() {
    if (!running_.load())
        return;
    running_.store(false);
    queue_cv_.notify_one();
    if (sequencer_thread_.joinable()) {
        sequencer_thread_.join();
    }
    spdlog::debug("[SoundSequencer] shutdown complete");
}

void SoundSequencer::sequencer_loop() {
    spdlog::debug("[SoundSequencer] sequencer loop started");

    // Park the device before the first tick. The backend opened it during
    // initialize(), but device_active_ starts false — and the idle branch below
    // only suspends `if (device_active_)`, so nothing ever parked the device
    // until a first sound had played and finished. A printer with sounds
    // switched off never plays one, so the device stayed open for the whole
    // process lifetime: ALSA's render thread writing silence every period, and
    // on Android an AudioTrack held open, which is the PlayerBase::stop() spam
    // #1253 closed. Suspending here makes the flag and the hardware agree from
    // the start; the first resume() reopens on demand.
    if (backend_) {
        backend_->suspend();
        device_active_ = false;
    }

    auto last_tick = std::chrono::steady_clock::now();
    bool was_playing = false;

    // Respect backend's minimum tick interval for sleep duration
    const float min_tick = backend_ ? backend_->min_tick_ms() : 1.0f;
    const auto tick_interval =
        std::chrono::microseconds(static_cast<int>(std::max(1.0f, min_tick) * 1000.0f));

    while (running_.load()) {
        // Check for stop request
        if (stop_requested_.load()) {
            stop_requested_.store(false);
            if (playing_.load()) {
                end_playback();
                was_playing = false;
            }
        }

        // Check queue for new requests
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            if (!playing_.load() && request_queue_.empty() && !external_tick_) {
                // Nothing playing, nothing queued — wait for a signal.
                // Suspend the backend device so it stops running its render
                // callback over silence (Android AudioTrack churn + idle CPU).
                if (device_active_) {
                    backend_->suspend();
                    device_active_ = false;
                }
                was_playing = false;
                queue_cv_.wait_for(lock, std::chrono::milliseconds(10));
                last_tick = std::chrono::steady_clock::now();
                continue;
            }

            // Process all queued requests — last one at highest priority wins
            while (!request_queue_.empty()) {
                auto& req = request_queue_.front();

                if (!playing_.load()) {
                    // Not playing — start this sound
                    begin_playback(std::move(req));
                    request_queue_.pop();
                } else if (static_cast<int>(req.priority) >= static_cast<int>(current_priority_)) {
                    // Higher or equal priority — preempt
                    end_playback();
                    begin_playback(std::move(req));
                    request_queue_.pop();
                } else {
                    // Lower priority — drop it
                    request_queue_.pop();
                }
            }
        }

        // External tick routing (for TrackerPlayer) and SFX playback.
        // Both can run concurrently on PCM-capable backends (SDL, ALSA).
        std::function<void(float)> ext_tick;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            ext_tick = external_tick_;
        }

        bool has_work = ext_tick || playing_.load();
        if (has_work) {
            if (!device_active_) {
                backend_->resume();
                device_active_ = true;
            }
            if (!was_playing) {
                last_tick = std::chrono::steady_clock::now();
                was_playing = true;
            }
            auto now = std::chrono::steady_clock::now();
            float dt_ms = std::chrono::duration<float, std::milli>(now - last_tick).count();
            last_tick = now;
            // Cap at 500ms to prevent runaway after long stalls (e.g. suspend/resume),
            // but allow large deltas so sounds complete on time even when the sequencer
            // thread is starved on slow CPUs (e.g. AD5M with 48 BogoMIPS).
            // Previous 5ms cap caused multi-second sound playback on thread-starved systems.
            dt_ms = std::min(dt_ms, 500.0f);

            if (ext_tick) {
                ext_tick(dt_ms);
            }
            if (playing_.load()) {
                tick(dt_ms);
            }
        } else {
            // Queue drained without starting playback — also idle. Suspend here
            // since the idle branch above was skipped (queue was non-empty at check).
            if (device_active_) {
                backend_->suspend();
                device_active_ = false;
            }
            was_playing = false;
            last_tick = std::chrono::steady_clock::now();
        }

        // Sleep for backend's minimum tick interval
        std::this_thread::sleep_for(tick_interval);
    }

    // Clean shutdown
    if (playing_.load()) {
        end_playback();
    }
    if (device_active_) {
        backend_->suspend();
        device_active_ = false;
    }
}

void SoundSequencer::apply_step_voices(const SoundStep& step, float freq, float amplitude,
                                       float duty) {
    if (step.chord_count > 0) {
        // Polyphonic: assign each chord note to a voice slot.
        // When freq differs from step.freq_hz (sweep/LFO applied), scale chord
        // intervals by the same ratio so they track the modulation together.
        int voices = backend_->voice_count();
        float ratio = 1.0f;
        if (step.freq_hz > 0 && std::abs(freq - step.freq_hz) > 0.01f) {
            ratio = freq / step.freq_hz;
        }
        for (int v = 0; v < voices; ++v) {
            if (v < step.chord_count) {
                backend_->set_voice(v, step.chord_freqs[v] * ratio, amplitude, duty);
                if (backend_->supports_waveforms()) {
                    backend_->set_voice_waveform(v, step.wave);
                }
            } else {
                backend_->silence_voice(v);
            }
        }
    } else {
        // Monophonic: voice 0 only via existing set_tone path
        if (backend_->supports_waveforms()) {
            backend_->set_waveform(step.wave);
        }
        backend_->set_tone(freq, amplitude, duty);
        // Silence other voices (no-op on mono backends via default impl)
        for (int v = 1; v < backend_->voice_count(); ++v)
            backend_->silence_voice(v);
    }
    // Release fence: make all voice writes visible to render thread
    std::atomic_thread_fence(std::memory_order_release);
}

void SoundSequencer::tick(float dt_ms) {
    if (!playing_.load())
        return;

    auto& steps = current_sound_.steps;
    if (step_state_.step_index >= static_cast<int>(steps.size())) {
        advance_step();
        return;
    }

    auto& step = steps[step_state_.step_index];
    step_state_.elapsed_ms += dt_ms;

    if (step_state_.elapsed_ms >= step_state_.total_ms) {
        advance_step();
        return;
    }

    if (step.is_pause) {
        backend_->silence();
        return;
    }

    // PCM backends handle everything per-sample — sequencer only manages timing
    if (backend_->supports_note_events()) {
        return;
    }

    // Non-PCM backends: compute parameters per-tick as before
    float elapsed = step_state_.elapsed_ms;
    float duration = step_state_.total_ms;
    float progress = (duration > 0) ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;

    float freq = step.freq_hz;
    float amplitude = step.velocity;
    float duty = 0.5f;

    float env_mul = compute_envelope(step.envelope, elapsed, duration);
    amplitude *= env_mul;

    if (!step.sweep.target.empty() && step.sweep.target == "freq") {
        freq = compute_sweep(step.freq_hz, step.sweep.end_value, progress);
    }

    if (step.lfo.rate > 0 && step.lfo.depth > 0) {
        float lfo_val = compute_lfo(step.lfo, elapsed);
        if (step.lfo.target == "freq")
            freq += lfo_val;
        else if (step.lfo.target == "amplitude")
            amplitude += lfo_val;
        else if (step.lfo.target == "duty")
            duty += lfo_val;
    }

    amplitude *= AudioSettingsManager::instance().get_volume_scaled();

    freq = std::clamp(freq, 20.0f, 20000.0f);
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    duty = std::clamp(duty, 0.0f, 1.0f);

    if (backend_->supports_filter() && !step.filter.type.empty()) {
        float cutoff = step.filter.cutoff;
        if (step.filter.sweep_to > 0) {
            cutoff = compute_sweep(step.filter.cutoff, step.filter.sweep_to, progress);
        }
        backend_->set_filter(step.filter.type, cutoff);
    }

    apply_step_voices(step, freq, amplitude, duty);
}

void SoundSequencer::advance_step() {
    step_state_.step_index++;

    if (step_state_.step_index >= static_cast<int>(current_sound_.steps.size())) {
        // Sequence complete — check repeat
        step_state_.repeat_remaining--;
        if (step_state_.repeat_remaining > 0) {
            // Restart the sequence
            step_state_.step_index = 0;
            step_state_.elapsed_ms = 0;
            if (!current_sound_.steps.empty()) {
                auto& step = current_sound_.steps[0];
                float env_min =
                    step.envelope.attack_ms + step.envelope.decay_ms + step.envelope.release_ms;
                step_state_.total_ms = std::max(step.duration_ms, env_min);
                if (!step.is_pause) {
                    publish_note_for_step(step);
                }
            }
            // Silence between repeats (non-PCM only; PCM backends auto-silence via envelope)
            if (!backend_->supports_note_events())
                backend_->silence();
            return;
        }

        // Done playing
        end_playback();
        return;
    }

    // Set up the next step
    step_state_.elapsed_ms = 0;
    auto& step = current_sound_.steps[step_state_.step_index];
    float env_min = step.envelope.attack_ms + step.envelope.decay_ms + step.envelope.release_ms;
    step_state_.total_ms = std::max(step.duration_ms, env_min);

    // Skip zero-duration steps
    if (step_state_.total_ms <= 0) {
        advance_step();
        return;
    }

    // Publish note to backends for next step
    if (!step.is_pause) {
        publish_note_for_step(step);
    } else if (!backend_->supports_note_events()) {
        // Non-PCM backends need explicit silence for pauses
        backend_->silence();
    }
}

float SoundSequencer::compute_envelope(const ADSREnvelope& env, float elapsed_ms,
                                       float duration_ms) const {
    float a = env.attack_ms;
    float d = env.decay_ms;
    float s = env.sustain_level;
    float r = env.release_ms;

    // If all ADSR times are 0, return full amplitude
    if (a <= 0 && d <= 0 && r <= 0)
        return 1.0f;

    // Release starts at (duration_ms - release_ms)
    float release_start = duration_ms - r;

    if (elapsed_ms < a) {
        // Attack phase: ramp 0 -> 1
        return (a > 0) ? (elapsed_ms / a) : 1.0f;
    } else if (elapsed_ms < a + d) {
        // Decay phase: ramp 1 -> sustain
        float decay_progress = (d > 0) ? ((elapsed_ms - a) / d) : 1.0f;
        return 1.0f - (1.0f - s) * decay_progress;
    } else if (elapsed_ms < release_start) {
        // Sustain phase: hold at sustain level
        return s;
    } else {
        // Release phase: ramp sustain -> 0
        float release_elapsed = elapsed_ms - release_start;
        float release_progress = (r > 0) ? std::clamp(release_elapsed / r, 0.0f, 1.0f) : 1.0f;
        return s * (1.0f - release_progress);
    }
}

float SoundSequencer::compute_lfo(const LFOParams& lfo, float elapsed_ms) const {
    if (lfo.rate <= 0)
        return 0.0f;
    // Sinusoidal modulation
    float phase = 2.0f * static_cast<float>(M_PI) * lfo.rate * elapsed_ms / 1000.0f;
    return std::sin(phase) * lfo.depth;
}

float SoundSequencer::compute_sweep(float start, float end, float progress) const {
    return start + (end - start) * progress;
}

void SoundSequencer::publish_note_for_step(const SoundStep& step) {
    float vel = step.velocity * AudioSettingsManager::instance().get_volume_scaled();

    if (backend_->supports_note_events()) {
        // PCM backend: publish complete NoteEvent — callback owns rendering
        NoteEvent event;
        event.freq_hz = step.freq_hz;
        event.velocity = vel;
        event.duration_ms = step_state_.total_ms;
        event.duty_cycle = 0.5f;
        event.wave = step.wave;

        // ADSR
        event.attack_ms = step.envelope.attack_ms;
        event.decay_ms = step.envelope.decay_ms;
        event.sustain_level = step.envelope.sustain_level;
        event.release_ms = step.envelope.release_ms;

        // Sweep
        if (!step.sweep.target.empty() && step.sweep.target == "freq") {
            event.sweep_end_freq = step.sweep.end_value;
        }

        // LFO
        if (step.lfo.rate > 0 && step.lfo.depth > 0) {
            event.lfo_rate = step.lfo.rate;
            event.lfo_depth = step.lfo.depth;
            if (step.lfo.target == "freq")
                event.lfo_target = 1;
            else if (step.lfo.target == "amplitude")
                event.lfo_target = 2;
            else if (step.lfo.target == "duty")
                event.lfo_target = 3;
        }

        // Filter
        if (!step.filter.type.empty()) {
            event.filter_cutoff = step.filter.cutoff;
            event.filter_sweep_to = step.filter.sweep_to;
            if (step.filter.type == "lowpass")
                event.filter_type = 1;
            else if (step.filter.type == "highpass")
                event.filter_type = 2;
        }

        if (step.chord_count > 0) {
            int voices = backend_->voice_count();
            for (int v = 0; v < voices && v < step.chord_count; ++v) {
                NoteEvent chord_event = event;
                chord_event.freq_hz = step.chord_freqs[v];
                if (event.sweep_end_freq > 0 && step.freq_hz > 0) {
                    // Scale sweep endpoint by chord interval ratio
                    chord_event.sweep_end_freq =
                        event.sweep_end_freq * (step.chord_freqs[v] / step.freq_hz);
                }
                backend_->publish_note(v, chord_event);
            }
            // Silence unused voices
            for (int v = step.chord_count; v < voices; ++v) {
                NoteEvent silence;
                backend_->publish_note(v, silence);
            }
        } else {
            backend_->publish_note(0, event);
            // Silence other voices
            for (int v = 1; v < backend_->voice_count(); ++v) {
                NoteEvent silence;
                backend_->publish_note(v, silence);
            }
        }
    } else {
        // Non-PCM backend: set initial tone (sequencer handles per-tick updates)
        if (step.chord_count > 0) {
            int voices = backend_->voice_count();
            for (int v = 0; v < voices && v < step.chord_count; ++v) {
                backend_->set_voice(v, step.chord_freqs[v], vel, 0.5f);
                if (backend_->supports_waveforms()) {
                    backend_->set_voice_waveform(v, step.wave);
                }
            }
            for (int v = step.chord_count; v < voices; ++v) {
                backend_->silence_voice(v);
            }
        } else {
            if (backend_->supports_waveforms()) {
                backend_->set_waveform(step.wave);
            }
            backend_->set_tone(step.freq_hz, vel, 0.5f);
            for (int v = 1; v < backend_->voice_count(); ++v)
                backend_->silence_voice(v);
        }
    }
}

void SoundSequencer::begin_playback(PlayRequest&& req) {
    current_sound_ = std::move(req.sound);
    current_priority_ = req.priority;

    // Skip empty sequences
    if (current_sound_.steps.empty()) {
        return;
    }

    step_state_ = {};
    step_state_.step_index = 0;
    step_state_.repeat_remaining = std::max(1, current_sound_.repeat);
    step_state_.elapsed_ms = 0;

    auto& step = current_sound_.steps[0];
    float env_min = step.envelope.attack_ms + step.envelope.decay_ms + step.envelope.release_ms;
    step_state_.total_ms = std::max(step.duration_ms, env_min);

    // Skip zero-duration first step
    if (step_state_.total_ms <= 0) {
        advance_step();
        if (step_state_.step_index >= static_cast<int>(current_sound_.steps.size()) &&
            step_state_.repeat_remaining <= 0) {
            return; // Entire sequence was zero-duration
        }
    }

    // Send envelope to PCM backends for per-sample computation
    if (!current_sound_.steps.empty() && !current_sound_.steps[step_state_.step_index].is_pause) {
        publish_note_for_step(current_sound_.steps[step_state_.step_index]);
    }

    playing_.store(true);
    spdlog::debug("[SoundSequencer] begin playback: {} ({} steps, {} repeats)", current_sound_.name,
                  current_sound_.steps.size(), step_state_.repeat_remaining);
}

void SoundSequencer::end_playback() {
    backend_->silence();
    playing_.store(false);
    spdlog::debug("[SoundSequencer] end playback");
}
