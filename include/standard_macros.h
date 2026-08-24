// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file standard_macros.h
 * @brief Unified registry for mapping semantic operations to printer macros
 *
 * The StandardMacros system provides:
 * - Semantic macro slots (Load Filament, Pause, Clean Nozzle, etc.)
 * - Auto-detection from printer via naming patterns
 * - Fallback to HELIX_* helper macros when printer doesn't have its own
 * - User configuration via Settings overlay
 * - Graceful handling of empty slots
 *
 * @pattern Singleton with priority-based resolution
 * @threading Main thread only (not thread-safe)
 */

#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "subject_managed_panel.h" // SubjectManager

#include <functional>
#include <lvgl.h>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Forward declarations
class IMoonrakerAPI;
struct MoonrakerError;

namespace helix {
class PrinterDiscovery;
}

/**
 * @brief Standard macro slot identifiers
 *
 * These represent semantic operations that can be mapped to printer-specific macros.
 */
enum class StandardMacroSlot {
    LoadFilament,   ///< Load filament into toolhead
    UnloadFilament, ///< Unload filament from toolhead
    Purge,          ///< Purge/prime nozzle
    Pause,          ///< Pause current print
    Resume,         ///< Resume paused print
    Cancel,         ///< Cancel current print
    BedMesh,        ///< Bed mesh calibration (BED_MESH_CALIBRATE/G29)
    BedLevel,       ///< Physical bed leveling (QGL/Z-Tilt)
    CleanNozzle,    ///< Nozzle cleaning/wiping
    HeatSoak,       ///< Chamber/bed heat soak

    COUNT ///< Number of slots (for iteration)
};

/**
 * @brief Source of the macro assignment for a slot
 */
enum class MacroSource {
    NONE,       ///< No macro assigned
    CONFIGURED, ///< User explicitly configured in Settings
    DETECTED,   ///< Auto-detected from printer
    FALLBACK    ///< Using HELIX_* fallback macro
};

/**
 * @brief Information about a standard macro slot
 *
 * Contains the slot's identity, current assignment, and resolution details.
 */
struct StandardMacroInfo {
    StandardMacroSlot slot; ///< The slot enum value

    std::string slot_name;    ///< Machine name: "load_filament"
    std::string display_name; ///< English key for translation: "Load Filament"

    /// @brief Get translated display name for current locale
    [[nodiscard]] const char* translated_name() const;

    std::string configured_macro; ///< User override the printer defines (or empty)
    std::string detected_macro;   ///< Auto-detected (or empty)
    std::string fallback_macro;   ///< HELIX_* fallback (or empty)

    /**
     * @brief The user's configured macro when the connected printer has no such
     *        gcode_macro.
     *
     * StandardMacros::init() moves an unverifiable name out of
     * `configured_macro` and into here, so is_empty() / get_macro() /
     * get_source() answer as though the slot were unconfigured: dispatch falls
     * through to the detected macro, the HELIX fallback, or the caller's own
     * fallback path instead of sending a command Klipper will reject.
     *
     * The name is kept rather than dropped for two reasons — save_to_config()
     * round-trips the user's choice (the printer may just be mid-restart), and
     * the UI can distinguish "you configured this and it is broken" from
     * "nothing is assigned here".
     */
    std::string missing_macro;

    /**
     * @brief Did the user configure a macro this printer does not define?
     * @return true when the configured name was demoted by init()
     */
    [[nodiscard]] bool has_missing_macro() const {
        return !missing_macro.empty();
    }

    /**
     * @brief The macro name the user asked for, whether or not it resolves.
     *
     * Settings shows this so a demoted slot still displays what was chosen.
     */
    [[nodiscard]] const std::string& requested_macro() const {
        return configured_macro.empty() ? missing_macro : configured_macro;
    }

    /**
     * @brief Check if this slot has no usable macro
     * @return true if all three sources are empty
     */
    [[nodiscard]] bool is_empty() const {
        return configured_macro.empty() && detected_macro.empty() && fallback_macro.empty();
    }

    /**
     * @brief Get the resolved macro name
     *
     * Priority: configured > detected > fallback
     *
     * @return First non-empty macro name, or empty string if none
     */
    [[nodiscard]] std::string get_macro() const {
        if (!configured_macro.empty())
            return configured_macro;
        if (!detected_macro.empty())
            return detected_macro;
        return fallback_macro;
    }

    /**
     * @brief Get the source of the current macro assignment
     * @return The MacroSource indicating where the macro came from
     */
    [[nodiscard]] MacroSource get_source() const {
        if (!configured_macro.empty())
            return MacroSource::CONFIGURED;
        if (!detected_macro.empty())
            return MacroSource::DETECTED;
        if (!fallback_macro.empty())
            return MacroSource::FALLBACK;
        return MacroSource::NONE;
    }
};

/**
 * @brief Unified registry for standard macro operations (singleton)
 *
 * Maps semantic operations (Load Filament, Pause, etc.) to printer-specific
 * G-code macros using a priority-based resolution system:
 *
 * 1. User configured - Explicit selection in Settings
 * 2. Auto-detected - Found on printer via pattern matching
 * 3. HELIX fallback - HelixScreen's helper macro (if available)
 * 4. Empty - No macro; functionality disabled
 *
 * @code
 * // Initialize after printer discovery
 * StandardMacros::instance().init(capabilities);
 *
 * // Execute a macro
 * StandardMacros::instance().execute(
 *     StandardMacroSlot::LoadFilament, api,
 *     []() { spdlog::info("Loading..."); },
 *     [](const auto& err) { spdlog::error("Failed: {}", err.message); }
 * );
 *
 * // Check if slot is available
 * if (!StandardMacros::instance().get(StandardMacroSlot::CleanNozzle).is_empty()) {
 *     // Show clean nozzle button
 * }
 * @endcode
 */
class StandardMacros {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    /**
     * @brief Get singleton instance
     * @return Reference to global StandardMacros instance
     */
    static StandardMacros& instance();

    // Non-copyable
    StandardMacros(const StandardMacros&) = delete;
    StandardMacros& operator=(const StandardMacros&) = delete;

    /**
     * @brief Initialize with hardware discovery
     *
     * Call this after printer discovery to enable auto-detection.
     * Loads user config and runs pattern matching on available macros.
     *
     * @param hardware Hardware discovery with discovered macros
     */
    void init(const helix::PrinterDiscovery& hardware);

    // ========================================================================
    // Observability
    // ========================================================================

    /**
     * @brief Initialize the subjects this registry publishes
     *
     * Called from SubjectInitializer before any XML is instantiated. Separate
     * from init(): the resolution table exists from construction, but LVGL is
     * not up yet at that point.
     *
     * @param register_xml Expose the subjects under their XML names
     */
    void init_subjects(bool register_xml = true);

    /// @brief Tear down the subjects; self-registered with StaticSubjectRegistry.
    void deinit_subjects();

    /**
     * @brief Death signal for the subjects this singleton owns.
     *
     * Observers outside StandardMacros MUST pass this to observe_*().
     * deinit_subjects() frees every observer node on these subjects, so a guard
     * without the token dereferences freed memory on its next reset().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_lifetime_;
    }

    /**
     * @brief Monotonic counter bumped whenever slot resolution changes
     *
     * Which macros a printer has is not fixed: a Klipper restart or a config
     * change adds and removes gcode_macros under a live UI, and every init()
     * re-runs detection and re-validates the configured names against the new
     * list. Surfaces that render slot availability observe this instead of
     * sampling once at panel build.
     */
    lv_subject_t* get_macros_version_subject() {
        return &macros_version_;
    }

    /**
     * @brief Reset to uninitialized state
     *
     * Clears all detected macros. User config is preserved.
     * Call init() again after reconnecting to printer.
     */
    void reset();

    /**
     * @brief Check if initialized
     * @return true if init() has been called
     */
    [[nodiscard]] bool is_initialized() const {
        return initialized_;
    }

    // ========================================================================
    // Slot Access
    // ========================================================================

    /**
     * @brief Get info for a specific slot
     * @param slot The slot to query
     * @return Reference to slot info (valid until next init/reset)
     */
    [[nodiscard]] const StandardMacroInfo& get(StandardMacroSlot slot) const;

    /**
     * @brief Get all slot infos
     *
     * Returns slots in enum order. Useful for UI listing.
     *
     * @return Vector of all slot infos
     */
    [[nodiscard]] const std::vector<StandardMacroInfo>& all() const {
        return slots_;
    }

    /**
     * @brief Get slot enum from slot name
     * @param name Slot name (e.g., "load_filament")
     * @return Slot enum, or nullopt if not found
     */
    [[nodiscard]] static std::optional<StandardMacroSlot> slot_from_name(const std::string& name);

    /**
     * @brief Get slot name from enum
     * @param slot The slot enum
     * @return Slot name string
     */
    [[nodiscard]] static std::string slot_to_name(StandardMacroSlot slot);

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set user-configured macro for a slot
     *
     * Pass empty string to clear configuration and use auto-detection.
     * Automatically saves to config file.
     *
     * @param slot The slot to configure
     * @param macro Macro name, or empty to clear
     */
    void set_macro(StandardMacroSlot slot, const std::string& macro);

    /**
     * @brief Load slot configurations from config file
     */
    void load_from_config();

    /**
     * @brief Save current configurations to config file
     */
    void save_to_config();

    // ========================================================================
    // Execution
    // ========================================================================

    /**
     * @brief Execute the macro for a slot
     *
     * Resolves the macro using priority chain, then executes via API.
     * If slot is empty, returns false immediately without calling callbacks.
     *
     * @param slot The slot to execute
     * @param api IMoonrakerAPI instance for execution
     * @param on_success Called when macro execution starts
     * @param on_error Called on execution failure
     * @param suppress_auto_toast Maps to helix::rpc_error_policy::CallerIntent::silent
     *        — see the parameterized overload below for the full contract.
     * @return true if macro was found and execution attempted,
     *         false if slot is empty (no callbacks called)
     */
    bool execute(StandardMacroSlot slot, IMoonrakerAPI* api, SuccessCallback on_success,
                 ErrorCallback on_error, uint32_t timeout_ms = 0, bool suppress_auto_toast = false);

    /**
     * @brief Execute macro with parameters
     *
     * @param slot The slot to execute
     * @param api IMoonrakerAPI instance for execution
     * @param params Parameters to pass to macro
     * @param on_success Called when macro execution starts
     * @param on_error Called on execution failure
     * @param timeout_ms Timeout override (0 = default macro timeout)
     * @param suppress_auto_toast Maps to helix::rpc_error_policy::CallerIntent::silent.
     *        If true, the Request Tracker will NOT emit the generic "Printer
     *        command '...' failed" RPC_ERROR toast on failure; the caller's
     *        on_error callback is expected to surface the error to the user with
     *        action-specific context. The cross-source dedup that suppresses
     *        Klipper's `!!` broadcast for the same root cause follows from
     *        on_error being a real user-facing report, not from this flag (see
     *        rpc_error_policy.h and rpc_error_correlation.h).
     * @return true if macro was found and execution attempted
     */
    bool execute(StandardMacroSlot slot, IMoonrakerAPI* api,
                 const std::map<std::string, std::string>& params, SuccessCallback on_success,
                 ErrorCallback on_error, uint32_t timeout_ms = 0, bool suppress_auto_toast = false);

  private:
    StandardMacros();
    ~StandardMacros() = default;

    /**
     * @brief Initialize slot definitions (names, display names, fallbacks)
     */
    void init_slot_definitions();

    /**
     * @brief Run auto-detection for all slots
     * @param hardware Hardware discovery with macro list
     */
    void auto_detect(const helix::PrinterDiscovery& hardware);

    /**
     * @brief Try to detect a macro for a slot using patterns
     * @param hardware Hardware discovery
     * @param slot Slot to detect for
     * @param patterns Patterns to match (uppercase)
     * @return Detected macro name, or empty if none found
     */
    std::string try_detect(const helix::PrinterDiscovery& hardware, StandardMacroSlot slot,
                           const std::vector<std::string>& patterns);

    /**
     * @brief Demote configured macros the printer does not define
     *
     * @param hardware Hardware discovery with the printer's macro list
     */
    void validate_configured(const helix::PrinterDiscovery& hardware);

    /// @brief Bump macros_version_ so observers re-read slot availability.
    void bump_version();

    std::vector<StandardMacroInfo> slots_;
    bool initialized_ = false;

    SubjectManager subjects_;
    /// See get_subjects_lifetime(). Created with the object and REPLACED (never
    /// nulled) by deinit_subjects() — an empty token reads as "dead" and would
    /// suppress removal for observers registered after the teardown.
    SubjectLifetime subjects_lifetime_ = std::make_shared<bool>(true);
    bool subjects_initialized_ = false;
    lv_subject_t macros_version_{};
};
