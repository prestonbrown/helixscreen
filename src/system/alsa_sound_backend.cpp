// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_HAS_ALSA

#include "alsa_sound_backend.h"

#include "alsa_clock_keepalive.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

ALSASoundBackend::ALSASoundBackend() = default;

ALSASoundBackend::~ALSASoundBackend() {
    shutdown();
}

bool ALSASoundBackend::initialize(const std::string& device) {
    // Device name is resolved by the caller (SoundManager via
    // helix::audio::resolve_alsa_device(), precedence env > settings > default).
    // Open with NONBLOCK to avoid hanging if the device is busy.
    int err = snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        // A device that won't open is usually just "no audio hardware here", which
        // is benign — SoundManager retries "default" and, only if that also fails,
        // logs the authoritative error. Warn (not error) so an audio-less device
        // doesn't surface a scary error line for an expected, recoverable state.
        spdlog::warn("[ALSASound] Cannot open PCM device '{}': {}", device, snd_strerror(err));
        return false;
    }

    // Switch to blocking mode for write loop
    snd_pcm_nonblock(pcm_, 0);

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);

    err = snd_pcm_hw_params_any(pcm_, hw_params);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot get hardware params: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    err = snd_pcm_hw_params_set_access(pcm_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot set access type: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // Try float format first, fall back to S16
    use_s16_ = false;
    err = snd_pcm_hw_params_set_format(pcm_, hw_params, SND_PCM_FORMAT_FLOAT_LE);
    if (err < 0) {
        spdlog::debug("[ALSASound] Float format not supported, trying S16_LE");
        err = snd_pcm_hw_params_set_format(pcm_, hw_params, SND_PCM_FORMAT_S16_LE);
        if (err < 0) {
            spdlog::error("[ALSASound] Cannot set audio format: {}", snd_strerror(err));
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            return false;
        }
        use_s16_ = true;
    }

    // Try mono first, fall back to stereo (HDMI often requires stereo)
    channels_ = 1;
    err = snd_pcm_hw_params_set_channels(pcm_, hw_params, 1);
    if (err < 0) {
        spdlog::debug("[ALSASound] Mono not supported, trying stereo");
        err = snd_pcm_hw_params_set_channels(pcm_, hw_params, 2);
        if (err < 0) {
            spdlog::error("[ALSASound] Cannot set channel count: {}", snd_strerror(err));
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            return false;
        }
        channels_ = 2;
    }

    // Sample rate
    sample_rate_ = 44100;
    err = snd_pcm_hw_params_set_rate_near(pcm_, hw_params, &sample_rate_, nullptr);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot set sample rate: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // Period size
    period_size_ = 256;
    err = snd_pcm_hw_params_set_period_size_near(pcm_, hw_params, &period_size_, nullptr);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot set period size: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // Buffer size: 8x period for scheduling headroom. On a busy Pi (klipper +
    // helix-screen UI), 2× period (~11.6 ms) is too tight — the writer thread
    // gets descheduled longer than the buffer drains and underruns continuously.
    // 8× period (~46 ms) is still well under any noticeable latency for UI
    // sounds while giving the kernel ample slack.
    snd_pcm_uframes_t buffer_size = period_size_ * 8;
    err = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw_params, &buffer_size);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot set buffer size: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // Apply hardware params
    err = snd_pcm_hw_params(pcm_, hw_params);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot apply hw params: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // Software params: only start playback once the buffer is nearly full
    // (start_threshold = buffer - one period), and wake the writer when one
    // period of space is available. Without this, ALSA's default
    // start_threshold of 1 means playback starts as soon as the first writei
    // lands — and every recover→prepare→writei cycle immediately underruns
    // again because the thread can't refill faster than hardware drains.
    snd_pcm_sw_params_t* sw_params = nullptr;
    snd_pcm_sw_params_alloca(&sw_params);
    err = snd_pcm_sw_params_current(pcm_, sw_params);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot get sw params: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }
    snd_pcm_sw_params_set_start_threshold(pcm_, sw_params, buffer_size - period_size_);
    snd_pcm_sw_params_set_avail_min(pcm_, sw_params, period_size_);
    err = snd_pcm_sw_params(pcm_, sw_params);
    if (err < 0) {
        spdlog::error("[ALSASound] Cannot apply sw params: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    spdlog::info("[ALSASound] Audio initialized: {} Hz, {} ch, period {}, buffer {}, format {}",
                 sample_rate_, channels_, period_size_, buffer_size,
                 use_s16_ ? "S16_LE" : "FLOAT_LE");

    // Decide once whether this sink can tolerate the idle park. An HDMI/SPDIF
    // receiver mutes itself while it re-locks a restarted clock (0.5-1.0 s
    // measured, #1337), which is longer than every UI sound we play, so the
    // park has to be suppressed there. Ask the driver for its own PCM name as
    // well as matching the caller's device string: `hw:1,0` hides an HDMI sink
    // that the driver still calls "hdmi ...".
    std::string pcm_name;
    {
        snd_pcm_info_t* info = nullptr;
        snd_pcm_info_alloca(&info);
        if (snd_pcm_info(pcm_, info) >= 0) {
            if (const char* n = snd_pcm_info_get_name(info))
                pcm_name = n;
        }
    }
    keep_clock_alive_ = helix::audio::device_needs_clock_keepalive(device, pcm_name);
    if (const char* env = std::getenv("HELIX_ALSA_KEEPALIVE"); env && env[0] != '\0') {
        const bool forced = (env[0] != '0');
        if (forced != keep_clock_alive_)
            spdlog::info("[ALSASound] HELIX_ALSA_KEEPALIVE={} overrides detection ({})", env,
                         keep_clock_alive_ ? "keep-alive" : "park");
        keep_clock_alive_ = forced;
    }
    spdlog::info("[ALSASound] Idle behaviour: {} (device '{}', pcm '{}')",
                 keep_clock_alive_ ? "keep clock running (sink re-locks)" : "park when idle",
                 device, pcm_name);

    // Allocate scratch buffer for mixing
    mix_buf_.resize(period_size_);

    // Start render thread
    running_.store(true, std::memory_order_relaxed);
    render_thread_ = std::thread(&ALSASoundBackend::render_loop, this);

    return true;
}

void ALSASoundBackend::shutdown() {
    if (!running_.load(std::memory_order_relaxed) && pcm_ == nullptr)
        return;

    running_.store(false, std::memory_order_relaxed);
    // Wake a parked render thread, or the join below never returns. Same
    // store-under-mutex discipline as resume(); the ack lock orders running_
    // for any resume() still waiting on the handoff.
    {
        std::lock_guard<std::mutex> lock(suspend_mutex_);
        suspended_.store(false, std::memory_order_relaxed);
    }
    suspend_cv_.notify_all();
    { std::lock_guard<std::mutex> ack_lock(ack_mutex_); }
    ack_cv_.notify_all();

    // Join render thread before closing device
    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    if (pcm_) {
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }

    spdlog::info("[ALSASound] Audio shutdown");
}

void ALSASoundBackend::set_tone(float freq_hz, float amplitude, float duty_cycle) {
    set_voice(0, freq_hz, amplitude, duty_cycle);
}

void ALSASoundBackend::silence() {
    for (int v = 0; v < MAX_VOICES; ++v) {
        voice_slots_[v].event.velocity = 0;
        voice_slots_[v].generation.fetch_add(1, std::memory_order_release);
    }
}

void ALSASoundBackend::set_waveform(Waveform w) {
    set_voice_waveform(0, w);
}

void ALSASoundBackend::set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    auto& s = voice_slots_[slot];
    s.event.freq_hz = freq_hz;
    s.event.velocity = amplitude;
    s.event.duty_cycle = duty_cycle;
    s.generation.fetch_add(1, std::memory_order_release);
}

void ALSASoundBackend::set_voice_waveform(int slot, Waveform w) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    voice_slots_[slot].event.wave = w;
}

void ALSASoundBackend::silence_voice(int slot) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    voice_slots_[slot].event.velocity = 0;
    voice_slots_[slot].generation.fetch_add(1, std::memory_order_release);
}

void ALSASoundBackend::publish_note(int slot, const NoteEvent& event) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    voice_slots_[slot].event = event;
    voice_slots_[slot].generation.fetch_add(1, std::memory_order_release);
}

void ALSASoundBackend::set_render_source(std::function<void(float*, size_t, int)> fn) {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = std::move(fn);
}

void ALSASoundBackend::clear_render_source() {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = nullptr;
}

void ALSASoundBackend::set_filter(const std::string& type, float cutoff) {
    // Shared filter for external render source (tracker PCM).
    // Synth voices use per-voice filter in VoiceSlot.
    if (type.empty()) {
        filter_type_.store(helix::audio::FilterType::NONE, std::memory_order_relaxed);
        return;
    }
    auto ft = helix::audio::filter_type_from_string(type);
    bool type_changed = (ft != filter_type_.load(std::memory_order_relaxed));
    filter_cutoff_.store(cutoff, std::memory_order_relaxed);
    helix::audio::compute_biquad_coeffs(filter_, ft, cutoff, static_cast<float>(sample_rate_));
    if (type_changed) {
        filter_.z1 = 0;
        filter_.z2 = 0;
    }
    filter_type_.store(ft, std::memory_order_release);
}

void ALSASoundBackend::suspend() {
    if (keep_clock_alive_) {
        // Never stop the clock on a sink that re-locks. The render thread keeps
        // writing silence, which is exactly what this backend did before
        // v0.99.114 and what kept the receiver locked; parking here is what
        // made every short UI sound inaudible (#1337). resume() is then a no-op
        // too, since suspended_ never becomes true.
        return;
    }
    if (suspended_.exchange(true))
        return;
    // Deliberately no snd_pcm_* here — see the header. The render thread drops
    // the device itself when it observes the flag.
    spdlog::debug("[ALSA] Suspending render thread (idle)");
}

void ALSASoundBackend::resume() {
    bool was_suspended;
    {
        // Store under the mutex: paired with the predicate re-check inside
        // the render thread's cv wait, this makes the wakeup lost-notify-free
        // (notify without the mutex can fire in the window between the
        // predicate evaluating true and the thread blocking).
        std::lock_guard<std::mutex> lock(suspend_mutex_);
        was_suspended = suspended_.exchange(false, std::memory_order_relaxed);
    }
    if (!was_suspended)
        return;

    // Synchronous handoff (see header): wait until the render thread has
    // completed a full pass for this resume request before letting the
    // caller's clock start. The timeout only bites when the render thread is
    // wedged — falling back to the old asynchronous behavior for that one
    // sound is better than stalling the sequencer indefinitely.
    const uint32_t seq = resume_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    suspend_cv_.notify_all();
    if (!running_.load(std::memory_order_relaxed) || pcm_ == nullptr)
        return; // no live render thread to hand off to
    std::unique_lock<std::mutex> lock(ack_mutex_);
    const bool handed_off = ack_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
        return resume_acked_.load(std::memory_order_relaxed) >= seq ||
               !running_.load(std::memory_order_relaxed);
    });
    spdlog::debug("[ALSA] Resuming render thread{}", handed_off ? "" : " (handoff timed out)");
}

void ALSASoundBackend::render_loop() {
    const size_t frames = period_size_;

    // Allocate output buffers once
    std::vector<float> stereo;
    std::vector<int16_t> s16_buf;

    if (channels_ == 2) {
        stereo.resize(frames * 2);
    }
    if (use_s16_) {
        s16_buf.resize(frames * channels_);
    }

    while (running_.load(std::memory_order_relaxed)) {
        // Idle park. Block rather than writing silence forever; resume() or
        // shutdown() wakes us. snd_pcm_* calls stay here, on the render
        // thread, never on the caller's.
        if (suspended_.load(std::memory_order_relaxed)) {
            // Drain rather than drop: the sequencer suspends as soon as its
            // clock says a sound is over, but up to one hardware buffer of
            // already-rendered audio is still queued (~46 ms at the
            // negotiated buffer size). snd_pcm_drop discarded that tail
            // (audible cut-off at the end of sounds); drain lets it play out
            // before parking. On a stream that never started (PREPARED)
            // drain returns immediately.
            if (pcm_) {
                // A sound shorter than the start threshold never started the
                // stream, and drain on a PREPARED stream is a no-op, so its
                // audio would be discarded by the prepare() below. Start it so
                // drain actually plays it.
                if (needs_start_before_drain(snd_pcm_state(pcm_), wrote_since_prepare_)) {
                    int serr = snd_pcm_start(pcm_);
                    if (serr < 0)
                        spdlog::debug("[ALSASound] start before drain: {}", snd_strerror(serr));
                }
                int err = snd_pcm_drain(pcm_);
                if (err < 0)
                    spdlog::debug("[ALSASound] drain on park: {}", snd_strerror(err));
            }
            {
                std::unique_lock<std::mutex> lock(suspend_mutex_);
                suspend_cv_.wait(lock, [this] {
                    return !suspended_.load(std::memory_order_relaxed) ||
                           !running_.load(std::memory_order_relaxed);
                });
            }
            if (!running_.load(std::memory_order_relaxed))
                break;
            if (pcm_) {
                snd_pcm_prepare(pcm_);
                wrote_since_prepare_ = false;
            }
        }

        std::memset(mix_buf_.data(), 0, frames * sizeof(float));
        bool has_audio = false;

        // Check for external render source (tracker PCM playback)
        {
            std::function<void(float*, size_t, int)> source;
            {
                std::lock_guard<std::mutex> lock(render_source_mutex_);
                source = render_source_;
            }
            if (source) {
                source(mix_buf_.data(), frames, static_cast<int>(sample_rate_));
                // Apply shared filter for tracker output
                auto ft = filter_type_.load(std::memory_order_acquire);
                if (ft != helix::audio::FilterType::NONE) {
                    float cutoff = filter_cutoff_.load(std::memory_order_relaxed);
                    helix::audio::update_filter_if_needed(filter_, ft, cutoff,
                                                          static_cast<float>(sample_rate_));
                    helix::audio::apply_filter(filter_, mix_buf_.data(), static_cast<int>(frames));
                }
                has_audio = true;
            }
        }

        // Mix synth voices using VoiceSlot per-sample rendering
        float sr = static_cast<float>(sample_rate_);
        int num_samples = static_cast<int>(frames);
        for (int v = 0; v < MAX_VOICES; ++v) {
            auto& slot = voice_slots_[v];

            // Check for new note (generation changed)
            uint32_t gen = slot.generation.load(std::memory_order_acquire);
            if (gen != slot.cb_generation) {
                slot.cb_generation = gen;
                slot.reset_for_new_note();
                if (slot.active.filter_type != 0) {
                    auto ft = (slot.active.filter_type == 1) ? helix::audio::FilterType::LOWPASS
                                                             : helix::audio::FilterType::HIGHPASS;
                    helix::audio::compute_biquad_coeffs(slot.filter, ft, slot.active.filter_cutoff,
                                                        sr);
                }
            }

            // Skip if silent
            if (slot.active.velocity <= 0.001f && slot.current_amplitude <= 0.001f) {
                continue;
            }

            // Render per-sample
            for (int i = 0; i < num_samples; ++i) {
                mix_buf_[i] += slot.render_sample(sr);
            }
            has_audio = true;
        }

        if (!has_audio) {
            // Write silence but still feed ALSA to avoid underruns
            std::memset(mix_buf_.data(), 0, frames * sizeof(float));
        } else {
            // Clamp
            for (size_t i = 0; i < frames; ++i)
                mix_buf_[i] = std::clamp(mix_buf_[i], -1.0f, 1.0f);
        }

        // Determine what to write
        const void* write_buf = nullptr;

        if (channels_ == 2 && use_s16_) {
            mono_to_stereo(mix_buf_.data(), stereo.data(), frames);
            float_to_s16(stereo.data(), s16_buf.data(), frames * 2);
            write_buf = s16_buf.data();
        } else if (channels_ == 2) {
            mono_to_stereo(mix_buf_.data(), stereo.data(), frames);
            write_buf = stereo.data();
        } else if (use_s16_) {
            float_to_s16(mix_buf_.data(), s16_buf.data(), frames);
            write_buf = s16_buf.data();
        } else {
            write_buf = mix_buf_.data();
        }

        snd_pcm_sframes_t written =
            snd_pcm_writei(pcm_, write_buf, static_cast<snd_pcm_uframes_t>(frames));
        if (written > 0) {
            wrote_since_prepare_ = true;
        }
        if (written < 0) {
            written = recover_xrun(written);
            if (written < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        // Handoff ack: announce that a full pass (render + write) completed
        // for the newest resume request. Bottom-of-loop, not after prepare,
        // so resume() returning means audio is actually flowing. No-op cost
        // when no resume is pending (single atomic load in the common case).
        const uint32_t seq = resume_seq_.load(std::memory_order_relaxed);
        if (resume_acked_.load(std::memory_order_relaxed) != seq) {
            {
                std::lock_guard<std::mutex> lock(ack_mutex_);
                resume_acked_.store(seq, std::memory_order_relaxed);
            }
            ack_cv_.notify_all();
        }
    }
}

snd_pcm_sframes_t ALSASoundBackend::recover_xrun(snd_pcm_sframes_t err) {
    if (err == -EPIPE) {
        // Rate-limited: log at most once every 5 seconds with cumulative
        // count. Default-constructed last_xrun_log_ is epoch, so the first
        // underrun always logs. Avoids 13/sec syslog spam when a device is
        // stuck in a recover loop (see bundle ND8PEP2E).
        ++xrun_count_;
        auto now = std::chrono::steady_clock::now();
        if (now - last_xrun_log_ >= std::chrono::seconds(5)) {
            spdlog::debug("[ALSASound] Buffer underrun (count={}), recovering", xrun_count_);
            last_xrun_log_ = now;
        }
        int ret = snd_pcm_prepare(pcm_);
        wrote_since_prepare_ = false;
        if (ret < 0) {
            spdlog::error("[ALSASound] Cannot recover from underrun: {}", snd_strerror(ret));
            return static_cast<snd_pcm_sframes_t>(ret);
        }
        return 0;
    }

    if (err == -ESTRPIPE) {
        spdlog::debug("[ALSASound] Device suspended, resuming");
        int ret;
        while ((ret = snd_pcm_resume(pcm_)) == -EAGAIN) {
            if (!running_.load(std::memory_order_relaxed))
                return err;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (ret < 0) {
            ret = snd_pcm_prepare(pcm_);
            if (ret < 0) {
                spdlog::error("[ALSASound] Cannot recover from suspend: {}", snd_strerror(ret));
                return static_cast<snd_pcm_sframes_t>(ret);
            }
        }
        return 0;
    }

    return err;
}

void ALSASoundBackend::mono_to_stereo(const float* mono, float* stereo, size_t frame_count) {
    for (size_t i = 0; i < frame_count; ++i) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
}

void ALSASoundBackend::float_to_s16(const float* src, int16_t* dst, size_t sample_count) {
    for (size_t i = 0; i < sample_count; ++i) {
        dst[i] = static_cast<int16_t>(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f);
    }
}

bool ALSASoundBackend::needs_start_before_drain(snd_pcm_state_t state, bool wrote_since_prepare) {
    return state == SND_PCM_STATE_PREPARED && wrote_since_prepare;
}

#endif // HELIX_HAS_ALSA
