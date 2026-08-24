// SPDX-License-Identifier: GPL-3.0-or-later
#include "plr_backend.h"

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <sstream>

#include "hv/json.hpp"

namespace helix {

namespace {

/// Data roots to try for the Creality recovery sidecar. K1-class images mount
/// user storage at /usr/data; the OpenWrt-class K2 uses /mnt/UDISK.
constexpr std::array<const char*, 2> CREALITY_DATA_ROOTS = {"/usr/data", "/mnt/UDISK"};

/// Characters that would end the gcode command or break out of the quoted
/// `FILENAME="<f>"` value. Klipper tokenizes extended parameters with `shlex`
/// in POSIX mode, so `"` closes the value early and `\` is an escape character;
/// `;`/`#` are shlex comment introducers and `*` is a checksum marker, all of
/// which older regex-path forks strip before shlex ever runs. Spaces are NOT
/// here — that is exactly what the quoting buys us.
constexpr const char* GCODE_PARAM_BREAKERS = "\n\r;#*=\"\\";

/// Path segment that ends the virtual_sdcard root in every Creality image.
constexpr const char* GCODES_DIR_MARKER = "/gcodes/";

} // namespace

std::string plr_creality_sdcard_relative_name(const std::string& path) {
    if (path.empty() || path.front() != '/') {
        return path; // already relative — nothing to strip
    }
    // Preferred: the exact roots we know, so a directory that merely happens to
    // be called "gcodes" deeper in the tree cannot win.
    for (const char* root : CREALITY_DATA_ROOTS) {
        std::string prefix = std::string(root) + "/printer_data/gcodes/";
        if (path.rfind(prefix, 0) == 0) {
            return path.substr(prefix.size());
        }
    }
    // Fallback for a relocated virtual_sdcard path: first "/gcodes/" segment.
    // First, not last — a job really stored in a "gcodes" subfolder must keep
    // that subfolder in its relative name.
    size_t pos = path.find(GCODES_DIR_MARKER);
    if (pos != std::string::npos) {
        return path.substr(pos + std::string(GCODES_DIR_MARKER).size());
    }
    spdlog::warn("[PLR] Creality recovery path '{}' has no recognizable gcodes root — "
                 "sending it unchanged",
                 path);
    return path;
}

PlrBackendType plr_select_backend(const PlrCapabilitySignals& caps) {
    // Snapmaker first: its signal is passive, so choosing it never costs a
    // side-effectful probe. The two markers come from different firmware forks
    // and should never coexist, but the ordering makes the tie deterministic.
    if (caps.snapmaker_pl_env_valid) {
        return PlrBackendType::SNAPMAKER;
    }
    if (caps.creality_power_loss_field) {
        return PlrBackendType::CREALITY;
    }
    return PlrBackendType::NONE;
}

bool plr_creality_recovery_available(const PlrDetectResult& r) {
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
    return name.find_first_of(GCODE_PARAM_BREAKERS) == std::string::npos;
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
        plan.resume_gcode = SNAPMAKER_RESUME_GCODE;
        plan.discard_gcode = SNAPMAKER_DISCARD_GCODE;
        break;

    case PlrBackendType::CREALITY:
        // Discard is always safe to expose — it touches no motion.
        plan.discard_rpc_method = CREALITY_DISCARD_RPC;

        // SAFETY GATE. Resume is authorized ONLY by a completed probe reporting
        // both states. The probe is what sets print_stats.power_loss=1, which
        // the stock sensorless-homing macro reads to choose a full Z clearance
        // lift; without it the machine lifts 0.1mm and homes X/Y through the
        // part. Leaving resume_gcode empty is the refusal.
        if (!plr_creality_recovery_available(detect)) {
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

std::string plr_read_creality_recovery_filename() {
    for (const char* root : CREALITY_DATA_ROOTS) {
        std::string path = std::string(root) + "/" + CREALITY_SIDECAR_REL_PATH;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            continue;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        std::string name = plr_parse_creality_sidecar(buf.str());
        if (!name.empty()) {
            spdlog::info("[PLR] Creality recovery filename from {}: '{}'", path, name);
            return name;
        }
        spdlog::debug("[PLR] Creality sidecar {} present but yielded no file_path", path);
    }
    spdlog::debug("[PLR] No readable Creality recovery sidecar (looked for {})",
                  CREALITY_SIDECAR_REL_PATH);
    return {};
}

} // namespace helix
