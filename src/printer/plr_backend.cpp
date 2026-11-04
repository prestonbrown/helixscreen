// SPDX-License-Identifier: GPL-3.0-or-later
#include "plr_backend.h"

#include "core/text_io.h"
#include "json_utils.h"
#include "moonraker_subscription_merge.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <array>

#include "hv/json.hpp"

namespace helix {

namespace {

/// Data roots to try for the Creality recovery sidecar. K1-class images mount
/// user storage at /usr/data; the OpenWrt-class K2 uses /mnt/UDISK.
constexpr std::array<const char*, 2> kCrealityDataRoots = {"/usr/data", "/mnt/UDISK"};

/// Characters that would end the gcode command or break out of the quoted
/// `FILENAME="<f>"` value. Klipper tokenizes extended parameters with `shlex`
/// in POSIX mode, so `"` closes the value early and `\` is an escape character;
/// `;`/`#` are shlex comment introducers and `*` is a checksum marker, all of
/// which older regex-path forks strip before shlex ever runs. Spaces are NOT
/// here — that is exactly what the quoting buys us.
constexpr const char* kGcodeParamBreakers = "\n\r;#*=\"\\";

/// Path segment that ends the virtual_sdcard root in every Creality image.
constexpr const char* kGcodesDirMarker = "/gcodes/";

using json = nlohmann::json;

/// One recovery backend: which status fields it reads on a discovered printer,
/// which markers select it, and its availability and recovery-file rules.
/// The table order is the selection precedence.
struct PlrProvider {
    PlrBackendType backend;
    const char* name;
    /// Subscription objects map; an empty object when this printer cannot
    /// carry the backend.
    json (*status_fields)(const PrinterDiscovery& hw);
    bool (*selects)(const PlrCapabilitySignals& caps);
    bool (*available)(const PlrCapabilitySignals& caps, const PlrProbeSession& probe);
    std::string (*recovery_file)(const PlrCapabilitySignals& caps, const PlrProbeSession& probe);
};

json no_status_fields(const PrinterDiscovery& /*hw*/) {
    return json::object();
}

bool plugin_ready_with(const PlrCapabilitySignals& caps, PlrPluginBackend backend) {
    return caps.plr_feature == PluginFeatureState::Ready && caps.plugin_backend == backend;
}

bool plugin_offers(const PlrCapabilitySignals& caps, const PlrProbeSession& /*probe*/) {
    // The plugin decided availability itself: an interrupted newest job, a
    // snapshot for it, an unchanged file, Klipper ready.
    return caps.plugin_offer_available;
}

std::string plugin_recovery_file(const PlrCapabilitySignals& caps,
                                 const PlrProbeSession& /*probe*/) {
    return caps.plugin_offer_available ? caps.plugin_recovery_file : std::string();
}

std::string status_recovery_file(const PlrCapabilitySignals& caps,
                                 const PlrProbeSession& /*probe*/) {
    return caps.status_recovery_file;
}

const std::array<PlrProvider, 5>& providers() {
    static const std::array<PlrProvider, 5> kProviders = {{
        // Snapmaker U1 fork: virtual_sdcard.pl_env_valid is the firmware's own
        // validation of the snapshot against MCU flash on boot, so the flag IS
        // availability. filament_detect marks the firmware in discovery.
        {PlrBackendType::SNAPMAKER, "snapmaker",
         [](const PrinterDiscovery& hw) {
             return hw.has_snapmaker()
                        ? json{{"virtual_sdcard", json::array({"pl_env_valid", "file_path"})}}
                        : json::object();
         },
         [](const PlrCapabilitySignals& caps) { return caps.snapshot_valid; },
         [](const PlrCapabilitySignals&, const PlrProbeSession&) { return true; },
         status_recovery_file},
        // The plugin's print_state adapter serves the firmware layout the
        // legacy Qidi macros also live on, and it must win there: its resume
        // corrects the firmware's file offset, the legacy one does not.
        {PlrBackendType::GENERIC, "plugin (print_state)", no_status_fields,
         [](const PlrCapabilitySignals& caps) {
             return plugin_ready_with(caps, PlrPluginBackend::QidiPrintState);
         },
         plugin_offers, plugin_recovery_file},
        // Qidi stock firmware: the macro says Qidi, save_variables'
        // was_interrupted says an interrupted print is on record; the stock
        // firmware clears it on every normal end or cancel. The macros record
        // no filename, so virtual_sdcard.file_path is display-only.
        {PlrBackendType::QIDI, "qidi",
         [](const PrinterDiscovery& hw) {
             return plr_resume_macro_present(hw)
                        ? json{{"save_variables", nullptr},
                               {"virtual_sdcard", json::array({"file_path"})}}
                        : json::object();
         },
         [](const PlrCapabilitySignals& caps) {
             return caps.resume_macro && caps.interrupted_flag;
         },
         [](const PlrCapabilitySignals&, const PlrProbeSession&) { return true; },
         status_recovery_file},
        // Creality K/Ender/Hi fork: print_stats.power_loss marks the firmware,
        // and nothing in discovery does, so every printer subscribes it;
        // Moonraker answers it with null on the rest. Availability takes the
        // one-shot probe. virtual_sdcard.file_path is the fallback when the
        // sidecar names no file.
        {PlrBackendType::CREALITY, "creality",
         [](const PrinterDiscovery& /*hw*/) {
             return json{{"print_stats", json::array({"power_loss"})},
                         {"virtual_sdcard", json::array({"file_path"})}};
         },
         [](const PlrCapabilitySignals& caps) { return caps.probe_capable; },
         [](const PlrCapabilitySignals&, const PlrProbeSession& probe) {
             return plr_probe_recovery_available(probe.result);
         },
         [](const PlrCapabilitySignals&, const PlrProbeSession& probe) {
             return probe.recovery_file;
         }},
        // The plugin's own snapshot and resume, last of all, so it never
        // displaces a firmware's own recovery.
        {PlrBackendType::GENERIC, "plugin", no_status_fields,
         [](const PlrCapabilitySignals& caps) {
             return plugin_ready_with(caps, PlrPluginBackend::Generic);
         },
         plugin_offers, plugin_recovery_file},
    }};
    return kProviders;
}

const PlrProvider* select_provider(const PlrCapabilitySignals& caps) {
    for (const PlrProvider& p : providers()) {
        if (p.selects(caps)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

std::string plr_creality_sdcard_relative_name(const std::string& path) {
    if (path.empty() || path.front() != '/') {
        return path; // already relative — nothing to strip
    }
    // Preferred: the exact roots we know, so a directory that merely happens to
    // be called "gcodes" deeper in the tree cannot win.
    for (const char* root : kCrealityDataRoots) {
        std::string prefix = std::string(root) + "/printer_data/gcodes/";
        if (path.rfind(prefix, 0) == 0) {
            return path.substr(prefix.size());
        }
    }
    // Fallback for a relocated virtual_sdcard path: first "/gcodes/" segment.
    // First, not last — a job really stored in a "gcodes" subfolder must keep
    // that subfolder in its relative name.
    size_t pos = path.find(kGcodesDirMarker);
    if (pos != std::string::npos) {
        return path.substr(pos + std::string(kGcodesDirMarker).size());
    }
    spdlog::warn("[PLR] Creality recovery path '{}' has no recognizable gcodes root — "
                 "sending it unchanged",
                 path);
    return path;
}

PlrBackendType plr_select_backend(const PlrCapabilitySignals& caps) {
    // Passive markers first: choosing them never costs a side-effectful probe.
    // The markers come from different firmware forks and should never coexist,
    // but the table order makes the tie deterministic.
    const PlrProvider* p = select_provider(caps);
    return p != nullptr ? p->backend : PlrBackendType::NONE;
}

PlrOfferAvailability plr_offer_availability(const PlrCapabilitySignals& caps,
                                            const PlrProbeSession& probe) {
    PlrOfferAvailability out;
    const PlrProvider* p = select_provider(caps);
    if (p == nullptr) {
        return out;
    }
    out.backend = p->backend;
    out.available = p->available(caps, probe);
    out.recovery_file = p->recovery_file(caps, probe);
    return out;
}

const char* plr_backend_name(PlrBackendType backend) {
    switch (backend) {
    case PlrBackendType::SNAPMAKER:
        return "snapmaker";
    case PlrBackendType::CREALITY:
        return "creality";
    case PlrBackendType::QIDI:
        return "qidi";
    case PlrBackendType::GENERIC:
        return "plugin";
    case PlrBackendType::NONE:
        break;
    }
    return "none";
}

bool plr_reoffers_after_job_drop(PlrBackendType backend) {
    // The plugin offers only when Moonraker's history calls the newest job
    // interrupted. The firmware flags can lag the standby frame of a print that
    // finished, so they never offer from this edge.
    return backend == PlrBackendType::GENERIC;
}

bool plr_resume_macro_present(const PrinterDiscovery& hw) {
    return hw.has_macro(kQidiDetectMacro) || hw.has_macro(kQidiResumeGcode);
}

nlohmann::json plr_required_status_fields(const PrinterDiscovery& hw) {
    json fields = json::object();
    for (const PlrProvider& p : providers()) {
        fields = merge_subscription_objects(fields, p.status_fields(hw));
    }
    return fields;
}

int plr_parse_snapshot_valid(const nlohmann::json& status) {
    const json* sdcard = json_util::find_member(status, "virtual_sdcard");
    if (sdcard == nullptr) {
        return -1;
    }
    const json* valid = json_util::find_member(*sdcard, "pl_env_valid");
    if (valid == nullptr || !valid->is_boolean()) {
        return -1;
    }
    return valid->get<bool>() ? 1 : 0;
}

const std::string* plr_parse_recovery_file(const nlohmann::json& status) {
    const json* sdcard = json_util::find_member(status, "virtual_sdcard");
    if (sdcard == nullptr) {
        return nullptr;
    }
    const json* path = json_util::find_member(*sdcard, "file_path");
    if (path == nullptr || !path->is_string()) {
        return nullptr;
    }
    return &path->get_ref<const std::string&>();
}

int plr_parse_interrupted_flag(const nlohmann::json& status) {
    auto sv_it = status.find("save_variables");
    if (sv_it == status.end() || !sv_it->is_object()) {
        return -1;
    }
    auto vars_it = sv_it->find("variables");
    if (vars_it == sv_it->end() || !vars_it->is_object()) {
        return -1;
    }
    auto wi_it = vars_it->find("was_interrupted");
    if (wi_it == vars_it->end() || !wi_it->is_boolean()) {
        return -1;
    }
    return wi_it->get<bool>() ? 1 : 0;
}

bool plr_parse_power_loss_signal(const nlohmann::json& status) {
    auto stats_it = status.find("print_stats");
    if (stats_it == status.end() || !stats_it->is_object()) {
        return false;
    }
    auto pl_it = stats_it->find("power_loss");
    return pl_it != stats_it->end() && pl_it->is_number();
}

bool plr_probe_recovery_available(const PlrDetectResult& r) {
    // `completed` is load-bearing, not a redundancy check: it certifies that the
    // probe ran this connection, which is what set print_stats.power_loss=1 in
    // firmware. See PlrDetectResult and docs/devel/POWER_LOSS_RECOVERY.md.
    return r.completed && r.file_state && r.eeprom_state;
}

bool plr_parse_check_continue_response(const nlohmann::json& response, PlrDetectResult& out) {
    auto result_it = response.find("result");
    if (result_it == response.end() || !result_it->is_object()) {
        return false;
    }
    auto file_it = result_it->find("file_state");
    auto eeprom_it = result_it->find("eeprom_state");
    // Both must be present AND boolean. A firmware that answers with strings or
    // numbers is not one we understand, and "not understood" must never
    // authorize a resume.
    if (file_it == result_it->end() || !file_it->is_boolean() || eeprom_it == result_it->end() ||
        !eeprom_it->is_boolean()) {
        return false;
    }
    out.file_state = file_it->get<bool>();
    out.eeprom_state = eeprom_it->get<bool>();
    out.completed = true;
    return true;
}

bool plr_is_safe_recovery_filename(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    return name.find_first_of(kGcodeParamBreakers) == std::string::npos;
}

PlrRecoveryPlan plr_build_plan(PlrBackendType backend, const std::string& recovery_file,
                               const PlrDetectResult& detect) {
    PlrRecoveryPlan plan;
    plan.backend = backend;
    plan.recovery_file = recovery_file;

    switch (backend) {
    case PlrBackendType::SNAPMAKER:
        // Passive backend: pl_env_valid already IS the firmware's own
        // validation of the snapshot, so there is nothing further to confirm.
        // The gcode carries no parameters, so the filename is display-only.
        plan.resume_gcode = kSnapmakerResumeGcode;
        plan.discard_gcode = kSnapmakerDiscardGcode;
        break;

    case PlrBackendType::CREALITY:
        // Discard is always safe to expose — it touches no motion.
        plan.discard_rpc_method = kCrealityDiscardRpc;
        // The restore re-homes X/Y sensorless and (on K1/KE) never re-probes
        // Z, so the resumed layer routinely lands a millimetre or two off.
        plan.resume_layer_approximate = true;

        // SAFETY GATE. Resume is authorized ONLY by a completed probe reporting
        // both states. The probe is what sets print_stats.power_loss=1, which
        // the stock sensorless-homing macro reads to choose a full Z clearance
        // lift; without it the machine lifts 0.1mm and homes X/Y through the
        // part. Leaving resume_gcode empty is the refusal.
        if (!plr_probe_recovery_available(detect)) {
            spdlog::warn("[PLR] Creality resume withheld: detect not confirmed "
                         "(completed={} file_state={} eeprom_state={})",
                         detect.completed, detect.file_state, detect.eeprom_state);
            break;
        }
        {
            // The sidecar stores an ABSOLUTE path, but cmd_SDCARD_PRINT_FILE looks
            // the name up in virtual_sdcard's file list, which is relative to the
            // sdcard root — an absolute path just loses its leading '/' and misses,
            // giving "Unable to open file". Worse, the reopened file's absolute
            // name is compared back against the sidecar's file_path, and a mismatch
            // DELETES the recovery data and prints from the beginning. Both hinge
            // on sending the relative name.
            std::string wire_name = plr_creality_sdcard_relative_name(recovery_file);
            if (!plr_is_safe_recovery_filename(wire_name)) {
                // The gcode embeds FILENAME=, so with nothing safe to substitute
                // there is no command we can send at all.
                spdlog::warn("[PLR] Creality resume withheld: unusable recovery filename '{}'",
                             recovery_file);
                break;
            }
            // The filename MUST be double-quoted. Klipper tokenizes extended
            // parameters with shlex(posix, whitespace_split), so an unquoted name
            // containing a space splits into tokens with no `=` and the whole
            // command is rejected as "Malformed command args". Moonraker's own
            // print-start path quotes it the same way (klippy_apis.py).
            plan.resume_gcode =
                std::string("SDCARD_PRINT_FILE FILENAME=\"") + wire_name + "\" ISCONTINUEPRINT=1";
            break;
        }

    case PlrBackendType::QIDI:
        // Passive backend: selection already required was_interrupted true,
        // which only a print that never reached a normal end or cancel leaves
        // behind. Both actions are plain macros with no parameters, so the
        // filename is display-only and may be empty.
        plan.resume_gcode = kQidiResumeGcode;
        plan.discard_gcode = kQidiDiscardGcode;
        break;

    case PlrBackendType::GENERIC:
        // The plugin renders and sends the resume itself, step by step, after
        // the wizard's confirmations; the app only opens that flow. Discard
        // deletes the plugin's snapshot and touches no motion.
        plan.resume_rpc = kPlrPlanRpc;
        plan.discard_rpc_method = kPlrDiscardRpc;
        break;

    case PlrBackendType::NONE:
        break;
    }
    return plan;
}

std::string plr_parse_creality_sidecar(const std::string& json_text) {
    if (json_text.empty()) {
        return {};
    }
    // Non-throwing parse: the sidecar is firmware-owned and may be truncated if
    // power was lost mid-write, which is exactly the situation we are in.
    nlohmann::json doc = nlohmann::json::parse(json_text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        spdlog::debug("[PLR] Creality sidecar is not parseable JSON");
        return {};
    }
    auto it = doc.find("file_path");
    if (it == doc.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

std::string plr_read_probe_recovery_filename() {
    for (const char* root : kCrealityDataRoots) {
        std::string path = std::string(root) + "/" + kCrealitySidecarRelPath;
        auto text = helix::text_io::read_file(path);
        if (!text) {
            continue;
        }
        std::string name = plr_parse_creality_sidecar(*text);
        if (!name.empty()) {
            spdlog::info("[PLR] Creality recovery filename from {}: '{}'", path, name);
            return name;
        }
        spdlog::debug("[PLR] Creality sidecar {} present but yielded no file_path", path);
    }
    spdlog::debug("[PLR] No readable Creality recovery sidecar (looked for {})",
                  kCrealitySidecarRelPath);
    return {};
}

} // namespace helix
