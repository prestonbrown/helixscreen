// SPDX-License-Identifier: GPL-3.0-or-later

#include "jz_pwm_sound_backend.h"

#include "note_event.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kVoices = 4;
constexpr const char* kDevice = "/dev/jz_pwm";
// fx-pwm's home depends on whose namespace we exec from: on the rig
// HelixScreen runs INSIDE the Forge-X chroot, where the tool is at its
// ordinary path; a host-side dev build reaches it through the chroot
// mount. tone_player needs the chroot prefix only because klippy runs
// on the host.
constexpr const char* kFxPwmPaths[] = {"/usr/bin/fx-pwm", "/usr/data/.mod/.forge-x/usr/bin/fx-pwm",
                                       nullptr};
constexpr const char* kGpio = "pc12";
// Channel-register bookkeeping only (the DMA rate is fixed by the
// encoded words); 770 MHz / prescale 2 = the calibrated 385 MHz clock.
constexpr long kPrescale = 2;
constexpr const char* kWordsFile = "/tmp/helixscreen-jz-pwm.words";

} // namespace

std::vector<uint32_t> jz_pwm_render_step(const NoteEvent* events, int n_events,
                                         const JzPwmRenderParams& p) {
    std::vector<uint32_t> words;

    if (events == nullptr || n_events <= 0)
        return words;

    VoiceSlot slots[kVoices];
    int active = 0;
    float total_ms = 0;

    for (int v = 0; v < n_events && v < kVoices; ++v) {
        slots[v].event = events[v];
        slots[v].reset_for_new_note();
        if (events[v].velocity > 0.001f && events[v].freq_hz > 0)
            active++;
        total_ms = std::max(total_ms, events[v].duration_ms);
    }
    if (active == 0 || total_ms <= 0)
        return words;

    /* Render the step at its TRUE duration, then tile that content out
     * to the audible floor - a 6 ms theme tick cannot be reproduced
     * duty-encoded (rig-measured), but its tone repeated for the floor
     * is both audible and honest to the note's content. */
    long floor_ms = p.min_note_ms;
    long cap_ms = p.max_note_ms;
    long play_ms = std::clamp((long)total_ms, floor_ms, cap_ms);
    size_t n_true = static_cast<size_t>(total_ms * p.carrier_hz / 1000.0);
    n_true = std::max(n_true, p.word_align);
    n_true -= n_true % p.word_align;
    size_t n = static_cast<size_t>(play_ms * p.carrier_hz / 1000.0);
    n -= n % p.word_align;

    const long period = static_cast<long>(p.clock_hz / p.carrier_hz);
    if (period < 4)
        return words;

    std::vector<uint32_t> rendered;
    rendered.reserve(std::min(n, n_true));
    for (size_t i = 0; i < n_true; ++i) {
        float mix = 0;

        for (int v = 0; v < n_events && v < kVoices; ++v)
            mix += slots[v].render_sample(static_cast<float>(p.carrier_hz));
        mix /= active; /* normalize by sounding voices, not slots */
        mix = std::clamp(mix, -1.0f, 1.0f);

        long on = static_cast<long>(period * (0.5 + p.duty_swing * mix));
        on = std::clamp(on, 1L, period - 1L);
        /* upper 16 bits: inactive counts; lower 16: active counts */
        rendered.push_back(static_cast<uint32_t>(((period - on) << 16) | on));
    }

    words.reserve(n);
    for (size_t i = 0; i < n; ++i)
        words.push_back(rendered[i % n_true]);

    /* Peak-normalize to the full duty swing. Theme velocities and sustain
     * levels leave the effective modulation far under this transducer's
     * audible floor - every rig-verified sound used the full +-40% swing.
     * A one-shot UI sound has no dynamic-range budget worth preserving. */
    const long half = period / 2;
    const long full = (long)(period * p.duty_swing);
    long peak = 0;
    for (uint32_t w : words)
        peak = std::max(peak, std::abs((long)(w & 0xffff) - half));
    if (peak > 0 && peak < full) {
        double gain = (double)full / peak;
        for (auto& w : words) {
            long on = (long)(w & 0xffff);
            long dev = (long)std::lround((on - half) * gain);
            on = std::clamp(half + dev, 1L, period - 1L);
            w = (uint32_t)(((period - on) << 16) | on);
        }
    }
    return words;
}

bool JzPwmSoundBackend::available() {
    if (access(kDevice, F_OK) != 0)
        return false;
    for (int i = 0; kFxPwmPaths[i]; ++i)
        if (access(kFxPwmPaths[i], X_OK) == 0)
            return true;
    return false;
}

JzPwmSoundBackend::JzPwmSoundBackend() = default;

bool JzPwmSoundBackend::initialize() {
    for (int i = 0; kFxPwmPaths[i]; ++i) {
        if (access(kFxPwmPaths[i], X_OK) == 0) {
            fx_pwm_ = kFxPwmPaths[i];
            break;
        }
    }
    if (fx_pwm_.empty() || access(kDevice, F_OK) != 0)
        return false;
    return true;
}

float JzPwmSoundBackend::min_tick_ms() const {
    // One fx-pwm spawn + config + DMA setup per step; below ~60 ms the
    // process churn outpaces the notes.
    return 60.0f;
}

void JzPwmSoundBackend::set_tone(float freq_hz, float amplitude, float duty_cycle) {
    // Never called while supports_note_events() is true (the sequencer
    // drives steps through publish_note); kept for interface parity.
    (void)freq_hz;
    (void)amplitude;
    (void)duty_cycle;
}

void JzPwmSoundBackend::publish_note(int slot, const NoteEvent& event) {
    if (slot < 0 || slot >= kVoices)
        return;
    pending_[slot] = event;
    if (++received_ < kVoices)
        return;
    received_ = 0;
    flush_step();
}

void JzPwmSoundBackend::flush_step() {
    if (fx_pwm_.empty())
        return; /* initialize() never found the tool */

    std::vector<uint32_t> words = jz_pwm_render_step(pending_, kVoices, params_);
    if (words.empty())
        return;

    /* One buffer at a time, no exceptions: a previous child still holding
     * the channel means this sound is dropped, not raced (see
     * stop_child). */
    if (child_ > 0) {
        int status = 0;
        if (waitpid(child_, &status, WNOHANG) == 0) {
            spdlog::debug("[JzPwmBackend] previous step still playing; dropping");
            return;
        }
    }
    stop_child();

    FILE* f = fopen(kWordsFile, "wb");
    if (!f) {
        spdlog::warn("[JzPwmBackend] cannot write {}: {}", kWordsFile, std::strerror(errno));
        return;
    }
    size_t written = fwrite(words.data(), sizeof(uint32_t), words.size(), f);
    fclose(f);
    if (written != words.size()) {
        spdlog::warn("[JzPwmBackend] short write to {}", kWordsFile);
        return;
    }

    float ms = 0;
    for (const NoteEvent& e : pending_)
        ms = std::max(ms, e.duration_ms);
    /* match the renderer's floor/cap so --ms equals the buffer's length */
    ms = std::clamp(ms, static_cast<float>(params_.min_note_ms),
                    static_cast<float>(params_.max_note_ms));

    std::string ms_flag = "--ms=" + std::to_string(static_cast<long>(ms));
    std::string prescale_flag = "--prescale=" + std::to_string(kPrescale);
    /* argv MUST be NULL-terminated: execv walks it until the sentinel,
     * and a missing one reads past the array into garbage - EFAULT, the
     * child dies before a single byte of output. Measured on the rig. */
    std::array<char*, 7> argv = {const_cast<char*>(fx_pwm_.c_str()),
                                 const_cast<char*>("words"),
                                 const_cast<char*>(kGpio),
                                 const_cast<char*>(kWordsFile),
                                 ms_flag.data(),
                                 prescale_flag.data(),
                                 nullptr};

    pid_t pid = fork();
    if (pid == 0) {
        /* Child hygiene before exec: dispositions set to SIG_IGN and
         * blocked signals both SURVIVE execv. An ignored SIGALRM would
         * neuter fx-pwm's watchdog and a blocked mask would make a hung
         * child unkillable - reset both so the child sees the defaults
         * tone_player's children always had. */
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        signal(SIGALRM, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        /* Child diagnostics land in a log, not /dev/null: a silently
         * dying child is indistinguishable from an inaudible one, and
         * the rig proved that difference matters. */
        int log = open("/tmp/jz-child.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
        }
        {
            const char pre[] = "child: pre-exec\n";
            if (write(STDERR_FILENO, pre, sizeof(pre) - 1)) {
            }
        }
        execv(fx_pwm_.c_str(), argv.data());
        {
            char buf[128];
            int n = snprintf(buf, sizeof(buf), "child: execv failed: %s\n", std::strerror(errno));
            if (write(STDERR_FILENO, buf, n)) {
            }
        }
        _exit(127);
    }
    if (pid > 0)
        child_ = pid;
    else
        spdlog::warn("[JzPwmBackend] fork failed: {}", std::strerror(errno));
}

void JzPwmSoundBackend::stop_child() {
    /* Reap only - NEVER signal a running child. Killing fx-pwm between
     * REQUEST and RELEASE orphans the channel claim, and the driver's
     * next REQUEST on an orphaned claim can D-wedge until reboot: a
     * disconnect-driven error-sound storm SIGTERMing its way through
     * children wedged the channel eight deep on the rig. A live child
     * simply outlives the dropped sound; buffers are capped at 2.5 s so
     * the wait is bounded. */
    if (child_ <= 0)
        return;
    int status = 0;
    waitpid(child_, &status, WNOHANG);
    child_ = -1;
}

void JzPwmSoundBackend::silence() {
    stop_child();
}
