// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_print_state_thumbnail_identity.cpp
 * @brief The print thumbnail subject carries the filename it was produced for.
 *
 * `print_thumbnail_path` is a bare string subject: it says "here is a path", never
 * "here is a path FOR file X". set_print_thumbnail() attaches that identity by
 * storing `for_file` BEFORE publishing the path, so that any observer firing on the
 * path write already sees a `get_print_thumbnail_file()` that describes the path it
 * is holding.
 *
 * The ordering is the entire contract, so every assertion below that matters is made
 * from INSIDE the observer callback. Asserting after the call returns would pass with
 * the two writes in either order and would pin nothing.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "printer_state.h"

#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// What an observer of print_thumbnail_path could see at the instant it fired.
struct ThumbnailIdentityProbe {
    PrinterState* state = nullptr;
    /// (path from the subject, for_file read from PrinterState) — BOTH sampled
    /// inside the callback, which is the only place the ordering is observable.
    std::vector<std::pair<std::string, std::string>> seen;
};

void probe_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* probe = static_cast<ThumbnailIdentityProbe*>(lv_observer_get_user_data(observer));
    probe->seen.emplace_back(lv_subject_get_string(subject),
                             probe->state->print_state().get_print_thumbnail_file());
}

} // namespace

class ThumbnailIdentityFixture : public LVGLTestFixture {
  public:
    ThumbnailIdentityFixture() {
        PrinterStateTestAccess::reset(state_);
        state_.init_subjects(false);
    }

    ~ThumbnailIdentityFixture() override {
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

  private:
    PrinterState state_;
};

TEST_CASE_METHOD(ThumbnailIdentityFixture,
                 "PrinterState: an observer of the thumbnail path already sees the matching file",
                 "[printer_state][thumbnail]") {
    // Establish a DIFFERENT prior identity. Without this, "still holds the old
    // value" and "already holds the new value" are indistinguishable and the test
    // would pass with the writes in either order.
    state().print_state().set_print_thumbnail("model_a.gcode", "A:/cache/a.bin");
    REQUIRE(state().print_state().get_print_thumbnail_file() == "model_a.gcode");

    ThumbnailIdentityProbe probe;
    probe.state = &state();
    lv_observer_t* obs = lv_subject_add_observer(
        state().print_state().get_print_thumbnail_path_subject(), probe_cb, &probe);

    // LVGL fires an observer once on registration with the current value.
    REQUIRE(probe.seen.size() == 1);
    CHECK(probe.seen[0].first == "A:/cache/a.bin");
    CHECK(probe.seen[0].second == "model_a.gcode");

    SECTION("a new path publishes its file first") {
        state().print_state().set_print_thumbnail("model_b.gcode", "A:/cache/b.bin");

        REQUIRE(probe.seen.size() == 2);
        CHECK(probe.seen[1].first == "A:/cache/b.bin");
        // THE ordering assertion. Publish-then-store leaves "model_a.gcode" visible
        // here — an observer would pair print B's path with print A's filename,
        // which is exactly the mis-identification this API exists to prevent.
        CHECK(probe.seen[1].second == "model_b.gcode");
    }

    SECTION("clearing the path also re-identifies it") {
        // The clear on a new print start is a write like any other: it means
        // "nothing for model_b yet", not "nothing for model_a".
        state().print_state().set_print_thumbnail("model_b.gcode", "");

        REQUIRE(probe.seen.size() == 2);
        CHECK(probe.seen[1].first.empty());
        CHECK(probe.seen[1].second == "model_b.gcode");
    }

    SECTION("an identical path does not re-notify, but the file still updates") {
        // De-duplication is inherited from the old setter: the subject is only
        // copied when the string actually differs. The identity is unconditional.
        state().print_state().set_print_thumbnail("model_b.gcode", "A:/cache/a.bin");

        CHECK(probe.seen.size() == 1); // no second fire
        CHECK(state().print_state().get_print_thumbnail_file() == "model_b.gcode");
    }

    lv_observer_delete(obs);
}

TEST_CASE_METHOD(ThumbnailIdentityFixture,
                 "PrinterState: thumbnail identity starts empty and survives repeat writes",
                 "[printer_state][thumbnail]") {
    CHECK(state().print_state().get_print_thumbnail_file().empty());

    state().print_state().set_print_thumbnail("benchy.gcode", "A:/cache/benchy.bin");
    CHECK(state().print_state().get_print_thumbnail_file() == "benchy.gcode");
    CHECK(std::string(lv_subject_get_string(
              state().print_state().get_print_thumbnail_path_subject())) == "A:/cache/benchy.bin");

    // A full clear (print ended) drops both halves together.
    state().print_state().set_print_thumbnail("", "");
    CHECK(state().print_state().get_print_thumbnail_file().empty());
    CHECK(
        std::string(lv_subject_get_string(state().print_state().get_print_thumbnail_path_subject()))
            .empty());
}
