// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/// Where the idle state machine currently sits. Sleeping outranks Dimmed.
enum class IdleState { Awake, Dimmed, Sleeping };

/// What the tick should do about it.
enum class IdleAction {
    None,
    Wake,       ///< Dimmed or sleeping, and the user is back (or the host resumed).
    Dim,        ///< Awake -> Dimmed by lowering the backlight only.
    StartSaver, ///< Awake -> Dimmed by starting the screensaver.
    Sleep,      ///< Awake or Dimmed -> Sleeping.
};

struct IdleInputs {
    IdleState state = IdleState::Awake;
    uint32_t inactive_ms = 0;
    int dim_timeout_sec = 0;   ///< 0 = never dim
    int sleep_timeout_sec = 0; ///< 0 = never sleep
    /// Sleep-while-printing is off and a job holds the machine: entering sleep or dim is
    /// refused, but waking is not.
    bool inhibit_entry = false;
    bool can_dim = false;
    bool has_screensaver = false;
    bool saver_running = false;
    bool wake_requested = false; ///< The input wrapper saw a press while asleep or dimmed.
    bool host_resumed = false;   ///< The host powered the panel back on (Android).
    bool is_preview = false;     ///< The running screensaver is a settings preview.
    uint32_t preview_elapsed_ms = 0;
    /// Someone is watching the screen through the remote screen server. Idle may dim and
    /// start the screensaver, never sleep: a sleeping panel serves a black or frozen image.
    bool remote_viewer = false;
    /// remote_viewer became true this tick. Wakes a sleeping display, as a touch does.
    bool remote_viewer_arrived = false;
};

struct IdleDecision {
    IdleAction action = IdleAction::None;
    /// The sleep timeout has passed and only remote_viewer kept the display out of sleep.
    bool sleep_held = false;
};

/// Activity means input within this many milliseconds.
constexpr uint32_t kIdleActivityWindowMs = 500;
/// A preview ignores activity this long, so the click that launched it cannot close it.
constexpr uint32_t kPreviewGraceMs = 750;

inline IdleDecision decide_idle(const IdleInputs& in) {
    const uint32_t dim_timeout_ms =
        (in.dim_timeout_sec > 0) ? static_cast<uint32_t>(in.dim_timeout_sec) * 1000U : UINT32_MAX;
    const uint32_t sleep_timeout_ms = (in.sleep_timeout_sec > 0)
                                          ? static_cast<uint32_t>(in.sleep_timeout_sec) * 1000U
                                          : UINT32_MAX;
    const bool activity = in.inactive_ms < kIdleActivityWindowMs;

    if (in.state == IdleState::Sleeping) {
        if (in.wake_requested || activity || in.host_resumed || in.remote_viewer_arrived) {
            return {IdleAction::Wake};
        }
        return {IdleAction::None};
    }

    const bool sleep_due = in.sleep_timeout_sec > 0 && in.inactive_ms >= sleep_timeout_ms;
    const bool sleep_held = !in.inhibit_entry && sleep_due && in.remote_viewer;

    if (in.state == IdleState::Dimmed) {
        bool dismiss_on_activity = activity;
        if (in.is_preview && in.preview_elapsed_ms < kPreviewGraceMs) {
            dismiss_on_activity = false;
        }
        if (in.wake_requested || dismiss_on_activity) {
            return {IdleAction::Wake};
        }
        if (!in.inhibit_entry && sleep_due && !in.remote_viewer) {
            return {IdleAction::Sleep};
        }
        return {IdleAction::None, sleep_held};
    }

    if (in.inhibit_entry) {
        return {IdleAction::None};
    }
    if (sleep_due && !in.remote_viewer) {
        return {IdleAction::Sleep};
    }
    // A sleep the viewer holds back dims instead, whatever the dim timeout says.
    const bool dim_due = (in.dim_timeout_sec > 0 && in.inactive_ms >= dim_timeout_ms) || sleep_held;
    if (dim_due && (in.can_dim || in.has_screensaver)) {
        return {(!in.saver_running && in.has_screensaver) ? IdleAction::StartSaver
                                                          : IdleAction::Dim,
                sleep_held};
    }
    return {IdleAction::None, sleep_held};
}

/// Whether a wake shows the lock screen. A screensaver preview is user-initiated from
/// settings, so dismissing one never locks.
inline bool wake_should_auto_lock(bool was_sleeping, bool was_dimmed, bool was_preview,
                                  bool auto_lock_enabled, bool has_pin) {
    return (was_sleeping || was_dimmed) && !was_preview && auto_lock_enabled && has_pin;
}

/**
 * @brief Decide whether real panel power-off (FB_BLANK_POWERDOWN / DRM DPMS)
 *        is the sleep mechanism (#1049). Pure, no side effects.
 *
 * Power-off is a last resort, only for panels with no controllable backlight
 * (generic HDMI, Backlight-None, CB1), where it is the only way to actually
 * cut the panel. A device with a hardware blank or a usable backlight turns
 * the backlight off instead. Powering down a panel whose driver does not
 * expect it can wedge the display engine or leave the panel lit showing a
 * no-signal pattern:
 *   - Snapmaker U1: DPMS-off disables the Rockchip VOP2 CRTC and DPMS-on does
 *     not reliably re-enable it, so the panel stays black until reboot.
 *   - AD5X: unblanking leaves the display engine cycling solid fill colours.
 *   - Creality K1 / K2: the panel edges glow and flicker white (#1708).
 *   - Raspberry Pi DSI panels: the DSI stream stops and the panel cycles
 *     through its colour test pattern.
 *
 * A panel whose backlight write leaves the LEDs lit opts in through the
 * /display/panel_power_off config override (#1594), which callers apply
 * instead of this decision.
 *
 * @param use_hardware_blank         Whether a hardware backlight blank is used
 * @param has_usable_backlight       Whether a controllable backlight is available
 * @param backend_supports_power_off Whether the display backend can power off
 * @return true only when there is neither a hardware blank nor a usable
 *         backlight AND the backend can power off
 */
inline bool should_use_power_off(bool use_hardware_blank, bool has_usable_backlight,
                                 bool backend_supports_power_off) {
    return !use_hardware_blank && !has_usable_backlight && backend_supports_power_off;
}

/**
 * @brief How enter_sleep() actually cuts the panel on this device.
 *
 * Selected by select_sleep_mechanism(); recorded in m_last_sleep_mechanism so
 * the wake path and the logs agree on what was done.
 */
enum class SleepMechanism {
    HardwareBlank,   ///< FBIOBLANK at the display controller (AD5M/Allwinner)
    PanelPowerOff,   ///< FB_BLANK_POWERDOWN / DRM DPMS-off (#1049)
    HostSleep,       ///< Let the OS power the panel off (Android, #1245)
    SoftwareOverlay, ///< Black LVGL rect over a lit panel — universal fallback
};

/** @brief Human-readable name for a mechanism (logging + test failure output). */
inline const char* sleep_mechanism_name(SleepMechanism m) {
    switch (m) {
    case SleepMechanism::HardwareBlank:
        return "hardware blank";
    case SleepMechanism::PanelPowerOff:
        return "panel power-off";
    case SleepMechanism::HostSleep:
        return "host sleep (keep-screen-on cleared)";
    case SleepMechanism::SoftwareOverlay:
        break;
    }
    return "software overlay";
}

/** @brief True when this binary was built for Android. Compile-time constant. */
constexpr bool platform_is_android() {
#ifdef __ANDROID__
    return true;
#else
    return false;
#endif
}

/**
 * @brief Decide how idle entry should cut the panel (#1245). Pure, no side effects.
 *
 * Ordering is "most direct control first": if we can blank or power the panel
 * ourselves we always do, because that honors the user's timeout exactly.
 * Host sleep is the Android last resort — no backlight sysfs is reachable from
 * an untrusted app and DisplayBackendSDL has no blank/power-off, so the only
 * real way to darken the panel is to stop asserting FLAG_KEEP_SCREEN_ON and
 * let Android's own display timeout run. Everything else keeps the software
 * overlay.
 *
 * @p sleep_timeout_sec is the configured Display Sleep value; 0 (and any
 * non-positive value) means "Never", and must never select HostSleep — a
 * wall-mounted tablet has to stay lit even though the panel is idle.
 *
 * With @p on_android false the choice is blank, then power-off, then overlay.
 *
 * @param on_android           Built for Android (see platform_is_android())
 * @param use_hardware_blank   A hardware backlight blank is in use
 * @param can_power_off        Panel power-off is enabled AND a backend exists
 * @param sleep_timeout_sec    Configured Display Sleep timeout (0 = Never)
 */
inline SleepMechanism select_sleep_mechanism(bool on_android, bool use_hardware_blank,
                                             bool can_power_off, int sleep_timeout_sec) {
    if (use_hardware_blank) {
        return SleepMechanism::HardwareBlank;
    }
    if (can_power_off) {
        return SleepMechanism::PanelPowerOff;
    }
    if (on_android && sleep_timeout_sec > 0) {
        return SleepMechanism::HostSleep;
    }
    return SleepMechanism::SoftwareOverlay;
}

/**
 * @brief Whether a host-sleeping display must self-wake (#1245). Pure.
 *
 * Android pauses the app when it powers the panel down and resumes it when the
 * panel comes back — and neither transition is a touch, so the normal
 * activity-based wake never fires. Left alone, m_display_sleeping would stay
 * true with keep-screen-on still cleared: the device would immediately re-sleep
 * and the sleep callbacks (camera suspend) would never resume.
 * HelixActivity.onResume() bumps a counter; a change in it while host-sleeping
 * means the panel is on again.
 *
 * Only meaningful while HostSleep is the active mechanism — a resume must not
 * spuriously wake a hardware-blank / power-off / overlay device.
 *
 * @param sleeping_via_host   Currently asleep via SleepMechanism::HostSleep
 * @param resume_seq_at_sleep Resume counter captured when sleep was entered
 * @param resume_seq_now      Resume counter right now
 */
inline bool host_sleep_needs_wake(bool sleeping_via_host, int resume_seq_at_sleep,
                                  int resume_seq_now) {
    return sleeping_via_host && resume_seq_now != resume_seq_at_sleep;
}

/// What losing the touch pointer asks of the idle state.
struct PointerLossResponse {
    /// Wake the display, which powers the panel back on.
    bool wake = false;
    /// Sleep without panel power-off for the rest of this session.
    bool stop_power_off = false;
};

/**
 * @brief React to LVGL deleting the touch pointer (prestonbrown/helixscreen#2069). Pure.
 *
 * A panel whose touch controller is powered from the panel board drops off the
 * bus when the panel is powered down, and comes back only when it is powered
 * on. Touch is then the one wake path that cannot work, so a pointer lost while
 * asleep under panel power-off wakes the display to let the device return.
 * Under automatic power-off the panel then sleeps another way for the rest of
 * the session, or every sleep would end the same way. A power-off the user
 * chose through /display/panel_power_off stays.
 *
 * @param sleeping           The display is asleep
 * @param mechanism          How the current (or last) sleep cut the panel
 * @param power_off_explicit /display/panel_power_off is set, not automatic
 */
inline PointerLossResponse decide_pointer_loss(bool sleeping, SleepMechanism mechanism,
                                               bool power_off_explicit) {
    if (!sleeping || mechanism != SleepMechanism::PanelPowerOff) {
        return {};
    }
    return {true, !power_off_explicit};
}

} // namespace helix
