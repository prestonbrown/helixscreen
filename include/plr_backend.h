// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_plugin_features.h"
#include "json_fwd.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {

class PrinterDiscovery;

/// Which Power-Loss-Recovery mechanism the connected printer exposes. The three
/// firmware backends are SELF-GATING: each is chosen by markers only that
/// firmware ever carries, so no printer-model or AMS-backend gate is needed
/// (and none should be added: tests/unit/test_plr_offer.cpp pins the
/// AFC-modded U1 such a gate gets wrong). GENERIC is the HelixPrint plugin's recovery,
/// gated on the plugin's `plr` feature.
///
/// Full mechanism writeup: docs/devel/POWER_LOSS_RECOVERY.md
enum class PlrBackendType {
    NONE = 0,  ///< No PLR support detected (mainline Klipper).
    SNAPMAKER, ///< Snapmaker U1 fork — virtual_sdcard.pl_env_valid (passive).
    CREALITY,  ///< Creality K/Ender/Hi fork — print_stats.power_loss (active probe).
    QIDI,      ///< Qidi Q2/Q1 Pro/Plus 4 stock macros, save_variables.was_interrupted (passive).
    GENERIC,   ///< HelixPrint plugin `plr` feature; its status `backend` says which flavor.
};

/// The recovery backend the HelixPrint plugin reports (server.helix.plr.status
/// `backend`). Both plugin flavors select PlrBackendType::GENERIC; this says
/// which one, and where it sits in the precedence of plr_select_backend().
enum class PlrPluginBackend {
    None,           ///< plugin has no recovery for this printer ("none")
    Generic,        ///< the plugin's own snapshot and resume sequence ("generic")
    QidiPrintState, ///< the plugin feeds the firmware's print_state rows ("qidi_print_state")
};

/// Capability markers harvested from the Moonraker status payload and the
/// discovery snapshot. Each field names a recovery MECHANISM; which firmware
/// publishes it, and under which key, is plr_backend's knowledge alone.
struct PlrCapabilitySignals {
    /// The firmware reports a snapshot it validated itself (plr_parse_snapshot_valid
    /// read 1). Capability and availability coincide for this mechanism.
    bool snapshot_valid = false;
    /// The firmware carries the active recovery probe (plr_parse_power_loss_signal).
    /// Presence, not value, is the marker; availability takes the probe itself.
    bool probe_capable = false;
    /// Discovery found a resume macro the PLR module knows
    /// (plr_resume_macro_present()). Without it the interrupted flag is just a
    /// user-writable name any Klipper could carry.
    bool resume_macro = false;
    /// The firmware's interrupted flag reads true (plr_parse_interrupted_flag).
    /// It is ALSO true during every normal print on the firmware that keeps
    /// it; the printer_idle input of the offer decision is what scopes it to a
    /// boot after power loss.
    bool interrupted_flag = false;
    /// The firmware's saved filename decides whether a job is on record
    /// (plr_saved_file_gates_offer()). When false, saved_recovery_file is ignored.
    bool saved_file_gates_offer = false;
    /// The saved filename (plr_parse_saved_file): nullopt when the firmware
    /// keeps none, empty when it was cleared.
    std::optional<std::string> saved_recovery_file;
    /// The recovery file the status payload names (plr_parse_recovery_file),
    /// empty when none arrived.
    std::string status_recovery_file;
    /// The plugin's snapshot filename, empty when it reported none.
    std::string plugin_recovery_file;
    /// server.helix.plr.status `backend`; None until the plugin has answered.
    PlrPluginBackend plugin_backend = PlrPluginBackend::None;
    /// server.helix.plr.status `offer.available`: the plugin's own verdict that
    /// a resumable snapshot exists for the newest, interrupted job.
    bool plugin_offer_available = false;
    /// plugin_feature("plr", kPlrFeatureMinVersion). GENERIC is never selected
    /// unless this is Ready.
    PluginFeatureState plr_feature = PluginFeatureState::Absent;
};

/// Pick the backend: the first provider-table row whose markers are present.
/// Passive markers outrank probe-based ones: SNAPMAKER first, QIDI before
/// CREALITY. No firmware carries more than one marker set,
/// so the ordering only makes a tie deterministic, and a passive signal
/// chosen by mistake costs no side-effectful probe.
///
/// The plugin's two flavors slot in around them, both only while the `plr`
/// feature is Ready: its QIDI print_state adapter right after SNAPMAKER, so it
/// outranks the legacy QIDI macros on the firmware layout it serves, and its
/// generic recovery last of all, so it never displaces a firmware's own.
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

/// A probed recovery is offerable only when the probe completed AND BOTH
/// states are true. Do not weaken: without `eeprom_state` the Klipper-side
/// resume branch silently restarts the print FROM THE BEGINNING (the
/// `bl24c16f` object is absent, so `ISCONTINUEPRINT=1` degrades to a fresh
/// print rather than failing).
bool plr_probe_recovery_available(const PlrDetectResult& r);

/// The active probe's state for one connection. The caller owns it and resets
/// it on every disconnect: `result.completed` must never survive into a
/// session whose probe has not run.
struct PlrProbeSession {
    bool fired = false;        ///< the one probe of this connection was sent
    PlrDetectResult result{};  ///< set only from the probe's parsed reply
    std::string recovery_file; ///< the file the probed recovery would resume
};

/// What the current signals say about an offer, before the offer's own
/// self-guards (plr_should_offer) and the plan's safety gate (plr_build_plan).
struct PlrOfferAvailability {
    PlrBackendType backend = PlrBackendType::NONE;
    /// A resumable snapshot exists by this backend's own rule.
    bool available = false;
    /// The file the recovery would resume; may be empty (display-only for the
    /// backends whose resume takes no filename).
    std::string recovery_file;
};

/// Select the backend and apply its availability and recovery-file rules.
/// Pure.
PlrOfferAvailability plr_offer_availability(const PlrCapabilitySignals& caps,
                                            const PlrProbeSession& probe);

/// Short backend name for logs.
const char* plr_backend_name(PlrBackendType backend);

/// Whether a job dropping straight to Idle should re-evaluate the offer for
/// `backend`. Only a backend whose availability a normal print end cannot fake
/// does: a flag that lags the standby frame of a print that finished would
/// otherwise offer to resume it.
bool plr_reoffers_after_job_drop(PlrBackendType backend);

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
    std::string discard_rpc_method; ///< JSON-RPC discard (Creality, plugin)
    std::string recovery_file;      ///< raw path/name, for the prompt body
    /// The resumed layer may land off the interrupted one (the restore re-homes
    /// and does not re-probe Z), so the prompt must not promise an exact resume.
    bool resume_layer_approximate = false;
    /// Plugin-driven resume: the RPC that opens the guided resume (the plan
    /// request). Set for GENERIC instead of resume_gcode: that resume moves the
    /// machine only through the plugin's prepare/commit steps, after the user's
    /// confirmations.
    std::string resume_rpc;

    [[nodiscard]] bool resume_allowed() const {
        return !resume_gcode.empty() || !resume_rpc.empty();
    }
    [[nodiscard]] bool discard_available() const {
        return !discard_gcode.empty() || !discard_rpc_method.empty();
    }
};

/// Build the plan for `backend`. `detect` is consulted for the probed backend
/// only, and is the ONLY thing that can authorize its resume — see
/// PlrDetectResult.
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

/// Best-effort read of the probed recovery's filename from the firmware's
/// on-device sidecar. HelixScreen runs on the printer, so this normally
/// succeeds; when it does not the caller must degrade (no probed resume is
/// possible without a filename). Never writes to the sidecar.
std::string plr_read_probe_recovery_filename();

// --- Wire constants -------------------------------------------------------

/// HelixPrint plugin `plr` feature: the feature name in its status `features`
/// map, the contract version this app speaks, and its endpoints. Moonraker
/// delivers a component event `helix:<name>` to clients as `notify_<name>`.
inline constexpr const char* kPlrFeatureName = "plr";
/// Version 2 added server.helix.plr.mesh_include, the printer.cfg include
/// behind the mesh carry-over.
inline constexpr int kPlrFeatureMinVersion = 2;
inline constexpr const char* kPlrStatusRpc = "server.helix.plr.status";
inline constexpr const char* kPlrPlanRpc = "server.helix.plr.plan";
inline constexpr const char* kPlrPrepareRpc = "server.helix.plr.prepare";
inline constexpr const char* kPlrCommitRpc = "server.helix.plr.commit";
inline constexpr const char* kPlrAbortRpc = "server.helix.plr.abort";
inline constexpr const char* kPlrDiscardRpc = "server.helix.plr.discard";
inline constexpr const char* kPlrMeshIncludeRpc = "server.helix.plr.mesh_include";
inline constexpr const char* kPlrProgressNotify = "notify_plr_progress";
inline constexpr const char* kPlrPlanReadyNotify = "notify_plr_plan_ready";
/// helix_print:plr_status; its payload is the server.helix.plr.status reply.
inline constexpr const char* kPlrStatusNotify = "notify_plr_status";

/// Snapmaker fork gcode.
inline constexpr const char* kSnapmakerResumeGcode = "SDCARD_PRINT_PL_RESTORE";
inline constexpr const char* kSnapmakerDiscardGcode = "SDCARD_PRINT_PL_CLEAR_ENV";

/// Creality fork. The detect endpoint is a Klipper webhook that Moonraker
/// auto-registers as a JSON-RPC method (klippy_connection.py re-exports every
/// non-reserved endpoint). It is SIDE-EFFECTFUL — call at most once per
/// connection, only while print_stats.state == "standby". Never poll it.
inline constexpr const char* kCrealityDetectRpc = "printer.pause_resume.check_continue_print_state";
inline constexpr const char* kCrealityDiscardRpc = "printer.pause_resume.cancel_continue_print";

/// Sidecar holding the interrupted job's path, relative to the data root.
inline constexpr const char* kCrealitySidecarRelPath =
    "creality/userdata/config/print_file_name.json";

/// Qidi stock firmware (Q2 / Q1 Pro / Plus 4). Both are the plain gcode
/// scripts the stock screen's Yes/No buttons send; neither takes parameters,
/// so a recovery filename is display-only for this backend. RESUME_INTERRUPTED
/// rebuilds the resume gcode from the .temp/ backup, lifts Z clear of the part
/// before homing X/Y, then prints it.
inline constexpr const char* kQidiResumeGcode = "RESUME_INTERRUPTED";
inline constexpr const char* kQidiDiscardGcode = "CLEAR_LAST_FILE";
/// The boot-time check the stock screen sends. Newer Qidi firmware (Q2
/// 01.01.02.03 at least) registers RESUME_INTERRUPTED and CLEAR_LAST_FILE as Python commands, which
/// never appear as gcode_macro objects, so this macro is the one that marks
/// every Qidi firmware generation.
inline constexpr const char* kQidiDetectMacro = "DETECT_INTERRUPTION";

/// Whether the connected printer runs a firmware whose resume command is one
/// the PLR module knows (currently Qidi's stock RESUME_INTERRUPTED, recognised
/// by DETECT_INTERRUPTION or a RESUME_INTERRUPTED macro - the macro is the
/// firmware discriminator, because the was_interrupted save_variable it
/// maintains is user-writable on ANY Klipper). Capability-named on purpose:
/// generic modules (PrinterState, the subscription builder) call this without
/// learning the vendor. Out-of-line to keep this header free of the
/// PrinterDiscovery dependency.
bool plr_resume_macro_present(const PrinterDiscovery& hw);

/// Whether this firmware's saved filename is the record of an interrupted job,
/// so that an empty one means there is nothing to resume even while the
/// interrupted flag still reads true. Macro sets that resume from a saved name
/// clear only the name on discard. Firmware carrying the boot-time detection
/// macro keeps its record elsewhere and answers false.
bool plr_saved_file_gates_offer(const PrinterDiscovery& hw);

/// Status fields the PLR backends read on the discovered firmware, as a
/// printer.objects.subscribe objects map (object -> field list, or null for
/// every field), for the subscription builder to merge with
/// merge_subscription_objects. It never learns the vendor.
nlohmann::json plr_required_status_fields(const PrinterDiscovery& hw);

/// Read the firmware's own validated-snapshot flag out of a status payload:
/// 1/0 when it arrived as a JSON boolean, -1 when this frame says nothing about
/// it (absent, or Moonraker's null for a field this firmware never populates).
int plr_parse_snapshot_valid(const nlohmann::json& status);

/// The recovery file this status payload names, or nullptr when the frame
/// carries none as a string. Points into `status`.
const std::string* plr_parse_recovery_file(const nlohmann::json& status);

/// Read the passive backend's interrupted flag out of a status payload:
/// 1/0 when save_variables.variables.was_interrupted arrived as a JSON boolean,
/// -1 when this frame says nothing about it (no save_variables key, no
/// variables dict, no such entry, or a non-boolean value - Moonraker notifies
/// at top-level-field granularity, so absence in a delta is silence, not
/// false). Booleans only: save_variables values are Python literals Klipper
/// re-parses, and another type under this name is not our signal.
int plr_parse_interrupted_flag(const nlohmann::json& status);

/// Read the firmware's saved filename out of a status payload. Returns false
/// when this frame has no save_variables.variables dict and so says nothing.
/// Otherwise sets @p out: nullopt when the dict has no string under the name,
/// else the name, with the empty-quote spellings macros store normalized to "".
bool plr_parse_saved_file(const nlohmann::json& status, std::optional<std::string>& out);

/// True when this status frame carries print_stats.power_loss as a JSON number.
/// Presence, not value, is the marker (the key exists only in the firmware
/// fork that supports the active probe, and normally reads 0); a Moonraker
/// null for a subscribed-but-unpopulated field does not count.
bool plr_parse_power_loss_signal(const nlohmann::json& status);

} // namespace helix
