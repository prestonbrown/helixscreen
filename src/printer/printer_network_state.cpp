// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_network_state.cpp
 * @brief Network and connection state management extracted from PrinterState
 *
 * Manages WebSocket connection state, network connectivity, and Klipper firmware
 * state. Maintains a derived nav_buttons_enabled subject for UI gating.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_network_state.h"

#include "connection_state.h" // For ConnectionState enum
#include "json_utils.h"
#include "printer_state.h" // For KlippyState enum
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix {

std::optional<KlippyState> parse_klippy_state(std::string_view state) {
    if (state == "ready")
        return KlippyState::READY;
    if (state == "startup")
        return KlippyState::STARTUP;
    if (state == "shutdown")
        return KlippyState::SHUTDOWN;
    if (state == "error")
        return KlippyState::ERROR;
    return std::nullopt;
}

const char* klippy_state_name(KlippyState state) {
    switch (state) {
    case KlippyState::READY:
        return "ready";
    case KlippyState::STARTUP:
        return "startup";
    case KlippyState::SHUTDOWN:
        return "shutdown";
    case KlippyState::ERROR:
        return "error";
    }
    return "error";
}

bool is_klippy_config_error(std::string_view state_message) {
    // Klipper's configfile/pins errors, verbatim. Each one names the option,
    // section, pin or file at fault, so the message itself says where to look.
    static constexpr std::string_view kConfigErrorPhrases[] = {
        "in section '", // Option 'x' in section 'y' must .../is not valid ...
        "is not a valid config section",
        "Unable to open config file",
        "Unable to parse option",
        "Error loading template", // gcode_macro templates are config too
        "Unknown pin chip name",
        "used multiple times in config",
    };
    for (std::string_view phrase : kConfigErrorPhrases) {
        if (state_message.find(phrase) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

PrinterNetworkState::PrinterNetworkState() {
    // Initialize string buffer
    std::memset(printer_connection_message_buf_, 0, sizeof(printer_connection_message_buf_));
    std::strcpy(printer_connection_message_buf_, "Disconnected");
}

void PrinterNetworkState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterNetworkState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterNetworkState] Initializing subjects (register_xml={})", register_xml);

    // Printer connection state subjects (Moonraker WebSocket)
    INIT_SUBJECT_INT(printer_connection_state, 0, subjects_, register_xml); // 0 = disconnected
    INIT_SUBJECT_STRING(printer_connection_message, "Disconnected", subjects_, register_xml);

    // Network connectivity subject (WiFi/Ethernet)
    // Default to connected for mock mode
    INIT_SUBJECT_INT(network_status, 2, subjects_,
                     register_xml); // 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED

    // Klipper firmware state subject (default to SHUTDOWN until confirmed ready)
    INIT_SUBJECT_INT(klippy_state, static_cast<int>(KlippyState::SHUTDOWN), subjects_,
                     register_xml);
    // A fresh subject has heard from no printer, so no reason describes it yet.
    klippy_state_reported_ = false;
    klippy_state_message_.clear();
    klippy_state_message_describes_.reset();

    // Combined nav button enabled subject (connected AND klippy ready)
    // Starts disabled (0) - will be updated when connection/klippy state changes
    INIT_SUBJECT_INT(nav_buttons_enabled, 0, subjects_, register_xml);

    // Moonraker WebSocket connection state (independent of Klipper)
    // Used by power widgets that only need Moonraker, not Klipper
    INIT_SUBJECT_INT(moonraker_connection_state, 0, subjects_, register_xml);

    // Remote-screen verdict: 1 when the connected Moonraker endpoint does not
    // resolve to this host. Set on CONNECTED edges by MoonrakerManager; the
    // UI-facing mirror of helix::is_moonraker_on_same_host().
    INIT_SUBJECT_INT(moonraker_is_remote, 0, subjects_, register_xml);

    // Locality verdict for the current connection (MoonrakerLocality). Unlike
    // moonraker_is_remote it distinguishes "not settled yet" from "local".
    INIT_SUBJECT_INT(moonraker_locality, static_cast<int>(MoonrakerLocality::Unknown), subjects_,
                     register_xml);

    INIT_SUBJECT_INT(klippy_state_message_seq, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterNetworkState] Subjects initialized successfully");
}

void PrinterNetworkState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterNetworkState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterNetworkState::set_printer_connection_state_internal(int state, const char* message) {
    // Called from main thread via ui_queue_update()
    // Log "Connected" at info level, transitional states at debug to reduce noise
    if (state == static_cast<int>(ConnectionState::CONNECTED)) {
        spdlog::info("[PrinterNetworkState] Printer connection state: Connected");
    } else {
        spdlog::debug("[PrinterNetworkState] Printer connection state changed: {} - {}", state,
                      message);
    }

    // Track if we've ever successfully connected
    if (state == static_cast<int>(ConnectionState::CONNECTED) && !was_ever_connected_) {
        was_ever_connected_ = true;
        spdlog::debug(
            "[PrinterNetworkState] First successful connection - was_ever_connected_ = true");
    }

    spdlog::trace(
        "[PrinterNetworkState] Setting printer_connection_state_ subject (at {}) to value {}",
        (void*)&printer_connection_state_, state);
    lv_subject_set_int(&printer_connection_state_, state);
    spdlog::trace("[PrinterNetworkState] Subject value now: {}",
                  lv_subject_get_int(&printer_connection_state_));
    lv_subject_copy_string(&printer_connection_message_, message);
    update_nav_buttons_enabled();
    spdlog::trace("[PrinterNetworkState] Printer connection state update complete, observers "
                  "should be notified");
}

void PrinterNetworkState::set_moonraker_is_remote_internal(bool remote) {
    const int value = remote ? 1 : 0;
    if (lv_subject_get_int(&moonraker_is_remote_) == value)
        return;
    spdlog::info("[PrinterNetworkState] moonraker_is_remote: {} ({} Moonraker)", value,
                 remote ? "remote" : "same-host");
    lv_subject_set_int(&moonraker_is_remote_, value);
}

void PrinterNetworkState::set_moonraker_locality_internal(MoonrakerLocality locality) {
    if (!subjects_initialized_) {
        return;
    }
    const int value = static_cast<int>(locality);
    if (lv_subject_get_int(&moonraker_locality_) == value)
        return;
    spdlog::debug("[PrinterNetworkState] moonraker_locality: {}", value);
    lv_subject_set_int(&moonraker_locality_, value);
}

void PrinterNetworkState::set_network_status(int status) {
    if (lv_subject_get_int(&network_status_) == status)
        return;
    spdlog::debug("[PrinterNetworkState] Network status changed: {}", status);
    lv_subject_set_int(&network_status_, status);
}

bool PrinterNetworkState::apply_webhooks(const nlohmann::json& webhooks, double eventtime,
                                         bool from_cached_snapshot,
                                         std::optional<uint64_t> frame_epoch) {
    // Klippy state is a liveness signal written by two queues that are not ordered
    // against each other: live WebSocket frames, and the discovery subscription
    // snapshot replayed at the end of discovery. Last-write-wins would let the replay
    // resurrect READY over a live SHUTDOWN: nav re-enabled, the recovery dialog
    // auto-dismissed, and the gcode guards re-opened against a dead printer.
    //
    // The state_message is gated with the state because they arrive in the same
    // blob: a snapshot too stale to set the state carries an equally stale reason.
    //
    // Provenance is STATED by the caller, never inferred from a zero eventtime:
    // the mock client drives its simulated shutdown/recovery through the same
    // untimestamped dispatch, and those are the current truth for their session.
    // The eventtime watermark covers the other case — two genuinely live frames
    // arriving out of order across the queues.
    bool changed = false;
    double watermark;
    {
        std::lock_guard<std::mutex> lock(klippy_freshness_mutex_);
        watermark = klippy_state_eventtime_;
    }
    const bool stale = (from_cached_snapshot && klippy_state_from_live_.load()) ||
                       (eventtime > 0.0 && eventtime < watermark);

    if (stale) {
        spdlog::debug("[PrinterNetworkState] Ignoring stale klippy webhooks (state='{}', "
                      "eventtime={} vs watermark={}, cached_snapshot={})",
                      helix::json_util::safe_string(webhooks, "state", "<absent>"), eventtime,
                      watermark, from_cached_snapshot);
    } else {
        bool applied_state = false;
        auto message_describes = static_cast<KlippyState>(lv_subject_get_int(&klippy_state_));

        if (webhooks.contains("state") && webhooks["state"].is_string()) {
            std::string klippy_state_str = webhooks["state"].get<std::string>();
            if (const std::optional<KlippyState> parsed = parse_klippy_state(klippy_state_str)) {
                changed = set_klippy_state_internal(*parsed);
                applied_state = true;
                message_describes = *parsed;
            } else if (last_unknown_klippy_state_ != klippy_state_str) {
                // Leave the current state alone: a stale-but-known state is
                // safer than an invented one. Deduped on the string so a value
                // Klipper repeats every frame warns once, not per frame.
                last_unknown_klippy_state_ = klippy_state_str;
                spdlog::warn("[PrinterNetworkState] Unrecognised webhooks.state '{}' — leaving "
                             "klippy state unchanged",
                             klippy_state_str);
            }
        }

        // Capture state_message (error/shutdown reason text)
        if (webhooks.contains("state_message") && webhooks["state_message"].is_string()) {
            set_klippy_state_message(webhooks["state_message"].get<std::string>(),
                                     message_describes);
        }

        // Only a frame that actually carried a usable state moves the guard.
        // A delta carrying just state_message must not latch "live seen" and
        // lock out the snapshot that still has to seed the state.
        if (applied_state) {
            // A frame received before the last reset belongs to the previous
            // session, whose clock the watermark no longer measures, so it does
            // not move the guard. Checked under the lock the reset takes.
            std::lock_guard<std::mutex> lock(klippy_freshness_mutex_);
            if (!frame_epoch || *frame_epoch == klippy_epoch_.load()) {
                if (eventtime > 0.0) {
                    klippy_state_eventtime_ = eventtime;
                }
                if (!from_cached_snapshot) {
                    klippy_state_from_live_ = true;
                }
            }
        }
    }
    return changed;
}

void PrinterNetworkState::reset_klippy_state_freshness() {
    // Synchronous on the caller's thread: the next session's frames arrive
    // through a different queue than deferred UI work, so a queued reset could
    // land after them and wipe their watermark.
    std::lock_guard<std::mutex> lock(klippy_freshness_mutex_);
    klippy_state_eventtime_ = 0.0;
    klippy_state_from_live_.store(false);
    ++klippy_epoch_;
}

bool PrinterNetworkState::set_klippy_state_internal(KlippyState state) {
    const char* state_names[] = {"READY", "STARTUP", "SHUTDOWN", "ERROR"};
    int state_int = static_cast<int>(state);
    const bool first_report = !klippy_state_reported_;
    klippy_state_reported_ = true;
    if (lv_subject_get_int(&klippy_state_) == state_int) {
        // The subject's placeholder is SHUTDOWN, so a printer that really is shut
        // down when first heard from sets no new value. Observers that ignore the
        // placeholder still have to learn it has become the reported state.
        if (first_report) {
            lv_subject_notify(&klippy_state_);
        }
        return false;
    }
    spdlog::debug("[PrinterNetworkState] Klippy state changed: {} ({})", state_names[state_int],
                  state_int);
    lv_subject_set_int(&klippy_state_, state_int);
    update_nav_buttons_enabled();

    // Clear state message when returning to READY (error is resolved)
    if (state == KlippyState::READY) {
        set_klippy_state_message("", state);
    }
    return true;
}

std::string PrinterNetworkState::get_klippy_fault_message() const {
    if (klippy_state_message_describes_ == KlippyState::READY ||
        klippy_state_message_describes_ == KlippyState::STARTUP) {
        return {};
    }
    return klippy_state_message_;
}

void PrinterNetworkState::set_klippy_state_message(const std::string& message,
                                                   std::optional<KlippyState> describes) {
    const bool changed =
        message != klippy_state_message_ || describes != klippy_state_message_describes_;
    klippy_state_message_ = message;
    klippy_state_message_describes_ = describes;
    if (changed && subjects_initialized_) {
        lv_subject_set_int(&klippy_state_message_seq_,
                           lv_subject_get_int(&klippy_state_message_seq_) + 1);
    }
    if (!message.empty()) {
        spdlog::info("[PrinterNetworkState] Klippy state message: {}", message);
    }
}

void PrinterNetworkState::update_nav_buttons_enabled() {
    // Compute combined state: enabled when connected AND klippy ready
    int connection = lv_subject_get_int(&printer_connection_state_);
    int klippy = lv_subject_get_int(&klippy_state_);
    bool connected = (connection == static_cast<int>(ConnectionState::CONNECTED));
    bool klippy_ready = (klippy == static_cast<int>(KlippyState::READY));
    int enabled = (connected && klippy_ready) ? 1 : 0;

    // Only update if changed to avoid unnecessary observer notifications
    if (lv_subject_get_int(&nav_buttons_enabled_) != enabled) {
        spdlog::debug(
            "[PrinterNetworkState] nav_buttons_enabled: {} (connected={}, klippy_ready={})",
            enabled, connected, klippy_ready);
        lv_subject_set_int(&nav_buttons_enabled_, enabled);
    }

    // Moonraker connection state: WebSocket connected, independent of Klipper
    int moonraker_connected = connected ? 1 : 0;
    if (lv_subject_get_int(&moonraker_connection_state_) != moonraker_connected) {
        spdlog::debug("[PrinterNetworkState] moonraker_connection_state: {}", moonraker_connected);
        lv_subject_set_int(&moonraker_connection_state_, moonraker_connected);
    }
}

} // namespace helix
