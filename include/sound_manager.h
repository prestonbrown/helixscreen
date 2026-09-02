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

    /// Set Moonraker client for M300 backend. Clearing the client (nullptr)
    /// drops an M300 backend; with @p host_recovery (default) the eager
    /// host-backend probe re-runs so a displaced backend returns — printer
    /// switch and transient disconnect want that, since with the client gone
    /// the gcode path is dead and local audio is the only fallback. Full app
    /// shutdown passes false: the manager is torn down moments later and
    /// re-opening audio hardware there is wasted work.
    void set_moonraker_client(IMoonrakerClient* client, bool host_recovery = true);

    /// Auto-detect backend, load theme, start sequencer.
    /// Only considers host-side audio backends (SDL/ALSA/PWM). The M300
    /// (Klipper gcode beeper) backend is installed lazily via
    /// try_install_m300_backend() once hardware discovery confirms the
    /// printer's Klipper config declares a beeper output_pin.
    void initialize();

    /// Install the M300 (Klipper gcode beeper) backend.
    ///
    /// Called from PrinterCapabilitiesState::set_hardware() after hardware
    /// discovery. @p detected_m300_handler is true only when a REAL signal
    /// says klippy answers M300 — a beeper output_pin or an M300 macro in
    /// the Klipper config (has_speaker covers both) — and never when the
    /// speaker capability was merely forced on: firmware-native M300
    /// printers exist, but so do printers where M300 is unhandled and every
    /// command surfaces as `!! Unknown command:M300`, an error toast that
    /// plays error_tone, which sends another M300 — the loop this gate
    /// exists to prevent.
    ///
    /// No-op when a backend is already installed, EXCEPT when detection was
    /// real and the active backend is the PWM sysfs backend: klippy's
    /// tone_player writes the same buzzer channel for every M300/TONE it
    /// handles, so the two backends fight and M300 takes the channel over
    /// (the PWM backend is destroyed; klippy becomes the single writer).
    /// If M300 cannot install (no client, sound disabled) the PWM backend
    /// stays — the box is never left soundless by the swap.
    void try_install_m300_backend(bool detected_m300_handler);

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
    friend class SoundManagerTestAccess;

    SoundManager() = default;
    ~SoundManager() = default;

    /// Detect best available host-side audio backend (SDL/ALSA/PWM).
    /// Does NOT include M300 — see try_install_m300_backend().
    std::shared_ptr<SoundBackend> create_backend();

    /// Run the eager host-backend probe and finish setup when one is found
    /// (sequencer + theme via finalize_backend_setup()). When none is found,
    /// mark initialized_ anyway so try_install_m300_backend() stays reachable
    /// for the late M300 install.
    void probe_and_install_host_backend();

    /// Join the sequencer thread, then release the backend it drives — in
    /// that order: the sequencer's tick loop calls into the backend, so the
    /// thread must be dead before the backend it references is destroyed.
    /// Every path that swaps or drops a backend goes through here.
    void tear_down_active_backend();

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
