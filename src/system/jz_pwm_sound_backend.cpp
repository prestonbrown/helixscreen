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

    total_ms = std::min(total_ms, static_cast<float>(p.max_note_ms));
    size_t n = static_cast<size_t>(total_ms * p.carrier_hz / 1000.0);
    n = std::max(n, p.word_align);
    n -= n % p.word_align;

    const long period = static_cast<long>(p.clock_hz / p.carrier_hz);
    if (period < 4)
        return words;

    words.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        float mix = 0;

        for (int v = 0; v < n_events && v < kVoices; ++v)
            mix += slots[v].render_sample(static_cast<float>(p.carrier_hz));
        mix /= active; /* normalize by sounding voices, not slots */
        mix = std::clamp(mix, -1.0f, 1.0f);

        long on = static_cast<long>(period * (0.5 + p.duty_swing * mix));
        on = std::clamp(on, 1L, period - 1L);
        /* upper 16 bits: inactive counts; lower 16: active counts */
        words.push_back(static_cast<uint32_t>(((period - on) << 16) | on));
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
    ms = std::min(ms, static_cast<float>(params_.max_note_ms));

    std::string ms_flag = "--ms=" + std::to_string(static_cast<long>(ms));
    std::string prescale_flag = "--prescale=" + std::to_string(kPrescale);
    std::array<char*, 6> argv = {const_cast<char*>(fx_pwm_.c_str()),
                                 const_cast<char*>("words"),
                                 const_cast<char*>(kGpio),
                                 const_cast<char*>(kWordsFile),
                                 ms_flag.data(),
                                 prescale_flag.data()};

    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execv(fx_pwm_.c_str(), argv.data());
        _exit(127);
    }
    if (pid > 0)
        child_ = pid;
    else
        spdlog::warn("[JzPwmBackend] fork failed: {}", std::strerror(errno));
}

void JzPwmSoundBackend::stop_child() {
    if (child_ <= 0)
        return;
    int status = 0;
    if (waitpid(child_, &status, WNOHANG) == 0) {
        // fx-pwm's signal handler releases the channel before exiting -
        // arming it is why the tool exists in this shape. If the child
        // is wedged in the driver (D-state) the signal is a no-op: the
        // next spawn will wedge too and sound is lost until reboot, but
        // the printer is never affected.
        kill(child_, SIGTERM);
        waitpid(child_, &status, WNOHANG);
    }
    child_ = -1;
}

void JzPwmSoundBackend::silence() {
    stop_child();
}
