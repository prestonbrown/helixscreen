// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_fan_state.cpp
 * @brief What a re-run of init_fans() and a teardown do to PrinterFanState's
 *        per-fan speed subjects, and to the observers on them.
 */

#include "core/subject.h"
#include "printer_fan_state.h"
#include "printer_state.h"
#include "test_helpers/fresh_printer_state.h"

#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

/// Records every value an observer on a subject is told.
struct SeenValues {
    std::vector<int> values;
    static void cb(lv_observer_t* obs, lv_subject_t* subj) {
        static_cast<SeenValues*>(lv_observer_get_user_data(obs))
            ->values.push_back(lv_subject_get_int(subj));
    }
};

lv_observer_t* watch(lv_subject_t* subject, SeenValues& seen) {
    return lv_subject_add_observer(subject, SeenValues::cb, &seen);
}

} // namespace

TEST_CASE_METHOD(FreshPrinterState,
                 "A fan re-discovery re-aligns a reused speed subject with the speed it carries "
                 "(#1181)",
                 "[fan][reinit][subject_sync][2044]") {
    state.fan_state().init_fans({"fan"});
    state.update_from_status({{"fan", {{"speed", 0.6}}}});
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 60);

    // Force a split no production path creates: subject 0, struct still 60.
    lv_subject_set_int(state.fan_state().get_fan_speed_subject("fan"), 0);

    state.fan_state().init_fans({"fan"});

    CHECK(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 60);
}

TEST_CASE_METHOD(FreshPrinterState,
                 "A fan that discovery drops releases its speed subject's observers at once",
                 "[fan][reinit][2044]") {
    state.fan_state().init_fans({"fan", "fan_generic aux"});
    SubjectLifetime aux_lifetime;
    lv_subject_t* aux = state.fan_state().get_fan_speed_subject("fan_generic aux", aux_lifetime);
    REQUIRE(aux != nullptr);
    SeenValues seen;
    lv_observer_t* observer = watch(aux, seen);
    REQUIRE(seen.values == std::vector<int>{0});

    state.fan_state().init_fans({"fan"});

    REQUIRE(state.fan_state().get_fan_speed_subject("fan_generic aux") == nullptr);
    REQUIRE(aux_lifetime);
    REQUIRE_FALSE(*aux_lifetime);
    // Before the update queue runs: an ObserverGuard reads a dead lifetime token
    // as "my observer is already freed".
    CHECK_FALSE(helix::subject_has_observer(aux, observer));
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(FreshPrinterState, "Fan teardown releases each speed subject's observers at once",
                 "[fan][reset][2044]") {
    state.fan_state().init_fans({"fan"});
    SubjectLifetime fan_lifetime;
    lv_subject_t* fan = state.fan_state().get_fan_speed_subject("fan", fan_lifetime);
    REQUIRE(fan != nullptr);
    SeenValues seen;
    lv_observer_t* observer = watch(fan, seen);

    state.deinit_subjects();

    REQUIRE(state.fan_state().get_fan_speed_subject("fan") == nullptr);
    REQUIRE(fan_lifetime);
    REQUIRE_FALSE(*fan_lifetime);
    CHECK_FALSE(helix::subject_has_observer(fan, observer));
    helix::ui::UpdateQueue::instance().drain();
}
