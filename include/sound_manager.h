// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sound_sequencer.h"
#include "sound_theme.h"

#ifdef HELIX_HAS_TRACKER
#include "tracker_player.h"
#endif

#include <memory>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerClient;
}

namespace helix {

/**
 * @brief Audio feedback manager using the synth engine
 *
 * Plays named sounds from JSON themes through a backend-agnostic sequencer.
 * Auto-detects the best host-side backend (SDL/ALSA/PWM) at initialize();
 * M300 (Klipper gcode beeper) is installed lazily via
 * try_install_m300_backend() once hardware discovery confirms the printer's
 * Klipper config has a `[output_pin beeper]` (and matching M300 macro).
 *
 * Respects SettingsManager toggles:
 * - sounds_enabled: master switch for all sounds
 * - ui_sounds_enabled: separate toggle for UI interaction sounds (button taps, nav)
 *
 * ## Usage:
 * @code
 * auto& sound = SoundManager::instance();
 * sound.set_moonraker_client(client);
 * sound.initialize();
 *
 * sound.play("button_tap");
 * sound.play("print_complete", SoundPriority::EVENT);
 * @endcode
 */
class SoundManager {
  public:
    static SoundManager& instance();

    // Prevent copying
    SoundManager(const SoundManager&) = delete;
    SoundManager& operator=(const SoundManager&) = delete;

    /// Set Moonraker client for M300 backend
    void set_moonraker_client(IMoonrakerClient* client);

    /// Auto-detect backend, load theme, start sequencer.
    /// Only considers host-side audio backends (SDL/ALSA/PWM). The M300
    /// (Klipper gcode beeper) backend is installed lazily via
    /// try_install_m300_backend() once hardware discovery confirms the
    /// printer's Klipper config declares a beeper output_pin.
    void initialize();

    /// Install the M300 (Klipper gcode beeper) backend if no audio backend
    /// is currently installed and a Moonraker client is available.
    ///
    /// Called from PrinterCapabilitiesState::set_hardware() after hardware
    /// discovery confirms `hardware.has_speaker()`. Installing M300 only
    /// when Klipper actually has a `[output_pin beeper]` (and matching
    /// `[gcode_macro M300]`) prevents the feedback loop where M300 commands
    /// to a printer without the macro produce `!! Unknown command:M300`,
    /// surface as error toasts, and trigger the error_tone sound — which
    /// goes back out as another M300, ad infinitum.
    ///
    /// No-op if a backend is already installed, if no client has been set,
    /// if sounds are globally disabled, or if SoundManager has been
    /// shutdown.
    void try_install_m300_backend();

    /// Stop sequencer, cleanup
    void shutdown();

    /// Play a named sound from the current theme (UI priority)
    void play(const std::string& sound_name);

    /// Play a named sound with explicit priority
    void play(const std::string& sound_name, SoundPriority priority);

    /// Play a raw SoundDefinition directly (for hardcoded SFX)
    void play(const SoundDefinition& sound, SoundPriority priority);

    /// Backward compatibility: calls play("test_beep")
    void play_test_beep();

    /// Backward compatibility: calls play("print_complete", EVENT)
    void play_print_complete();

    /// Backward compatibility: calls play("error_alert", EVENT)
    void play_error_alert();

    /// Set active theme by name (loads from config/sounds/<name>.json)
    void set_theme(const std::string& theme_name);

    /// Get current theme name
    std::string get_current_theme() const;

    /// Scan config/sounds/ for available .json theme files
    std::vector<std::string> get_available_themes() const;

    /// Switch the live ALSA output device. Persists, tears down and rebuilds the
    /// ALSA backend, reloads theme/sequencer, plays a test beep. Returns true if
    /// the requested device opened; false if not ALSA, if HELIX_ALSA_DEVICE is
    /// set (env lock), or if it fell back to "default".
    bool set_output_device(const std::string& pcm);

    /// True when the active backend is the runtime-selectable ALSA backend.
    [[nodiscard]] bool has_alsa_backend() const;

    /// Get sorted list of sound names in the current theme
    std::vector<std::string> get_sound_names() const;

    /// Check if sound playback is available (backend exists + sounds enabled)
    [[nodiscard]] bool is_available() const;

    /// Check if a sound backend was detected (regardless of sounds_enabled toggle)
    [[nodiscard]] bool has_backend() const;

#ifdef HELIX_HAS_TRACKER
    /// Play a MOD/MED tracker file
    void play_file(const std::string& path, SoundPriority priority = SoundPriority::EVENT);

    /// Stop tracker playback
    void stop_tracker();

    /// Fade tracker volume to zero over duration_ms, then stop
    void fade_out_tracker(uint32_t duration_ms);

    /// Check if tracker is currently playing
    bool is_tracker_playing() const;
#endif

    /// Check if backend supports concurrent tracker + SFX mixing
    [[nodiscard]] bool can_mix() const;

  private:
    SoundManager() = default;
    ~SoundManager() = default;

    /// Detect best available host-side audio backend (SDL/ALSA/PWM).
    /// Does NOT include M300 — see try_install_m300_backend().
    std::shared_ptr<SoundBackend> create_backend();

    /// Common setup after a backend is installed: load theme, create and
    /// start sequencer, mark initialized. Idempotent.
    void finalize_backend_setup();

    /// Load theme JSON from config/sounds/
    void load_theme(const std::string& theme_name);

    /// Check if a sound name is a UI sound (affected by ui_sounds_enabled)
    static bool is_ui_sound(const std::string& name);

    IMoonrakerClient* client_ = nullptr;
    std::unique_ptr<SoundSequencer> sequencer_;
    std::shared_ptr<SoundBackend> backend_;
    SoundTheme current_theme_;
    std::string theme_name_ = "default";
    bool initialized_ = false;

#ifdef HELIX_HAS_TRACKER
    std::unique_ptr<helix::audio::TrackerPlayer> tracker_;
    SoundPriority tracker_priority_ = SoundPriority::UI;
    std::string tracker_path_;
#endif
};

} // namespace helix
