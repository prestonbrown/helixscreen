// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace helix {

enum class ErrorSeverity { INFO, WARNING, CRITICAL };

enum class ErrorSource {
    GENERIC,
    KLIPPER,
    HEATER,
    AFC,
    CFS,
    IFS,
    QIDI,
    HAPPY_HARE,
    SNAPMAKER,
    ACE,
    TOOLCHANGER
};

/// A one-tap fix offered alongside an error, rendered as a button by
/// RecoveryModalPresenter. Two kinds of producer populate these: the generic
/// error_classify::classify() for the cases it recognizes by Klipper error code
/// (key840 "Reset CFS", key298, plus the paused-print Resume/OK fallbacks), and
/// the AMS backends' own build_recovery_actions(), which derive a context-aware
/// set from live hardware state (AFC, Happy Hare, AD5X IFS; QIDI hardcodes a
/// lone dismiss). An empty list is meaningful, not merely unpopulated — it is
/// what makes decide_presentation() choose MODAL over MODAL_WITH_RECOVER.
struct RecoveryAction {
    std::string label;   ///< Button label, e.g. "Unload"
    std::string gcode;   ///< G-code to run on tap
    std::string log_tag; ///< spdlog tag on tap
    std::string style;   ///< "" (neutral) | "primary" | "danger" — maps to PromptButton.color
    /// True when running @ref gcode pushes filament through the nozzle, so it
    /// needs the hotend at or above Klipper's min_extrude_temp. The presenter
    /// preheats and defers the send instead of firing into a cold nozzle: the
    /// heater can be off by the time the user taps (the post-op cooldown timer
    /// armed by an earlier operation, TURN_OFF_HEATERS on a print ERROR, or
    /// Klipper's idle_timeout), and a cold recovery fails exactly like the
    /// operation that raised the error. False for anything that moves filament
    /// only between the lane and the spool, or that just clears state.
    bool needs_hot_nozzle = false;
};

/// Result of classifying one gcode-response line. Produced by the pure
/// `error_classify::classify()`; consumed by the router's presenter.
struct ErrorEvent {
    ErrorSource source = ErrorSource::GENERIC;
    ErrorSeverity severity = ErrorSeverity::WARNING;
    std::string title;  ///< short, already-translated; "" -> presenter default
    std::string detail; ///< FULL, untruncated, translated message text
    /// FULL, untruncated, UN-translated text exactly as Klipper sent it (the
    /// `!!`/`Error:` prefix stripped, nothing else). This is the cross-channel
    /// dedup identity: the RPC channel records Klipper's raw wording, while
    /// `detail` may have been rewritten by clean_error_text() ("Must home axis
    /// first" -> "Must home axes first"). Comparing raw-to-raw keeps both sides
    /// in the same normalization. Empty when a classifier doesn't populate it,
    /// in which case `detail` is already the raw text.
    std::string raw_detail;
    std::string code; ///< Klipper error code if any ("key840"), else ""
    std::vector<RecoveryAction> recovery_actions;
    bool sticky = false;
};

/// Inputs the classifier needs beyond the raw line. Kept as a plain struct
/// so the classifier stays pure and unit-testable without globals.
struct ClassifyContext {
    bool is_paused = false;   ///< print currently paused (pause_resume.is_paused)
    bool is_printing = false; ///< print active (print_stats.state == "printing")
};

} // namespace helix
