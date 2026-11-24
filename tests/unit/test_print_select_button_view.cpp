// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_button_view.cpp
 * @brief Pure decision behind the file detail view's Print button: Print,
 * Queue or Disabled (+ reason) from the machine-held flag, the job_queue
 * component and macro analysis.
 *
 * Run with: ./build/bin/helix-tests "[print_select][button_view]"
 *
 * No fixture: compute_print_select_button_view() is a free function. The
 * panel wiring that maps the result onto subjects is covered in
 * test_print_select_start_blocked.cpp.
 */

#include "print_select_button_view.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::compute_print_select_button_view;
using helix::ui::PrintSelectButtonInputs;
using helix::ui::PrintSelectButtonMode;

namespace {

PrintSelectButtonInputs machine_held(bool job_queue_available) {
    PrintSelectButtonInputs in;
    in.machine_busy = true;
    in.job_queue_available = job_queue_available;
    return in;
}

/// The view the panel renders as "usable": a reason slot that is empty — in
/// every mode, Queue included (queue mode's captions are contextual text the
/// panel maps, not blocks).
bool view_actionable(const helix::ui::PrintSelectButtonView& v) {
    return v.blocked_reason[0] == '\0';
}

} // namespace

TEST_CASE("button view: idle printer offers Print", "[print_select][button_view]") {
    const auto v = compute_print_select_button_view({});
    CHECK(v.mode == PrintSelectButtonMode::Print);
    CHECK(view_actionable(v));
}

TEST_CASE("button view: machine held with job_queue offers Queue", "[print_select][button_view]") {
    const auto v = compute_print_select_button_view(machine_held(true));
    CHECK(v.mode == PrintSelectButtonMode::Queue);
    CHECK(view_actionable(v));
}

TEST_CASE("button view: machine held without job_queue stays Disabled with today's reason",
          "[print_select][button_view]") {
    const auto v = compute_print_select_button_view(machine_held(false));
    CHECK(v.mode == PrintSelectButtonMode::Print);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason) == "Printing: start after this job");
}

TEST_CASE("button view: macro analysis kills queue mode but the busy reason wins",
          "[print_select][button_view]") {
    // can_start_new_print() decides first, so a machine-held print carries the
    // busy reason even when an analysis is also running. Analysis only names
    // itself on an idle machine.
    auto in = machine_held(true);
    in.macro_analysis_running = true;
    const auto v = compute_print_select_button_view(in);
    CHECK(v.mode == PrintSelectButtonMode::Print);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason) == "Printing: start after this job");
}

TEST_CASE("button view: macro analysis disables an idle printer too",
          "[print_select][button_view]") {
    PrintSelectButtonInputs in;
    in.macro_analysis_running = true;
    const auto v = compute_print_select_button_view(in);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason) == "Analyzing data...");
}

TEST_CASE("button view: a pending file check holds Print, never a queue add",
          "[print_select][button_view][file_check]") {
    PrintSelectButtonInputs in;
    in.file_check_pending = true;
    const auto v = compute_print_select_button_view(in);
    CHECK(v.mode == PrintSelectButtonMode::Print);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason) == "Checking file…");

    auto queued = machine_held(true);
    queued.file_check_pending = true;
    const auto q = compute_print_select_button_view(queued);
    CHECK(q.mode == PrintSelectButtonMode::Queue);
    CHECK(view_actionable(q));
}

TEST_CASE("button view: a committed but unconfirmed start blocks without queueing",
          "[print_select][button_view]") {
    // The machine is not held here — the block is this app's own preparing
    // job, so queue mode must not appear for it.
    PrintSelectButtonInputs in;
    in.print_start_committed = true;
    in.job_queue_available = true;
    const auto v = compute_print_select_button_view(in);
    CHECK(v.mode == PrintSelectButtonMode::Print);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason) == "Printing: start after this job");
}

TEST_CASE("button view: an in-flight queue add keeps Queue mode but disables",
          "[print_select][button_view][job_queue]") {
    PrintSelectButtonInputs in = machine_held(true);
    in.queue_add_in_flight = true;
    const auto v = compute_print_select_button_view(in);
    CHECK(v.mode == PrintSelectButtonMode::Queue);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason).find("Adding to queue") == 0);
}

TEST_CASE("button view: the print ending mid-flight still leaves the button disabled",
          "[print_select][button_view][job_queue]") {
    // The machine is idle here because the print ended while the add was on
    // the wire; an actionable button in this state is a Print tap for the
    // very file the add is about to queue.
    PrintSelectButtonInputs in;
    in.queue_add_in_flight = true;
    const auto v = compute_print_select_button_view(in);
    CHECK(v.mode == PrintSelectButtonMode::Queue);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason).find("Adding to queue") == 0);
}

TEST_CASE("button view: an in-flight add outranks the busy reason",
          "[print_select][button_view][job_queue]") {
    auto in = machine_held(false);
    in.queue_add_in_flight = true;
    const auto v = compute_print_select_button_view(in);
    CHECK(v.mode == PrintSelectButtonMode::Queue);
    CHECK_FALSE(view_actionable(v));
    CHECK(std::string(v.blocked_reason).find("Adding to queue") == 0);
}
