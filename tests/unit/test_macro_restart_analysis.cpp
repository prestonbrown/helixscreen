// SPDX-License-Identifier: GPL-3.0-or-later
//
// A macro that WRAPS a host-restarting command is invisible to a name-only
// check. ZMOD's AUTO_FULL_BED_LEVEL reaches SAVE_CONFIG two levels down
// (ghzserg/z_ad5x@204105d), so tapping the shipped "Bed Level" button restarts
// klippy with no confirmation. These tests pin the call-graph analysis that
// resolves that from the printer's own configfile.settings.

#include "macro_executor.h"
#include "moonraker_error.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

/// Klipper lowercases section names in configfile.settings.
json macro_section(const char* lower_name, const char* gcode) {
    json section = json::object();
    section[std::string("gcode_macro ") + lower_name] = json{{"gcode", gcode}};
    return section;
}

json merge(std::initializer_list<json> parts) {
    json out = json::object();
    for (const auto& p : parts) {
        for (const auto& [k, v] : p.items()) {
            out[k] = v;
        }
    }
    return out;
}

/// The real ZMOD 1.7.x chain: bed level -> _SAVE_CONFIG wrapper -> SAVE_CONFIG.
/// The SAVE_CONFIG section is ZMOD's rename_existing override, whose own body
/// only calls the renamed builtin - the flag has to come from the wrapper.
json zmod_bed_level_config() {
    return merge({
        macro_section("auto_full_bed_level", "    _FULL_BED_LEVEL EXTRUDER_TEMP={extruder_temp}\n"
                                             "\n"
                                             "    # Turn the heat off\n"
                                             "    _STOP\n"
                                             "    _SAVE_CONFIG\n"),
        macro_section("_save_config",
                      "    {% set znew = printer.save_variables.variables['new_save_config']|"
                      "default(0)|int %}\n"
                      "    {% if znew == 1 %}\n"
                      "        NEW_SAVE_CONFIG\n"
                      "    {% else %}\n"
                      "        SAVE_CONFIG\n"
                      "    {% endif %}\n"),
        macro_section("save_config", "    M400\n"
                                     "    RESPOND PREFIX=\"info\" MSG=\"SAVE_CONFIG...\"\n"
                                     "    _ORIG_SAVE_CONFIG\n"),
        macro_section("_full_bed_level", "    BED_MESH_CLEAR FROM=_FULL_BED_LEVEL\n"
                                         "    _BED_MESH_CALIBRATE PROFILE=\"auto\"\n"),
        macro_section("_stop", "    TURN_OFF_HEATERS\n    M107\n"),
    });
}

} // namespace

TEST_CASE("Macro reaching a host restart through a wrapper is flagged", "[macro][restart]") {
    auto flagged = analyze_host_restarting_macros(zmod_bed_level_config());

    // Two levels down, and the wrapper itself.
    CHECK(flagged.count("AUTO_FULL_BED_LEVEL") == 1);
    CHECK(flagged.count("_SAVE_CONFIG") == 1);

    // Neither of these reaches a restart: _FULL_BED_LEVEL probes and returns,
    // _STOP kills the heaters. Flagging the whole config would make the
    // confirmation meaningless.
    CHECK(flagged.count("_FULL_BED_LEVEL") == 0);
    CHECK(flagged.count("_STOP") == 0);
}

TEST_CASE("A command named only inside a message is not a call", "[macro][restart]") {
    // ZMOD's own SAVE_CONFIG override carries exactly this line. Matching
    // anywhere in the body instead of in command position would flag every
    // macro that merely talks about saving.
    auto flagged = analyze_host_restarting_macros(
        macro_section("chatty", "    RESPOND PREFIX=\"info\" MSG=\"SAVE_CONFIG...\"\n"
                                "    RESPOND MSG=\"run FIRMWARE_RESTART afterwards\"\n"));
    CHECK(flagged.empty());
}

TEST_CASE("Commented-out restarts do not count", "[macro][restart]") {
    auto flagged = analyze_host_restarting_macros(macro_section("commented", "    # SAVE_CONFIG\n"
                                                                             "    ; RESTART\n"
                                                                             "    M400\n"));
    CHECK(flagged.empty());
}

TEST_CASE("A direct restart command is flagged", "[macro][restart]") {
    auto flagged = analyze_host_restarting_macros(
        macro_section("reboot_klipper", "    M400\n    FIRMWARE_RESTART\n"));
    CHECK(flagged.count("REBOOT_KLIPPER") == 1);
}

TEST_CASE("A command in a one-line Jinja branch is still a call", "[macro][restart]") {
    auto flagged = analyze_host_restarting_macros(
        macro_section("conditional_save", "    {% if params.SAVE|default(0)|int %} SAVE_CONFIG "
                                          "{% endif %}\n"));
    CHECK(flagged.count("CONDITIONAL_SAVE") == 1);
}

TEST_CASE("A macro cycle terminates and still propagates", "[macro][restart]") {
    // Mutual recursion is legal in Klipper config (it only fails at runtime),
    // so the fixpoint loop must not depend on the graph being acyclic.
    auto flagged = analyze_host_restarting_macros(merge({
        macro_section("ping", "    PONG\n"),
        macro_section("pong", "    PING\n    DEEP_SAVE\n"),
        macro_section("deep_save", "    SAVE_CONFIG\n"),
    }));
    CHECK(flagged.count("DEEP_SAVE") == 1);
    CHECK(flagged.count("PONG") == 1);
    CHECK(flagged.count("PING") == 1);
}

TEST_CASE("A cycle with no restart in it stays unflagged", "[macro][restart]") {
    auto flagged = analyze_host_restarting_macros(merge({
        macro_section("loop_a", "    LOOP_B\n"),
        macro_section("loop_b", "    LOOP_A\n"),
    }));
    CHECK(flagged.empty());
}

TEST_CASE("Malformed configfile.settings yields nothing", "[macro][restart]") {
    CHECK(analyze_host_restarting_macros(json::array()).empty());
    CHECK(analyze_host_restarting_macros(json(nullptr)).empty());
    CHECK(analyze_host_restarting_macros(json{{"gcode_macro broken", {{"gcode", 42}}}}).empty());
    CHECK(analyze_host_restarting_macros(json{{"bed_mesh", {{"speed", 120}}}}).empty());
}

TEST_CASE("PrinterDiscovery answers the restart question case-insensitively", "[macro][restart]") {
    PrinterDiscovery hw;
    CHECK_FALSE(hw.macro_restarts_host("AUTO_FULL_BED_LEVEL"));

    hw.set_host_restarting_macros(analyze_host_restarting_macros(zmod_bed_level_config()));

    CHECK(hw.macro_restarts_host("AUTO_FULL_BED_LEVEL"));
    CHECK(hw.macro_restarts_host("auto_full_bed_level"));
    CHECK_FALSE(hw.macro_restarts_host("PRINT_START"));
}

TEST_CASE("The confirmation gate covers both literal names and wrappers", "[macro][restart]") {
    PrinterDiscovery hw;

    // Literal names hold with nothing discovered at all.
    CHECK(is_dangerous_macro("SAVE_CONFIG", hw));
    CHECK(is_dangerous_macro("emergency_stop", hw));
    CHECK_FALSE(is_dangerous_macro("AUTO_FULL_BED_LEVEL", hw));

    hw.set_host_restarting_macros(analyze_host_restarting_macros(zmod_bed_level_config()));

    // THE GATE: the shipped "Bed Level" button now asks first.
    CHECK(is_dangerous_macro("AUTO_FULL_BED_LEVEL", hw));
    CHECK_FALSE(is_dangerous_macro("PRINT_START", hw));
}

// ---------------------------------------------------------------------------
// The other half of #1345: what the user is TOLD when the macro runs.
//
// Flagging the macro got it a confirmation dialog. It did not change the
// outcome report, and that is its own defect: Klipper's SAVE_CONFIG restarts
// the host as its last act and so never acks, Moonraker fails the pending
// printer.gcode.script with 503 "Klippy Disconnected", and
// execute_macro_gcode()'s error callback showed a red "<name> failed" toast
// unconditionally. So the shipped "Bed Level" button reports failure every time
// it SUCCEEDS - the #1359 shape, one layer out.
//
// classify_macro_rpc_failure() is the seam. It has to absorb the dropped rpc
// WITHOUT absorbing Klipper's own rejections, which arrive through the same
// JSON-RPC channel.
// ---------------------------------------------------------------------------

namespace {

MoonrakerError klippy_disconnect_error() {
    // Exactly what the mock produces for SAVE_CONFIG, which is modelled on what
    // Moonraker sends: a JSON-RPC error carrying Moonraker's own wording.
    return MoonrakerError::json_rpc_error("printer.gcode.script", "Klippy Disconnected");
}

MoonrakerError klipper_rejection(const char* what) {
    return MoonrakerError::json_rpc_error("printer.gcode.script", what);
}

} // namespace

TEST_CASE("A dropped rpc from a restarting macro is not a failure", "[macro][restart][1345]") {
    const auto verdict = classify_macro_rpc_failure(true, klippy_disconnect_error());
    REQUIRE(verdict == MacroFailureReport::ExpectedRestart);
}

TEST_CASE("The same error from a macro that does NOT restart is still a failure",
          "[macro][restart][1345]") {
    // The flag is half the decision. A plain macro whose rpc dies because the
    // printer went away really did fail, and the user has to hear about it.
    const auto verdict = classify_macro_rpc_failure(false, klippy_disconnect_error());
    REQUIRE(verdict == MacroFailureReport::Error);
}

TEST_CASE("Klipper's own rejection of a restarting macro is still reported",
          "[macro][restart][1345]") {
    // The trap in absorbing on the flag alone: Klipper rejects G-code through
    // the same JSON-RPC channel the disconnect arrives on. A macro that never
    // ran must not be reported as a restart in progress.
    REQUIRE(classify_macro_rpc_failure(true, klipper_rejection("Unknown command:\"BED_LEVEL\"")) ==
            MacroFailureReport::Error);
    REQUIRE(classify_macro_rpc_failure(true, klipper_rejection("Must home axis first")) ==
            MacroFailureReport::Error);
    REQUIRE(classify_macro_rpc_failure(true, klipper_rejection("Extrude below minimum temp")) ==
            MacroFailureReport::Error);
}

TEST_CASE("Transport-level failures of a restarting macro are the restart",
          "[macro][restart][1345]") {
    // A host that goes down mid-command can drop the socket or simply never
    // answer. Neither can be Klipper's opinion of the macro.
    MoonrakerError lost;
    lost.type = MoonrakerErrorType::CONNECTION_LOST;
    lost.message = "WebSocket closed";
    REQUIRE(classify_macro_rpc_failure(true, lost) == MacroFailureReport::ExpectedRestart);

    const auto timed_out = MoonrakerError::timeout("printer.gcode.script", 300000);
    REQUIRE(classify_macro_rpc_failure(true, timed_out) == MacroFailureReport::ExpectedRestart);

    const auto halted = MoonrakerError::not_ready("printer.gcode.script", "Klipper is halted");
    REQUIRE(classify_macro_rpc_failure(true, halted) == MacroFailureReport::ExpectedRestart);

    // ...and none of them get absorbed for an ordinary macro.
    REQUIRE(classify_macro_rpc_failure(false, lost) == MacroFailureReport::Error);
    REQUIRE(classify_macro_rpc_failure(false, timed_out) == MacroFailureReport::Error);
}

TEST_CASE("A 503 is read as the disconnect whatever it says", "[macro][restart][1345]") {
    // Moonraker's status code for "Klippy Disconnected". Pinned separately from
    // the message text so a reworded Moonraker release does not resurrect the
    // false failure toast.
    MoonrakerError err = klipper_rejection("Service Unavailable");
    err.code = 503;
    REQUIRE(classify_macro_rpc_failure(true, err) == MacroFailureReport::ExpectedRestart);
}

TEST_CASE("Disconnect wording is matched case- and phrasing-insensitively",
          "[macro][restart][1345]") {
    REQUIRE(classify_macro_rpc_failure(true, klipper_rejection("klippy disconnected")) ==
            MacroFailureReport::ExpectedRestart);
    REQUIRE(classify_macro_rpc_failure(true, klipper_rejection("Klippy has disconnected")) ==
            MacroFailureReport::ExpectedRestart);
    // Only one of the two words is not enough - "klippy" alone appears in
    // plenty of ordinary Moonraker errors.
    REQUIRE(classify_macro_rpc_failure(true, klipper_rejection("Klippy is not ready")) ==
            MacroFailureReport::Error);
}

TEST_CASE("The ZMOD bed-level chain reaches the report, not just the dialog",
          "[macro][restart][1345]") {
    // End to end over the two halves: the call-graph analysis flags
    // AUTO_FULL_BED_LEVEL, and that same flag is what stops its dropped rpc
    // being called a failure. Neither half is useful without the other.
    PrinterDiscovery hw;
    hw.set_host_restarting_macros(analyze_host_restarting_macros(zmod_bed_level_config()));

    const bool restarts = is_dangerous_macro("AUTO_FULL_BED_LEVEL", hw);
    REQUIRE(restarts);
    REQUIRE(classify_macro_rpc_failure(restarts, klippy_disconnect_error()) ==
            MacroFailureReport::ExpectedRestart);

    // A macro from the same config that reaches nothing dangerous keeps the
    // honest error path.
    const bool plain = is_dangerous_macro("_STOP", hw);
    REQUIRE_FALSE(plain);
    REQUIRE(classify_macro_rpc_failure(plain, klippy_disconnect_error()) ==
            MacroFailureReport::Error);
}
