// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_state.cpp
 * @brief Unit tests for PrinterState::pl_env_valid / pl_recovery_file
 *
 * Snapmaker-fork Power-Loss-Recovery fields carried on virtual_sdcard.
 * Absent (harmless) on mainline Klipper; Moonraker sends explicit null for
 * subscribed fields the connected firmware doesn't populate, so parsing must
 * type-check every field rather than assume presence implies a usable value.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "printer_print_state.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

class PlrStateTestFixture : public LVGLTestFixture {
  public:
    PlrStateTestFixture() {
        PrinterStateTestAccess::reset(state_);
        state_.init_subjects(false);
    }

    ~PlrStateTestFixture() {
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

    bool pl_env_valid() {
        return state_.print_state().is_plr_snapshot_valid();
    }

    const std::string& pl_recovery_file() {
        return state_.print_state().pl_recovery_file();
    }

    bool plr_power_loss_signal() {
        return state_.print_state().is_plr_power_loss_signal();
    }

    bool qidi_plr_capable() {
        return state_.print_state().is_plr_resume_macro_present();
    }

    bool qidi_was_interrupted() {
        return state_.print_state().is_plr_interrupted_flag();
    }

  private:
    PrinterState state_;
};

} // namespace

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: default is invalid with no recovery file",
                 "[plr][state]") {
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: pl_env_valid=true + file_path parses",
                 "[plr][state]") {
    json status = {{"virtual_sdcard",
                    {{"progress", 0.0},
                     {"layer", nullptr},
                     {"layer_count", nullptr},
                     {"is_active", false},
                     {"pl_env_valid", true},
                     {"file_path", "benchy.gcode"}}}};
    state().update_from_status(status);
    REQUIRE(pl_env_valid() == true);
    REQUIRE(pl_recovery_file() == "benchy.gcode");
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: explicit null pl_env_valid leaves default",
                 "[plr][state]") {
    // Mainline Klipper (no power_loss_check module) subscribes the field but
    // Moonraker sends an explicit null since virtual_sdcard never sets it.
    json status = {{"virtual_sdcard",
                    {{"progress", 0.0},
                     {"is_active", false},
                     {"pl_env_valid", nullptr},
                     {"file_path", nullptr}}}};
    state().update_from_status(status);
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: missing fields entirely leaves default",
                 "[plr][state]") {
    json status = {{"virtual_sdcard", {{"progress", 0.0}, {"is_active", false}}}};
    state().update_from_status(status);
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: file_path change is applied (changed-guard)",
                 "[plr][state]") {
    // update_from_status only assigns pl_recovery_file_ when the incoming
    // file_path differs (avoids a per-status temporary). A genuine change must
    // still flow through.
    json first = {{"virtual_sdcard",
                   {{"progress", 0.0},
                    {"is_active", false},
                    {"pl_env_valid", true},
                    {"file_path", "first.gcode"}}}};
    state().update_from_status(first);
    REQUIRE(pl_recovery_file() == "first.gcode");

    json second = {{"virtual_sdcard",
                    {{"progress", 0.0},
                     {"is_active", false},
                     {"pl_env_valid", true},
                     {"file_path", "second.gcode"}}}};
    state().update_from_status(second);
    REQUIRE(pl_recovery_file() == "second.gcode");
}

TEST_CASE_METHOD(PlrStateTestFixture,
                 "PLR state: disconnect-edge reset clears pl_env_valid + recovery file",
                 "[plr][state]") {
    json valid = {{"virtual_sdcard",
                   {{"progress", 0.0},
                    {"is_active", false},
                    {"pl_env_valid", true},
                    {"file_path", "interrupted.gcode"}}}};
    state().update_from_status(valid);
    REQUIRE(pl_env_valid() == true);
    REQUIRE(pl_recovery_file() == "interrupted.gcode");

    // The offer controller performs exactly this on a CONNECTED->not-CONNECTED
    // edge so a reconnect re-derives a genuine 0->1 pl_env_valid edge from the
    // fresh status: force the subject to 0 and drop the stale recovery file.
    lv_subject_set_int(state().print_state().get_plr_snapshot_valid_subject(), 0);
    state().print_state().clear_pl_recovery_file();
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: pl_env_valid flips true then back to false",
                 "[plr][state]") {
    json valid = {{"virtual_sdcard",
                   {{"progress", 0.0},
                    {"is_active", false},
                    {"pl_env_valid", true},
                    {"file_path", "recovered.gcode"}}}};
    state().update_from_status(valid);
    REQUIRE(pl_env_valid() == true);

    // Boot-time recovery consumed (e.g. SDCARD_PRINT_PL_CLEAR_ENV or a fresh
    // print start clears the env) — firmware reports false on the next update.
    json cleared = {
        {"virtual_sdcard", {{"progress", 0.0}, {"is_active", false}, {"pl_env_valid", false}}}};
    state().update_from_status(cleared);
    REQUIRE(pl_env_valid() == false);
}

// ===========================================================================
// Creality backend capability — print_stats.power_loss
//
// The key exists ONLY in Creality's Klipper fork; mainline has no such field.
// PRESENCE (as a JSON number) is the capability marker, not the value — it
// normally reads 0 and only becomes 1 after the side-effectful detect probe.
// ===========================================================================

TEST_CASE_METHOD(PlrStateTestFixture, "PLR signal: default is unset", "[plr][state][creality]") {
    REQUIRE(plr_power_loss_signal() == false);
}

TEST_CASE_METHOD(PlrStateTestFixture,
                 "PLR capability: print_stats.power_loss==0 still marks Creality capable",
                 "[plr][state][creality]") {
    // This is the normal idle value on a K1C/K2. Gating on the VALUE would make
    // the backend undetectable until after a probe we would never fire.
    json status = {{"print_stats", {{"state", "standby"}, {"power_loss", 0}}}};
    state().update_from_status(status);
    REQUIRE(plr_power_loss_signal() == true);
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR capability: explicit null power_loss is NOT capability",
                 "[plr][state][creality]") {
    // Moonraker answers a subscribed-but-unpopulated field with an explicit
    // null. Treating that as presence would fire the side-effectful Creality
    // probe at every mainline-Klipper printer.
    json status = {{"print_stats", {{"state", "standby"}, {"power_loss", nullptr}}}};
    state().update_from_status(status);
    REQUIRE(plr_power_loss_signal() == false);
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR capability: absent power_loss key is NOT capability",
                 "[plr][state][creality]") {
    json status = {{"print_stats", {{"state", "standby"}}}};
    state().update_from_status(status);
    REQUIRE(plr_power_loss_signal() == false);
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR capability: latches up, survives a status delta",
                 "[plr][state][creality]") {
    // Moonraker sends DELTAS: a later print_stats notification carrying only
    // print_duration has no power_loss key. Capability must not flicker off, or
    // the offer controller would see a spurious 1->0->1 edge and re-probe.
    json first = {{"print_stats", {{"state", "standby"}, {"power_loss", 0}}}};
    state().update_from_status(first);
    REQUIRE(plr_power_loss_signal() == true);

    json delta = {{"print_stats", {{"print_duration", 12.0}}}};
    state().update_from_status(delta);
    REQUIRE(plr_power_loss_signal() == true);
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR capability: power_loss==1 is also capability",
                 "[plr][state][creality]") {
    json status = {{"print_stats", {{"state", "standby"}, {"power_loss", 1}}}};
    state().update_from_status(status);
    REQUIRE(plr_power_loss_signal() == true);
}

// ===========================================================================
// Qidi backend - save_variables.variables.was_interrupted
//
// The stock macros maintain it: PRINT_START's save_last_file sets it True, a
// normal end or cancel (CLEAR_LAST_FILE) sets it False, so it reads true at
// boot after power loss. Moonraker sends deltas at top-level-field granularity
// (the whole `variables` dict arrives when it changes), so a frame without the
// key must leave the flag alone.
// ===========================================================================

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: default is not interrupted",
                 "[plr][state][qidi]") {
    REQUIRE(qidi_was_interrupted() == false);
    REQUIRE(qidi_plr_capable() == false);
}

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: was_interrupted=true parses",
                 "[plr][state][qidi]") {
    json status = {
        {"save_variables", {{"variables", {{"was_interrupted", true}, {"print_temp", 220}}}}}};
    state().update_from_status(status);
    REQUIRE(qidi_was_interrupted() == true);
}

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: was_interrupted=false parses as clear",
                 "[plr][state][qidi]") {
    // CLEAR_LAST_FILE's write after a normal end or cancel.
    json status = {{"save_variables", {{"variables", {{"was_interrupted", false}}}}}};
    state().update_from_status(status);
    REQUIRE(qidi_was_interrupted() == false);
}

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: true then false edge clears",
                 "[plr][state][qidi]") {
    json interrupted = {{"save_variables", {{"variables", {{"was_interrupted", true}}}}}};
    state().update_from_status(interrupted);
    REQUIRE(qidi_was_interrupted() == true);

    // The discard action (CLEAR_LAST_FILE) must actually drop the flag, or a
    // reconnect within the same session would re-offer a recovery the user
    // just refused.
    json cleared = {{"save_variables", {{"variables", {{"was_interrupted", false}}}}}};
    state().update_from_status(cleared);
    REQUIRE(qidi_was_interrupted() == false);
}

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: non-boolean value is not our signal",
                 "[plr][state][qidi]") {
    // save_variables values are Python literals Klipper re-parses; another
    // type with this name is someone else's variable. Not-available means
    // never available AND never a clear.
    json string_val = {{"save_variables", {{"variables", {{"was_interrupted", "True"}}}}}};
    state().update_from_status(string_val);
    REQUIRE(qidi_was_interrupted() == false);

    json interrupted = {{"save_variables", {{"variables", {{"was_interrupted", true}}}}}};
    state().update_from_status(interrupted);
    REQUIRE(qidi_was_interrupted() == true);

    json number_val = {{"save_variables", {{"variables", {{"was_interrupted", 1}}}}}};
    state().update_from_status(number_val);
    REQUIRE(qidi_was_interrupted() == true);
}

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: explicit nulls leave the default",
                 "[plr][state][qidi]") {
    // Moonraker answers a subscribed-but-unpopulated object with an explicit
    // null; the type checks must reject the whole chain.
    json status = {{"save_variables", nullptr}};
    state().update_from_status(status);
    REQUIRE(qidi_was_interrupted() == false);

    json null_vars = {{"save_variables", {{"variables", nullptr}}}};
    state().update_from_status(null_vars);
    REQUIRE(qidi_was_interrupted() == false);
}

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: a delta without save_variables does not clear",
                 "[plr][state][qidi]") {
    json interrupted = {{"save_variables", {{"variables", {{"was_interrupted", true}}}}}};
    state().update_from_status(interrupted);
    REQUIRE(qidi_was_interrupted() == true);

    json delta = {{"print_stats", {{"print_duration", 12.0}}}};
    state().update_from_status(delta);
    REQUIRE(qidi_was_interrupted() == true);
}

TEST_CASE_METHOD(PlrStateTestFixture, "Qidi signal: variables without the key does not clear",
                 "[plr][state][qidi]") {
    // Another SAVE_VARIABLE rewrites the whole dict; a missing was_interrupted
    // key is not evidence the print was not interrupted.
    json interrupted = {{"save_variables", {{"variables", {{"was_interrupted", true}}}}}};
    state().update_from_status(interrupted);
    REQUIRE(qidi_was_interrupted() == true);

    json other_var = {{"save_variables", {{"variables", {{"bed_temp", 60}}}}}};
    state().update_from_status(other_var);
    REQUIRE(qidi_was_interrupted() == true);
}

// The disconnect-edge reset of these subjects is covered at the controller
// level in test_plr_offer_preparing.cpp: a reset performed by the test itself
// would only assert the test's own write.
