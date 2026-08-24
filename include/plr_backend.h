// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <string>

namespace helix {

/// Which firmware-specific Power-Loss-Recovery mechanism the connected printer
/// exposes. Both backends are SELF-GATING: each is chosen by the presence of a
/// status field that only that Klipper fork ever emits, so no printer-model or
/// AMS-backend gate is needed (and none should be added — see
/// tests/unit/test_plr_offer.cpp for the AFC-modded-U1 regression).
///
/// Full mechanism writeup: docs/devel/POWER_LOSS_RECOVERY.md
enum class PlrBackendType {
    NONE = 0,  ///< No PLR support detected (mainline Klipper).
    SNAPMAKER, ///< Snapmaker U1 fork — virtual_sdcard.pl_env_valid (passive).
    CREALITY,  ///< Creality K/Ender/Hi fork — print_stats.power_loss (active probe).
};

/// Capability markers harvested from the Moonraker status payload.
struct PlrCapabilitySignals {
    /// virtual_sdcard.pl_env_valid arrived as a JSON boolean AND is true.
    /// Snapmaker's fork is the only firmware that emits this key, and the value
    /// itself is the "a validated snapshot exists" signal, so capability and
    /// availability coincide for this backend.
    bool snapmaker_pl_env_valid = false;
    /// print_stats.power_loss arrived as a JSON NUMBER. Presence — not value —
    /// is the marker: the key exists only in Creality's fork and normally reads
    /// 0. Mainline Klipper omits it, and Moonraker answers a subscribed-but-
    /// unpopulated field with an explicit null, so "present and numeric" is what
    /// distinguishes the fork from everything else.
    bool creality_power_loss_field = false;
};

/// Pick the backend. SNAPMAKER wins if both markers somehow appear (they never
/// should) because its passive signal needs no side-effectful probe.
PlrBackendType plr_select_backend(const PlrCapabilitySignals& caps);

/// Outcome of Creality's one-shot `pause_resume/check_continue_print_state`
/// probe. `completed` is the safety-critical field: it records that the probe
/// actually ran this connection AND returned a well-formed response. The probe
/// is what sets `print_stats.power_loss = 1` in firmware, which the stock
/// sensorless-homing macro reads to choose a full Z clearance lift before
/// homing. Resuming without it drags the nozzle through a tall part.
struct PlrDetectResult {
    bool completed = false;    ///< probe ran and parsed; NEVER set this by hand
    bool file_state = false;   ///< recovery sidecar present and coherent
    bool eeprom_state = false; ///< bl24c16f EEPROM snapshot present and coherent
};

/// A Creality recovery is offerable only when the probe completed AND BOTH
/// states are true. Do not weaken: without `eeprom_state` the Klipper-side
/// resume branch silently restarts the print FROM THE BEGINNING (the
/// `bl24c16f` object is absent, so `ISCONTINUEPRINT=1` degrades to a fresh
/// print rather than failing).
bool plr_creality_recovery_available(const PlrDetectResult& r);

/// Parse a `printer.pause_resume.check_continue_print_state` JSON-RPC response
/// into `out`. Sets `out.completed` only on a well-formed
/// `result.{file_state,eeprom_state}` pair of booleans. Returns false (leaving
/// `out.completed` false, i.e. resume-forbidden) on anything else.
bool plr_parse_check_continue_response(const nlohmann::json& response, PlrDetectResult& out);

/// The resolved resume/discard actions for one offer. Built once, at prompt
/// time, and carried by value into the modal so the button handlers never have
/// to re-derive the backend or re-check the safety invariant against live state
/// that may have moved on.
struct PlrRecoveryPlan {
    PlrBackendType backend = PlrBackendType::NONE;
    /// EMPTY means Resume must not be offered at all. For CREALITY that happens
    /// when the probe did not confirm both states, or when no recovery filename
    /// could be resolved (the gcode needs FILENAME=).
    std::string resume_gcode;
    std::string discard_gcode;      ///< gcode-based discard (Snapmaker)
    std::string discard_rpc_method; ///< JSON-RPC discard (Creality)
    std::string recovery_file;      ///< raw path/name, for the prompt body

    [[nodiscard]] bool resume_allowed() const {
        return !resume_gcode.empty();
    }
    [[nodiscard]] bool discard_available() const {
        return !discard_gcode.empty() || !discard_rpc_method.empty();
    }
};

/// Build the plan for `backend`. `detect` is consulted for CREALITY only, and
/// is the ONLY thing that can authorize a Creality resume — see PlrDetectResult.
/// An unsafe or empty `recovery_file` also suppresses the Creality resume,
/// since its gcode embeds the name as a parameter.
PlrRecoveryPlan plr_build_plan(PlrBackendType backend, const std::string& recovery_file,
                               const PlrDetectResult& detect);

/// Reject filenames that could break out of `SDCARD_PRINT_FILE FILENAME="<f>"`.
/// The value is emitted double-quoted, so spaces are fine; `"` and `\` (shlex's
/// quote and escape characters), comment/terminator characters and `=` are not.
bool plr_is_safe_recovery_filename(const std::string& name);

/// Convert the sidecar's ABSOLUTE `file_path` into the name
/// `SDCARD_PRINT_FILE` expects: relative to the virtual_sdcard root. Klipper
/// matches FILENAME against a file list built relative to that root (an
/// absolute path merely loses its leading `/` and then misses), and on the
/// resume path the reopened file's absolute name is compared back against the
/// sidecar's `file_path` — a mismatch discards the recovery data and restarts
/// the print. An already-relative name is returned unchanged.
std::string plr_creality_sdcard_relative_name(const std::string& path);

/// Extract `file_path` from the Creality recovery sidecar JSON. Returns an
/// empty string for malformed input or a missing/non-string key.
std::string plr_parse_creality_sidecar(const std::string& json_text);

/// Best-effort read of the Creality recovery filename from the on-device
/// sidecar. HelixScreen runs on the printer, so this normally succeeds; when it
/// does not the caller must degrade (no Creality resume is possible without a
/// filename). Never writes to the sidecar.
std::string plr_read_creality_recovery_filename();

// --- Wire constants -------------------------------------------------------

/// Snapmaker fork gcode.
inline constexpr const char* SNAPMAKER_RESUME_GCODE = "SDCARD_PRINT_PL_RESTORE";
inline constexpr const char* SNAPMAKER_DISCARD_GCODE = "SDCARD_PRINT_PL_CLEAR_ENV";

/// Creality fork. The detect endpoint is a Klipper webhook that Moonraker
/// auto-registers as a JSON-RPC method (klippy_connection.py re-exports every
/// non-reserved endpoint). It is SIDE-EFFECTFUL — call at most once per
/// connection, only while print_stats.state == "standby". Never poll it.
inline constexpr const char* CREALITY_DETECT_RPC =
    "printer.pause_resume.check_continue_print_state";
inline constexpr const char* CREALITY_DISCARD_RPC = "printer.pause_resume.cancel_continue_print";

/// Sidecar holding the interrupted job's path, relative to the data root.
inline constexpr const char* CREALITY_SIDECAR_REL_PATH =
    "creality/userdata/config/print_file_name.json";

} // namespace helix
