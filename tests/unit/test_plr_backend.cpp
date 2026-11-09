// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure tests for the two-backend Power-Loss-Recovery strategy
// (include/plr_backend.h). No LVGL, no network, no singletons.
//
// The safety-critical property under test is the CREALITY resume invariant:
// the `pause_resume/check_continue_print_state` probe is what sets
// print_stats.power_loss = 1 in firmware, and the stock sensorless-homing macro
// gates its pre-homing Z clearance lift on that flag. Resuming without a
// completed probe makes the machine lift 0.1mm and sensorless-home X/Y through
// a tall part. plr_build_plan() is where that invariant is enforced, so every
// path that could produce a resume gcode is pinned here.

#include "plr_backend.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PlrBackendType;
using helix::PlrCapabilitySignals;
using helix::PlrDetectResult;
using helix::PlrRecoveryPlan;
using json = nlohmann::json;

namespace {

/// A probe that ran and confirmed both halves — the only state that may
/// authorize a Creality resume.
PlrDetectResult confirmed_detect() {
    PlrDetectResult r;
    r.completed = true;
    r.file_state = true;
    r.eeprom_state = true;
    return r;
}

} // namespace

// ===========================================================================
// Backend selection
// ===========================================================================

TEST_CASE("plr_select_backend: no capability markers => NONE", "[plr][backend]") {
    PlrCapabilitySignals caps;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::NONE);
}

TEST_CASE("plr_select_backend: print_stats.power_loss present => CREALITY", "[plr][backend]") {
    PlrCapabilitySignals caps;
    caps.probe_capable = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::CREALITY);
}

TEST_CASE("plr_select_backend: power_loss key absent => never CREALITY", "[plr][backend]") {
    // Mainline Klipper has no power_loss key at all; Moonraker answers the
    // subscribed field with an explicit null, which the parser must NOT treat
    // as presence. Selecting CREALITY here would fire a side-effectful probe at
    // a printer whose firmware has no such endpoint.
    PlrCapabilitySignals caps;
    caps.probe_capable = false;
    caps.snapshot_valid = false;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::NONE);
}

TEST_CASE("plr_select_backend: pl_env_valid => SNAPMAKER", "[plr][backend]") {
    PlrCapabilitySignals caps;
    caps.snapshot_valid = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::SNAPMAKER);
}

TEST_CASE("plr_select_backend: both markers => SNAPMAKER wins (no probe needed)",
          "[plr][backend]") {
    PlrCapabilitySignals caps;
    caps.snapshot_valid = true;
    caps.probe_capable = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::SNAPMAKER);
}

// ===========================================================================
// Creality availability — BOTH states required
// ===========================================================================

TEST_CASE("plr_probe_recovery_available: both states true after a completed probe",
          "[plr][backend][creality]") {
    REQUIRE(helix::plr_probe_recovery_available(confirmed_detect()) == true);
}

TEST_CASE("plr_probe_recovery_available: file_state only => unavailable",
          "[plr][backend][creality]") {
    PlrDetectResult r = confirmed_detect();
    r.eeprom_state = false;
    // Without the bl24c16f EEPROM snapshot, ISCONTINUEPRINT=1 silently restarts
    // the print from the beginning. This gate is what prevents that.
    REQUIRE(helix::plr_probe_recovery_available(r) == false);
}

TEST_CASE("plr_probe_recovery_available: eeprom_state only => unavailable",
          "[plr][backend][creality]") {
    PlrDetectResult r = confirmed_detect();
    r.file_state = false;
    REQUIRE(helix::plr_probe_recovery_available(r) == false);
}

TEST_CASE("plr_probe_recovery_available: both true but probe never completed => unavailable",
          "[plr][backend][creality]") {
    PlrDetectResult r;
    r.completed = false;
    r.file_state = true;
    r.eeprom_state = true;
    REQUIRE(helix::plr_probe_recovery_available(r) == false);
}

TEST_CASE("plr_probe_recovery_available: idle printer response (both false)",
          "[plr][backend][creality]") {
    // Verbatim shape of a live idle K1C: {"result":{"file_state":false,"eeprom_state":false}}
    PlrDetectResult r;
    r.completed = true;
    REQUIRE(helix::plr_probe_recovery_available(r) == false);
}

// ===========================================================================
// Probe response parsing
// ===========================================================================

TEST_CASE("plr_parse_check_continue_response: idle K1C response parses",
          "[plr][backend][creality]") {
    json response = {{"result", {{"file_state", false}, {"eeprom_state", false}}}};
    PlrDetectResult r;
    REQUIRE(helix::plr_parse_check_continue_response(response, r) == true);
    REQUIRE(r.completed == true);
    REQUIRE(r.file_state == false);
    REQUIRE(r.eeprom_state == false);
}

TEST_CASE("plr_parse_check_continue_response: recoverable response parses",
          "[plr][backend][creality]") {
    json response = {{"result", {{"file_state", true}, {"eeprom_state", true}}}};
    PlrDetectResult r;
    REQUIRE(helix::plr_parse_check_continue_response(response, r) == true);
    REQUIRE(r.completed == true);
    REQUIRE(r.file_state == true);
    REQUIRE(r.eeprom_state == true);
}

TEST_CASE("plr_parse_check_continue_response: missing result => not completed",
          "[plr][backend][creality]") {
    json response = {{"error", {{"message", "Unknown method"}}}};
    PlrDetectResult r;
    REQUIRE(helix::plr_parse_check_continue_response(response, r) == false);
    REQUIRE(r.completed == false);
}

TEST_CASE("plr_parse_check_continue_response: non-boolean fields => not completed",
          "[plr][backend][creality]") {
    // A firmware that answers with strings must not be read as a confirmation.
    json response = {{"result", {{"file_state", "true"}, {"eeprom_state", 1}}}};
    PlrDetectResult r;
    REQUIRE(helix::plr_parse_check_continue_response(response, r) == false);
    REQUIRE(r.completed == false);
}

TEST_CASE("plr_parse_check_continue_response: half a payload => not completed",
          "[plr][backend][creality]") {
    json response = {{"result", {{"file_state", true}}}};
    PlrDetectResult r;
    REQUIRE(helix::plr_parse_check_continue_response(response, r) == false);
    REQUIRE(r.completed == false);
}

// ===========================================================================
// Plan building — Snapmaker (must be byte-identical to the pre-refactor gcode)
// ===========================================================================

TEST_CASE("plr_build_plan: SNAPMAKER uses the fork's restore/clear gcode", "[plr][backend]") {
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::SNAPMAKER, "interrupted.gcode", PlrDetectResult{});
    REQUIRE(plan.backend == PlrBackendType::SNAPMAKER);
    REQUIRE(plan.resume_gcode == "SDCARD_PRINT_PL_RESTORE");
    REQUIRE(plan.discard_gcode == "SDCARD_PRINT_PL_CLEAR_ENV");
    REQUIRE(plan.discard_rpc_method.empty());
    REQUIRE(plan.recovery_file == "interrupted.gcode");
    REQUIRE(plan.resume_allowed() == true);
}

TEST_CASE("plr_build_plan: SNAPMAKER does not depend on the Creality probe", "[plr][backend]") {
    // The Snapmaker path is passive — pl_env_valid already means "validated
    // snapshot". Requiring a probe there would break every U1.
    PlrDetectResult never_probed;
    PlrRecoveryPlan plan = helix::plr_build_plan(PlrBackendType::SNAPMAKER, "", never_probed);
    REQUIRE(plan.resume_allowed() == true);
    REQUIRE(plan.resume_gcode == "SDCARD_PRINT_PL_RESTORE");
    REQUIRE(plan.recovery_file.empty()); // prompt degrades to the generic body
}

// ===========================================================================
// Plan building — Creality, including the resume-without-detect invariant
// ===========================================================================

TEST_CASE("plr_build_plan: CREALITY builds SDCARD_PRINT_FILE with ISCONTINUEPRINT=1",
          "[plr][backend][creality]") {
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::CREALITY, "benchy.gcode", confirmed_detect());
    REQUIRE(plan.backend == PlrBackendType::CREALITY);
    REQUIRE(plan.resume_gcode == "SDCARD_PRINT_FILE FILENAME=\"benchy.gcode\" ISCONTINUEPRINT=1");
    REQUIRE(plan.discard_rpc_method == "printer.pause_resume.cancel_continue_print");
    REQUIRE(plan.discard_gcode.empty());
    REQUIRE(plan.resume_allowed() == true);
}

TEST_CASE("plr_build_plan: CREALITY refuses resume when the probe never ran",
          "[plr][backend][creality]") {
    // THE invariant. The probe is what sets print_stats.power_loss=1; the stock
    // sensorless-homing macro reads that flag to decide between a full Z
    // clearance lift and a 0.1mm lift. Resuming without it homes X/Y through
    // the part.
    PlrDetectResult never_probed;
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::CREALITY, "benchy.gcode", never_probed);
    REQUIRE(plan.resume_gcode.empty());
    REQUIRE(plan.resume_allowed() == false);
    // Discard is still safe to expose — it touches no motion.
    REQUIRE(plan.discard_rpc_method == "printer.pause_resume.cancel_continue_print");
}

TEST_CASE("plr_build_plan: CREALITY refuses resume on a half-confirmed probe",
          "[plr][backend][creality]") {
    PlrDetectResult half = confirmed_detect();
    half.eeprom_state = false;
    REQUIRE(
        helix::plr_build_plan(PlrBackendType::CREALITY, "benchy.gcode", half).resume_allowed() ==
        false);

    PlrDetectResult other_half = confirmed_detect();
    other_half.file_state = false;
    REQUIRE(helix::plr_build_plan(PlrBackendType::CREALITY, "benchy.gcode", other_half)
                .resume_allowed() == false);
}

TEST_CASE("plr_build_plan: CREALITY refuses resume without a filename",
          "[plr][backend][creality]") {
    // The gcode embeds FILENAME=; with nothing to substitute there is no safe
    // command to send at all.
    PlrRecoveryPlan plan = helix::plr_build_plan(PlrBackendType::CREALITY, "", confirmed_detect());
    REQUIRE(plan.resume_allowed() == false);
}

TEST_CASE("plr_build_plan: CREALITY refuses an injection-shaped filename",
          "[plr][backend][creality]") {
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::CREALITY, "a.gcode\nM112", confirmed_detect());
    REQUIRE(plan.resume_allowed() == false);
}

TEST_CASE("plr_build_plan: CREALITY quotes a filename containing spaces",
          "[plr][backend][creality]") {
    // Klipper tokenizes extended parameters with shlex in POSIX mode, so an
    // unquoted "my part v2.gcode" splits into three tokens, two of which carry
    // no '=' — Klipper answers "Malformed command args ... not enough values to
    // unpack". Spaces are the common case, not the exotic one: slicers put the
    // model name and filament name in the filename.
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::CREALITY, "my part v2.gcode", confirmed_detect());
    REQUIRE(plan.resume_allowed() == true);
    REQUIRE(plan.resume_gcode ==
            "SDCARD_PRINT_FILE FILENAME=\"my part v2.gcode\" ISCONTINUEPRINT=1");
}

TEST_CASE("plr_build_plan: CREALITY sends the sidecar path relative and quoted",
          "[plr][backend][creality]") {
    // Verbatim shape of the field report: the sidecar hands back an absolute
    // path and the model name contains spaces. Both have to be handled — the
    // quoting so Klipper's shlex tokenizer keeps the name in one piece, the
    // root stripping so virtual_sdcard's relative file list actually matches.
    const std::string path =
        "/usr/data/printer_data/gcodes/PTOP Phone Stand_Elegoo PLA Matte Slate Grey_1h55m.gcode";
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::CREALITY, path, confirmed_detect());
    REQUIRE(plan.resume_allowed() == true);
    REQUIRE(plan.resume_gcode ==
            "SDCARD_PRINT_FILE FILENAME=\"PTOP Phone Stand_Elegoo PLA Matte Slate "
            "Grey_1h55m.gcode\" ISCONTINUEPRINT=1");
    // The prompt body still shows what the sidecar actually said.
    REQUIRE(plan.recovery_file == path);
}

// ===========================================================================
// Sidecar path -> SDCARD_PRINT_FILE name
// ===========================================================================

TEST_CASE("plr_creality_sdcard_relative_name: strips the known data roots", "[plr][backend]") {
    REQUIRE(helix::plr_creality_sdcard_relative_name("/usr/data/printer_data/gcodes/a.gcode") ==
            "a.gcode");
    REQUIRE(helix::plr_creality_sdcard_relative_name("/mnt/UDISK/printer_data/gcodes/a.gcode") ==
            "a.gcode");
}

TEST_CASE("plr_creality_sdcard_relative_name: keeps subdirectories", "[plr][backend]") {
    // check_subdirs=True, and the file list stores "sub/a.gcode" — dropping the
    // subdirectory would miss just as badly as leaving the root on.
    REQUIRE(helix::plr_creality_sdcard_relative_name(
                "/usr/data/printer_data/gcodes/sub dir/a.gcode") == "sub dir/a.gcode");
    // A job genuinely stored in a folder called "gcodes" keeps it: the marker
    // search takes the FIRST segment, not the last.
    REQUIRE(helix::plr_creality_sdcard_relative_name("/opt/printer_data/gcodes/gcodes/a.gcode") ==
            "gcodes/a.gcode");
}

TEST_CASE("plr_creality_sdcard_relative_name: relative input passes through", "[plr][backend]") {
    REQUIRE(helix::plr_creality_sdcard_relative_name("a.gcode") == "a.gcode");
    REQUIRE(helix::plr_creality_sdcard_relative_name("sub/a.gcode") == "sub/a.gcode");
    REQUIRE(helix::plr_creality_sdcard_relative_name("").empty());
}

TEST_CASE("plr_creality_sdcard_relative_name: unrecognizable root left alone", "[plr][backend]") {
    // Nothing safe to strip. Sending it unchanged fails loudly with "Unable to
    // open file" rather than silently addressing the wrong job.
    REQUIRE(helix::plr_creality_sdcard_relative_name("/somewhere/else/a.gcode") ==
            "/somewhere/else/a.gcode");
}

// ===========================================================================
// Qidi backend - passive, like Snapmaker's
//
// The stock PRINT_START chain (save_last_file) leaves
// save_variables.variables.was_interrupted true; a normal end or cancel runs
// CLEAR_LAST_FILE, which sets it false. So the variable reads true at boot
// after power loss, and ALSO during every normal print, which the offer's
// printer_idle gate scopes to the boot case.
// ===========================================================================

TEST_CASE("plr_select_backend: qidi macro + was_interrupted => QIDI", "[plr][backend][qidi]") {
    PlrCapabilitySignals caps;
    caps.resume_macro = true;
    caps.interrupted_flag = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::QIDI);
}

TEST_CASE("plr_select_backend: was_interrupted without the macro => NONE", "[plr][backend][qidi]") {
    // save_variables entries are user-writable on ANY Klipper, so a variable
    // with this name alone must never select the backend: only the stock
    // firmware's RESUME_INTERRUPTED macro identifies Qidi.
    PlrCapabilitySignals caps;
    caps.interrupted_flag = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::NONE);
}

TEST_CASE("plr_select_backend: macro without was_interrupted => NONE", "[plr][backend][qidi]") {
    // A Qidi printer whose last print ended normally: CLEAR_LAST_FILE set the
    // variable false. Capability is not availability.
    PlrCapabilitySignals caps;
    caps.resume_macro = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::NONE);
}

TEST_CASE("plr_select_backend: SNAPMAKER outranks QIDI", "[plr][backend][qidi]") {
    // No firmware carries both markers; the order just makes the tie
    // deterministic, passive flag first.
    PlrCapabilitySignals caps;
    caps.snapshot_valid = true;
    caps.resume_macro = true;
    caps.interrupted_flag = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::SNAPMAKER);
}

TEST_CASE("plr_select_backend: QIDI outranks CREALITY", "[plr][backend][qidi]") {
    // Same tie rule: a passive signal beats one that needs a side-effectful
    // probe.
    PlrCapabilitySignals caps;
    caps.probe_capable = true;
    caps.resume_macro = true;
    caps.interrupted_flag = true;
    REQUIRE(helix::plr_select_backend(caps) == PlrBackendType::QIDI);
}

TEST_CASE("plr_build_plan: QIDI uses the stock resume/discard macros", "[plr][backend][qidi]") {
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::QIDI, "benchy.gcode", PlrDetectResult{});
    REQUIRE(plan.backend == PlrBackendType::QIDI);
    REQUIRE(plan.resume_gcode == "RESUME_INTERRUPTED");
    REQUIRE(plan.discard_gcode == "CLEAR_LAST_FILE");
    REQUIRE(plan.discard_rpc_method.empty());
    REQUIRE(plan.recovery_file == "benchy.gcode");
    REQUIRE(plan.resume_allowed() == true);
}

TEST_CASE("plr_build_plan: QIDI resume does not depend on the Creality probe",
          "[plr][backend][qidi]") {
    // Passive backend: was_interrupted already means the stock firmware
    // recorded an interrupted print. Requiring a probe would break every Qidi.
    PlrDetectResult never_probed;
    PlrRecoveryPlan plan = helix::plr_build_plan(PlrBackendType::QIDI, "", never_probed);
    REQUIRE(plan.resume_allowed() == true);
    REQUIRE(plan.resume_gcode == "RESUME_INTERRUPTED");
    // The stock macros record no filename; the prompt degrades to its generic
    // body and the gcode carries none.
    REQUIRE(plan.recovery_file.empty());
}

TEST_CASE("plr_offer_availability: an empty saved file withdraws the offer only where it gates",
          "[plr][backend][qidi]") {
    PlrCapabilitySignals caps;
    caps.resume_macro = true;
    caps.interrupted_flag = true;
    caps.status_recovery_file = "status.gcode";

    SECTION("gated, cleared") {
        caps.saved_file_gates_offer = true;
        caps.saved_recovery_file = std::string();
        CHECK_FALSE(helix::plr_offer_availability(caps, helix::PlrProbeSession{}).available);
    }
    SECTION("gated, set: the saved name is the recovery file") {
        caps.saved_file_gates_offer = true;
        caps.saved_recovery_file = "benchy.gcode";
        const auto offer = helix::plr_offer_availability(caps, helix::PlrProbeSession{});
        CHECK(offer.available);
        CHECK(offer.recovery_file == "benchy.gcode");
    }
    SECTION("gated, firmware keeps no saved name") {
        caps.saved_file_gates_offer = true;
        const auto offer = helix::plr_offer_availability(caps, helix::PlrProbeSession{});
        CHECK(offer.available);
        CHECK(offer.recovery_file == "status.gcode");
    }
    SECTION("not gated: the saved name is ignored") {
        caps.saved_recovery_file = std::string();
        const auto offer = helix::plr_offer_availability(caps, helix::PlrProbeSession{});
        CHECK(offer.available);
        CHECK(offer.recovery_file == "status.gcode");
    }
}

TEST_CASE("plr_saved_file_gates_offer: a resume macro without the detection macro",
          "[plr][backend][qidi]") {
    helix::PrinterDiscovery macro_set;
    macro_set.parse_objects(json::array({"gcode_macro RESUME_INTERRUPTED"}));
    CHECK(helix::plr_saved_file_gates_offer(macro_set));

    helix::PrinterDiscovery detecting;
    detecting.parse_objects(
        json::array({"gcode_macro RESUME_INTERRUPTED", "gcode_macro DETECT_INTERRUPTION"}));
    CHECK_FALSE(helix::plr_saved_file_gates_offer(detecting));

    helix::PrinterDiscovery neither;
    neither.parse_objects(json::array({"gcode_macro START_PRINT"}));
    CHECK_FALSE(helix::plr_saved_file_gates_offer(neither));
}

TEST_CASE("plr_parse_saved_file reads the name and normalizes empty spellings",
          "[plr][backend][qidi]") {
    const auto frame = [](json vars) { return json{{"save_variables", {{"variables", vars}}}}; };
    std::optional<std::string> out = "untouched";

    CHECK_FALSE(helix::plr_parse_saved_file(json{{"print_stats", json::object()}}, out));
    CHECK(out == "untouched");

    REQUIRE(helix::plr_parse_saved_file(frame({{"last_file", "benchy.gcode"}}), out));
    CHECK(out == "benchy.gcode");
    for (const char* empty : {"", "''", "\"\""}) {
        CAPTURE(empty);
        REQUIRE(helix::plr_parse_saved_file(frame({{"last_file", empty}}), out));
        REQUIRE(out.has_value());
        CHECK(out->empty());
    }
    REQUIRE(helix::plr_parse_saved_file(frame({{"was_interrupted", true}}), out));
    CHECK_FALSE(out.has_value());
    REQUIRE(helix::plr_parse_saved_file(frame({{"last_file", 3}}), out));
    CHECK_FALSE(out.has_value());
}

TEST_CASE("plr_resume_macro_present: keyed on the RESUME_INTERRUPTED macro",
          "[plr][backend][qidi]") {
    helix::PrinterDiscovery stock;
    stock.parse_objects(json::array({"gcode_macro RESUME_INTERRUPTED", "save_variables"}));
    REQUIRE(helix::plr_resume_macro_present(stock));

    // Detection is case-insensitive like every other macro gate.
    helix::PrinterDiscovery lower;
    lower.parse_objects(json::array({"gcode_macro resume_interrupted"}));
    REQUIRE(helix::plr_resume_macro_present(lower));

    helix::PrinterDiscovery other;
    other.parse_objects(json::array({"gcode_macro START_PRINT", "save_variables"}));
    REQUIRE_FALSE(helix::plr_resume_macro_present(other));
}

TEST_CASE("plr_resume_macro_present: DETECT_INTERRUPTION alone marks Python-command firmware",
          "[plr][backend][qidi]") {
    // Q2 firmware 01.01.02.03: RESUME_INTERRUPTED / CLEAR_LAST_FILE are Python
    // commands, so the only Qidi recovery object discovery sees is the macro.
    helix::PrinterDiscovery q2;
    q2.parse_objects(json::array({"gcode_macro DETECT_INTERRUPTION", "gcode_macro PRINT_START",
                                  "save_variables", "print_stats_manager"}));
    REQUIRE(helix::plr_resume_macro_present(q2));
    const json fields = helix::plr_required_status_fields(q2);
    REQUIRE(fields.contains("save_variables"));
    REQUIRE(fields["save_variables"].is_null());
}

// ===========================================================================
// Provider table: subscription fields, status readers, availability
// ===========================================================================

namespace {

bool lists_field(const json& fields, const char* obj, const char* field) {
    if (!fields.contains(obj) || !fields[obj].is_array()) {
        return false;
    }
    for (const auto& f : fields[obj]) {
        if (f == field) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("plr_required_status_fields: each firmware's fields only where discovery finds it",
          "[plr][backend]") {
    SECTION("mainline Klipper: only the probe marker and the recovery file") {
        helix::PrinterDiscovery plain;
        plain.parse_objects(json::array({"extruder", "heater_bed", "virtual_sdcard"}));
        const json fields = helix::plr_required_status_fields(plain);
        // The probe's marker is its own detection, so every printer carries it.
        CHECK(lists_field(fields, "print_stats", "power_loss"));
        CHECK(lists_field(fields, "virtual_sdcard", "file_path"));
        CHECK_FALSE(lists_field(fields, "virtual_sdcard", "pl_env_valid"));
        CHECK_FALSE(fields.contains("save_variables"));
    }

    SECTION("validated-snapshot firmware adds its flag") {
        helix::PrinterDiscovery u1;
        u1.parse_objects(json::array({"extruder", "filament_detect"}));
        const json fields = helix::plr_required_status_fields(u1);
        CHECK(lists_field(fields, "virtual_sdcard", "pl_env_valid"));
        CHECK(lists_field(fields, "virtual_sdcard", "file_path"));
        CHECK_FALSE(fields.contains("save_variables"));
    }
}

TEST_CASE("plr_parse_snapshot_valid: booleans only, silence is -1", "[plr][backend]") {
    CHECK(helix::plr_parse_snapshot_valid(json{{"virtual_sdcard", {{"pl_env_valid", true}}}}) == 1);
    CHECK(helix::plr_parse_snapshot_valid(json{{"virtual_sdcard", {{"pl_env_valid", false}}}}) ==
          0);
    CHECK(helix::plr_parse_snapshot_valid(json{{"virtual_sdcard", {{"pl_env_valid", nullptr}}}}) ==
          -1);
    CHECK(helix::plr_parse_snapshot_valid(json{{"virtual_sdcard", {{"progress", 0.5}}}}) == -1);
    CHECK(helix::plr_parse_snapshot_valid(json{{"print_stats", json::object()}}) == -1);
}

TEST_CASE("plr_parse_recovery_file: strings only", "[plr][backend]") {
    const json with = {{"virtual_sdcard", {{"file_path", "gcodes/a.gcode"}}}};
    const std::string* fp = helix::plr_parse_recovery_file(with);
    REQUIRE(fp != nullptr);
    CHECK(*fp == "gcodes/a.gcode");
    CHECK(helix::plr_parse_recovery_file(json{{"virtual_sdcard", {{"file_path", nullptr}}}}) ==
          nullptr);
    CHECK(helix::plr_parse_recovery_file(json{{"print_stats", json::object()}}) == nullptr);
}

TEST_CASE("plr_offer_availability: each backend's availability and recovery-file rule",
          "[plr][backend]") {
    PlrCapabilitySignals caps;
    caps.status_recovery_file = "status.gcode";
    helix::PlrProbeSession probe;
    probe.recovery_file = "probe.gcode";

    SECTION("validated snapshot: the flag is availability, file from status") {
        caps.snapshot_valid = true;
        auto a = helix::plr_offer_availability(caps, probe);
        CHECK(a.backend == PlrBackendType::SNAPMAKER);
        CHECK(a.available);
        CHECK(a.recovery_file == "status.gcode");
    }

    SECTION("macro + flag: available, file from status") {
        caps.resume_macro = true;
        caps.interrupted_flag = true;
        auto a = helix::plr_offer_availability(caps, probe);
        CHECK(a.backend == PlrBackendType::QIDI);
        CHECK(a.available);
        CHECK(a.recovery_file == "status.gcode");
    }

    SECTION("probe: unavailable until a completed probe confirms both states") {
        caps.probe_capable = true;
        auto before = helix::plr_offer_availability(caps, probe);
        CHECK(before.backend == PlrBackendType::CREALITY);
        CHECK_FALSE(before.available);
        CHECK(before.recovery_file == "probe.gcode");

        probe.result = confirmed_detect();
        CHECK(helix::plr_offer_availability(caps, probe).available);
    }

    SECTION("plugin: its own verdict, and its file only when it offers") {
        caps.plr_feature = helix::PluginFeatureState::Ready;
        caps.plugin_backend = helix::PlrPluginBackend::Generic;
        caps.plugin_recovery_file = "plugin.gcode";
        auto withheld = helix::plr_offer_availability(caps, probe);
        CHECK(withheld.backend == PlrBackendType::GENERIC);
        CHECK_FALSE(withheld.available);
        CHECK(withheld.recovery_file.empty());

        caps.plugin_offer_available = true;
        auto offered = helix::plr_offer_availability(caps, probe);
        CHECK(offered.available);
        CHECK(offered.recovery_file == "plugin.gcode");
    }

    SECTION("nothing selected") {
        auto a = helix::plr_offer_availability(caps, probe);
        CHECK(a.backend == PlrBackendType::NONE);
        CHECK_FALSE(a.available);
    }
}

TEST_CASE("plr_reoffers_after_job_drop: only the plugin re-offers on a job drop",
          "[plr][backend]") {
    CHECK(helix::plr_reoffers_after_job_drop(PlrBackendType::GENERIC));
    CHECK_FALSE(helix::plr_reoffers_after_job_drop(PlrBackendType::SNAPMAKER));
    CHECK_FALSE(helix::plr_reoffers_after_job_drop(PlrBackendType::QIDI));
    CHECK_FALSE(helix::plr_reoffers_after_job_drop(PlrBackendType::CREALITY));
    CHECK_FALSE(helix::plr_reoffers_after_job_drop(PlrBackendType::NONE));
}

TEST_CASE("plr_build_plan: NONE backend yields no actions at all", "[plr][backend]") {
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::NONE, "benchy.gcode", confirmed_detect());
    REQUIRE(plan.resume_allowed() == false);
    REQUIRE(plan.discard_available() == false);
}

// ===========================================================================
// Filename safety predicate
// ===========================================================================

TEST_CASE("plr_is_safe_recovery_filename: ordinary names accepted", "[plr][backend]") {
    REQUIRE(helix::plr_is_safe_recovery_filename("benchy.gcode") == true);
    REQUIRE(helix::plr_is_safe_recovery_filename("gcodes/sub dir/part_v2.gcode") == true);
    REQUIRE(helix::plr_is_safe_recovery_filename("Häuschen.gcode") == true);
}

TEST_CASE("plr_is_safe_recovery_filename: gcode terminators rejected", "[plr][backend]") {
    REQUIRE(helix::plr_is_safe_recovery_filename("") == false);
    REQUIRE(helix::plr_is_safe_recovery_filename("a\nM112") == false);
    REQUIRE(helix::plr_is_safe_recovery_filename("a\rM112") == false);
    REQUIRE(helix::plr_is_safe_recovery_filename("a;comment") == false);
    REQUIRE(helix::plr_is_safe_recovery_filename("a*checksum") == false);
    REQUIRE(helix::plr_is_safe_recovery_filename("a#hash") == false);
    // '=' would start a new extended parameter, truncating the filename.
    REQUIRE(helix::plr_is_safe_recovery_filename("a=b.gcode") == false);
}

TEST_CASE("plr_is_safe_recovery_filename: shlex quoting characters rejected", "[plr][backend]") {
    // The value is emitted inside double quotes, so a '"' closes it early and
    // everything after becomes fresh parameters; '\' is shlex's POSIX escape.
    REQUIRE(helix::plr_is_safe_recovery_filename("a\" ISCONTINUEPRINT=0 X=\"b") == false);
    REQUIRE(helix::plr_is_safe_recovery_filename("a\\b.gcode") == false);
}

// ===========================================================================
// Sidecar parsing (best-effort filename source)
// ===========================================================================

TEST_CASE("plr_parse_creality_sidecar: file_path extracted", "[plr][backend][creality]") {
    REQUIRE(helix::plr_parse_creality_sidecar(
                R"({"file_path":"/usr/data/printer_data/gcodes/a.gcode"})") ==
            "/usr/data/printer_data/gcodes/a.gcode");
}

TEST_CASE("plr_parse_creality_sidecar: extra keys ignored", "[plr][backend][creality]") {
    REQUIRE(helix::plr_parse_creality_sidecar(
                R"({"print_file_name":"a","file_path":"b.gcode","x":1})") == "b.gcode");
}

TEST_CASE("plr_parse_creality_sidecar: missing key => empty", "[plr][backend][creality]") {
    REQUIRE(helix::plr_parse_creality_sidecar(R"({"print_file_name":"a"})").empty());
}

TEST_CASE("plr_parse_creality_sidecar: non-string file_path => empty", "[plr][backend][creality]") {
    REQUIRE(helix::plr_parse_creality_sidecar(R"({"file_path":123})").empty());
}

TEST_CASE("plr_parse_creality_sidecar: malformed JSON => empty, no throw",
          "[plr][backend][creality]") {
    REQUIRE_NOTHROW(helix::plr_parse_creality_sidecar("{not json"));
    REQUIRE(helix::plr_parse_creality_sidecar("{not json").empty());
    REQUIRE(helix::plr_parse_creality_sidecar("").empty());
}

TEST_CASE("plr_parse_power_loss_signal: numeric presence only", "[plr][backend]") {
    using json = nlohmann::json;
    REQUIRE(helix::plr_parse_power_loss_signal(json{{"print_stats", {{"power_loss", 0}}}}));
    REQUIRE(helix::plr_parse_power_loss_signal(json{{"print_stats", {{"power_loss", 1.0}}}}));
    REQUIRE_FALSE(
        helix::plr_parse_power_loss_signal(json{{"print_stats", {{"power_loss", nullptr}}}}));
    REQUIRE_FALSE(helix::plr_parse_power_loss_signal(json{{"print_stats", {{"power_loss", "1"}}}}));
    REQUIRE_FALSE(
        helix::plr_parse_power_loss_signal(json{{"print_stats", {{"state", "standby"}}}}));
    REQUIRE_FALSE(helix::plr_parse_power_loss_signal(json{{"print_stats", 5}}));
    REQUIRE_FALSE(helix::plr_parse_power_loss_signal(json::object()));
}

// ===========================================================================
// GENERIC (HelixPrint plugin) selection: the design's precedence table
// ===========================================================================

namespace {

struct SelectRow {
    bool pl_env_valid;
    helix::PlrPluginBackend plugin_backend;
    bool legacy_qidi;
    bool creality_field;
    bool plugin_offer;
    helix::PluginFeatureState feature;
    PlrBackendType expected;
};

} // namespace

TEST_CASE("plr_select_backend: plugin backends slot into the vendor precedence",
          "[plr][backend][generic]") {
    using helix::PlrPluginBackend;
    using helix::PluginFeatureState;
    const SelectRow rows[] = {
        // pl_env_valid outranks everything, the plugin included
        {true, PlrPluginBackend::QidiPrintState, true, true, true, PluginFeatureState::Ready,
         PlrBackendType::SNAPMAKER},
        // the plugin's print_state adapter outranks the legacy QIDI macros
        {false, PlrPluginBackend::QidiPrintState, true, false, false, PluginFeatureState::Ready,
         PlrBackendType::GENERIC},
        {false, PlrPluginBackend::None, true, false, false, PluginFeatureState::Ready,
         PlrBackendType::QIDI},
        // a firmware's own recovery outranks the generic plugin
        {false, PlrPluginBackend::Generic, false, true, true, PluginFeatureState::Ready,
         PlrBackendType::CREALITY},
        {false, PlrPluginBackend::Generic, true, false, true, PluginFeatureState::Ready,
         PlrBackendType::QIDI},
        {false, PlrPluginBackend::Generic, false, false, true, PluginFeatureState::Ready,
         PlrBackendType::GENERIC},
        {false, PlrPluginBackend::None, false, false, false, PluginFeatureState::Ready,
         PlrBackendType::NONE},
        // the feature gate: absent or switched off selects nothing from the plugin
        {false, PlrPluginBackend::Generic, false, false, true, PluginFeatureState::Absent,
         PlrBackendType::NONE},
        {false, PlrPluginBackend::Generic, false, false, true, PluginFeatureState::Disabled,
         PlrBackendType::NONE},
        {false, PlrPluginBackend::QidiPrintState, true, false, true, PluginFeatureState::Absent,
         PlrBackendType::QIDI},
    };
    for (const auto& row : rows) {
        PlrCapabilitySignals caps;
        caps.snapshot_valid = row.pl_env_valid;
        caps.plugin_backend = row.plugin_backend;
        caps.resume_macro = row.legacy_qidi;
        caps.interrupted_flag = row.legacy_qidi;
        caps.probe_capable = row.creality_field;
        caps.plugin_offer_available = row.plugin_offer;
        caps.plr_feature = row.feature;
        CAPTURE(row.pl_env_valid, static_cast<int>(row.plugin_backend), row.legacy_qidi,
                row.creality_field, row.plugin_offer, static_cast<int>(row.feature));
        CHECK(helix::plr_select_backend(caps) == row.expected);
    }
}

TEST_CASE("plr_build_plan: GENERIC opens the plugin's guided resume, never a gcode",
          "[plr][backend][generic]") {
    PlrRecoveryPlan plan =
        helix::plr_build_plan(PlrBackendType::GENERIC, "part.gcode", PlrDetectResult{});
    CHECK(plan.backend == PlrBackendType::GENERIC);
    CHECK(plan.resume_gcode.empty());
    CHECK(plan.discard_gcode.empty());
    CHECK(plan.resume_rpc == std::string(helix::kPlrPlanRpc));
    CHECK(plan.discard_rpc_method == std::string(helix::kPlrDiscardRpc));
    CHECK(plan.resume_allowed());
    CHECK(plan.discard_available());
}

TEST_CASE("plr_build_plan: no firmware backend carries a plugin resume RPC",
          "[plr][backend][generic]") {
    for (PlrBackendType b : {PlrBackendType::SNAPMAKER, PlrBackendType::QIDI,
                             PlrBackendType::CREALITY, PlrBackendType::NONE}) {
        CAPTURE(static_cast<int>(b));
        CHECK(helix::plr_build_plan(b, "part.gcode", confirmed_detect()).resume_rpc.empty());
    }
}
