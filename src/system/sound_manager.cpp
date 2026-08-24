// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sound_manager.h"

#include "audio_settings_manager.h"
#include "config.h"
#include "data_root_resolver.h"
#include "i_moonraker_client.h"
#include "m300_sound_backend.h"
#include "pwm_sound_backend.h"
#include "runtime_config.h"
#include "sound_backend.h"
#include "sound_sequencer.h"
#include "sound_theme.h"

#ifdef HELIX_DISPLAY_SDL
#include "sdl_sound_backend.h"
#endif

#ifdef HELIX_HAS_ALSA
#include "alsa_device_enum.h"
#include "alsa_sound_backend.h"
#endif

#ifdef HELIX_HAS_TRACKER
#include "tracker_module.h"
#include "tracker_player.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <string>

using namespace helix;

// ============================================================================
// SoundManager singleton
// ============================================================================

SoundManager& SoundManager::instance() {
    static SoundManager instance;
    return instance;
}

void SoundManager::set_moonraker_client(IMoonrakerClient* client) {
    client_ = client;
    spdlog::debug("[SoundManager] Moonraker client set: {}", client ? "connected" : "nullptr");

    // If the client is being cleared (printer switch / app shutdown) and the
    // active backend is M300, drop it. The next printer's hardware discovery
    // will reinstall via try_install_m300_backend() only if appropriate —
    // otherwise we'd carry M300 over to a new printer that has no beeper,
    // resurrecting the "!! Unknown command:M300" feedback loop.
    if (!client && backend_ && backend_->needs_moonraker_client()) {
        spdlog::info("[SoundManager] Dropping M300 backend (client cleared)");
        if (sequencer_) {
            sequencer_->shutdown();
            sequencer_.reset();
        }
        backend_.reset();
    }
}

void SoundManager::initialize() {
    if (initialized_) {
        spdlog::debug("[SoundManager] Already initialized");
        return;
    }

    // Check if sounds are disabled via CLI flag or persistent setting
    auto* rtconfig = get_runtime_config();
    if (rtconfig->disable_sound) {
        spdlog::info("[SoundManager] Sound disabled via --no-sound, skipping initialization");
        return;
    }
    {
        Config* cfg = Config::get_instance();
        if (cfg && cfg->get<bool>("/disable_sound", false)) {
            spdlog::info(
                "[SoundManager] Sound disabled via settings.json, skipping initialization");
            rtconfig->disable_sound = true;
            return;
        }
    }

    // Create the best available host-side backend (SDL/ALSA/PWM). M300 is
    // NOT a fallback here — it's installed lazily once hardware discovery
    // confirms the printer's Klipper config has a beeper. Selecting M300
    // for any Moonraker-connected printer would let the
    // M300 → "!! Unknown command:M300" → error toast → error_tone → M300
    // feedback loop fire on any host where local audio fails.
    backend_ = create_backend();
    if (!backend_) {
        // Mark initialized so try_install_m300_backend() can still install
        // an M300 backend later when discovery confirms the printer has a
        // beeper. Without this, has_backend() would gate on initialized_
        // and miss the late install.
        initialized_ = true;
        spdlog::info(
            "[SoundManager] No host audio backend; awaiting hardware discovery for M300 gate");
        return;
    }

    finalize_backend_setup();
}

void SoundManager::try_install_m300_backend() {
    if (backend_) {
        return; // Real audio already winning — don't displace it.
    }
    if (!client_) {
        spdlog::debug("[SoundManager] try_install_m300_backend: no Moonraker client");
        return;
    }
    if (get_runtime_config()->disable_sound) {
        return;
    }
    Config* cfg = Config::get_instance();
    if (cfg && cfg->get<bool>("/disable_sound", false)) {
        return;
    }

    spdlog::info("[SoundManager] Installing M300 backend (Klipper beeper confirmed)");
    backend_ = std::make_shared<M300SoundBackend>([this](const std::string& gcode) -> int {
        auto* c = client_;
        return c ? c->gcode_script(gcode) : -1;
    });

    finalize_backend_setup();
}

void SoundManager::finalize_backend_setup() {
    if (!backend_) {
        return;
    }

    // Defer theme loading until sounds are actually needed
    if (current_theme_.sounds.empty()) {
        theme_name_ = AudioSettingsManager::instance().get_sound_theme();
        if (AudioSettingsManager::instance().get_sounds_enabled()) {
            load_theme(theme_name_);
        } else {
            spdlog::info("[SoundManager] Sounds disabled, deferring theme load");
        }
    }

    if (!sequencer_) {
        sequencer_ = std::make_unique<SoundSequencer>(backend_);
        sequencer_->start();
    }

    initialized_ = true;
    spdlog::info("[SoundManager] Backend ready, theme '{}'", theme_name_);
}

bool SoundManager::has_alsa_backend() const {
    return backend_ && backend_->supports_device_selection();
}

bool SoundManager::set_output_device(const std::string& pcm) {
#ifdef HELIX_HAS_ALSA
    // Env override locks the picker (HELIX_ALSA_DEVICE wins, mirrors HELIX_DRM_DEVICE).
    if (const char* e = std::getenv("HELIX_ALSA_DEVICE"); e && e[0] != '\0') {
        spdlog::info("[SoundManager] set_output_device ignored: HELIX_ALSA_DEVICE override active");
        return false;
    }
    if (!has_alsa_backend()) {
        spdlog::debug("[SoundManager] set_output_device: active backend is not ALSA");
        return false;
    }

    // Tear down sequencer (joins its thread) and the current backend.
    if (sequencer_) {
        sequencer_->shutdown();
        sequencer_.reset();
    }
    backend_.reset();

    // Open the requested device; fall back to "default" on failure.
    auto alsa = std::make_shared<ALSASoundBackend>();
    std::string effective = pcm;
    bool ok = alsa->initialize(pcm);
    if (!ok) {
        spdlog::warn("[SoundManager] ALSA device '{}' failed; reverting to 'default'", pcm);
        effective = "default";
        ok = alsa->initialize("default");
    }
    if (!ok) {
        spdlog::error("[SoundManager] Audio reinit failed; no backend installed");
        initialized_ = true; // keep the M300 gate reachable
        return false;
    }

    backend_ = alsa;
    AudioSettingsManager::instance().set_output_device(effective); // persist what opened
    finalize_backend_setup();                                      // recreate sequencer + theme
    play("test_beep");
    return effective == pcm;
#else
    (void)pcm;
    return false;
#endif
}

void SoundManager::shutdown() {
    if (!initialized_)
        return;

#ifdef HELIX_HAS_TRACKER
    stop_tracker();
#endif

    if (sequencer_) {
        sequencer_->shutdown();
        sequencer_.reset();
    }

    backend_.reset();
    initialized_ = false;

    spdlog::info("[SoundManager] Shutdown complete");
}

void SoundManager::play(const std::string& sound_name) {
    play(sound_name, SoundPriority::UI);
}

void SoundManager::play(const std::string& sound_name, SoundPriority priority) {
    // Master switch
    if (!AudioSettingsManager::instance().get_sounds_enabled()) {
        spdlog::trace("[SoundManager] play('{}') skipped - sounds disabled", sound_name);
        return;
    }

    // UI sounds have their own toggle
    if (is_ui_sound(sound_name) && !AudioSettingsManager::instance().get_ui_sounds_enabled()) {
        spdlog::trace("[SoundManager] play('{}') skipped - UI sounds disabled", sound_name);
        return;
    }

    if (!sequencer_ || !backend_) {
        spdlog::debug("[SoundManager] play('{}') skipped - no sequencer/backend", sound_name);
        return;
    }

    // Lazy-load theme on first use (deferred if sounds were disabled at init)
    if (current_theme_.sounds.empty() && !theme_name_.empty()) {
        load_theme(theme_name_);
    }

    // Look up sound in current theme
    auto it = current_theme_.sounds.find(sound_name);
    if (it == current_theme_.sounds.end()) {
        spdlog::debug("[SoundManager] play('{}') - sound not in theme '{}'", sound_name,
                      theme_name_);
        return;
    }

#ifdef HELIX_HAS_TRACKER
    if (tracker_ && tracker_->is_playing()) {
        if (static_cast<int>(priority) >= static_cast<int>(SoundPriority::ALARM)) {
            // ALARM always kills tracker
            stop_tracker();
        } else if (backend_ && backend_->supports_render_source()) {
            // PCM-capable backend — SFX layers on top of tracker, no preemption
            spdlog::trace("[SoundManager] play('{}') layered with tracker", sound_name);
        } else {
            // Frequency-only backend — can't mix, skip SFX
            spdlog::debug("[SoundManager] play('{}') skipped - tracker active, no mixing",
                          sound_name);
            return;
        }
    }
#endif

    sequencer_->play(it->second, priority);
    spdlog::debug("[SoundManager] play('{}', priority={})", sound_name, static_cast<int>(priority));
}

void SoundManager::play(const SoundDefinition& sound, SoundPriority priority) {
    if (!AudioSettingsManager::instance().get_sounds_enabled())
        return;
    if (!sequencer_ || !backend_)
        return;

#ifdef HELIX_HAS_TRACKER
    if (tracker_ && tracker_->is_playing()) {
        if (static_cast<int>(priority) >= static_cast<int>(SoundPriority::ALARM)) {
            stop_tracker();
        } else if (!backend_->supports_render_source()) {
            return; // Can't mix on frequency-only backends
        }
    }
#endif

    sequencer_->play(sound, priority);
}

void SoundManager::play_test_beep() {
    play("test_beep");
}

void SoundManager::play_print_complete() {
    play("print_complete", SoundPriority::EVENT);
}

void SoundManager::play_error_alert() {
    play("error_alert", SoundPriority::EVENT);
}

void SoundManager::set_theme(const std::string& name) {
    theme_name_ = name;
    load_theme(name);
    spdlog::info("[SoundManager] Theme changed to '{}'", name);
}

std::string SoundManager::get_current_theme() const {
    return theme_name_;
}

std::vector<std::string> SoundManager::get_available_themes() const {
    std::vector<std::string> themes;

    // Sound themes are RO seeds shipped with the package; user themes
    // (if any) are merged from the writable config dir.
    auto enumerate = [&themes](const std::string& sounds_dir) {
        DIR* dir = opendir(sounds_dir.c_str());
        if (!dir) {
            spdlog::debug("[SoundManager] Could not open {}", sounds_dir);
            return;
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json") {
                themes.push_back(filename.substr(0, filename.size() - 5));
            }
        }
        closedir(dir);
    };

    // Shipped RO sound theme JSONs live under HELIX_DATA_DIR/assets/config/sounds;
    // user-added themes live in the writable config dir.
    enumerate(helix::get_data_dir() + "/assets/config/sounds");
    enumerate(helix::writable_path("sounds"));

    // Dedupe before sorting (user themes can shadow shipped ones by name).
    std::sort(themes.begin(), themes.end());
    themes.erase(std::unique(themes.begin(), themes.end()), themes.end());
    return themes;
}

std::vector<std::string> SoundManager::get_sound_names() const {
    std::vector<std::string> names;
    names.reserve(current_theme_.sounds.size());
    for (const auto& [key, _] : current_theme_.sounds) {
        names.push_back(key);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool SoundManager::is_available() const {
    return initialized_ && backend_ != nullptr &&
           AudioSettingsManager::instance().get_sounds_enabled();
}

bool SoundManager::has_backend() const {
    return initialized_ && backend_ != nullptr;
}

std::shared_ptr<SoundBackend> SoundManager::create_backend() {
    // Host-side audio backends only. M300 (Klipper beeper) is installed
    // separately via try_install_m300_backend() once hardware discovery
    // confirms the printer's Klipper config has a beeper output_pin.
    //
    // Auto-detection order:
    // 1. SDL audio available (desktop build) -> SDLBackend
    // 2. ALSA PCM available (Linux with audio) -> ALSABackend
    // 3. /sys/class/pwm/pwmchip0 exists -> PWMBackend
    // 4. None -> wait for M300 gate, or sounds disabled

#ifdef HELIX_DISPLAY_SDL
    auto sdl_backend = std::make_shared<SDLSoundBackend>();
    if (sdl_backend->initialize()) {
        spdlog::info("[SoundManager] Using SDL audio backend");
        return sdl_backend;
    }
    spdlog::warn("[SoundManager] SDL audio init failed, falling back");
#endif

#ifdef HELIX_HAS_ALSA
    {
        auto alsa_backend = std::make_shared<ALSASoundBackend>();
        std::string dev = helix::audio::resolve_alsa_device();
        if (alsa_backend->initialize(dev)) {
            spdlog::info("[SoundManager] Using ALSA PCM backend ('{}')", dev);
            return alsa_backend;
        }
        // Resilience: a stale saved/env device must not kill audio — retry the
        // ALSA default before falling through to PWM/M300.
        if (dev != "default" && alsa_backend->initialize("default")) {
            spdlog::warn("[SoundManager] ALSA device '{}' failed; using 'default'", dev);
            return alsa_backend;
        }
        spdlog::debug("[SoundManager] ALSA not available, falling back");
    }
#endif

#ifdef HELIX_HAS_PWM_SOUND
    // PWM sysfs buzzer. Gated per-platform, NOT probed everywhere: the probe is
    // "does /sys/class/pwm/pwmchip0 exist", which is true on boards that have a
    // PWM controller and no buzzer on it. A CC1 has 8 channels there and no
    // beeper wired to any of them, so an ungated probe means exporting and
    // driving a channel that belongs to something else -- its backlight is a
    // platform device on the same controller. Only enable this where the buzzer
    // is known to exist (AD5M/AD5X).
    auto pwm_backend = std::make_shared<PWMSoundBackend>();
    if (pwm_backend->initialize()) {
        spdlog::info("[SoundManager] Using PWM sysfs backend ({})", pwm_backend->channel_path());
        return pwm_backend;
    }
    spdlog::debug("[SoundManager] PWM sysfs not available, falling back");
#endif

    spdlog::debug("[SoundManager] No host audio backend available");
    return nullptr;
}

void SoundManager::load_theme(const std::string& name) {
    std::string path = helix::find_readable("sounds/" + name + ".json");
    auto theme = SoundThemeParser::load_from_file(path);

    if (theme) {
        current_theme_ = std::move(*theme);
        spdlog::info("[SoundManager] Loaded theme '{}' ({} sounds)", name,
                     current_theme_.sounds.size());
    } else {
        spdlog::warn("[SoundManager] Failed to load theme '{}', keeping current", name);
        // If no theme is loaded at all, try default as fallback
        if (current_theme_.sounds.empty() && name != "default") {
            spdlog::info("[SoundManager] Attempting fallback to 'default' theme");
            auto fallback =
                SoundThemeParser::load_from_file(helix::find_readable("sounds/default.json"));
            if (fallback) {
                current_theme_ = std::move(*fallback);
                theme_name_ = "default";
            }
        }
    }
}

bool SoundManager::is_ui_sound(const std::string& name) {
    // UI interaction sounds — affected by ui_sounds_enabled toggle
    return name == "button_tap" || name == "toggle_on" || name == "toggle_off" ||
           name == "nav_forward" || name == "nav_back" || name == "dropdown_open";
}

bool SoundManager::can_mix() const {
    return backend_ && backend_->supports_render_source();
}

// ============================================================================
// Tracker playback (MOD/MED files)
// ============================================================================

#ifdef HELIX_HAS_TRACKER
void SoundManager::play_file(const std::string& path, SoundPriority priority) {
    if (!AudioSettingsManager::instance().get_sounds_enabled()) {
        spdlog::trace("[SoundManager] play_file('{}') skipped - sounds disabled", path);
        return;
    }
    if (!backend_ || !sequencer_) {
        spdlog::debug("[SoundManager] play_file('{}') skipped - no backend/sequencer", path);
        return;
    }

    // Don't restart if same file is already playing
    if (tracker_ && tracker_->is_playing() && tracker_path_ == path) {
        spdlog::debug("[SoundManager] play_file('{}') skipped - already playing", path);
        return;
    }

    auto module = helix::audio::TrackerModule::load(path);
    if (!module) {
        spdlog::warn("[SoundManager] play_file('{}') - failed to load", path);
        return;
    }

    // IMPORTANT: Clear external tick FIRST to prevent use-after-free on old tracker
    sequencer_->set_external_tick(nullptr);
    if (tracker_) {
        tracker_->stop();
    }
    sequencer_->stop();

    // Create and start tracker
    tracker_ = std::make_unique<helix::audio::TrackerPlayer>(backend_);
    tracker_->load(std::move(*module));
    tracker_priority_ = priority;
    tracker_path_ = path;
    tracker_->play();

    // Route ticks to tracker for sequencing
    auto* tp = tracker_.get();
    sequencer_->set_external_tick([tp](float dt_ms) { tp->tick(dt_ms); });

    // Set render source for PCM sample playback on capable backends.
    // Frequency-only backends (PWM, M300) get synth fallback via set_voice().
    if (backend_->supports_render_source()) {
        backend_->set_render_source(
            [tp](float* buf, size_t frames, int sr) { tp->render_audio(buf, frames, sr); });
    }

    spdlog::info("[SoundManager] play_file('{}', priority={})", path, static_cast<int>(priority));
}

void SoundManager::stop_tracker() {
    // IMPORTANT: Clear render source and external tick BEFORE destroying tracker
    if (backend_) {
        backend_->clear_render_source();
    }
    if (sequencer_) {
        sequencer_->set_external_tick(nullptr);
    }
    if (tracker_) {
        tracker_->stop();
        tracker_.reset();
    }
    tracker_path_.clear();
    spdlog::debug("[SoundManager] stop_tracker");
}

bool SoundManager::is_tracker_playing() const {
    return tracker_ && tracker_->is_playing();
}

void SoundManager::fade_out_tracker(uint32_t duration_ms) {
    if (!tracker_ || !tracker_->is_playing())
        return;

    spdlog::debug("[SoundManager] fade_out_tracker over {}ms", duration_ms);

    // Fade volume from current to 0 in steps on a background thread.
    // TrackerPlayer::set_volume_override() is thread-safe.
    auto* tp = tracker_.get();
    auto fade_start = std::chrono::steady_clock::now();
    auto fade_dur = std::chrono::milliseconds(duration_ms);

    // Use sequencer thread indirectly: set up a tick-based fade via external_tick wrapper
    auto original_tick = [tp](float dt_ms) { tp->tick(dt_ms); };

    auto fade_end = fade_start + fade_dur;
    sequencer_->set_external_tick([this, tp, original_tick, fade_start, fade_end](float dt_ms) {
        original_tick(dt_ms);

        auto now = std::chrono::steady_clock::now();
        if (now >= fade_end) {
            // Fade complete — stop tracker on next iteration
            tp->set_volume_override(0);
            // Schedule stop (can't call stop_tracker from within the tick)
            tp->stop();
            return;
        }

        float elapsed_ms = std::chrono::duration<float, std::milli>(now - fade_start).count();
        float total_ms = std::chrono::duration<float, std::milli>(fade_end - fade_start).count();
        float progress = elapsed_ms / total_ms;
        int vol = static_cast<int>(100.0f * (1.0f - progress));
        tp->set_volume_override(std::max(0, vol));
    });
}
#endif
